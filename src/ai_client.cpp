/*
 * Arduino Editor
 * Copyright (c) 2025 Pavel Petržela
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ai_client.hpp"
#include "ard_ev.hpp"
#include "utils.hpp"

#include <wx/defs.h>
#include <wx/event.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/secretstore.h>
#include <wx/sstream.h>
#include <wx/weakref.h>

#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include <curl/curl.h>
#include <mutex>
#include <thread>
#include <cstdio>

#if defined(__WXMSW__)
#include <windows.h>
#else
#include <array>
#include <cstdio>
#include <memory>
#include <sys/wait.h>
#endif

using json = nlohmann::json;

// ===== Internal HTTP helper using libcurl (sync, shared by sync/async AI calls) =====

namespace {

std::once_flag g_curlInitFlag;

void EnsureCurlInitialized() {
  std::call_once(g_curlInitFlag, []() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
  });
}

size_t CurlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  const size_t total = size * nmemb;
  if (!userdata || total == 0)
    return total;

  auto *out = static_cast<std::string *>(userdata);
  out->append(ptr, total);
  return total;
}

static bool ParseExtraJsonObject(const wxString &s, json &out, wxString *errOut) {
  out = json::object();

  wxString t = TrimCopy(s);

  if (t.IsEmpty())
    return true; // empty is OK

  try {
    out = json::parse(std::string(t.utf8_str()));
    if (!out.is_object()) {
      if (errOut)
        *errOut = _("Extra JSON request parameters must be a JSON object (e.g. {\"temperature\":0.2}).");
      out = json::object();
      return false;
    }
    APP_DEBUG_LOG("AICLI: Added extra json: %s", wxToStd(t).c_str());
    return true;
  } catch (const std::exception &e) {
    if (errOut) {
      *errOut = wxString::Format(_("Invalid extra request JSON: %s"),
                                 wxString::FromUTF8(e.what()));
    }
    out = json::object();
    return false;
  }
}

static void MergeExtraNoOverride(json &dst,
                                 const json &extra,
                                 const std::set<std::string> &protectedKeys) {
  if (!extra.is_object())
    return;

  for (auto it = extra.begin(); it != extra.end(); ++it) {
    const std::string key = it.key();
    if (protectedKeys.count(key))
      continue;
    dst[key] = it.value(); // allow override for non-protected keys
  }
}

static bool IsChatCompletionsEndpoint(const wxString &url) {
  wxString u = url.Lower();
  return u.Contains(wxT("/v1/chat/completions"));
}

static wxString BuildCliPrompt(const wxString &systemPrompt, const wxString &userPrompt) {
  wxString prompt;
  wxString sys = TrimCopy(systemPrompt);
  if (!sys.IsEmpty()) {
    prompt << wxT("SYSTEM:\n") << sys << wxT("\n\n");
  }
  prompt << userPrompt;
  return prompt;
}

static bool IsCodexCliExecutable(const wxString &cliPath) {
  wxFileName fn(cliPath);
  wxString name = fn.GetName().Lower();
  return name == wxT("codex") || name == wxT("codex-cli");
}

static bool IsClaudeCliExecutable(const wxString &cliPath) {
  wxFileName fn(cliPath);
  wxString name = fn.GetName().Lower();
  return name == wxT("claude") || name == wxT("claude-code");
}

struct CliInvocation {
  wxString command;
  wxString captureFile;
  wxString promptFile;
  wxString workingDir;
  bool jsonStream{false};
};

static bool FinalizeCliInvocationResult(const CliInvocation &inv,
                                        int rc,
                                        const wxString &streamText,
                                        wxString &responseOut,
                                        wxString *errorOut);

static void QueueAiProgress(wxEvtHandler *target, const wxString &text) {
  if (!target)
    return;
  auto *evt = new wxThreadEvent(wxEVT_AI_SIMPLE_CHAT_PROGRESS);
  evt->SetString(text);
  wxQueueEvent(target, evt);
}

static void DebugCliStdout(const wxString &text) {
#ifdef AI_DEBUG
  if (text.IsEmpty())
    return;
  std::fprintf(stdout, "[AICLI-DEBUG] %s\n", wxToStd(text).c_str());
  std::fflush(stdout);
#else
  (void)text;
#endif
}

static void AppendUniqueWithSpacing(wxString &dst, const wxString &src) {
  if (src.IsEmpty())
    return;
  if (!dst.IsEmpty() && !dst.EndsWith(wxT("\n")) && !src.StartsWith(wxT("\n"))) {
    dst << wxT("\n");
  }
  dst << src;
}

static void CollectCliJsonText(const json &node,
                               const std::set<std::string> &preferredKeys,
                               wxString &out,
                               int depth = 0) {
  if (depth > 6)
    return;

  if (node.is_string()) {
    AppendUniqueWithSpacing(out, wxString::FromUTF8(node.get<std::string>().c_str()));
    return;
  }

  if (node.is_array()) {
    for (const auto &item : node) {
      CollectCliJsonText(item, preferredKeys, out, depth + 1);
    }
    return;
  }

  if (!node.is_object())
    return;

  for (auto it = node.begin(); it != node.end(); ++it) {
    if (!preferredKeys.empty() && !preferredKeys.count(it.key()))
      continue;
    CollectCliJsonText(it.value(), {}, out, depth + 1);
  }

  if (!preferredKeys.empty())
    return;

  for (auto it = node.begin(); it != node.end(); ++it) {
    CollectCliJsonText(it.value(), preferredKeys, out, depth + 1);
  }
}

static wxString ExtractClaudeMessageText(const json &msg) {
  wxString text;

  try {
    if (msg.contains("content") && msg["content"].is_array()) {
      for (const auto &part : msg["content"]) {
        if (!part.is_object())
          continue;
        if (part.contains("text") && part["text"].is_string()) {
          AppendUniqueWithSpacing(text, wxString::FromUTF8(part["text"].get<std::string>().c_str()));
        }
      }
    } else {
      CollectCliJsonText(msg, {"text", "message", "content"}, text);
    }
  } catch (...) {
  }

  return text;
}

static wxString ExtractCliJsonProgressText(const json &event) {
  const std::string method = event.value("method", "");
  const std::string type = event.value("type", "");
  const json *payload = event.contains("params") ? &event["params"] : &event;

  wxString text;
  if (!type.empty()) {
    if (type == "thread.started" || type == "thread.completed" ||
        type == "turn.started" || type == "turn.completed") {
      return wxEmptyString;
    }

    if (type == "system" || type == "user") {
      return wxEmptyString;
    }

    if (type == "assistant" && event.contains("message") && event["message"].is_object()) {
      return ExtractClaudeMessageText(event["message"]);
    }

    if (type == "result") {
      if (event.value("subtype", "") == "success") {
        return wxEmptyString;
      }
      CollectCliJsonText(event, {"result", "message", "text", "stderr"}, text);
      return text;
    }

    if ((type == "item.started" || type == "item.completed") &&
        event.contains("item") && event["item"].is_object()) {
      const json &item = event["item"];
      const std::string itemType = item.value("type", "");

      if (itemType == "agent_message") {
        CollectCliJsonText(item, {"text", "message", "content"}, text);
        return text;
      }

      if (itemType == "command_execution") {
        return wxEmptyString;
      }
    }

    if (type.find("error") != std::string::npos) {
      CollectCliJsonText(event, {"message", "text", "stderr"}, text);
      return text;
    }

    if (type.find("delta") != std::string::npos ||
        type.find("message") != std::string::npos ||
        type.find("item") != std::string::npos ||
        type.find("plan") != std::string::npos ||
        type.find("tool") != std::string::npos) {
      CollectCliJsonText(event, {"delta", "text", "content", "message", "stdout", "stderr"}, text);
      return text;
    }
  }

  if (method.find("item/agentMessage/delta") != std::string::npos) {
    CollectCliJsonText(*payload, {"delta", "text", "content", "message"}, text);
    return text;
  }

  if (method.find("item/plan/delta") != std::string::npos) {
    CollectCliJsonText(*payload, {"delta", "text", "message"}, text);
    return text;
  }

  if (method.find("progress") != std::string::npos) {
    CollectCliJsonText(*payload, {"message", "text", "delta", "stdout", "stderr"}, text);
    return text;
  }

  if (method.find("error") != std::string::npos) {
    CollectCliJsonText(*payload, {"message", "text", "stderr"}, text);
    return text;
  }

  if (!type.empty()) {
    CollectCliJsonText(event, {"delta", "text", "content", "message", "stdout", "stderr"}, text);
    return text;
  }

  return wxEmptyString;
}

static bool ExtractCliFinalResponseText(const wxString &rawText, wxString &out) {
  out.clear();

  std::string raw = wxToStd(rawText);
  wxString lastAssistant;
  wxString lastResult;

  size_t pos = 0;
  while (pos < raw.size()) {
    size_t end = raw.find('\n', pos);
    if (end == std::string::npos)
      end = raw.size();
    std::string line = raw.substr(pos, end - pos);
    pos = end + 1;

    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
      start++;
    if (start > 0)
      line.erase(0, start);
    if (line.empty())
      continue;

    json evt;
    try {
      evt = json::parse(line);
    } catch (...) {
      continue;
    }

    const std::string type = evt.value("type", "");
    if (type == "assistant" && evt.contains("message") && evt["message"].is_object()) {
      wxString piece = ExtractClaudeMessageText(evt["message"]);
      if (!piece.IsEmpty())
        lastAssistant = piece;
      continue;
    }

    if (type == "result" && evt.value("subtype", "") == "success" &&
        evt.contains("result") && evt["result"].is_string()) {
      wxString piece = wxString::FromUTF8(evt["result"].get<std::string>().c_str());
      if (!piece.IsEmpty())
        lastResult = piece;
      continue;
    }

    if (type == "item.completed" && evt.contains("item") && evt["item"].is_object()) {
      const json &item = evt["item"];
      if (item.value("type", "") == "agent_message") {
        wxString piece = ExtractClaudeMessageText(item);
        if (!piece.IsEmpty())
          lastAssistant = piece;
      }
    }
  }

  if (!lastResult.IsEmpty()) {
    out = lastResult;
    return true;
  }

  if (!lastAssistant.IsEmpty()) {
    out = lastAssistant;
    return true;
  }

  return false;
}

static void QueueCliJsonProgressEvents(wxEvtHandler *target,
                                       const std::string &chunk,
                                       std::string &lineBuffer) {
  lineBuffer.append(chunk);

  size_t pos = 0;
  while (pos < lineBuffer.size()) {
    size_t end = lineBuffer.find('\n', pos);
    if (end == std::string::npos)
      break;

    std::string line = lineBuffer.substr(pos, end - pos);
    pos = end + 1;

    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
      start++;
    if (start > 0)
      line.erase(0, start);
    if (line.empty())
      continue;

    DebugCliStdout(wxString::Format(wxT("JSON line: %s"), wxString::FromUTF8(line.c_str())));

    try {
      json evt = json::parse(line);
      wxString text = ExtractCliJsonProgressText(evt);
      if (!text.IsEmpty()) {
        DebugCliStdout(wxString::Format(wxT("Extracted progress: %s"), text));
        QueueAiProgress(target, text);
      } else {
        DebugCliStdout(wxT("Extracted progress: <empty>"));
      }
    } catch (...) {
      DebugCliStdout(wxT("JSON parse failed for progress line."));
    }
  }

  if (pos > 0) {
    lineBuffer.erase(0, pos);
  }
}

static void FlushCliJsonProgressEvents(wxEvtHandler *target, std::string &lineBuffer) {
  if (lineBuffer.empty())
    return;

  DebugCliStdout(wxString::Format(wxT("JSON tail: %s"), wxString::FromUTF8(lineBuffer.c_str())));

  try {
    json evt = json::parse(lineBuffer);
    wxString text = ExtractCliJsonProgressText(evt);
    if (!text.IsEmpty()) {
      DebugCliStdout(wxString::Format(wxT("Extracted tail progress: %s"), text));
      QueueAiProgress(target, text);
    } else {
      DebugCliStdout(wxT("Extracted tail progress: <empty>"));
    }
  } catch (...) {
    DebugCliStdout(wxT("JSON parse failed for progress tail."));
  }

  lineBuffer.clear();
}

static wxString BuildCliShellCommand(const CliInvocation &inv) {
  wxString cmd = inv.command;
  if (TrimCopy(inv.workingDir).IsEmpty()) {
    return cmd + wxT(" 2>&1");
  }

  const wxString quotedDir = wxString::FromUTF8(ShellQuote(wxToStd(inv.workingDir)).c_str());

#if defined(__WXMSW__)
  return wxString::Format(wxT("cmd.exe /C cd /D %s && (%s) 2>&1"), quotedDir, cmd);
#else
  return wxString::Format(wxT("cd %s && (%s) 2>&1"), quotedDir, cmd);
#endif
}

static CliInvocation BuildCliCommand(const AiSettings &settings,
                                     const wxString &systemPrompt,
                                     const wxString &userPrompt) {
  CliInvocation inv;
  wxString prompt = BuildCliPrompt(systemPrompt, userPrompt);
  wxString args = settings.cliArgs;
  bool autoCodexExec = false;
  bool autoClaudePrint = false;

  if (TrimCopy(args).IsEmpty() && IsCodexCliExecutable(settings.cliPath)) {
    wxString capturePath = wxFileName::CreateTempFileName(wxT("ardedit_codex_"));
    wxString promptPath = wxFileName::CreateTempFileName(wxT("ardedit_codex_prompt_"));
    if (!capturePath.IsEmpty() && !promptPath.IsEmpty() && WriteTextFile(promptPath, prompt)) {
      inv.captureFile = capturePath;
      inv.promptFile = promptPath;
      args = wxString::Format(wxT("exec --json --full-auto --color never --skip-git-repo-check --output-last-message %s"),
                              wxString::FromUTF8(ShellQuote(wxToStd(capturePath)).c_str()));
      if (!TrimCopy(settings.cliWorkingDir).IsEmpty()) {
        args << wxT(" -C ") << wxString::FromUTF8(ShellQuote(wxToStd(settings.cliWorkingDir)).c_str());
      }
      args << wxT(" -");
      autoCodexExec = true;
      inv.jsonStream = true;
    }
  } else if (TrimCopy(args).IsEmpty() && IsClaudeCliExecutable(settings.cliPath)) {
    wxString promptPath = wxFileName::CreateTempFileName(wxT("ardedit_claude_prompt_"));
    if (!promptPath.IsEmpty() && WriteTextFile(promptPath, userPrompt)) {
      inv.promptFile = promptPath;
      args = wxT("-p --output-format stream-json --permission-mode acceptEdits");
      if (!TrimCopy(systemPrompt).IsEmpty()) {
        args << wxT(" --append-system-prompt ")
             << wxString::FromUTF8(ShellQuote(wxToStd(systemPrompt)).c_str());
      }
      if (!TrimCopy(settings.cliWorkingDir).IsEmpty()) {
        args << wxT(" --cwd ")
             << wxString::FromUTF8(ShellQuote(wxToStd(settings.cliWorkingDir)).c_str());
      }
      autoClaudePrint = true;
      inv.jsonStream = true;
    }
  }

  const bool hasPromptPlaceholder = args.Contains(wxT("${prompt}"));

  args.Replace(wxT("${prompt}"), wxString::FromUTF8(ShellQuote(wxToStd(prompt)).c_str()));
  args.Replace(wxT("${system_prompt}"), wxString::FromUTF8(ShellQuote(wxToStd(systemPrompt)).c_str()));
  args.Replace(wxT("${user_prompt}"), wxString::FromUTF8(ShellQuote(wxToStd(userPrompt)).c_str()));
  args.Replace(wxT("${model}"), wxString::FromUTF8(ShellQuote(wxToStd(settings.model)).c_str()));

  wxString cmd = wxString::FromUTF8(ShellQuote(wxToStd(settings.cliPath)).c_str());
  if (!TrimCopy(args).IsEmpty()) {
    cmd << wxT(" ") << args;
  }
  if ((autoCodexExec || autoClaudePrint) && !inv.promptFile.IsEmpty()) {
#if defined(__WXMSW__)
    cmd = wxString::Format(wxT("cmd.exe /C type %s ^| %s"),
                           wxString::FromUTF8(ShellQuote(wxToStd(inv.promptFile)).c_str()),
                           cmd);
#else
    cmd << wxT(" < ") << wxString::FromUTF8(ShellQuote(wxToStd(inv.promptFile)).c_str());
#endif
  } else if (!hasPromptPlaceholder) {
    cmd << wxT(" ") << wxString::FromUTF8(ShellQuote(wxToStd(prompt)).c_str());
  }
  if (autoCodexExec) {
    APP_DEBUG_LOG("AICLI: Auto-configured codex exec mode.");
  }
  if (autoClaudePrint) {
    APP_DEBUG_LOG("AICLI: Auto-configured claude print mode.");
  }
  inv.command = cmd;
  inv.workingDir = settings.cliWorkingDir;
  return inv;
}

static int RunCliCommandStreaming(const CliInvocation &inv,
                                  wxEvtHandler *target,
                                  wxString &outputOut,
                                  wxString *errorOut) {
  outputOut.clear();

#if defined(__WXMSW__)
  wxString shellCmd = BuildCliShellCommand(inv);

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;

  HANDLE hRead = NULL;
  HANDLE hWrite = NULL;
  if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
    if (errorOut)
      *errorOut = _("Failed to execute CLI process.");
    return -1;
  }

  if (!SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(hRead);
    CloseHandle(hWrite);
    if (errorOut)
      *errorOut = _("Failed to execute CLI process.");
    return -1;
  }

  int wlen = MultiByteToWideChar(CP_UTF8, 0, wxToStd(shellCmd).c_str(), -1, nullptr, 0);
  if (wlen <= 0) {
    CloseHandle(hRead);
    CloseHandle(hWrite);
    if (errorOut)
      *errorOut = _("Failed to execute CLI process.");
    return -1;
  }

  std::wstring wcmd;
  wcmd.resize(wlen - 1);
  MultiByteToWideChar(CP_UTF8, 0, wxToStd(shellCmd).c_str(), -1, &wcmd[0], wlen);

  STARTUPINFOW si{};
  si.cb = sizeof(STARTUPINFOW);
  si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.hStdOutput = hWrite;
  si.hStdError = hWrite;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(hWrite);
  if (!ok) {
    CloseHandle(hRead);
    if (errorOut)
      *errorOut = _("Failed to execute CLI process.");
    return -1;
  }

  std::string raw;
  std::string jsonLineBuffer;
  char buffer[512];
  DWORD bytesRead = 0;
  while (ReadFile(hRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
    raw.append(buffer, bytesRead);
    if (inv.jsonStream) {
      QueueCliJsonProgressEvents(target, std::string(buffer, bytesRead), jsonLineBuffer);
    } else {
      QueueAiProgress(target, wxString::FromUTF8(std::string(buffer, bytesRead).c_str()));
    }
  }
  if (inv.jsonStream) {
    FlushCliJsonProgressEvents(target, jsonLineBuffer);
  }
  CloseHandle(hRead);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 0;
  if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
    exitCode = (DWORD)-1;
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  outputOut = wxString::FromUTF8(raw.c_str());
  return (int)exitCode;
#else
  wxString shellCmd = BuildCliShellCommand(inv);
  using PipeCloser = int (*)(FILE *);
  std::unique_ptr<FILE, PipeCloser> pipe(popen(wxToStd(shellCmd).c_str(), "r"), pclose);
  if (!pipe) {
    if (errorOut)
      *errorOut = _("Failed to execute CLI process.");
    return -1;
  }

  std::array<char, 512> buffer{};
  std::string raw;
  std::string jsonLineBuffer;
  while (!feof(pipe.get())) {
    const size_t bytesRead = fread(buffer.data(), 1, buffer.size(), pipe.get());
    if (bytesRead == 0) {
      if (ferror(pipe.get())) {
        break;
      }
      continue;
    }

    raw.append(buffer.data(), bytesRead);
    if (inv.jsonStream) {
      QueueCliJsonProgressEvents(target, std::string(buffer.data(), bytesRead), jsonLineBuffer);
    } else {
      QueueAiProgress(target, wxString::FromUTF8(std::string(buffer.data(), bytesRead).c_str()));
    }
  }
  if (inv.jsonStream) {
    FlushCliJsonProgressEvents(target, jsonLineBuffer);
  }

  int status = pclose(pipe.release());
  int rc = -1;
  if (status >= 0) {
    if (WIFEXITED(status))
      rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
      rc = -WTERMSIG(status);
  }
  outputOut = wxString::FromUTF8(raw.c_str());
  return rc;
#endif
}

static bool RunCliCommand(const AiSettings &settings,
                          const wxString &systemPrompt,
                          const wxString &userPrompt,
                          wxString &responseOut,
                          wxString *errorOut) {
  responseOut.clear();

  const wxString cliPath = TrimCopy(settings.cliPath);
  if (cliPath.IsEmpty()) {
    if (errorOut)
      *errorOut = _("CLI path is empty.");
    return false;
  }

  wxFileName cliFn(cliPath);
  if (cliFn.IsAbsolute() && !cliFn.FileExists()) {
    if (errorOut)
      *errorOut = wxString::Format(_("CLI executable was not found: %s"), cliPath);
    return false;
  }

  CliInvocation inv = BuildCliCommand(settings, systemPrompt, userPrompt);
  APP_DEBUG_LOG("AICLI: CLI COMMAND: %s", wxToStd(BuildCliShellCommand(inv)).c_str());

  std::string outputUtf8;
  int rc = ArduinoCli::ExecuteCommand(wxToStd(BuildCliShellCommand(inv)), outputUtf8);
  return FinalizeCliInvocationResult(inv, rc, wxString::FromUTF8(outputUtf8.c_str()), responseOut, errorOut);
}

static bool FinalizeCliInvocationResult(const CliInvocation &inv,
                                        int rc,
                                        const wxString &streamText,
                                        wxString &responseOut,
                                        wxString *errorOut) {
  responseOut = streamText;

  if (!inv.captureFile.IsEmpty()) {
    std::string capturedUtf8;
    if (LoadFileToString(wxToStd(inv.captureFile), capturedUtf8) && !capturedUtf8.empty()) {
      responseOut = wxString::FromUTF8(capturedUtf8.c_str());
    }
    wxRemoveFile(inv.captureFile);
  } else if (inv.jsonStream) {
    wxString extracted;
    if (ExtractCliFinalResponseText(streamText, extracted) && !extracted.IsEmpty()) {
      responseOut = extracted;
    }
  }
  if (!inv.promptFile.IsEmpty()) {
    wxRemoveFile(inv.promptFile);
  }

  if (rc == -1) {
    if (errorOut) {
      *errorOut = responseOut.IsEmpty() ? _("Failed to execute CLI process.") : responseOut;
    }
    return false;
  }

  if (rc != 0) {
    if (errorOut) {
      if (!responseOut.IsEmpty()) {
        *errorOut = wxString::Format(_("CLI process failed with exit code %d.\n%s"), rc, responseOut);
      } else {
        *errorOut = wxString::Format(_("CLI process failed with exit code %d."), rc);
      }
    }
    return false;
  }

  if (responseOut.IsEmpty()) {
    if (errorOut)
      *errorOut = _("CLI process finished without output.");
    return false;
  }

  return true;
}

bool HttpPostWithCurl(const AiSettings &settings,
                      const wxString &bodyJson,
                      const wxString &apiKey,
                      wxString &responseOut,
                      wxString *errorOut) {
  responseOut.clear();

  EnsureCurlInitialized();

  CURL *curl = curl_easy_init();
  if (!curl) {
    if (errorOut) {
      *errorOut = _("Failed to initialize HTTP client (cURL).");
    }
    return false;
  }

  std::string url = wxToStd(settings.endpointUrl);
  std::string body = std::string(bodyJson.utf8_str());
  std::string apiKeyStd = std::string(apiKey.utf8_str());
  struct curl_slist *headers = nullptr;

  headers = curl_slist_append(headers, "Content-Type: application/json");

  if (!apiKeyStd.empty()) {
    std::string authHeader = "Authorization: Bearer " + apiKeyStd;
    headers = curl_slist_append(headers, authHeader.c_str());
  }

  APP_TRACE_LOG("AICLI: REQ JSON:\n%s", body.c_str());

  std::string responseBody;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  // Connect timeout: avoid hanging forever on DNS/TCP issues
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);

  // disable low-speed abort
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 0L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 0L);

  // Multi-thread safety: avoid signals in libcurl
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  // Keepalive helps with long-lived connections on some networks
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);

  long timeoutSec = 0;
  timeoutSec = settings.requestTimeout;

  if (timeoutSec > 0) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
  }

  // Security: we leave certificate verification enabled by default
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

  CURLcode res = CURLE_OK;
  long httpCode = 0;

  for (int attempt = 0; attempt < 3; attempt++) {
    responseBody.clear();
    httpCode = 0;

    res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    }

    bool success = (res == CURLE_OK && httpCode >= 200 && httpCode < 300);
    if (success) {
      responseOut = wxString::FromUTF8(responseBody.c_str());
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return true;
    }

    bool retryable = false;

    if (res != CURLE_OK) {
      retryable = true; // network / timeout / TLS hiccup
    } else if (httpCode == 429 ||
               httpCode == 500 ||
               httpCode == 502 ||
               httpCode == 503 ||
               httpCode == 504) {
      retryable = true;
    }

    if (!retryable || attempt == 2) {
      break;
    }

    int backoffMs = 300 + attempt * attempt * 700; // ~300ms, ~1000ms
    APP_DEBUG_LOG("AICLI: retry %d after %d ms (curl=%d http=%ld)",
                  attempt + 1, backoffMs, (int)res, httpCode);

    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
      if (errorOut) {
        *errorOut = _("HTTP request failed: ") +
                    wxString::FromUTF8(curl_easy_strerror(res));
      }
      return false;
    }
  }

  if (httpCode < 200 || httpCode >= 300) {
    if (errorOut) {
      wxString bodyWx = wxString::FromUTF8(responseBody.c_str());

      // Try extract JSON
      wxString detailed;
      try {
        auto errJson = json::parse(responseBody);
        if (errJson.contains("error") && errJson["error"].is_object()) {
          const auto &err = errJson["error"];
          if (err.contains("message") && err["message"].is_string()) {
            std::string msg = err["message"].get<std::string>();
            detailed = wxString::FromUTF8(msg.c_str());
          }
        }
      } catch (...) {
        // ignore
      }

      if (!detailed.empty()) {
        if (errorOut) {
          *errorOut = wxString::Format(_("HTTP error %ld from AI endpoint: %s"), httpCode, detailed);
        }
      } else {
        // fallback – part of response body
        if (bodyWx.length() > 400) {
          bodyWx.Truncate(400);
          bodyWx.Append(wxT(" [...]"));
        }
        if (errorOut) {
          *errorOut = wxString::Format(_("HTTP error %ld from AI endpoint. Response body:\n%s"), httpCode, bodyWx);
        }
      }
    }
    return false;
  }

  APP_TRACE_LOG("AICLI: RESP JSON:\n%s", responseBody.c_str());

  responseOut = wxString::FromUTF8(responseBody.c_str());
  return true;
}

} // namespace

// ===== AiClient =====

AiClient::AiClient(const AiSettings &settings)
    : m_settings(settings) {
}

bool AiClient::IsEnabled() const {
  if (!m_settings.enabled)
    return false;

  if (m_settings.UsesCliProvider()) {
    if (m_settings.cliPath.empty())
      return false;
  } else {
    if (m_settings.endpointUrl.empty())
      return false;

    if (m_settings.model.empty())
      return false;
  }

  return true;
}

bool AiClient::LoadApiKey(wxString &keyOut, wxString *errorOut) const {
  keyOut.clear();

  // NOTE:
  // Reading OPENAI_API_KEY from the environment is intended primarily for
  // local development and debugging.
  //
  // On macOS, any change to the application binary invalidates the existing
  // code signature, which in turn causes macOS to re-prompt for Keychain
  // access (including password confirmation) when using the system keychain.
  // This quickly becomes disruptive during active development.
  //
  // Using an environment variable avoids repeated Keychain permission dialogs
  // while iterating on the binary. Production builds are expected to rely on
  // the system keychain instead.
  wxString envKey;
  if (wxGetEnv(wxT("OPENAI_API_KEY"), &envKey) && !envKey.empty()) {
    keyOut = envKey;
    return true;
  }

  wxSecretStore store = wxSecretStore::GetDefault();
  if (store.IsOk()) {
    wxString serviceName = wxString::Format(wxT("ArduinoEditor/AI/%s"), m_settings.id);
    APP_DEBUG_LOG("AICLI: Loading secret API key for %s", wxToStd(serviceName).c_str());
    wxString username = wxT("api_key");
    wxSecretValue secret;
    if (store.Load(serviceName, username, secret)) {
      keyOut = secret.GetAsString();
      APP_DEBUG_LOG("AICLI: Loaded secret API key len=%u", keyOut.Length());
      if (!keyOut.empty())
        return true;
    } else {
      APP_DEBUG_LOG("AICLI: No API Key for service %s available.", wxToStd(serviceName).c_str());
    }
  }

  if (errorOut) {
    *errorOut = _("No API key found in the system secret store or OPENAI_API_KEY.");
  }
  return false;
}

bool AiClient::TestConnection(wxString *errorOut) const {
  if (!IsEnabled()) {
    if (errorOut) {
      *errorOut = _("AI is not enabled or not configured.");
    }
    return false;
  }

  wxString systemPrompt = wxT("You are a minimal test assistant.");
  wxString userPrompt = wxT("Reply with a short confirmation.");

  wxString reply;
  return SimpleChat(systemPrompt, userPrompt, reply, errorOut);
}

bool AiClient::SimpleChat(const wxString &systemPrompt,
                          const wxString &userPrompt,
                          wxString &assistantReply,
                          wxString *errorOut) const {
  assistantReply.clear();

  if (!IsEnabled()) {
    if (errorOut) {
      *errorOut = _("AI is not enabled or not configured.");
    }
    return false;
  }

  if (m_settings.UsesCliProvider()) {
    const bool ok = RunCliCommand(m_settings, systemPrompt, userPrompt, assistantReply, errorOut);
    m_lastInputTokens.store(0, std::memory_order_relaxed);
    m_lastOutputTokens.store(0, std::memory_order_relaxed);
    m_lastTotalTokens.store(0, std::memory_order_relaxed);
    return ok;
  }

  // Responses API style: instructions + input
  json j = json::object();
  j["model"] = std::string(m_settings.model.utf8_str());

  if (IsChatCompletionsEndpoint(m_settings.endpointUrl)) {
    // chat/completions shape
    j["messages"] = json::array({json{{"role", "system"}, {"content", std::string(systemPrompt.utf8_str())}},
                                 json{{"role", "user"}, {"content", std::string(userPrompt.utf8_str())}}});
  } else {
    // responses-like shape (OpenAI /v1/responses apod.)
    j["instructions"] = std::string(systemPrompt.utf8_str());
    j["input"] = std::string(userPrompt.utf8_str());
  }

  // Merge extraRequestJson (do not allow overriding core keys)
  wxString extraErr;
  json extra;
  if (!ParseExtraJsonObject(m_settings.extraRequestJson, extra, &extraErr)) {
    if (errorOut)
      *errorOut = extraErr;
    return false;
  }
  std::set<std::string> protectedKeys =
      IsChatCompletionsEndpoint(m_settings.endpointUrl)
          ? std::set<std::string>{"model", "messages"}
          : std::set<std::string>{"model", "instructions", "input"};

  MergeExtraNoOverride(j, extra, protectedKeys);

  wxString bodyJson = wxString::FromUTF8(j.dump().c_str());

  APP_DEBUG_LOG("AICLI: SYNC REQUEST:\n%s", wxToStd(bodyJson).c_str());

  wxString rawResponse, err;
  if (!DoHttpPost(bodyJson, rawResponse, &err)) {
    if (errorOut) {
      (*errorOut) = err;
    }

    APP_DEBUG_LOG("AICLI: SYNC REQUEST ERROR %s", wxToStd(err).c_str());
    return false;
  }

  wxString extractErr;
  if (!ExtractAssistantText(rawResponse, assistantReply, &extractErr)) {
    if (errorOut)
      *errorOut = extractErr.IsEmpty() ? _("Could not extract assistant text.") : extractErr;

    APP_DEBUG_LOG("AICLI: SYNC EXTRACT ERROR %s", wxToStd(err).c_str());
    return false;
  }

  if (errorOut) {
    (*errorOut) = wxEmptyString;
  }

  APP_DEBUG_LOG("AICLI: SYNC RESPONSE:\n%s", wxToStd(rawResponse).c_str());
  return true;
}

bool AiClient::SimpleChatAsync(const wxString &systemPrompt,
                               const wxString &userPrompt,
                               wxEvtHandler *handler) const {
  if (!handler) {
    return false;
  }

  if (!IsEnabled()) {
    auto *evt = new wxThreadEvent(wxEVT_AI_SIMPLE_CHAT_ERROR);
    evt->SetString(_("AI is not enabled or not configured."));
    wxQueueEvent(handler, evt);
    return false;
  }

  if (m_settings.UsesCliProvider()) {
    wxEvtHandler *target = handler;
    wxString systemCopy = systemPrompt;
    wxString userCopy = userPrompt;

    std::thread([systemCopy, userCopy, target, this]() {
      CliInvocation inv = BuildCliCommand(m_settings, systemCopy, userCopy);
      APP_DEBUG_LOG("AICLI: CLI COMMAND: %s", wxToStd(BuildCliShellCommand(inv)).c_str());

      wxString streamed;
      wxString err;
      int rc = RunCliCommandStreaming(inv, target, streamed, &err);

      wxString reply;
      if (FinalizeCliInvocationResult(inv, rc, streamed, reply, &err)) {
        auto *evt = new wxThreadEvent(wxEVT_AI_SIMPLE_CHAT_SUCCESS);
        evt->SetString(reply);
        wxQueueEvent(target, evt);
      } else {
        auto *evt = new wxThreadEvent(wxEVT_AI_SIMPLE_CHAT_ERROR);
        evt->SetString(err.IsEmpty() ? _("CLI process failed.") : err);
        wxQueueEvent(target, evt);
      }
    }).detach();

    return true;
  }

  json j = json::object();
  j["model"] = std::string(m_settings.model.utf8_str());

  if (IsChatCompletionsEndpoint(m_settings.endpointUrl)) {
    // chat/completions shape
    j["messages"] = json::array({json{{"role", "system"}, {"content", std::string(systemPrompt.utf8_str())}},
                                 json{{"role", "user"}, {"content", std::string(userPrompt.utf8_str())}}});
  } else {
    // responses-like shape (OpenAI /v1/responses apod.)
    j["instructions"] = std::string(systemPrompt.utf8_str());
    j["input"] = std::string(userPrompt.utf8_str());
  }

  // Merge extraRequestJson (do not allow overriding core keys)
  wxString extraErr;
  json extra;
  if (!ParseExtraJsonObject(m_settings.extraRequestJson, extra, &extraErr)) {
    auto *evt = new wxThreadEvent(wxEVT_AI_SIMPLE_CHAT_ERROR);
    evt->SetString(extraErr);
    wxQueueEvent(handler, evt);
    return false;
  }
  std::set<std::string> protectedKeys =
      IsChatCompletionsEndpoint(m_settings.endpointUrl)
          ? std::set<std::string>{"model", "messages"}
          : std::set<std::string>{"model", "instructions", "input"};

  MergeExtraNoOverride(j, extra, protectedKeys);

  wxString bodyJson = wxString::FromUTF8(j.dump().c_str());

  APP_DEBUG_LOG("AICLI: REQUEST:\n%s\n%s",
                wxToStd(systemPrompt).c_str(),
                wxToStd(userPrompt).c_str());

  wxEvtHandler *target = handler;
  wxString bodyCopy = bodyJson;

  // Worker thread with a blocking HTTP request via cURL,
  // returns wxEVT_AI_SIMPLE_CHAT_SUCCESS/ERROR when finished.
  std::thread([bodyCopy, target, this]() {
    wxString rawResponse;
    wxString err;

    if (!DoHttpPost(bodyCopy, rawResponse, &err)) {
      auto *evt = new wxThreadEvent(wxEVT_AI_SIMPLE_CHAT_ERROR);
      if (err.IsEmpty())
        err = _("HTTP request failed.");
      evt->SetString(err);
      wxQueueEvent(target, evt);

      APP_DEBUG_LOG("AICLI: COMMUNICATION ERROR %s", wxToStd(err).c_str());

      return;
    }

    wxString reply;
    wxString extractErr;
    if (ExtractAssistantText(rawResponse, reply, &extractErr)) {
      APP_DEBUG_LOG("AICLI: RESPONSE:\n%s", wxToStd(reply).c_str());
      auto *evt = new wxThreadEvent(wxEVT_AI_SIMPLE_CHAT_SUCCESS);
      evt->SetString(reply);
      wxQueueEvent(target, evt);
    } else {
      if (extractErr.IsEmpty())
        extractErr = _("Could not extract assistant text from AI response.");
      APP_DEBUG_LOG("AICLI: ERROR EXTRACT RESPONSE:\n%s", wxToStd(reply).c_str());
      auto *evt = new wxThreadEvent(wxEVT_AI_SIMPLE_CHAT_ERROR);
      evt->SetString(extractErr);
      wxQueueEvent(target, evt);
    }
  }).detach();

  return true;
}

bool AiClient::DoHttpPost(const wxString &bodyJson,
                          wxString &responseOut,
                          wxString *errorOut) const {
  responseOut.clear();

  wxString apiKey;
  wxString keyErr;
  if (m_settings.hasAuthentization) {
    if (!LoadApiKey(apiKey, &keyErr)) {
      // No token available -> proceed without Authorization header.
      // This enables local/self-hosted endpoints (e.g. Ollama).
      apiKey.clear();
    }
  } else {
    APP_DEBUG_LOG("AICLI: Endpoint %s has no authentication.", wxToStd(m_settings.endpointUrl).c_str());
  }

  return HttpPostWithCurl(m_settings, bodyJson, apiKey, responseOut, errorOut);
}

bool AiClient::ExtractAssistantText(const wxString &responseJson,
                                    wxString &assistantText,
                                    wxString *errorOut) const {
  assistantText.clear();

  auto SetError = [&](const wxString &msg) {
    if (errorOut)
      *errorOut = msg;
  };

  auto StoreTokenUsage = [&](int inTok, int outTok) {
    int totalTok = (inTok >= 0 && outTok >= 0) ? (inTok + outTok) : 0;
    m_lastInputTokens.store(std::max(0, inTok), std::memory_order_relaxed);
    m_lastOutputTokens.store(std::max(0, outTok), std::memory_order_relaxed);
    m_lastTotalTokens.store(std::max(0, totalTok), std::memory_order_relaxed);
  };

  auto ExtractFromSingleJsonObject = [&](const json &j, wxString &out, wxString *err) -> bool {
    // --- 0) error (OpenAI i Ollama varianty)
    try {
      if (j.contains("error")) {
        const auto &e = j["error"];
        if (e.is_string()) {
          if (err)
            *err = wxString::FromUTF8(e.get<std::string>().c_str());
          return false;
        }
        if (e.is_object()) {
          std::string msg;
          if (e.contains("message") && e["message"].is_string())
            msg = e["message"].get<std::string>();
          else if (e.contains("type") && e["type"].is_string())
            msg = e["type"].get<std::string>();
          else
            msg = "Unknown error from AI endpoint.";
          if (err)
            *err = wxString::FromUTF8(msg.c_str());
          return false;
        }
      }
    } catch (...) {
      // ignore
    }

    // --- 1) token usage: OpenAI Responses/Chat style
    try {
      if (j.contains("usage") && j["usage"].is_object()) {
        const auto &u = j["usage"];
        int inTok = 0, outTok = 0;

        if (u.contains("input_tokens") && u["input_tokens"].is_number_integer())
          inTok = u["input_tokens"].get<int>();
        else if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number_integer())
          inTok = u["prompt_tokens"].get<int>();

        if (u.contains("output_tokens") && u["output_tokens"].is_number_integer())
          outTok = u["output_tokens"].get<int>();
        else if (u.contains("completion_tokens") && u["completion_tokens"].is_number_integer())
          outTok = u["completion_tokens"].get<int>();

        StoreTokenUsage(inTok, outTok);
      }
    } catch (...) {
      // ignore
    }

    // --- 2) token usage: native Ollama (/api/chat, /api/generate)
    // prompt_eval_count ~ input tokens, eval_count ~ output tokens
    try {
      int inTok = -1, outTok = -1;
      if (j.contains("prompt_eval_count") && j["prompt_eval_count"].is_number_integer())
        inTok = j["prompt_eval_count"].get<int>();
      if (j.contains("eval_count") && j["eval_count"].is_number_integer())
        outTok = j["eval_count"].get<int>();
      if (inTok >= 0 || outTok >= 0) {
        StoreTokenUsage(std::max(0, inTok), std::max(0, outTok));
      }
    } catch (...) {
      // ignore
    }

    // --- 3) OpenAI Responses API: output[] -> type=="message" -> content[].text (maybe more parts)
    try {
      if (j.contains("output") && j["output"].is_array()) {
        wxString acc;
        for (const auto &outItem : j["output"]) {
          if (!outItem.is_object())
            continue;
          if (!outItem.contains("type") || !outItem["type"].is_string())
            continue;
          if (outItem["type"].get<std::string>() != "message")
            continue;

          if (!outItem.contains("content") || !outItem["content"].is_array())
            continue;
          for (const auto &c : outItem["content"]) {
            if (!c.is_object())
              continue;

            if (c.contains("text") && c["text"].is_string()) {
              std::string s = c["text"].get<std::string>();
              wxString w = wxString::FromUTF8(s.c_str());
              if (!w.empty())
                acc += w;
            } else if (c.contains("type") && c["type"].is_string() &&
                       c["type"].get<std::string>() == "output_text" &&
                       c.contains("text") && c["text"].is_string()) {
              std::string s = c["text"].get<std::string>();
              wxString w = wxString::FromUTF8(s.c_str());
              if (!w.empty())
                acc += w;
            }
          }
        }
        if (!acc.empty()) {
          out = acc;
          return true;
        }
      }
    } catch (...) {
      // ignore
    }

    // --- 4) OpenAI chat/completions: choices[0].message.content (string) or array parts
    try {
      if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
        const auto &choice = j["choices"][0];

        // non-stream
        if (choice.contains("message") && choice["message"].is_object() &&
            choice["message"].contains("content")) {
          const auto &mc = choice["message"]["content"];
          if (mc.is_string()) {
            out = wxString::FromUTF8(mc.get<std::string>().c_str());
            if (!out.empty())
              return true;
          }
          if (mc.is_array()) {
            wxString acc;
            for (const auto &part : mc) {
              if (!part.is_object())
                continue;
              if (part.contains("text") && part["text"].is_string()) {
                acc += wxString::FromUTF8(part["text"].get<std::string>().c_str());
              }
            }
            if (!acc.empty()) {
              out = acc;
              return true;
            }
          }
        }

        // stream delta
        if (choice.contains("delta") && choice["delta"].is_object() &&
            choice["delta"].contains("content") && choice["delta"]["content"].is_string()) {
          out = wxString::FromUTF8(choice["delta"]["content"].get<std::string>().c_str());
          if (!out.empty())
            return true;
        }
      }
    } catch (...) {
      // ignore
    }

    // --- 5) native Ollama /api/generate: { "response": "..." }
    try {
      if (j.contains("response") && j["response"].is_string()) {
        out = wxString::FromUTF8(j["response"].get<std::string>().c_str());
        if (!out.empty())
          return true;
      }
    } catch (...) {
      // ignore
    }

    // --- 6) native Ollama /api/chat: { "message": { "content": "..." } }
    try {
      if (j.contains("message") && j["message"].is_object()) {
        const auto &m = j["message"];
        if (m.contains("content") && m["content"].is_string()) {
          out = wxString::FromUTF8(m["content"].get<std::string>().c_str());
          if (!out.empty())
            return true;
        }
      }
    } catch (...) {
      // ignore
    }

    if (err) {
      wxString preview = wxString::FromUTF8(j.dump().c_str());
      if (preview.length() > 400) {
        preview.Truncate(400);
        preview.Append(wxT(" [...]"));
      }
      *err = _("Could not extract assistant text from AI response.\nRaw JSON (truncated):\n") + preview;
    }
    return false;
  };

  // ===== A) Try parse as one JSON (OpenAI / Ollama non-stream)
  try {
    json j = json::parse(std::string(responseJson.utf8_str()));
    wxString err;
    if (ExtractFromSingleJsonObject(j, assistantText, &err)) {
      if (errorOut)
        *errorOut = wxEmptyString;
      return true;
    }
    if (!err.empty())
      SetError(err);
    return false;
  } catch (const std::exception &) {
    // fallthrough: maybe JSONL/NDJSON stream
  }

  // ===== B) Fallback: NDJSON/JSONL (streaming)
  try {
    std::string raw = std::string(responseJson.utf8_str());
    wxString acc;
    wxString lastErr;

    size_t pos = 0;
    while (pos < raw.size()) {
      size_t end = raw.find('\n', pos);
      if (end == std::string::npos)
        end = raw.size();
      std::string line = raw.substr(pos, end - pos);
      pos = end + 1;

      // trim
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
      size_t start = 0;
      while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
        start++;
      if (start > 0)
        line.erase(0, start);

      if (line.empty())
        continue;

      json jline;
      try {
        jline = json::parse(line);
      } catch (...) {
        continue;
      }

      wxString piece;
      wxString err;
      if (!ExtractFromSingleJsonObject(jline, piece, &err)) {
        if (!err.empty())
          lastErr = err;
        continue;
      }

      if (!piece.empty())
        acc += piece;
    }

    if (!acc.empty()) {
      assistantText = acc;
      if (errorOut)
        *errorOut = wxEmptyString;
      return true;
    }

    if (!lastErr.empty())
      SetError(lastErr);
    else {
      wxString preview = responseJson;
      if (preview.length() > 400) {
        preview.Truncate(400);
        preview.Append(wxT(" [...]"));
      }
      SetError(_("Could not extract assistant text from AI response.\nRaw response (truncated):\n") + preview);
    }
    return false;

  } catch (const std::exception &e) {
    SetError(wxString::Format(_("Failed to parse AI response: %s"),
                              wxString::FromUTF8(e.what())));
    return false;
  }
}

bool AiClient::CheckExtraRequestJson(const wxString &s, wxString *errorOut) {
  json extra;
  wxString err;
  bool ok = ParseExtraJsonObject(s, extra, &err);
  if (!ok && errorOut)
    *errorOut = err;
  if (ok && errorOut)
    *errorOut = wxEmptyString;
  return ok;
}

int AiClient::GetLastInputTokens() const {
  return m_lastInputTokens.load(std::memory_order_relaxed);
}

int AiClient::GetLastOutputTokens() const {
  return m_lastOutputTokens.load(std::memory_order_relaxed);
}

int AiClient::GetLastTotalTokens() const {
  return m_lastTotalTokens.load(std::memory_order_relaxed);
}

wxString AiClient::GetModelName() const {
  if (m_settings.UsesCliProvider()) {
    wxFileName fn(m_settings.cliPath);
    if (!fn.GetFullName().IsEmpty())
      return fn.GetFullName();
    return m_settings.cliPath;
  }
  return m_settings.model;
}
