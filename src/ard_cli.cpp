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

#include "ard_cli.hpp"

#include "ard_cc.hpp"
#include "ard_ev.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <wx/app.h>

#if defined(__WXGTK__) || defined(__WXMAC__)
#include <sys/types.h>
#include <sys/wait.h>
#else
#include <windows.h>
#endif

#define DEFAULT_FQBN "arduino:avr:uno"

using nlohmann::json;

namespace fs = std::filesystem;

#if defined(__WXMSW__)
// Windows function for converting between the native system encoding and utf-8.
static std::string WideToUtf8(const std::wstring &w) {
  if (w.empty())
    return std::string();
  int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0)
    return std::string();
  std::string s;
  s.resize(len - 1);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
  return s;
}

static std::string LocalToUtf8(const std::string &s) {
  if (s.empty())
    return std::string();

  // ANSI / "local 8-bit" -> UTF-16
  int wlen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
  if (wlen <= 0) {
    return std::string();
  }

  std::wstring w;
  w.resize(wlen - 1);
  MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], wlen);

  // UTF-16 -> UTF-8
  return WideToUtf8(w);
}
#endif // __WXMSW__

static std::string NormalizeArchTarget(const std::string &s) {
  std::string resl = s;
  std::transform(resl.begin(), resl.end(), resl.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return resl;
}

static bool IsInterestingFlag(const std::string &a) {
  if (a.rfind("-D", 0) == 0)
    return true; // starts with "-D"
  if (a.rfind("-U", 0) == 0)
    return true;
  if (a.rfind("-I", 0) == 0)
    return true;
  if (a.rfind("-isystem", 0) == 0)
    return true;
  if (a.rfind("-include", 0) == 0)
    return true;
  if (a.rfind("-std=", 0) == 0)
    return true;
  if (a == "-x")
    return true;
  if (a.rfind("-mmcu=", 0) == 0)
    return true;
  return false;
}

static std::string NormalizeStdFlag(const std::string &a) {
  if (a.compare(0, 5, "-std=") != 0)
    return a;
  std::string val = a.substr(5);
  if (val == "gnu++2b" || val == "c++2b" ||
      val == "gnu++2a" || val == "c++2a") {
    return "-std=gnu++17"; // or c++20 depending on what libclang supports
  }
  return a;
}

static void ParseLibraryRelease(const json &jr, const std::string &versionKey, ArduinoLibraryRelease &out) {
  // version - prefer the map key (versionKey), fallback to "version" from JSON
  if (!versionKey.empty()) {
    out.version = versionKey;
  } else {
    out.version = jr.value("version", "");
  }

  out.author = jr.value("author", "");
  out.maintainer = jr.value("maintainer", "");
  out.sentence = jr.value("sentence", "");
  out.paragraph = jr.value("paragraph", "");
  out.website = jr.value("website", "");
  out.category = jr.value("category", "");

  // architectures[]
  if (jr.contains("architectures") && jr["architectures"].is_array()) {
    for (const auto &a : jr["architectures"]) {
      if (a.is_string()) {
        out.architectures.push_back(a.get<std::string>());
      }
    }
  }

  // types[]
  if (jr.contains("types") && jr["types"].is_array()) {
    for (const auto &t : jr["types"]) {
      if (t.is_string()) {
        out.types.push_back(t.get<std::string>());
      }
    }
  }

  // resources{...}
  if (jr.contains("resources") && jr["resources"].is_object()) {
    const auto &res = jr["resources"];
    out.url = res.value("url", "");
    out.archiveFileName = res.value("archive_filename", "");
    out.checksum = res.value("checksum", "");
    out.size = res.value("size", 0);
  }

  // provides_includes[]
  if (jr.contains("provides_includes") && jr["provides_includes"].is_array()) {
    for (const auto &inc : jr["provides_includes"]) {
      if (inc.is_string()) {
        out.providesIncludes.push_back(inc.get<std::string>());
      }
    }
  }

  // ---- dependencies ----
  if (jr.contains("dependencies") &&
      jr["dependencies"].is_array()) {
    for (const auto &dep : jr["dependencies"]) {
      if (dep.is_object() && dep.contains("name") && dep["name"].is_string()) {
        out.dependencies.push_back(dep["name"].get<std::string>());
      }
    }
  }
}

static bool ParseLibrary(const json &jl, ArduinoLibraryInfo &out) {
  if (!jl.is_object()) {
    return false;
  }

  out.name = jl.value("name", "");
  if (out.name.empty()) {
    return false;
  }

  out.availableVersions.clear();
  out.releases.clear();
  out.latest = ArduinoLibraryRelease{};

  // available_versions []
  if (jl.contains("available_versions") && jl["available_versions"].is_array()) {
    for (const auto &v : jl["available_versions"]) {
      if (v.is_string()) {
        out.availableVersions.push_back(v.get<std::string>());
      }
    }
  }

  // latest {}
  if (jl.contains("latest") && jl["latest"].is_object()) {
    ParseLibraryRelease(jl["latest"], "", out.latest);
  }

  // releases { "1.0.0": {...}, "1.1.0": {...} }
  if (jl.contains("releases") && jl["releases"].is_object()) {
    const auto &releasesJson = jl["releases"];
    for (auto it = releasesJson.begin(); it != releasesJson.end(); ++it) {
      if (!it.value().is_object()) {
        continue;
      }
      ArduinoLibraryRelease rel;
      ParseLibraryRelease(it.value(), it.key(), rel);
      out.releases.push_back(std::move(rel));
    }
  }

  return true;
}

static void ParseCoreRelease(const json &jr, const std::string &versionKey, ArduinoCoreRelease &out) {
  out.version = versionKey;
  out.name = jr.value("name", "");

  // types[]
  if (jr.contains("types") && jr["types"].is_array()) {
    for (const auto &t : jr["types"]) {
      if (t.is_string()) {
        out.types.push_back(t.get<std::string>());
      }
    }
  }

  out.compatible = jr.value("compatible", false);
  out.installed = jr.value("installed", false);

  // boards[]
  if (jr.contains("boards") && jr["boards"].is_array()) {
    for (const auto &jb : jr["boards"]) {
      if (!jb.is_object())
        continue;

      ArduinoCoreBoard b;
      b.name = jb.value("name", "");
      if (jb.contains("fqbn") && jb["fqbn"].is_string()) {
        b.fqbn = jb["fqbn"].get<std::string>();
      }
      if (!b.name.empty()) {
        out.boards.push_back(std::move(b));
      }
    }
  }
}

// --- simple command line splitting into arguments (respects quotes) ---
static std::vector<std::string> SplitArgsKeepingQuotes(const std::string &s) {
  std::vector<std::string> out;
  std::string current;
  bool inQuotes = false;

  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];

    if (c == '"') {
      inQuotes = !inQuotes;
      continue;
    }

    if (!inQuotes && std::isspace(static_cast<unsigned char>(c))) {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }

  if (!current.empty()) {
    out.push_back(current);
  }

  return out;
}

static std::string StripQuotes(const std::string &s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

static std::string ExpandPropertiesInString(const std::string &input, const std::unordered_map<std::string, std::string> &props, const std::string &sketchPath) {
  std::string s = input;
  size_t pos = 0;

  while (true) {
    size_t open = s.find('{', pos);
    if (open == std::string::npos)
      break;
    size_t close = s.find('}', open + 1);
    if (close == std::string::npos)
      break;

    std::string key = s.substr(open + 1, close - open - 1);
    std::string value;

    auto it = props.find(key);
    if (it != props.end()) {
      value = it->second;
    } else if (key == "build.source.path") {
      value = sketchPath;
    } else {
      pos = close + 1;
      continue;
    }

    s.replace(open, close - open + 1, value);
    pos = open + value.size();
  }

  return s;
}

// Selective expansion -I@<path>.
static void ExpandResponseFileSanitized(const std::string &atArg, std::vector<std::string> &out, const std::string &iprefixRoot, bool hasIprefixRoot) {
  if (atArg.size() <= 1)
    return;
  std::string path = atArg.substr(1);

  APP_DEBUG_LOG("CLI: ExpandResponseFileSanitized(%s)", atArg.c_str());

  std::string content;
  if (!LoadFileToString(path, content)) {
    APP_DEBUG_LOG("CLI: ExpandResponseFileSanitized: ignoring '%s'", path.c_str());
    return;
  }

  std::istringstream iss(content);
  std::string token;

  while (iss >> token) {
    // -x c++
    if (token == "-x") {
      out.push_back(token);
      if (iss >> token) {
        out.push_back(token);
      }
      continue;
    }

    if (token == "-iwithprefixbefore") {
      if (hasIprefixRoot && iss >> token) {
        // token is a subpath relative to iprefixRoot
        // deliberately skip embedded C lib stuff
        if (token.rfind("newlib/", 0) == 0 ||
            token.rfind("newlib\\", 0) == 0 ||
            token.rfind("xtensa/", 0) == 0 ||
            token.rfind("xtensa\\", 0) == 0) {
          // we'll leave these trees alone so they are pulled from the host toolchain (or not at all)
          continue;
        }

        std::string full = iprefixRoot;
        if (!full.empty() && full.back() != '/' && full.back() != '\\')
          full += '/';
        full += token;

        out.push_back("-I" + full);
      }
      continue;
    }

    // common interesting flags
    if (IsInterestingFlag(token)) {
      if (token.compare(0, 5, "-std=") == 0) {
        token = NormalizeStdFlag(token);
      }
      out.push_back(token);
    }
  }
}

static void CollectHeadersFromSrcRoot(const fs::path &srcRoot,
                                      const std::string &libSrcPath,
                                      std::unordered_map<std::string, std::string> &cache,
                                      bool allowOverride) {
  std::error_code ec;
  if (!fs::exists(srcRoot, ec) || !fs::is_directory(srcRoot, ec)) {
    return;
  }

  const bool recurse = (srcRoot.filename().string() == "src");

  auto processEntry = [&](const fs::directory_entry &entry) {
    if (!entry.is_regular_file(ec)) {
      return;
    }

    auto path = entry.path();
    auto ext = path.extension().string();
    if (ext != ".h" && ext != ".hpp") {
      return;
    }

    std::string fileName = path.filename().string();

    auto insertKey = [&](const std::string &key) {
      if (key.empty())
        return;
      if (!allowOverride) {
        if (cache.find(key) != cache.end())
          return;
      }
      cache[key] = libSrcPath;
    };

    const bool isTopLevel = (!recurse) || (path.parent_path() == srcRoot);

    // Only "public" headers (top-level in src/) should match bare includes like <Foo.h>
    if (isTopLevel) {
      insertKey(fileName);
    }

    std::error_code ecRel;
    fs::path rel = fs::relative(path, srcRoot, ecRel);
    if (!ecRel) {
      std::string relStr = rel.generic_string();
      if (!relStr.empty() && rel.has_parent_path()) {
        insertKey(relStr);
      }
    }
  };

  if (recurse) {
    for (auto it = fs::recursive_directory_iterator(srcRoot, ec);
         it != fs::recursive_directory_iterator(); ++it) {
      if (ec)
        break;
      processEntry(*it);
    }
  } else {
    for (auto it = fs::directory_iterator(srcRoot, ec);
         it != fs::directory_iterator(); ++it) {
      if (ec)
        break;
      processEntry(*it);
    }
  }
}

#if defined(__WXMSW__)
#include <windows.h>

/**
 * Windows execute without cmd in hidden mode.
 */
static int ExecWinCommandHidden(const std::string &cmdUtf8, std::string &output) {
  output.clear();

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;

  HANDLE hRead = NULL;
  HANDLE hWrite = NULL;

  if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
    DWORD err = GetLastError();
    output = "CreatePipe failed, error " + std::to_string(err);
    return -static_cast<int>(err);
  }

  if (!SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0)) {
    DWORD err = GetLastError();
    CloseHandle(hRead);
    CloseHandle(hWrite);
    output = "SetHandleInformation failed, error " + std::to_string(err);
    return -static_cast<int>(err);
  }

  int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdUtf8.c_str(), -1, nullptr, 0);
  if (wlen <= 0) {
    DWORD err = GetLastError();
    CloseHandle(hRead);
    CloseHandle(hWrite);
    output = "MultiByteToWideChar failed, error " + std::to_string(err);
    return -static_cast<int>(err);
  }

  std::wstring wcmd;
  wcmd.resize(wlen - 1);
  MultiByteToWideChar(CP_UTF8, 0, cmdUtf8.c_str(), -1, &wcmd[0], wlen);

  STARTUPINFOW si{};
  si.cb = sizeof(STARTUPINFOW);
  si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.hStdOutput = hWrite;
  si.hStdError = hWrite;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(
      nullptr,
      wcmd.data(),
      nullptr,
      nullptr,
      TRUE, // inherit handles
      CREATE_NO_WINDOW,
      nullptr,
      nullptr,
      &si,
      &pi);

  CloseHandle(hWrite);

  if (!ok) {
    DWORD err = GetLastError();

    wchar_t msgBuf[512] = {0};
    FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        msgBuf,
        static_cast<DWORD>(std::size(msgBuf)),
        nullptr);

    int u8len = WideCharToMultiByte(CP_UTF8, 0, msgBuf, -1, nullptr, 0, nullptr, nullptr);
    std::string msg;
    if (u8len > 0) {
      msg.resize(u8len - 1);
      WideCharToMultiByte(CP_UTF8, 0, msgBuf, -1, &msg[0], u8len, nullptr, nullptr);
    }

    output = "CreateProcessW failed, error " + std::to_string(err) + ": " + msg;

    CloseHandle(hRead);
    return -static_cast<int>(err);
  }

  char buffer[4096];
  DWORD bytesRead = 0;
  while (ReadFile(hRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
    output.append(buffer, bytesRead);
  }
  CloseHandle(hRead);

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 0;
  if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
    exitCode = (DWORD)-1;
  }

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return static_cast<int>(exitCode);
}
#endif

static unsigned int g_execCounter = 1;

/**
 * Synchronous execution of command. Output stored to output parameter and
 * return value of process is returned.
 */
int ArduinoCli::ExecuteCommand(const std::string &cmd, std::string &output) {
  unsigned int index = (g_execCounter++);
  ScopeTimer t("%04u ExecuteCommand (%s)", index, cmd.c_str());

  APP_DEBUG_LOG("CLI: %04u EXEC SYNC: %s", index, cmd.c_str());

#if defined(__WXMSW__)
  // On Windows we will use our helper function with CreateProcessA and a hidden window
  std::string full = cmd;
  int rc = ExecWinCommandHidden(full, output);
  APP_DEBUG_LOG("CLI: %04u EXIT %d: output.size=%d", index, rc, output.length());
  return rc;
#else
  output.clear();

  std::string _cmd = cmd;
  _cmd += " 2>&1";

  FILE *pipe = popen(_cmd.c_str(), "r");
  if (!pipe) {
    wxLogWarning(wxT("CLI: %04u ERROR %s"), index, wxString::FromUTF8(_cmd));
    return -1;
  }

  std::array<char, 128> buffer{};
  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    output += buffer.data();
  }

  int status = pclose(pipe);
  int rc = -1;

  if (status >= 0) {
    if (WIFEXITED(status)) {
      rc = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      rc = -WTERMSIG(status);
    }
  }

  APP_DEBUG_LOG("CLI: %04u EXIT %d: output-size=%d", index, rc, output.length());
  return rc;
#endif
}

/**
 * Asynchronous execution of command.
 */
int ArduinoCli::RunCliStreaming(const std::string &args, const wxWeakRef<wxEvtHandler> &weak, const char *finishedLabel) {
  unsigned int index = (g_execCounter++);

  int rc = -1;

#if defined(__WXMSW__)
  std::string cmd = GetCliBaseCommand() + " " + args;
  APP_DEBUG_LOG("CLI: %04u STREAM EXEC: %s", index, cmd.c_str());

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;

  HANDLE hRead = NULL;
  HANDLE hWrite = NULL;

  if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
    wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
    evt.SetInt(-1);
    evt.SetString(wxT("Failed to create pipe for arduino-cli.\n"));
    QueueUiEvent(weak, evt.Clone());
    APP_DEBUG_LOG("CLI: %04u STREAM ERROR: %s", index, cmd.c_str());
    return -1;
  }

  if (!SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(hRead);
    CloseHandle(hWrite);

    wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
    evt.SetInt(-1);
    evt.SetString(wxT("Failed to configure pipe for arduino-cli.\n"));
    QueueUiEvent(weak, evt.Clone());
    APP_DEBUG_LOG("CLI: %04u STREAM ERROR: %s", index, cmd.c_str());
    return -1;
  }

  // --- convert the entire command line from UTF-8 to UTF-16 ---
  int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
  if (wlen <= 0) {
    DWORD err = GetLastError();
    CloseHandle(hRead);
    CloseHandle(hWrite);

    wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
    evt.SetInt(-1);
    evt.SetString(wxString::Format(wxT("MultiByteToWideChar failed, error %lu\n"), err));
    QueueUiEvent(weak, evt.Clone());
    APP_DEBUG_LOG("CLI: %04u STREAM ERROR MultiByteToWideChar(%lu): %s", index, err, cmd.c_str());
    return -1;
  }

  std::wstring wcmd;
  wcmd.resize(wlen - 1);
  MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, &wcmd[0], wlen);

  STARTUPINFOW si{};
  si.cb = sizeof(STARTUPINFOW);
  si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.hStdOutput = hWrite;
  si.hStdError = hWrite;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(
      nullptr,
      wcmd.data(), // modifiable wide buffer with the entire command line
      nullptr,
      nullptr,
      TRUE, // inherit handles
      CREATE_NO_WINDOW,
      nullptr,
      nullptr,
      &si,
      &pi);

  CloseHandle(hWrite);

  if (!ok) {
    DWORD err = GetLastError();
    CloseHandle(hRead);

    wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
    evt.SetInt(-1);
    evt.SetString(wxString::Format(wxT("Failed to start arduino-cli (error %lu).\n"), err));
    QueueUiEvent(weak, evt.Clone());
    APP_DEBUG_LOG("CLI: %04u STREAM ERROR CreateProcessW(%lu): %s", index, err, cmd.c_str());
    return -1;
  }

  std::string partial;
  char buffer[256];
  DWORD bytesRead = 0;

  while (ReadFile(hRead, buffer, sizeof(buffer), &bytesRead, nullptr) &&
         bytesRead > 0) {
    partial.append(buffer, bytesRead);

    size_t pos = 0;
    while (true) {
      size_t newlinePos = partial.find('\n', pos);
      if (newlinePos == std::string::npos)
        break;

      std::string line = partial.substr(pos, newlinePos - pos);
      pos = newlinePos + 1;

      // Strip '\r'
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
      evt.SetInt(0);
      evt.SetString(wxString::FromUTF8(line.c_str()));
      QueueUiEvent(weak, evt.Clone());

      APP_DEBUG_LOG("CLI: %04u STREAM LINE: %s", index, line.c_str());
    }

    if (pos > 0) {
      partial.erase(0, pos);
    }
  }

  CloseHandle(hRead);

  if (!partial.empty()) {
    wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
    evt.SetInt(0);
    evt.SetString(wxString::FromUTF8(partial.c_str()));
    QueueUiEvent(weak, evt.Clone());
    APP_DEBUG_LOG("CLI: %04u STREAM LINE: %s", index, partial.c_str());
  }

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 0;
  if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
    exitCode = (DWORD)-1;
  }

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  rc = static_cast<int>(exitCode);
#else
  // Binary + args + redirect stderr->stdout
  std::string cmd = GetCliBaseCommand() + " " + args + " 2>&1";
  APP_DEBUG_LOG("CLI: %04u STREAM EXEC: %s", index, cmd.c_str());

  using PipeCloser = int (*)(FILE *);

  //
  // --- POSIX / macOS implementation: popen + fgets ---
  //
  std::unique_ptr<FILE, PipeCloser> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe) {
    wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
    evt.SetInt(-1);
    evt.SetString(wxT("Failed to start arduino-cli.\n"));
    QueueUiEvent(weak, evt.Clone());
    APP_DEBUG_LOG("CLI: %04u STREAM ERROR: %s", index, cmd.c_str());
    return -1;
  }

  std::array<char, 256> buffer{};
  std::string partial;

  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    partial.append(buffer.data());

    size_t pos = 0;
    while (true) {
      size_t newlinePos = partial.find('\n', pos);
      if (newlinePos == std::string::npos)
        break;

      std::string line = partial.substr(pos, newlinePos - pos);
      pos = newlinePos + 1;

      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
      evt.SetInt(0);
      evt.SetString(wxString::FromUTF8(line));
      QueueUiEvent(weak, evt.Clone());

      APP_DEBUG_LOG("CLI: %04u STREAM LINE: %s", index, line.c_str());
    }

    if (pos > 0) {
      partial.erase(0, pos);
    }
  }

  if (!partial.empty()) {
    wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
    evt.SetInt(0);
    evt.SetString(wxString::FromUTF8(partial));
    QueueUiEvent(weak, evt.Clone());
  }

  int status = pclose(pipe.release());
  rc = -1;

#if defined(__unix__) || defined(__APPLE__)
  if (status >= 0) {
    if (WIFEXITED(status)) {
      rc = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      rc = -WTERMSIG(status);
    }
  }
#else
  rc = status; // fallback
#endif

#endif // __WXMSW__

  wxCommandEvent doneEvt(EVT_COMMANDLINE_OUTPUT_MSG);
  doneEvt.SetInt(rc);
  wxString fl = wxString::FromUTF8(finishedLabel ? finishedLabel : "");
  doneEvt.SetString(wxString::Format(wxT("[arduino-cli %s finished, rc=%d]"), fl, rc));
  QueueUiEvent(weak, doneEvt.Clone());

  APP_DEBUG_LOG("CLI: %04u STREAM EXIT: %d", index, rc);

  return rc;
}

std::vector<std::string> ArduinoCli::GetSystemIncludeArgsForCompiler(const std::string &compilerPath) const {
  static std::map<std::string, std::vector<std::string>> cache;

  auto it = cache.find(compilerPath);
  if (it != cache.end()) {
    return it->second;
  }

  std::string out;
  int rc = 0;

#ifdef __WXMSW__
  fs::path tmp = fs::temp_directory_path() / "arduino_edit_empty.cpp";

  {
    std::error_code ec;
    fs::create_directories(tmp.parent_path(), ec);
    std::ofstream ofs(tmp.string(), std::ios::trunc);
  }

  std::string cmd = ShellQuote(compilerPath) +
                    " -E -x c++ -v " +
                    ShellQuote(tmp.string());

  rc = ExecuteCommand(cmd, out);

  {
    std::error_code ec;
    fs::remove(tmp, ec);
  }

  if (rc == 0) {
    out = LocalToUtf8(out);
  }
#else
  std::string cmd = ShellQuote(compilerPath) + " -E -x c++ - -v < /dev/null";
  rc = ExecuteCommand(cmd, out);
#endif

  if (rc != 0) {
    return {};
  }

  std::vector<std::string> result;
  std::istringstream iss(out);
  std::string line;
  bool inBlock = false;

  while (std::getline(iss, line)) {
    std::string raw = line;
    TrimInPlace(raw);

    if (raw.find("#include <...> search starts here:") != std::string::npos ||
        raw.find("#include \"...\" search starts here:") != std::string::npos) {
      inBlock = true;
      continue;
    }

    if (inBlock) {
      if (raw.find("End of search list.") != std::string::npos) {
        break;
      }
      if (!raw.empty() && raw[0] != '#') {
        std::string path = raw;

        // cut off " (framework directory)"
        auto pos = path.find(" (");
        if (pos != std::string::npos) {
          path.erase(pos);
        }

        // we also accept C:\... and similar, not just Unix /
        if (!path.empty()) {
          result.push_back("-I" + path);
          APP_DEBUG_LOG("CLI: Added compiler include %s", path.c_str());
        }
      }
    }
  }

  cache[compilerPath] = result;
  return result;
}

std::string ArduinoCli::DetectClangTarget(const std::string &compilerPath) const {
  std::string macros;
  int rc = 0;

#ifdef __WXMSW__
  fs::path tmp = fs::temp_directory_path() / "arduino_edit_empty.cpp";
  {
    std::error_code ec;
    fs::create_directories(tmp.parent_path(), ec);
    std::ofstream ofs(tmp.string(), std::ios::trunc);
  }

  std::string cmd = ShellQuote(compilerPath) +
                    " -dM -E -x c++ -v " +
                    ShellQuote(tmp.string());

  rc = ExecuteCommand(cmd, macros);

  {
    std::error_code ec;
    fs::remove(tmp, ec);
  }

  if (rc == 0) {
    macros = LocalToUtf8(macros);
  }
#else
  std::string cmd = ShellQuote(compilerPath) + " -dM -E -x c++ - -v < /dev/null";
  rc = ExecuteCommand(cmd, macros);
#endif

  if (rc != 0) {
    return "";
  }

  std::string target;

  // Architecture guess for -target
  if (macros.find("__AVR__") != std::string::npos) {
    APP_DEBUG_LOG("CLI: Is AVR architecture...");
    target = "avr";
  }
  if (macros.find("__XTENSA__") != std::string::npos) {
    APP_DEBUG_LOG("CLI: Is XTENSA architecture...");
    target = "xtensa";
  }
  if (macros.find("__riscv") != std::string::npos) {
    APP_DEBUG_LOG("CLI: Is RISCV architecture...");
    target = "riscv32";
  }
  if (macros.find("__arm__") != std::string::npos ||
      macros.find("__thumb__") != std::string::npos) {
    APP_DEBUG_LOG("CLI: Is ARM architecture...");
    target = "arm";
  }

  if (target.empty()) {
    APP_DEBUG_LOG("CLI: Unknown architecture, using default...");
  } else {
    if (!ArduinoCodeCompletion::IsClangTargetSupported(target)) {
      APP_DEBUG_LOG("CLI: Unsupported target architecture, using default...");
      target.clear();
    }
  }

  return target;
}

std::vector<std::string> ArduinoCli::BuildClangArgsFromBoardDetails(const nlohmann::json &j) {
  std::vector<std::string> result;

  if (!j.contains("build_properties") || !j["build_properties"].is_array()) {
    return result;
  }

  // ---- build_properties[] -> map key -> value ----
  std::unordered_map<std::string, std::string> props;

  for (const auto &item : j["build_properties"]) {
    if (!item.is_string())
      continue;
    std::string kv = item.get<std::string>();
    auto eq = kv.find('=');
    if (eq == std::string::npos)
      continue;

    std::string key = kv.substr(0, eq);
    std::string val = kv.substr(eq + 1);
    props[key] = val;
  }

  props["build.source.path"] = sketchPath;

  auto itPlat = props.find("build.board.platform.path");
  if (itPlat != props.end()) {
    m_platformPath = itPlat->second;
  } else {
    m_platformPath.clear();
  }

  auto itCorePlat = props.find("build.core.platform.path");
  if (itCorePlat != props.end()) {
    m_corePlatformPath = itCorePlat->second;
  } else {
    m_corePlatformPath.clear();
  }

  // ---- compilerPath: from recipe.cpp.o.pattern ----
  std::string compilerPath;
  std::vector<std::string> dummyRawFromPattern;

  auto itPattern = props.find("recipe.cpp.o.pattern");
  if (itPattern != props.end()) {
    std::string patternExpanded = ExpandPropertiesInString(itPattern->second, props, sketchPath);

    auto tokens = SplitArgsKeepingQuotes(patternExpanded);
    if (!tokens.empty()) {
      compilerPath = StripQuotes(tokens[0]);

      if (tokens.size() > 1) {
        dummyRawFromPattern.assign(tokens.begin() + 1, tokens.end());
      }
    }
  }

  // ---- fallback: compiler.path + compiler.cpp.cmd ----
  if (compilerPath.empty()) {
    auto itCmd = props.find("compiler.cpp.cmd");
    if (itCmd != props.end()) {
      std::string cmd = itCmd->second;
      auto itPath = props.find("compiler.path");
      if (itPath != props.end()) {
        std::string base = itPath->second;
        if (!base.empty() && base.back() != '/' && base.back() != '\\') {
          base.push_back('/');
        }
        compilerPath = base + cmd;
      } else {
        compilerPath = cmd;
      }
    }
  }

  if (compilerPath.empty()) {
    return result;
  }

  result.push_back("-I" + sketchPath);

  auto itBuildCorePath = props.find("build.core.path");
  if (itBuildCorePath != props.end()) {
    result.push_back("-I" + itBuildCorePath->second);
  }

  // Errors that are outside the sketch are ignored.
  // However, because the includes and generally the clang parameters are not 100% correct,
  // it could happen that errors in the sketch would not be in the list at all due to the number
  // of errors in the toolchain. Therefore, we do not limit errors.
  result.push_back("-ferror-limit=0");

  std::vector<std::string> rawFlags;

  auto appendFlagsFromProp = [&](const char *key) {
    auto it = props.find(key);
    if (it == props.end())
      return;
    std::string valueExpanded =
        ExpandPropertiesInString(it->second, props, sketchPath);
    auto tokens = SplitArgsKeepingQuotes(valueExpanded);
    rawFlags.insert(rawFlags.end(), tokens.begin(), tokens.end());
  };

  appendFlagsFromProp("compiler.cpreprocessor.flags");
  appendFlagsFromProp("compiler.cpp.flags");
  appendFlagsFromProp("compiler.cpp.extra_flags");
  appendFlagsFromProp("build.extra_flags");

  rawFlags.insert(rawFlags.end(),
                  dummyRawFromPattern.begin(),
                  dummyRawFromPattern.end());

  std::string iprefixRoot;
  bool hasIprefixRoot = false;

  for (size_t i = 0; i < rawFlags.size(); ++i) {
    std::string a = StripQuotes(rawFlags[i]);

    // -iprefix <root>
    if (a == "-iprefix") {
      if (i + 1 < rawFlags.size()) {
        std::string val = StripQuotes(rawFlags[i + 1]);
        val = ExpandPropertiesInString(val, props, sketchPath);
        iprefixRoot = val;
        hasIprefixRoot = !iprefixRoot.empty();
        ++i;
      }
      continue;
    }

    if (!a.empty() && a[0] == '@') {
      ExpandResponseFileSanitized(a, result, iprefixRoot, hasIprefixRoot);
      continue;
    }

    // -x c++
    if (a == "-x") {
      result.push_back(a);
      if (i + 1 < rawFlags.size()) {
        std::string lang = StripQuotes(rawFlags[++i]);
        result.push_back(lang);
      }
      continue;
    }

    if (IsInterestingFlag(a)) {
      if (a.compare(0, 5, "-std=") == 0) {
        a = NormalizeStdFlag(a);
      }
      result.push_back(a);
    }
  }

  auto sysIncludes = GetSystemIncludeArgsForCompiler(compilerPath);
  result.insert(result.end(), sysIncludes.begin(), sysIncludes.end());

  std::string target = DetectClangTarget(compilerPath);
  if (!target.empty()) {
    result.push_back("-target");
    result.push_back(target);
  }

  auto itVariantPath = props.find("build.variant.path");
  if (itVariantPath != props.end()) {
    std::string vpath =
        ExpandPropertiesInString(itVariantPath->second, props, sketchPath);
    if (!vpath.empty()) {
      result.push_back("-I" + vpath);
    }
  }

  // I couldn't find this define anywhere, but the toolchain needs it,
  // so we'll add it synthetically.
  bool foundCoreBuild = false;
  for (const auto &f : result) {
    if (f == "-DARDUINO_CORE_BUILD") {
      foundCoreBuild = true;
      break;
    }
  }
  if (!foundCoreBuild) {
    result.push_back("-DARDUINO_CORE_BUILD");
  }

  return result;
}

std::vector<std::string> ArduinoCli::BuildClangArgsFromCompileCommands(const std::string &inoBaseName) {
  std::vector<std::string> result;

  // 1) load compile_commands.json
  std::filesystem::path cc = std::filesystem::path(sketchPath) / ".ardedit" / "build" / "compile_commands.json";
  std::string ccPath = cc.string();
  std::string ccContent;
  if (!LoadFileToString(ccPath, ccContent)) {
    wxLogError(wxT("Failed to load %s\n"), wxString::FromUTF8(ccPath.c_str()));
    return result;
  }

  APP_TRACE_LOG("CLI: Found compile command content %s", ccContent.c_str());

  json j;
  try {
    j = json::parse(ccContent);
  } catch (...) {
    wxLogError(wxT("Failed to parse %s\n"), wxString::FromUTF8(ccPath.c_str()));
    return result;
  }

  if (!j.is_array()) {
    wxLogError(wxT("JSON is not array %s\n"), wxString::FromUTF8(ccPath.c_str()));
    return result;
  }

  // 2) find the record for .ino.cpp
#ifdef __WXMSW__
  std::string wantedSuffix = "\\build\\sketch\\" + inoBaseName + ".ino.cpp";
#else
  std::string wantedSuffix = "/build/sketch/" + inoBaseName + ".ino.cpp";
#endif

  APP_TRACE_LOG("CLI: wantedSuffix=%s\n", wantedSuffix.c_str());
  json entry;
  bool found = false;

  for (size_t i = 0; i < j.size(); ++i) {
    const json &e = j[i];
    if (!e.is_object())
      continue;
    if (!e.contains("file"))
      continue;

    std::string filePath = e["file"].get<std::string>();
    // simple check based on the suffix
    if (filePath.size() >= wantedSuffix.size() &&
        filePath.compare(filePath.size() - wantedSuffix.size(),
                         wantedSuffix.size(),
                         wantedSuffix) == 0) {
      entry = e;
      found = true;
      break;
    }
  }

  if (!found) {
    wxLogError(wxT("Failed to find %s in %s\n"), wxString::FromUTF8(inoBaseName.c_str()), wxString::FromUTF8(ccPath.c_str()));
    return result;
  }

  // 3) extract arguments[]
  if (!entry.contains("arguments")) {
    wxLogError(wxT("Failed to find arguments in %s\n"), wxString::FromUTF8(ccPath.c_str()));
    return result;
  }

  const json &argsJson = entry["arguments"];
  if (!argsJson.is_array() || argsJson.empty()) {
    wxLogError(wxT("Failed to parse arguments in %s\n"), wxString::FromUTF8(ccPath.c_str()));
    return result;
  }

  // compilerPath
  std::string compilerPath = argsJson[0].get<std::string>();

  // the first argument is the path to the compiler -> we ignore it
  std::vector<std::string> rawArgs;
  for (size_t i = 1; i < argsJson.size(); ++i) {
    rawArgs.push_back(argsJson[i].get<std::string>());
  }

  // file that compiles the actual build (path to .ino.cpp)
  std::string compiledFile = entry["file"].get<std::string>();

  result.push_back("-I" + sketchPath);
  result.push_back("-ferror-limit=0");

  std::string iprefixRoot; // e.g. /.../esp32/include/
  bool hasIprefixRoot = false;

  // 4) iterate through rawArgs, filter out gcc-specific things, expand @
  for (size_t i = 0; i < rawArgs.size(); ++i) {
    std::string &a = rawArgs[i];

    // remove -c/-MMD/-o
    if (a == "-c" || a == "-MMD")
      continue;
    if (a == "-o") {
      ++i;
      continue;
    }
    if (a == compiledFile)
      continue;

    // capture -iprefix <root>
    if (a == "-iprefix") {
      if (i + 1 < rawArgs.size()) {
        iprefixRoot = rawArgs[i + 1];
        hasIprefixRoot = true;
        ++i; // we will also skip that path
      }
      continue; // we do not send anything to result
    }

    // response file - now we pass iprefixRoot to it
    if (!a.empty() && a[0] == '@') {
      ExpandResponseFileSanitized(a, result, iprefixRoot, hasIprefixRoot);
      continue;
    }

    // -x c++
    if (a == "-x") {
      result.push_back(a);
      if (i + 1 < rawArgs.size()) {
        result.push_back(rawArgs[i + 1]);
        ++i;
      }
      continue;
    }

    if (IsInterestingFlag(a)) {
      if (a.compare(0, 5, "-std=") == 0) {
        a = NormalizeStdFlag(a);
      }
      // simple -I/path works normally
      result.push_back(a);
    }
  }

  // Add system includes
  auto sysIncludes = GetSystemIncludeArgsForCompiler(compilerPath);
  result.insert(result.end(), sysIncludes.begin(), sysIncludes.end());

  // Add the target
  std::string target = DetectClangTarget(compilerPath);
  if (!target.empty()) {
    result.push_back("-target");
    result.push_back(target);
  }

  return result;
}

void ArduinoCli::InvalidateLibraryCache() {
  std::string sketchPath = GetSketchPath();
  if (!sketchPath.empty()) {
    fs::path cacheFile = fs::path(sketchPath) / ".ardedit" / "libraries.json";

    if (fs::exists(cacheFile)) {
      fs::remove(cacheFile);
    }
  }

  {
    std::lock_guard<std::mutex> lock(m_resolveCacheMutex);
    m_hasResolveLibrariesCache = false;
    m_resolveLibs.clear();
    m_resolveHeaderToLibSrc.clear();
    m_resolveSrcRootToLibIndex.clear();
    m_resolveNameToLibIndex.clear();
  }
}

bool ArduinoCli::LoadLibraries() {
  ScopeTimer t("CLI: LoadLibraries()");
  APP_DEBUG_LOG("CLI: LoadLibraries()");

  std::string output;
  std::string cmd = GetCliBaseCommand() + " lib search --format json --omit-releases-details";
  int rc = ExecuteCommand(cmd, output);
  if ((rc != 0) || output.empty()) {
    return false;
  }

  json j;
  try {
    j = json::parse(output);
  } catch (const std::exception &e) {
    wxLogWarning(wxT("Failed to parse arduino-cli lib search JSON: %s"),
                 wxString::FromUTF8(e.what()));
    return false;
  }

  if (!j.contains("libraries") || !j["libraries"].is_array()) {
    wxLogWarning(wxT("arduino-cli lib search JSON does not contain 'libraries' array."));
    return false;
  }

  const auto &libsJson = j["libraries"];

  std::vector<ArduinoLibraryInfo> tmp;
  tmp.reserve(libsJson.size());

  for (const auto &jl : libsJson) {
    if (!jl.is_object()) {
      continue;
    }

    ArduinoLibraryInfo info;
    if (!ParseLibrary(jl, info)) {
      continue;
    }
    tmp.push_back(std::move(info));
  }

  libraries.swap(tmp);

  APP_DEBUG_LOG("CLI: LoadLibraries() -> %zu libraries found...", libraries.size());
  return true;
}

bool ArduinoCli::SearchLibraryProvidingHeader(const std::string &header,
                                              std::vector<ArduinoLibraryInfo> &out) {
  ScopeTimer t("CLI: SearchLibraryProvidingHeader()");
  out.clear();

  APP_DEBUG_LOG("CLI: SearchLibraryProvidingHeader(%s)", header.c_str());

  if (header.empty()) {
    return false;
  }

  // arduino-cli lib search "MCP3421.h" --format json
  std::ostringstream args;
  args << " lib search " << ShellQuote(header)
       << " --format json --omit-releases-details";

  std::string cmd = GetCliBaseCommand() + " " + args.str();

  std::string output;
  int rc = ExecuteCommand(cmd, output);
  if (rc != 0 || output.empty()) {
    return false;
  }

  json j;
  try {
    j = json::parse(output);
  } catch (const std::exception &e) {
    wxLogWarning(wxT("Failed to parse arduino-cli lib search JSON: %s"),
                 wxString::FromUTF8(e.what()));
    return false;
  }

  const json *libsJson = nullptr;

  // Prefer explicit status handling.
  // When nothing matches, arduino-cli may return: { "status": "success" }
  if (j.is_object()) {
    if (j.contains("status") && j["status"].is_string()) {
      const std::string status = j["status"].get<std::string>();
      if (status != "success") {
        wxLogWarning(wxT("arduino-cli lib search returned status '%s'."),
                     wxString::FromUTF8(status));
        return false;
      }
    }

    // Typical structure: { "libraries": [ ... ] }
    if (j.contains("libraries")) {
      if (j["libraries"].is_array()) {
        libsJson = &j["libraries"];
      } else {
        // Weird but valid JSON; treat as empty result rather than hard failure.
        wxLogWarning(wxT("arduino-cli lib search JSON: 'libraries' is not an array; treating as empty."));
        return true;
      }
    } else {
      // status success + no 'libraries' => no results
      return true;
    }
  } else if (j.is_array()) {
    // Just in case some cli version returns an array directly
    libsJson = &j;
  } else {
    wxLogWarning(wxT("arduino-cli lib search JSON has unexpected top-level type."));
    return false;
  }

  out.reserve(libsJson->size());
  for (const auto &jl : *libsJson) {
    ArduinoLibraryInfo info;
    if (!ParseLibrary(jl, info)) {
      continue;
    }
    out.push_back(std::move(info));
  }

  // IMPORTANT: success == "command+parse ok", even if out is empty
  return true;
}

bool ArduinoCli::LoadInstalledLibraries() {
  ScopeTimer t("CLI: LoadInstalledLibraries()");

  // arduino-cli lib list --format json
  std::string cmd = GetCliBaseCommand() + " lib list --format json";
  std::string output;
  int rc = ExecuteCommand(cmd, output);
  if (rc != 0 || output.empty()) {
    return false;
  }

  json j;
  try {
    j = json::parse(output);
  } catch (const std::exception &e) {
    wxLogWarning(wxT("Failed to parse arduino-cli lib list JSON: %s"), wxString::FromUTF8(e.what()));
    return false;
  }

  const json *libsJson = nullptr;

  // different versions of arduino-cli:
  // - { "installed_libraries": [ ... ] }
  // - { "libraries": [ ... ] }   (just to be sure)
  // - [ ... ]                    (older/alternative format)
  if (j.is_object()) {
    if (j.contains("installed_libraries") && j["installed_libraries"].is_array()) {
      libsJson = &j["installed_libraries"];
    } else if (j.contains("libraries") && j["libraries"].is_array()) {
      libsJson = &j["libraries"];
    }
  } else if (j.is_array()) {
    libsJson = &j;
  }

  if (!libsJson) {
    // no installed libraries
    return true;
  }

  std::vector<ArduinoLibraryInfo> tmp;
  tmp.reserve(libsJson->size());

  for (const auto &item : *libsJson) {
    if (!item.is_object())
      continue;

    // in some formats it is { "library": { ... }, "version": "x.y.z", ... }
    const json *jl = nullptr;
    if (item.contains("library") && item["library"].is_object()) {
      jl = &item["library"];
    } else {
      jl = &item;
    }

    ArduinoLibraryInfo info;
    info.name = jl->value("name", "");
    if (info.name.empty())
      continue;

    ArduinoLibraryRelease rel;

    // version - can be in the root object or in the "library"
    std::string version;
    if (item.contains("version") && item["version"].is_string()) {
      version = item["version"].get<std::string>();
    } else if (jl->contains("version") && (*jl)["version"].is_string()) {
      version = (*jl)["version"].get<std::string>();
    }
    rel.version = version;

    // architectures
    if (jl->contains("architectures") && (*jl)["architectures"].is_array()) {
      for (const auto &a : (*jl)["architectures"]) {
        if (a.is_string()) {
          rel.architectures.push_back(a.get<std::string>());
        }
      }
    }

    // provides_includes
    if (jl->contains("provides_includes") && (*jl)["provides_includes"].is_array()) {
      for (const auto &inc : (*jl)["provides_includes"]) {
        if (inc.is_string()) {
          rel.providesIncludes.push_back(inc.get<std::string>());
        }
      }
    }

    if (jl->contains("dependencies") && (*jl)["dependencies"].is_array()) {
      for (const auto &d : (*jl)["dependencies"]) {
        if (d.is_object() &&
            d.contains("name") &&
            d["name"].is_string()) {
          rel.dependencies.push_back(d["name"].get<std::string>());
        }
      }
    }

    // installation-specific information
    rel.installDir = jl->value("install_dir", "");
    rel.sourceDir = jl->value("source_dir", "");
    rel.isLegacy = jl->value("is_legacy", false);
    rel.location = jl->value("location", "");
    rel.layout = jl->value("layout", "");

    if (jl->contains("examples") && (*jl)["examples"].is_array()) {
      for (const auto &ex : (*jl)["examples"]) {
        if (ex.is_string()) {
          rel.examples.push_back(ex.get<std::string>());
        }
      }
    }

    // the remainder (author/sentence/paragraph/...) lib list typically has - but if not, it doesn't matter:
    rel.author = jl->value("author", "");
    rel.maintainer = jl->value("maintainer", "");
    rel.sentence = jl->value("sentence", "");
    rel.paragraph = jl->value("paragraph", "");
    rel.website = jl->value("website", "");
    rel.category = jl->value("category", "");

    info.latest = rel;

    if (!version.empty()) {
      info.availableVersions.push_back(version);
    }

    info.releases.clear();
    info.releases.push_back(rel);

    tmp.push_back(std::move(info));
  }

  installedLibraries.swap(tmp);
  return true;
}

ArduinoCliConfig ArduinoCli::GetConfig() const {
  ArduinoCliConfig cfg;

  std::string cmd = GetCliBaseCommand() +
                    " config dump --format json";

  std::string output;
  int rc = ExecuteCommand(cmd, output);
  if (rc != 0 || output.empty()) {
    wxLogWarning(wxT("arduino-cli config dump failed (rc=%d)."), rc);
    return cfg;
  }

  cfg.rawJson = output;

  json j;
  try {
    j = json::parse(output);
  } catch (const std::exception &e) {
    wxLogWarning(wxT("Failed to parse arduino-cli config dump JSON: %s"), wxString::FromUTF8(e.what()));
    return cfg;
  }

  // From the version where they changed it, the config is wrapped in "config"
  // see https://docs.arduino.cc/arduino-cli/UPGRADING
  //   "config dump --format json results are now wrapped under config key"
  const json *root = nullptr;
  if (j.is_object() && j.contains("config") && j["config"].is_object()) {
    root = &j["config"];
  } else if (j.is_object()) {
    root = &j;
  } else {
    return cfg;
  }

  // --- board_manager.additional_urls ---
  try {
    auto itBm = root->find("board_manager");
    if (itBm != root->end() && itBm->is_object()) {
      auto itUrls = itBm->find("additional_urls");
      if (itUrls != itBm->end()) {
        if (itUrls->is_array()) {
          for (const auto &v : *itUrls) {
            if (v.is_string()) {
              cfg.boardManagerAdditionalUrls.push_back(v.get<std::string>());
            }
          }
        } else if (itUrls->is_string()) {
          // in case someone has only a single string there
          cfg.boardManagerAdditionalUrls.push_back(itUrls->get<std::string>());
        }
      }
    }
  } catch (const std::exception &e) {
    wxLogWarning(wxT("Failed to extract board_manager.additional_urls from config: %s"), wxString::FromUTF8(e.what()));
  }

  // --- network.connection_timeout ---
  try {
    auto itNet = root->find("network");
    if (itNet != root->end() && itNet->is_object()) {
      auto itTimeout = itNet->find("connection_timeout");
      if (itTimeout != itNet->end() && itTimeout->is_string()) {
        cfg.networkConnectionTimeout = itTimeout->get<std::string>();
      }
    }
  } catch (const std::exception &e) {
    wxLogWarning(wxT("Failed to extract network.connection_timeout from config: %s"), wxString::FromUTF8(e.what()));
  }

  // --- board_manager.enable_unsafe_install ---
  try {
    auto itBm = root->find("board_manager");
    if (itBm != root->end() && itBm->is_object()) {
      auto itUnsafe = itBm->find("enable_unsafe_install");
      if (itUnsafe != itBm->end()) {
        if (itUnsafe->is_boolean())
          cfg.boardManagerEnableUnsafeInstall = itUnsafe->get<bool>();
        else if (itUnsafe->is_string()) {
          // fallback, in case someone explicitly has a "true"/"false" string
          std::string s = itUnsafe->get<std::string>();
          std::transform(s.begin(), s.end(), s.begin(), ::tolower);
          cfg.boardManagerEnableUnsafeInstall = (s == "true" || s == "1" || s == "yes");
        }
      }
    }
  } catch (const std::exception &e) {
    wxLogWarning(wxT("Failed to extract board_manager.enable_unsafe_install: %s"), wxString::FromUTF8(e.what()));
  }

  // --- network.proxy ---
  try {
    auto itNet = root->find("network");
    if (itNet != root->end() && itNet->is_object()) {
      auto itProxy = itNet->find("proxy");
      if (itProxy != itNet->end()) {
        if (itProxy->is_string()) {
          cfg.networkProxy = itProxy->get<std::string>();
        }
      }
    }
  } catch (const std::exception &e) {
    wxLogWarning(wxT("Failed to extract network.proxy: %s"), wxString::FromUTF8(e.what()));
  }

  return cfg;
}

bool ArduinoCli::SetConfigValue(const std::string &key,
                                const std::string &value) {
  std::string cmd;

  if (value.empty()) {
    // empty value = delete key -> default will be used
    cmd = GetCliBaseCommand() +
          " config delete " + ShellQuote(key);
  } else {
    cmd = GetCliBaseCommand() +
          " config set " + ShellQuote(key) + " " + ShellQuote(value);
  }

  std::string output;
  int rc = ExecuteCommand(cmd, output);
  if (rc != 0) {
    wxLogWarning(wxT("arduino-cli config set/delete failed for key '%s' (rc=%d)."), wxString::FromUTF8(key), rc);
    return false;
  }
  return true;
}

bool ArduinoCli::SetConfigValue(const std::string &key,
                                const std::vector<std::string> &values) {
  std::string cmd;

  if (values.empty()) {
    // empty list -> delete the entire key
    cmd = GetCliBaseCommand() +
          " config delete " + ShellQuote(key);
  } else {
    cmd = GetCliBaseCommand() +
          " config set " + ShellQuote(key);
    for (const auto &v : values) {
      cmd += " " + ShellQuote(v);
    }
  }

  std::string output;
  int rc = ExecuteCommand(cmd, output);
  if (rc != 0) {
    wxLogWarning(wxT("arduino-cli config set/delete failed for multi key '%s' (rc=%d)."), wxString::FromUTF8(key), rc);
    return false;
  }
  return true;
}

bool ArduinoCli::ApplyConfig(const ArduinoCliConfig &cfg) {
  bool ok = true;

  // multi-value: []
  ok = SetConfigValue("board_manager.additional_urls",
                      cfg.boardManagerAdditionalUrls) &&
       ok;

  // string (delete when empty)
  ok = SetConfigValue("network.connection_timeout",
                      cfg.networkConnectionTimeout) &&
       ok;

  // boolean (CLI supports set/delete)
  if (cfg.boardManagerEnableUnsafeInstall) {
    ok = SetConfigValue("board_manager.enable_unsafe_install", "true") && ok;
  } else {
    ok = SetConfigValue("board_manager.enable_unsafe_install", "") && ok; // delete => default=false
  }

  // proxy (delete when empty)
  ok = SetConfigValue("network.proxy", cfg.networkProxy) && ok;

  return ok;
}

bool ArduinoCli::LoadBoardParameters() {
  if (m_cli.empty()) {
    return false;
  }

  // base FQBN for board details
  std::string baseFqbn = GetBoardName();
  if (baseFqbn.empty()) {
    baseFqbn = fqbn;
  }
  if (baseFqbn.empty()) {
    return false;
  }

  std::ostringstream args;
  args << " board details"
       << " --fqbn " << ShellQuote(baseFqbn)
       << " --format json";

  std::string cmd = GetCliBaseCommand() + " " + args.str();
  std::string output;
  int rc = ExecuteCommand(cmd, output);

  if (rc != 0 || output.empty()) {
    APP_DEBUG_LOG("arduino-cli board details failed in LoadBoardParameters (rc=%d).", rc);
    return false;
  }

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(output);
  } catch (const std::exception &e) {
    wxLogWarning(wxT("LoadBoardParameters: failed to parse board details JSON: %s"),
                 wxString::FromUTF8(e.what()));
    return false;
  }

  auto argsVec = BuildClangArgsFromBoardDetails(j);
  if (argsVec.empty()) {
    return false;
  }

  clangArgs = std::move(argsVec);
  return true;
}

void ArduinoCli::LoadBoardParametersAsync(wxEvtHandler *handler) {
  if (!handler)
    return;

  wxWeakRef<wxEvtHandler> weak(handler);

  APP_DEBUG_LOG("LoadBoardParametersAsync()");

  std::thread([this, weak]() {
    bool ok = this->LoadBoardParameters();

    wxThreadEvent evt(EVT_CLANG_ARGS_READY);
    evt.SetInt(ok ? 1 : 0);

    QueueUiEvent(weak, evt.Clone());
  }).detach();
}

std::vector<std::string> ArduinoCli::ResolveLibraries(const std::vector<std::string> &includes) {
  ScopeTimer timer("ArduinoCli::ResolveLibraries (includes.size=%d)", includes.size());

  std::lock_guard<std::mutex> lock(m_resolveCacheMutex);

  std::vector<std::string> result;

  APP_DEBUG_LOG("CLI: ResolveLibraries(%d includes, platformPath=%s)", (int)includes.size(), m_platformPath.c_str());

  if (m_platformPath.empty()) {
    return result;
  }

  auto normalizeLibName = [&](std::string s) -> std::string {
    TrimInPlace(s);
    for (char &c : s) {
      if (c == ' ')
        c = '_';
    }
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
  };

  auto parseLibraryProperties = [&](const fs::path &libRoot,
                                    const std::string &defaultName,
                                    std::string &outName,
                                    std::vector<std::string> &outDepends,
                                    std::string &outArchitectures) {
    outName = defaultName;
    outDepends.clear();
    outArchitectures.clear();

    fs::path propsPath = libRoot / "library.properties";
    std::ifstream in(propsPath);
    if (!in) {
      return;
    }

    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      std::string raw = line;
      TrimInPlace(raw);
      if (raw.empty() || raw[0] == '#')
        continue;

      auto eq = raw.find('=');
      if (eq == std::string::npos)
        continue;

      std::string key = raw.substr(0, eq);
      std::string val = raw.substr(eq + 1);
      TrimInPlace(key);
      TrimInPlace(val);

      if (key == "name") {
        if (!val.empty())
          outName = val;
      } else if (key == "depends") {
        std::stringstream ss(val);
        std::string dep;
        while (std::getline(ss, dep, ',')) {
          TrimInPlace(dep);
          if (!dep.empty()) {
            outDepends.push_back(dep);
          }
        }
      } else if (key == "architectures") {
        outArchitectures = val; // keep raw CSV
      }
    }
  };

  auto normalizeInclude = [&](std::string s) -> std::string {
    TrimInPlace(s);
    if (!s.empty() && (s.front() == '<' || s.front() == '"' || s.front() == '\'')) {
      s.erase(0, 1);
    }
    if (!s.empty() && (s.back() == '>' || s.back() == '"' || s.back() == '\'')) {
      s.pop_back();
    }
    TrimInPlace(s);
    return s;
  };

  // One-time build of library cache (if not already present)
  if (!m_hasResolveLibrariesCache) {
    APP_DEBUG_LOG("CLI: ResolveLibraries - building library cache...");

    m_resolveLibs.clear();
    m_resolveHeaderToLibSrc.clear();
    m_resolveSrcRootToLibIndex.clear();
    m_resolveNameToLibIndex.clear();

    auto registerLibNameKeys = [&](size_t idx) {
      auto &info = m_resolveLibs[idx];

      auto addKey = [&](const std::string &raw) {
        if (raw.empty())
          return;
        std::string key = normalizeLibName(raw);
        if (key.empty())
          return;
        if (m_resolveNameToLibIndex.find(key) == m_resolveNameToLibIndex.end()) {
          m_resolveNameToLibIndex[key] = idx;
        }
      };

      addKey(info.name);

      std::string dirName = info.libRoot.filename().string();
      addKey(dirName);
    };

    // core libs:
    //  - build.core.platform.path/libraries (referenced core platform; e.g. Arduino AVR)
    //  - build.board.platform.path/libraries (board platform; e.g. attiny)
    //
    // Order matters: scan core platform first, then board platform so board can override.
    std::vector<fs::path> corePlatformRoots;
    if (!m_corePlatformPath.empty()) {
      corePlatformRoots.push_back(fs::path(m_corePlatformPath));
    }
    if (!m_platformPath.empty()) {
      fs::path bp(m_platformPath);
      if (m_corePlatformPath.empty() || bp.string() != m_corePlatformPath) {
        corePlatformRoots.push_back(bp);
      }
    }

    std::error_code ec;
    for (const auto &platformRoot : corePlatformRoots) {
      fs::path libsRoot = platformRoot / "libraries";
      if (!fs::exists(libsRoot, ec) || !fs::is_directory(libsRoot, ec)) {
        continue;
      }

      APP_DEBUG_LOG("CLI: ResolveLibraries: scanning core libraries in %s",
                    libsRoot.string().c_str());

      for (const auto &dirEntry : fs::directory_iterator(libsRoot, ec)) {
        if (ec)
          break;
        if (!dirEntry.is_directory())
          continue;

        fs::path libDir = dirEntry.path();
        fs::path libRoot = libDir;

        // Find src root
        std::error_code ec2;
        fs::path srcRoot = libRoot;
        if (fs::exists(libRoot / "src", ec2) && fs::is_directory(libRoot / "src", ec2)) {
          srcRoot = libRoot / "src";
        } else if (!fs::exists(srcRoot, ec2) || !fs::is_directory(srcRoot, ec2)) {
          continue;
        }

        ResolveLibInfo info;
        info.libRoot = libRoot;
        info.srcRoot = srcRoot;
        info.isCore = true;

        std::string defaultName = libRoot.filename().string();

        std::string architecturesRaw;
        parseLibraryProperties(libRoot, defaultName, info.name, info.depends, architecturesRaw);
        if (!IsLibraryArchitectureCompatible(architecturesRaw)) {
          APP_DEBUG_LOG("CLI: ResolveLibraries: skip core lib '%s' (architectures='%s', target='%s')",
                        info.name.c_str(), architecturesRaw.c_str(), GetTargetFromFQBN().c_str());
          continue;
        }

        info.normalizedName = normalizeLibName(info.name);

        size_t idx = m_resolveLibs.size();
        m_resolveLibs.push_back(std::move(info));

        m_resolveSrcRootToLibIndex[srcRoot.string()] = idx;
        registerLibNameKeys(idx);

        // core libraries can overwrite user libs
        CollectHeadersFromSrcRoot(srcRoot, srcRoot.string(), m_resolveHeaderToLibSrc, /*allowOverride=*/true);
      }
    }

    // user libs
    for (const auto &libInfo : installedLibraries) {
      const std::string &srcDirStr = libInfo.latest.sourceDir;
      if (srcDirStr.empty())
        continue;

      fs::path libRoot = fs::path(srcDirStr);

      std::error_code ec2;

      // Determine srcRoot (where headers are)
      fs::path srcRoot = libRoot;
      if (fs::exists(libRoot / "src", ec2) && fs::is_directory(libRoot / "src", ec2)) {
        srcRoot = libRoot / "src";
      } else if (!fs::exists(srcRoot, ec2) || !fs::is_directory(srcRoot, ec2)) {
        continue;
      }

      // Determine propsRoot (where library.properties lives)
      // Some libraries come from CLI JSON with sourceDir pointing to ".../Lib/src".
      fs::path propsRoot = libRoot;
      if (!fs::exists(propsRoot / "library.properties", ec2)) {
        if (propsRoot.filename() == "src") {
          fs::path parent = propsRoot.parent_path();
          if (!parent.empty() && fs::exists(parent / "library.properties", ec2)) {
            propsRoot = parent;
          }
        }
      }

      ResolveLibInfo info;
      // IMPORTANT: libRoot in ResolveLibInfo should be the library root (properties root), not ".../src"
      info.libRoot = propsRoot;
      info.srcRoot = srcRoot;
      info.isCore = false;

      std::string defaultName =
          !libInfo.name.empty() ? libInfo.name : propsRoot.filename().string();

      std::string architecturesRaw;
      parseLibraryProperties(propsRoot, defaultName, info.name, info.depends, architecturesRaw);

      if (!IsLibraryArchitectureCompatible(architecturesRaw)) {
        APP_DEBUG_LOG("CLI: ResolveLibraries: skip user lib '%s' (architectures='%s', target='%s')",
                      info.name.c_str(), architecturesRaw.c_str(), GetTargetFromFQBN().c_str());
        continue;
      }

      if (!libInfo.latest.dependencies.empty()) {
        for (const auto &d : libInfo.latest.dependencies) {
          if (!d.empty())
            info.depends.push_back(d);
        }
      }

      info.normalizedName = normalizeLibName(info.name);

      APP_TRACE_LOG("CLI: ResolveLibraries: adding user library %s to cache...", info.normalizedName.c_str());

      size_t idx = m_resolveLibs.size();
      m_resolveLibs.push_back(std::move(info));

      m_resolveSrcRootToLibIndex[srcRoot.string()] = idx;
      registerLibNameKeys(idx);

      // user libraries, do not overwrite core libs
      CollectHeadersFromSrcRoot(srcRoot,
                                srcRoot.string(),
                                m_resolveHeaderToLibSrc,
                                /*allowOverride=*/false);
    }

    //
    // --- Pragmatic hack for ESP32 WiFi/Network -------------------------
    //
    // ESP32 core 3.x has a library "Network", which is a de facto dependency
    // WiFi/Ethernet, but it is not declared in library.properties (nor in CLI
    // JSON), because arduino-cli pulls it through its deep dependency
    // resolver (scans includes in libraries).
    //
    // To make the editor behave similarly (WiFi.h -> Network.h available without
    // the user having to do #include <Network.h> manually), we will manually add
    // the dependency WiFi -> Network (and possibly Ethernet -> Network), if
    // both libraries exist in the current environment.
    //
    // If there is ever a will, I will make it a deep scan.
    //
    auto findLibByKey = [&](const std::string &rawName) -> ssize_t {
      std::string key = normalizeLibName(rawName);
      if (key.empty())
        return -1;
      auto it = m_resolveNameToLibIndex.find(key);
      if (it == m_resolveNameToLibIndex.end())
        return -1;
      return static_cast<ssize_t>(it->second);
    };

    ssize_t networkIdx = findLibByKey("Network");
    if (networkIdx >= 0) {
      ssize_t wifiIdx = findLibByKey("WiFi");
      if (wifiIdx >= 0) {
        m_resolveLibs[static_cast<size_t>(wifiIdx)].depends.push_back("Network");
        APP_DEBUG_LOG("CLI: ResolveLibraries hack: adding dependency WiFi -> Network");
      }

      ssize_t ethernetIdx = findLibByKey("Ethernet");
      if (ethernetIdx >= 0) {
        m_resolveLibs[static_cast<size_t>(ethernetIdx)].depends.push_back("Network");
        APP_DEBUG_LOG("CLI: ResolveLibraries hack: adding dependency Ethernet -> Network");
      }
    }

    m_hasResolveLibrariesCache = true;
  }

  // deep resolution for include
  std::set<size_t> selectedLibIndices;
  std::unordered_set<size_t> visited;

  auto addLibWithDependencies = [&](size_t rootIdx) {
    std::vector<size_t> stack;
    stack.push_back(rootIdx);

    while (!stack.empty()) {
      size_t idx = stack.back();
      stack.pop_back();

      if (idx >= m_resolveLibs.size())
        continue;
      if (!visited.insert(idx).second)
        continue;

      selectedLibIndices.insert(idx);

      const auto &info = m_resolveLibs[idx];
      for (const auto &depRaw : info.depends) {
        std::string key = normalizeLibName(depRaw);
        if (key.empty())
          continue;

        auto itDep = m_resolveNameToLibIndex.find(key);
        if (itDep == m_resolveNameToLibIndex.end()) {
          APP_DEBUG_LOG("CLI: ResolveLibraries: unresolved dependency '%s' (from '%s')",
                        depRaw.c_str(), info.name.c_str());
          continue;
        }

        size_t depIdx = itDep->second;
        if (visited.find(depIdx) == visited.end()) {
          stack.push_back(depIdx);
        }
      }
    }
  };

  for (const auto &incRaw : includes) {
    std::string key = normalizeInclude(incRaw);
    if (key.empty())
      continue;

    auto it = m_resolveHeaderToLibSrc.find(key);

    if (it == m_resolveHeaderToLibSrc.end()) {
      fs::path p(key);
      std::string base = p.filename().string();
      if (!base.empty()) {
        it = m_resolveHeaderToLibSrc.find(base);
      }
    }

    if (it == m_resolveHeaderToLibSrc.end())
      continue;

    const std::string &libSrcPath = it->second;
    APP_DEBUG_LOG("CLI: Resolve(%s) -> %s", key.c_str(), libSrcPath.c_str());

    auto itIdx = m_resolveSrcRootToLibIndex.find(libSrcPath);
    if (itIdx != m_resolveSrcRootToLibIndex.end()) {
      addLibWithDependencies(itIdx->second);
    } else {
      result.push_back(libSrcPath);
    }
  }

  for (size_t i = 0; i < m_resolveLibs.size(); ++i) {
    if (selectedLibIndices.find(i) == selectedLibIndices.end())
      continue;
    const auto &info = m_resolveLibs[i];
    result.push_back(info.srcRoot.string());
  }

  return result;
}

bool ArduinoCli::GetResolvedLibraries(const std::vector<SketchFileBuffer> &files, std::vector<ResolvedLibraryInfo> &outLibs) {
  ScopeTimer t("CLI: GetResolvedLibraries(files=%d)", static_cast<int>(files.size()));

  outLibs.clear();

  std::string sketchDir = GetSketchPath();
  if (sketchDir.empty() || files.empty()) {
    return false;
  }

  // find all #include in sketch code (works on unsaved buffers)
  std::unordered_set<std::string> includesSet = SearchCodeIncludes(files, sketchDir);

  if (includesSet.empty()) {
    return true;
  }

  std::vector<std::string> includes;
  includes.reserve(includesSet.size());
  for (const auto &inc : includesSet) {
    includes.push_back(inc);
  }

  // resolve library include for includes from code
  std::vector<std::string> includePaths = ResolveLibraries(includes);

  // for every path try find library.properties.
  for (const auto &includePath : includePaths) {
    std::string name;
    std::string version;
    bool isCoreLibrary = false;

    fs::path libPath(includePath);

    // try <includePath>/library.properties
    fs::path propsPath = libPath / "library.properties";
    std::ifstream in(propsPath);

    // if not exist, try parent(includePath)/library.properties
    if (!in) {
      fs::path parent = libPath.parent_path();
      if (!parent.empty()) {
        propsPath = parent / "library.properties";
        in.close();
        in.clear();
        in.open(propsPath);
      }
    }

    isCoreLibrary = (!m_platformPath.empty() && includePath.rfind(m_platformPath, 0) == 0) || (!m_corePlatformPath.empty() && includePath.rfind(m_corePlatformPath, 0) == 0);

    if (in) {
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }

        // ignore empty lines and comments
        std::string raw = line;
        TrimInPlace(raw);
        if (raw.empty() || raw[0] == '#')
          continue;

        auto eq = raw.find('=');
        if (eq == std::string::npos)
          continue;

        std::string key = raw.substr(0, eq);
        std::string val = raw.substr(eq + 1);
        TrimInPlace(key);
        TrimInPlace(val);

        if (key == "name") {
          name = val;
        } else if (key == "version") {
          version = val;
        }
      }
    }

    outLibs.emplace_back(ResolvedLibraryInfo{
        name,
        version,
        includePath,
        isCoreLibrary});
  }

  return true;
}

void ArduinoCli::GetResolvedLibrariesAsync(const std::vector<SketchFileBuffer> &files, wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  // copy files into the worker thread
  std::thread([this, weak, files]() {
    std::vector<ResolvedLibraryInfo> libs;
    bool resl = this->GetResolvedLibraries(files, libs);

    wxThreadEvent evt(EVT_RESOLVED_LIBRARIES_READY);
    evt.SetInt(resl ? 1 : 0);
    evt.SetPayload(libs);

    QueueUiEvent(weak, evt.Clone());
  }).detach();
}

bool ArduinoCli::LoadProperties() {
  fs::path buildPath = fs::path(sketchPath) / ".ardedit" / "build";
  fs::create_directories(buildPath); // creates parents as well

  fs::path ccPath = buildPath / "compile_commands.json";
  std::string inoBaseName = fs::path(sketchPath).filename().string();

  // 1) Try to use the existing compile_commands.json
  if (fs::exists(ccPath)) {
    APP_DEBUG_LOG("CLI: Using existing compile_commands.json: %s\n",
                  ccPath.string().c_str());

    clangArgs = BuildClangArgsFromCompileCommands(inoBaseName);
    if (!clangArgs.empty()) {
      return true;
    }

    APP_DEBUG_LOG("CLI: Existing %s is unusable, regenerating...", ccPath.string().c_str());
  }

  std::string baseFqbn = GetBoardName();
  if (baseFqbn.empty()) {
    baseFqbn = fqbn;
  }

  std::string cmd = GetCliBaseCommand() +
                    " compile --fqbn " + ShellQuote(baseFqbn) +
                    " --build-path " + ShellQuote(buildPath.string()) +
                    " --only-compilation-database " + ShellQuote(sketchPath);

  std::string cmdOutput;
  int rc = ExecuteCommand(cmd, cmdOutput);
  if (rc != 0) {
    wxLogWarning(wxT("Error running %s -> %d\nOutput:\n%s"), wxString::FromUTF8(cmd), rc, wxString::FromUTF8(cmdOutput));
    return false;
  }

  clangArgs = BuildClangArgsFromCompileCommands(inoBaseName);

  return !clangArgs.empty();
}

void ArduinoCli::LoadPropertiesAsync(wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  std::thread([this, weak]() {
    bool ok = this->LoadProperties();

    wxThreadEvent evt(EVT_CLANG_ARGS_READY);
    evt.SetInt(ok ? 1 : 0);

    // sends event to the GUI thread
    QueueUiEvent(weak, evt.Clone());
  }).detach();
}

/*
 Parses sketchDir/.ardedit/build/build.options.json and returns the fqbn value from the structure.
 {
   ...
   fqbn: "<prepared_for_fqbn>"
   ..
 }
 If the file does not exist or fqbn is not present, returns "".
*/
std::string ArduinoCli::GetCachedEnviromentFqbn() const {
  std::string fqbn = "";

  fs::path optionsPath = fs::path(sketchPath) / ".ardedit" / "build" / "build.options.json";

  if (!fs::exists(optionsPath)) {
    APP_DEBUG_LOG("GetCachedEnviromentFqbn: '%s' does not exist.", optionsPath.string().c_str());
    return fqbn;
  }

  std::ifstream in(optionsPath);
  if (!in) {
    APP_DEBUG_LOG("GetCachedEnviromentFqbn: failed to open '%s'.", optionsPath.string().c_str());
    return fqbn;
  }

  json j;
  try {
    in >> j;
  } catch (const std::exception &e) {
    wxLogWarning(wxT("CLI: GetCachedEnviromentFqbn: failed to parse JSON from '%s': %s"), wxString::FromUTF8(optionsPath.string()), wxString::FromUTF8(e.what()));
    return fqbn;
  }

  if (j.contains("fqbn") && j["fqbn"].is_string()) {
    fqbn = j["fqbn"].get<std::string>();
  } else {
    APP_DEBUG_LOG("GetCachedEnviromentFqbn: JSON '%s' does not contain string field 'fqbn'.", optionsPath.string().c_str());
  }

  return fqbn;
}

void ArduinoCli::CleanCachedEnvironment() {
  APP_DEBUG_LOG("CleanCachedEnvironment()");
  {
    std::lock_guard<std::mutex> lock(m_resolveCacheMutex);
    m_hasResolveLibrariesCache = false;
    m_resolveLibs.clear();
    m_resolveHeaderToLibSrc.clear();
    m_resolveSrcRootToLibIndex.clear();
    m_resolveNameToLibIndex.clear();
  }

  fs::path buildPath = fs::path(sketchPath) / ".ardedit" / "build";
  std::error_code ec;
  fs::remove_all(buildPath, ec);
  if (ec) {
    wxLogWarning(wxT("Failed to remove build directory '%s': %s"), wxString::FromUTF8(buildPath.string()), wxString::FromUTF8(ec.message()));
  } else {
    APP_DEBUG_LOG("Build directory '%s' removed.", buildPath.string().c_str());
  }
}

void ArduinoCli::CancelAsyncOperations() {
  m_cancelAsync.store(true, std::memory_order_relaxed);
}

void ArduinoCli::LoadLibrariesAsync(wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  std::thread([this, weak]() {
    ThreadNice();

    bool ok = this->LoadLibraries();

    wxThreadEvent evt(EVT_LIBRARIES_UPDATED);
    evt.SetInt(ok ? 1 : 0);
    QueueUiEvent(weak, evt.Clone());
  }).detach();
}

void ArduinoCli::SearchLibraryProvidingHeaderAsync(const std::string &header, wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  std::thread([this, weak, header]() {
    std::vector<ArduinoLibraryInfo> libs;
    bool ok = this->SearchLibraryProvidingHeader(header, libs);

    wxThreadEvent evt(EVT_LIBRARIES_FOUND);
    evt.SetInt(ok ? 1 : 0);
    evt.SetString(wxString::FromUTF8(header));
    evt.SetPayload(libs);

    QueueUiEvent(weak, evt.Clone());
  }).detach();
}

void ArduinoCli::LoadInstalledLibrariesAsync(wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  std::thread([this, weak]() {
    bool ok = this->LoadInstalledLibraries();

    wxThreadEvent evt(EVT_INSTALLED_LIBRARIES_UPDATED);
    evt.SetInt(ok ? 1 : 0);
    QueueUiEvent(weak, evt.Clone());
  }).detach();
}

bool ArduinoCli::GetBoardOptions(const std::string &fqbn, std::vector<ArduinoBoardOption> &outOptions) {
  outOptions.clear();

  std::ostringstream args;
  args << " board details"
       << " --fqbn " << ShellQuote(fqbn)
       << " --format json";

  std::string cmd = GetCliBaseCommand() + " " + args.str();
  std::string output;
  int rc = ExecuteCommand(cmd, output);
  if (rc != 0 || output.empty()) {
    return false;
  }

  json j;
  try {
    j = json::parse(output);
  } catch (...) {
    return false;
  }

  if (!j.contains("config_options")) {
    return false;
  }

  for (auto &opt : j["config_options"]) {
    ArduinoBoardOption o;

    // correct keys according to JSON
    o.id = opt.value("option", "");            // e.g. "UploadSpeed"
    o.label = opt.value("option_label", o.id); // "Upload Speed"

    if (opt.contains("values")) {
      for (auto &vj : opt["values"]) {
        ArduinoBoardOptionValue v;

        // here are value / value_label
        v.id = vj.value("value", "");            // e.g. "921600"
        v.label = vj.value("value_label", v.id); // "921600"
        v.selected = vj.value("selected", false);

        o.values.push_back(std::move(v));
      }
    }

    outOptions.push_back(std::move(o));
  }

  return true;
}

bool ArduinoCli::GetDefaultBoardOptions(std::vector<ArduinoBoardOption> &outOptions) {
  std::string boardName = GetBoardName();

  return GetBoardOptions(boardName, outOptions);
}

void ArduinoCli::GetBoardOptionsAsync(wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  // effective FQBN - if not set, we will try to fetch it ourselves
  std::string effFqbn = fqbn;
  if (effFqbn.empty()) {
    effFqbn = GetFQBN();
  }

  if (effFqbn.empty()) {
    // nothing available -> send event with error / empty payload
    wxThreadEvent evt(EVT_BOARD_OPTIONS_READY);
    evt.SetInt(0); // 0 = fail / nothing
    // we do not send the payload
    QueueUiEvent(weak, evt.Clone());
    return;
  }

  std::thread([this, weak, effFqbn]() {
    std::vector<ArduinoBoardOption> options;

    bool resl = this->GetBoardOptions(effFqbn, options);

    wxThreadEvent evt(EVT_BOARD_OPTIONS_READY);
    evt.SetInt(resl);
    evt.SetPayload(options);

    QueueUiEvent(weak, evt.Clone());
  }).detach();
}

bool ArduinoCli::GetBoardOptions(std::vector<ArduinoBoardOption> &outOptions) {
  return GetBoardOptions(fqbn, outOptions);
}

std::string ArduinoCli::BuildFqbnFromOptions(const std::vector<ArduinoBoardOption> &options) {
  std::vector<ArduinoBoardOption> defaultOptions;

  if (!GetDefaultBoardOptions(defaultOptions)) {
    // unlikely - keep empty
    APP_DEBUG_LOG("CLI: Can't get default board options!");
  }

  // base = vendor:arch:board
  std::string base = fqbn;
  size_t first = base.find(':');
  if (first != std::string::npos) {
    size_t second = base.find(':', first + 1);
    if (second != std::string::npos) {
      size_t third = base.find(':', second + 1);
      if (third != std::string::npos) {
        // everything after the third ':' are options -> we cut them off
        base = base.substr(0, third);
      }
    }
  }

  std::ostringstream oss;
  oss << base;

  bool firstOpt = true;

  for (const auto &opt : options) {
    // Find the value marked in the current selection
    auto it = std::find_if(opt.values.begin(), opt.values.end(),
                           [](const ArduinoBoardOptionValue &v) { return v.selected; });
    if (it == opt.values.end()) {
      // nothing is selected, ignore
      continue;
    }

    // Find the corresponding default option by ID
    auto defOptIt = std::find_if(defaultOptions.begin(), defaultOptions.end(),
                                 [&](const ArduinoBoardOption &d) {
                                   return d.id == opt.id;
                                 });

    bool isDefault = false;
    if (defOptIt != defaultOptions.end()) {
      // in the default selection find the default chosen value
      auto defValIt = std::find_if(defOptIt->values.begin(), defOptIt->values.end(),
                                   [](const ArduinoBoardOptionValue &v) { return v.selected; });
      if (defValIt != defOptIt->values.end()) {
        // if the ID of the selected value matches the default, it is the default choice
        if (defValIt->id == it->id) {
          isDefault = true;
        }
      }
    }

    // If this is the default value, DO NOT WRITE it to FQBN
    if (isDefault) {
      continue;
    }

    // Otherwise, write it down
    if (firstOpt) {
      oss << ':'; // begins the options section
      firstOpt = false;
    } else {
      oss << ',';
    }

    // FQBN uses the menu ID as the key and the value ID as the value
    oss << opt.id << '=' << it->id;
  }

  return oss.str();
}

void ArduinoCli::CompileAsync(wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  // CACHE: sketchPath/.ardedit/build
  fs::path cachePath = fs::path(sketchPath) / ".ardedit" / "build";
  std::error_code ec;
  fs::create_directories(cachePath, ec);

  // BUILD: some subdirectory in /tmp
  fs::path buildPath = fs::temp_directory_path() / ("arduino_edit_build_" + fs::path(sketchPath).filename().string());
  fs::create_directories(buildPath, ec);

  // args without binary (this is handled by RunCliStreaming)
  std::string args = "-v --no-color compile";
  args += " -b " + ShellQuote(fqbn);
  args += " --build-cache-path " + ShellQuote(cachePath.string());
  args += " --output-dir " + ShellQuote(buildPath.string());
  args += " " + ShellQuote(sketchPath);

  std::thread([this, weak, args]() {
    this->RunCliStreaming(args, weak, "compile");
  }).detach();
}

void ArduinoCli::UploadAsync(wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  if (serialPort.empty()) {
    wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
    evt.SetInt(-1);
    evt.SetString(_("Serial port is not set. Cannot upload sketch.\n"));
    QueueUiEvent(weak, evt.Clone());
    return;
  }

  fs::path buildPath = fs::temp_directory_path() /
                       ("arduino_edit_build_" + fs::path(sketchPath).filename().string());
  std::error_code ec;
  fs::create_directories(buildPath, ec);

  std::string args = "-v --no-color upload";
  args += " -b " + ShellQuote(fqbn);
  args += " -p " + ShellQuote(serialPort);

  // If a programmer is selected, we upload using "upload using programmer"
  // -> arduino-cli upload --programmer <id> ...
  if (!programmer.empty()) {
    args += " --programmer " + ShellQuote(programmer);
  }

  args += " --input-dir " + ShellQuote(buildPath.string());
  args += " " + ShellQuote(sketchPath);

  std::thread([this, weak, args]() {
    this->RunCliStreaming(args, weak, "upload");
  }).detach();
}

void ArduinoCli::InstallLibrariesAsync(const std::vector<ArduinoLibraryInstallSpec> &specs, wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  if (specs.empty()) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  // make a copy to ensure thread-safe capture
  auto specsCopy = specs;

  std::thread([this, weak, specsCopy]() {
    // We split the requests by type so they can be processed in groups
    std::vector<std::string> repoArgs;
    std::vector<std::string> gitArgs;
    std::vector<std::string> zipArgs;

    for (const auto &s : specsCopy) {
      switch (s.source) {
        case ArduinoLibraryInstallSource::Repo:
          if (!s.spec.empty())
            repoArgs.push_back(s.spec);
          break;
        case ArduinoLibraryInstallSource::Git:
          if (!s.spec.empty())
            gitArgs.push_back(s.spec);
          break;
        case ArduinoLibraryInstallSource::Zip:
          if (!s.spec.empty())
            zipArgs.push_back(s.spec);
          break;
      }
    }

    int overallRc = 0;

    // 1) Library repository (registry)
    if (!repoArgs.empty()) {
      std::string args = "-v --no-color lib install --no-deps";
      for (const auto &name : repoArgs) {
        args += " " + ShellQuote(name);
      }
      int rc = this->RunCliStreaming(args, weak, "lib install");
      if (rc != 0 && overallRc == 0)
        overallRc = rc;
    }

    // 2) Git libraries
    if (!gitArgs.empty()) {
      std::string args = "-v --no-color lib install --no-deps --git-url";
      for (const auto &url : gitArgs) {
        args += " " + ShellQuote(url);
      }
      int rc = this->RunCliStreaming(args, weak, "lib install");
      if (rc != 0 && overallRc == 0)
        overallRc = rc;
    }

    // 3) ZIP libraries
    if (!zipArgs.empty()) {
      std::string args = "-v --no-color lib install --no-deps --zip-path";
      for (const auto &zip : zipArgs) {
        args += " " + ShellQuote(zip);
      }
      int rc = this->RunCliStreaming(args, weak, "lib install");
      if (rc != 0 && overallRc == 0)
        overallRc = rc;
    }

    // After completing the installations...
    bool okInstalled = this->LoadInstalledLibraries();

    if (okInstalled) {
      this->InvalidateLibraryCache();
    }

    wxThreadEvent evtInstalled(EVT_INSTALLED_LIBRARIES_UPDATED);
    evtInstalled.SetInt(okInstalled ? 1 : 0);
    QueueUiEvent(weak, evtInstalled.Clone());

    wxCommandEvent summaryEvt(EVT_COMMANDLINE_OUTPUT_MSG);
    summaryEvt.SetInt(overallRc);
    summaryEvt.SetString(wxString::Format(wxT("[library install batch finished, rc=%d]"), overallRc));
    QueueUiEvent(weak, summaryEvt.Clone());
  }).detach();
}

void ArduinoCli::UninstallLibrariesAsync(const std::vector<std::string> &names, wxEvtHandler *handler) {
  if (!handler)
    return;

  if (names.empty())
    return;

  wxWeakRef<wxEvtHandler> weak(handler);

  auto namesCopy = names;

  std::thread([this, weak, namesCopy]() {
    std::string args = "-v --no-color lib uninstall";
    for (const auto &name : namesCopy) {
      args += " " + ShellQuote(name);
    }

    int rc = this->RunCliStreaming(args, weak, "lib uninstall");

    bool okInstalled = false;
    if (rc == 0) {
      okInstalled = this->LoadInstalledLibraries();

      if (okInstalled) {
        this->InvalidateLibraryCache();
      }
    }

    wxThreadEvent evtInstalled(EVT_INSTALLED_LIBRARIES_UPDATED);
    evtInstalled.SetInt(okInstalled ? 1 : 0);
    QueueUiEvent(weak, evtInstalled.Clone());
  }).detach();
}

void ArduinoCli::InstallCoresAsync(const std::vector<std::string> &coreIds, wxEvtHandler *handler) {
  if (!handler)
    return;

  if (coreIds.empty())
    return;

  wxWeakRef<wxEvtHandler> weak(handler);

  auto idsCopy = coreIds;

  std::thread([this, weak, idsCopy]() {
    // arduino-cli core install <id> <id> ...
    std::string args = "-v --no-color core install";
    for (const auto &id : idsCopy) {
      if (!id.empty()) {
        args += " " + ShellQuote(id);
      }
    }

    int rc = this->RunCliStreaming(args, weak, "core install");

    // After installation, we load the fresh core list
    bool okCores = this->LoadCores();

    wxThreadEvent evt(EVT_CORES_UPDATED);
    evt.SetInt(okCores ? 1 : 0);
    QueueUiEvent(weak, evt.Clone());

    // Optional summary to the output panel (same pattern as in libs):
    wxCommandEvent summaryEvt(EVT_COMMANDLINE_OUTPUT_MSG);
    summaryEvt.SetInt(rc);
    summaryEvt.SetString(wxString::Format(wxT("[core install batch finished, rc=%d]"), rc));
    QueueUiEvent(weak, summaryEvt.Clone());
  }).detach();
}

void ArduinoCli::UninstallCoresAsync(const std::vector<std::string> &coreIds, wxEvtHandler *handler) {
  if (!handler)
    return;

  if (coreIds.empty())
    return;

  wxWeakRef<wxEvtHandler> weak(handler);

  auto idsCopy = coreIds;

  std::thread([this, weak, idsCopy]() {
    // arduino-cli core uninstall <id> <id> ...
    std::string args = "-v --no-color core uninstall";
    for (const auto &id : idsCopy) {
      if (!id.empty()) {
        args += " " + ShellQuote(id);
      }
    }

    int rc = this->RunCliStreaming(args, weak, "core uninstall");

    // After uninstall, refresh the core list
    bool okCores = this->LoadCores();

    wxThreadEvent evt(EVT_CORES_UPDATED);
    evt.SetInt(okCores ? 1 : 0);
    QueueUiEvent(weak, evt.Clone());

    wxCommandEvent summaryEvt(EVT_COMMANDLINE_OUTPUT_MSG);
    summaryEvt.SetInt(rc);
    summaryEvt.SetString(wxString::Format(wxT("[core uninstall batch finished, rc=%d]"), rc));
    QueueUiEvent(weak, summaryEvt.Clone());
  }).detach();
}

void ArduinoCli::UpdateCoreIndexAsync(wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  // arduino-cli --no-color core update-index
  std::string args = "--no-color core update-index";

  std::thread([this, weak, args]() {
    this->RunCliStreaming(args, weak, "core update-index");
  }).detach();
}

void ArduinoCli::SetFQBN(const std::string &newFqbn) {
  if (fqbn == newFqbn)
    return;

  fqbn = newFqbn;

  // If we don't have the path to the sketch or the CLI binary, there's no point in doing attach
  if (sketchPath.empty() || m_cli.empty()) {
    return;
  }

  // Library cache needs to be cleared because architecture maybe changed.
  InvalidateLibraryCache();

  // arduino-cli board attach -b "<fqbn>" [-p "<port>"] "<sketchPath>"
  std::ostringstream args;
  args << " board attach"
       << " -b " << ShellQuote(newFqbn);

  if (!serialPort.empty()) {
    args << " -p " << ShellQuote(serialPort);
  }

  args << " " << ShellQuote(sketchPath);

  std::string cmd = GetCliBaseCommand() + " " + args.str();
  std::string output;
  int rc = ExecuteCommand(cmd, output);
  if (rc != 0) {
    wxLogWarning(wxT("arduino-cli board attach failed (rc=%d)."), rc);
  } else {
    APP_DEBUG_LOG("CLI: board attach ok for fqbn=%s sketch=%s",
                  newFqbn.c_str(), sketchPath.c_str());
  }
}

std::string ArduinoCli::GetFQBN() {
  return fqbn;
}

std::string ArduinoCli::GetBoardName() const {
  // If fqbn is empty, return an empty string
  if (fqbn.empty())
    return "";

  // Find the first '=' - options start there, so we cut it off
  size_t eqPos = fqbn.find('=');
  std::string base = (eqPos == std::string::npos) ? fqbn : fqbn.substr(0, eqPos);

  // Now we take only the first three parts separated by ':' from base
  int colonCount = 0;

  for (size_t i = 0; i < base.size(); ++i) {
    if (base[i] == ':') {
      colonCount++;
      if (colonCount == 3) {
        // i is the position of the third colon -> board name ends here
        return base.substr(0, i);
      }
    }
  }

  // If there are not even three parts, we return what we have
  return base;
}

std::string ArduinoCli::GetTargetFromFQBN() const {
  // FQBN format: vendor:arch:board
  std::string f = GetBoardName();
  TrimInPlace(f);
  size_t p1 = f.find(':');
  if (p1 == std::string::npos) {
    return "";
  }
  size_t p2 = f.find(':', p1 + 1);
  if (p2 == std::string::npos) {
    // vendor:arch
    return NormalizeArchTarget(f.substr(p1 + 1));
  }
  return NormalizeArchTarget(f.substr(p1 + 1, p2 - (p1 + 1)));
}

template <typename It>
static bool IsArchCompatibleTokens(It begin, It end, const std::string &targetArchNorm) {
  if (targetArchNorm.empty()) {
    return true; // unknown target => don't filter
  }
  if (begin == end) {
    return true; // missing/empty => compatible
  }

  for (auto it = begin; it != end; ++it) {
    std::string a = *it;
    TrimInPlace(a);
    a = NormalizeArchTarget(a);

    if (a.empty()) {
      continue;
    }
    if (a == "*" || a == "all" || a == targetArchNorm) {
      return true;
    }
  }

  return false;
}

bool ArduinoCli::IsLibraryArchitectureCompatible(const std::string &architecturesRaw) const {
  const std::string target = NormalizeArchTarget(GetTargetFromFQBN());

  std::string raw = architecturesRaw;
  TrimInPlace(raw);
  if (raw.empty()) {
    return true; // missing/empty => compatible
  }

  std::vector<std::string> tokens;
  tokens.reserve(8);

  std::stringstream ss(raw);
  std::string item;
  while (std::getline(ss, item, ',')) {
    tokens.push_back(item);
  }

  return IsArchCompatibleTokens(tokens.begin(), tokens.end(), target);
}

bool ArduinoCli::IsLibraryArchitectureCompatible(const std::vector<std::string> &architectures) const {
  const std::string target = NormalizeArchTarget(GetTargetFromFQBN());
  return IsArchCompatibleTokens(architectures.begin(), architectures.end(), target);
}

std::string ArduinoCli::GetSketchPath() const {
  return sketchPath;
}

std::vector<ArduinoCoreBoard> ArduinoCli::GetAvailableBoards() {
  std::vector<ArduinoCoreBoard> boards;

  std::string cmd = GetCliBaseCommand() + " board listall";
  std::string output;
  int rc = ExecuteCommand(cmd, output);

  if (rc != 0 || output.empty()) {
    return boards;
  }

  std::istringstream stream(output);
  std::string line;
  bool firstLine = true;

  while (std::getline(stream, line)) {
    if (firstLine) {
      firstLine = false;
      continue; // skip header
    }

    if (line.empty())
      continue;

    // trim trailing newline/spaces
    auto lastNotSpace = line.find_last_not_of(" \t\r\n");
    if (lastNotSpace == std::string::npos)
      continue;
    line.erase(lastNotSpace + 1);

    // trim leading spaces
    auto firstNotSpace = line.find_first_not_of(" \t");
    if (firstNotSpace == std::string::npos)
      continue;
    if (firstNotSpace > 0)
      line.erase(0, firstNotSpace);

    // now we have something like:
    // "Waveshare ESP32-S3-LCD-1.46                 esp32:esp32:waveshare_esp32_s3_lcd_146"

    // find the last space - FQBN is after it
    size_t pos = line.find_last_of(' ');
    if (pos == std::string::npos)
      continue;

    std::string name = line.substr(0, pos);
    std::string fqbn = line.substr(pos + 1);

    // clean up the name (remove trailing spaces)
    auto nameLast = name.find_last_not_of(" \t");
    if (nameLast != std::string::npos)
      name.erase(nameLast + 1);

    if (fqbn.empty() || fqbn.find(':') == std::string::npos)
      continue;

    ArduinoCoreBoard info;
    info.name = std::move(name);
    info.fqbn = std::move(fqbn);
    boards.push_back(std::move(info));
  }

  std::sort(boards.begin(), boards.end(),
            [](const ArduinoCoreBoard &a, const ArduinoCoreBoard &b) {
              return a.fqbn < b.fqbn;
            });

  return boards;
}

void ArduinoCli::GetAvailableBoardsAsync(wxEvtHandler *handler) {
  if (!handler)
    return;

  APP_DEBUG_LOG("GetAvailableBoardsAsync()");

  wxWeakRef<wxEvtHandler> weak(handler);

  std::thread([this, weak]() {
    std::vector<ArduinoCoreBoard> brds = this->GetAvailableBoards();

    wxThreadEvent evt(EVT_AVAILABLE_BOARDS_UPDATED);
    evt.SetInt(1);
    evt.SetPayload(brds);
    QueueUiEvent(weak, evt.Clone());
  }).detach();
}

bool ArduinoCli::LoadCores() {
  // arduino-cli core list --format json
  std::string cmd = GetCliBaseCommand() + " core list --all --format json";
  std::string output;
  int rc = ExecuteCommand(cmd, output);

  if (rc != 0 || output.empty()) {
    wxLogWarning(wxT("arduino-cli core list failed (rc=%d)."), rc);
    return false;
  }

  json j;
  try {
    j = json::parse(output);
  } catch (const std::exception &e) {
    wxLogWarning(wxT("Failed to parse arduino-cli core list JSON: %s"), wxString::FromUTF8(e.what()));
    return false;
  }

  if (!j.contains("platforms") || !j["platforms"].is_array()) {
    wxLogWarning(wxT("arduino-cli core list JSON does not contain 'platforms' array."));
    return false;
  }

  std::vector<ArduinoCoreInfo> tmp;
  const auto &platformsJson = j["platforms"];
  tmp.reserve(platformsJson.size());

  for (const auto &jp : platformsJson) {
    if (!jp.is_object()) {
      continue;
    }

    ArduinoCoreInfo info;
    info.id = jp.value("id", "");
    info.maintainer = jp.value("maintainer", "");
    info.website = jp.value("website", "");
    info.email = jp.value("email", "");
    info.indexed = jp.value("indexed", false);

    info.installedVersion = jp.value("installed_version", "");
    info.latestVersion = jp.value("latest_version", "");

    if (info.id.empty()) {
      // without ID it doesn't make much sense, skip it
      continue;
    }

    if (jp.contains("releases") && jp["releases"].is_object()) {
      const auto &relsJson = jp["releases"];

      info.availableVersions.reserve(relsJson.size());
      info.releases.reserve(relsJson.size());

      for (auto it = relsJson.begin(); it != relsJson.end(); ++it) {
        if (!it.value().is_object()) {
          continue;
        }

        std::string ver = it.key();
        info.availableVersions.push_back(ver);

        ArduinoCoreRelease rel;
        ParseCoreRelease(it.value(), ver, rel);
        info.releases.push_back(std::move(rel));
      }
    }

    tmp.push_back(std::move(info));
  }

  cores.swap(tmp);
  return !cores.empty();
}

void ArduinoCli::LoadCoresAsync(wxEvtHandler *handler) {
  if (!handler)
    return;

  wxWeakRef<wxEvtHandler> weak(handler);

  APP_DEBUG_LOG("LoadCoresAsync()");

  std::thread([this, weak]() {
    bool ok = this->LoadCores();

    wxThreadEvent evt(EVT_CORES_LOADED);
    evt.SetInt(ok ? 1 : 0);
    QueueUiEvent(weak, evt.Clone());
  }).detach();
}

const std::vector<ArduinoCoreInfo> &ArduinoCli::GetCores() const {
  return cores;
}

std::vector<SerialPortInfo> ArduinoCli::GetSerialPorts() {
  std::vector<SerialPortInfo> ports;

  std::string cmd = GetCliBaseCommand() + " board list --format json";
  std::string output;
  int rc = ExecuteCommand(cmd, output);

  if (rc != 0 || output.empty()) {
    return ports;
  }

  try {
    json j = json::parse(output);

    if (!j.contains("detected_ports") || !j["detected_ports"].is_array()) {
      return ports;
    }

    for (const auto &item : j["detected_ports"]) {
      if (!item.contains("port") || !item["port"].is_object()) {
        continue;
      }

      const auto &jp = item["port"];

      std::string protocol = jp.value("protocol", "");

      if (protocol != "serial" && protocol != "network") {
        continue;
      }

      SerialPortInfo info;
      info.address = jp.value("address", "");
      info.label = jp.value("label", info.address);
      info.protocol = protocol;

      if (!info.address.empty()) {
        ports.push_back(std::move(info));
      }
    }
  } catch (const std::exception &e) {
    wxLogWarning(wxT("Failed to parse arduino-cli board list JSON: %s"), wxString::FromUTF8(e.what()));
  }

  return ports;
}

void ArduinoCli::UpdateLibraryIndexAsync(wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  std::string args = " -v --no-color lib update-index";

  std::thread([this, weak, args]() {
    this->RunCliStreaming(args, weak, "lib update-index");
  }).detach();
}

const std::vector<ArduinoLibraryInfo> &ArduinoCli::GetLibraries() const {
  return libraries;
}

const std::vector<ArduinoLibraryInfo> &ArduinoCli::GetInstalledLibraries() const {
  return installedLibraries;
}

bool ArduinoCli::IsArduinoLibraryInstalled(const ArduinoLibraryInfo &info) {
  for (const auto &inst : installedLibraries) {
    if (inst.name == info.name) {
      return true;
    }
  }
  return false;
}

void ArduinoCli::SetSerialPort(const std::string &port) {
  if (serialPort == port)
    return;

  serialPort = port;

  // If we don't have the path to the sketch or the CLI binary, there's no point in doing attach
  if (sketchPath.empty() || m_cli.empty()) {
    return;
  }

  // arduino-cli board attach -p "<port>" "<sketchPath>"
  std::ostringstream args;
  args << " board attach"
       << " -p " << ShellQuote(port)
       << " " << ShellQuote(sketchPath);

  std::string cmd = GetCliBaseCommand() + " " + args.str();
  std::string output;
  int rc = ExecuteCommand(cmd, output);
  if (rc != 0) {
    wxLogWarning(wxT("arduino-cli board attach (port) failed (rc=%d)."), rc);
  } else {
    APP_DEBUG_LOG("CLI: board attach ok for port=%s sketch=%s",
                  port.c_str(), sketchPath.c_str());
  }
}

const std::string &ArduinoCli::GetSerialPort() const {
  return serialPort;
}

std::string ArduinoCli::DetectCliExecutable(const std::string &configValue, std::string *version) {
  auto tryCandidate = [&](const std::string &exe) -> std::string {
    std::string cmd = ShellQuote(exe) + " version --json";
    std::string output;
    int rc = ExecuteCommand(cmd, output);

    if (rc != 0 || output.empty()) {
      return "";
    }

    try {
      auto j = nlohmann::json::parse(output);
      if (!j.contains("VersionString")) {
        return "";
      }

      return j["VersionString"].get<std::string>();
    } catch (...) {
      return "";
    }
  };

  std::vector<std::string> candidates;

  if (!configValue.empty()) {
    candidates.push_back(configValue);
  }

  candidates.push_back("arduino-cli");

  // OS-specific paths
#if defined(__APPLE__)
  candidates.push_back("arduino-cli");
  candidates.push_back("/opt/homebrew/bin/arduino-cli");
  candidates.push_back("/usr/local/bin/arduino-cli");
#elif defined(__linux__)
  candidates.push_back("arduino-cli");
  if (const char *home = std::getenv("HOME")) {
    std::string cand = std::string(home) + "/bin/arduino-cli";
    candidates.push_back(cand);
  }
  candidates.push_back("/usr/bin/arduino-cli");
  candidates.push_back("/usr/local/bin/arduino-cli");
#elif defined(_WIN32)
  candidates.push_back("arduino-cli.exe");
  candidates.push_back("C:\\\\Program Files\\\\Arduino CLI\\\\arduino-cli.exe");
  candidates.push_back("C:\\\\Program Files (x86)\\\\Arduino CLI\\\\arduino-cli.exe");
#endif

  for (auto c : candidates) {
    APP_DEBUG_LOG("CLI: Trying %s", c.c_str());
    std::string s = tryCandidate(c);
    if (!s.empty()) {
      APP_DEBUG_LOG("CLI: %s found with version %s...", c.c_str(), s.c_str());

      if (version) {
        *version = s;
      }

      return c;
    }
  }

  APP_DEBUG_LOG("CLI: No arduino-cli found!");

  return "";
}

std::string ArduinoCli::GetCliBaseCommand() const {
  return ShellQuote(m_cli);
}

std::string ArduinoCli::GetCliPath() const {
  return m_cli;
}

void ArduinoCli::QueueUiEvent(const wxWeakRef<wxEvtHandler> &weak, wxEvent *event) {
  if (!event)
    return;

  if (m_cancelAsync.load(std::memory_order_relaxed)) {
    delete event;
    return;
  }

  if (!wxTheApp) {
    delete event;
    return;
  }

  wxTheApp->CallAfter([weak, event]() {
    wxEvtHandler *h = weak.get();
    if (!h) {
      delete event;
      return;
    }
    wxQueueEvent(h, event);
  });
}

bool ArduinoCli::GetProgrammersForFqbn(const std::string &fqbnArg, std::vector<ArduinoProgrammerInfo> &out) {
  out.clear();

  if (m_cli.empty()) {
    return false;
  }

  std::string effFqbn = fqbnArg;
  if (effFqbn.empty()) {
    effFqbn = fqbn;
  }
  if (effFqbn.empty()) {
    return false;
  }

  // arduino-cli board details -b <fqbn> --format json
  std::ostringstream args;
  args << " board details"
       << " -b " << ShellQuote(effFqbn)
       << " --format json";

  std::string cmd = GetCliBaseCommand() + " " + args.str();
  std::string output;
  int rc = ExecuteCommand(cmd, output);

  if (rc != 0 || output.empty()) {
    APP_DEBUG_LOG("GetProgrammersForFqbn: board details failed (rc=%d, out='%s')", rc, output.c_str());
    return false;
  }

  try {
    json j = json::parse(output);

    if (!j.contains("programmers") || !j["programmers"].is_array()) {
      // Some boards may simply have no programmers at all
      APP_DEBUG_LOG("GetProgrammersForFqbn: no 'programmers' array in JSON for fqbn=%s", effFqbn.c_str());
      return true;
    }

    for (const auto &pj : j["programmers"]) {
      if (!pj.is_object()) {
        continue;
      }

      ArduinoProgrammerInfo info;
      info.id = pj.value("id", "");

      // Let's try to find a reasonable name - according to the current form of the JSON, it is often "name"
      if (pj.contains("name") && pj["name"].is_string()) {
        info.name = pj["name"].get<std::string>();
      } else {
        // fallback - at least something
        info.name = info.id;
      }

      if (!info.id.empty()) {
        out.push_back(std::move(info));
      }
    }
  } catch (const std::exception &e) {
    wxLogWarning(wxT("GetProgrammersForFqbn: failed to parse board details JSON: %s"), wxString::FromUTF8(e.what()));
    return false;
  }

  return true;
}

void ArduinoCli::GetProgrammersForFqbnAsync(wxEvtHandler *handler, const std::string &fqbnArg) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  std::thread([this, weak, fqbnArg]() {
    std::vector<ArduinoProgrammerInfo> programmers;

    bool resl = this->GetProgrammersForFqbn(fqbnArg, programmers);

    wxThreadEvent evt(EVT_PROGRAMMERS_READY);
    evt.SetInt(resl);
    evt.SetPayload(std::move(programmers));
    QueueUiEvent(weak, evt.Clone());
  }).detach();
}

bool ArduinoCli::SetProgrammer(const std::string &id) {
  programmer = id;

  // If we don't have the path to the sketch or the CLI binary, there's no point in doing attach
  if (sketchPath.empty() || m_cli.empty()) {
    return false;
  }

  // arduino-cli board attach -P "<programmer>" "<sketchPath>"
  std::ostringstream args;
  args << " board attach"
       << " -P " << ShellQuote(id)
       << " " << ShellQuote(sketchPath);

  std::string cmd = GetCliBaseCommand() + " " + args.str();
  std::string output;
  int rc = ExecuteCommand(cmd, output);

  if (rc != 0) {
    wxLogWarning(wxT("arduino-cli board attach (programmer) failed (rc=%d)."), rc);
    return false;
  } else {
    APP_DEBUG_LOG("CLI: board attach ok for programmer=%s sketch=%s", id.c_str(), sketchPath.c_str());
    return true;
  }
}

const std::string &ArduinoCli::GetProgrammer() const {
  return programmer;
}

void ArduinoCli::BurnBootloaderAsync(wxEvtHandler *handler) {
  if (!handler) {
    return;
  }

  wxWeakRef<wxEvtHandler> weak(handler);

  if (fqbn.empty()) {
    wxCommandEvent evt(EVT_COMMANDLINE_OUTPUT_MSG);
    evt.SetInt(-1);
    evt.SetString(wxT("Board (FQBN) is not set. Cannot burn bootloader.\n"));
    QueueUiEvent(weak, evt.Clone());
    return;
  }

  std::string args = "-v --no-color burn-bootloader";
  args += " -b " + ShellQuote(fqbn);

  // For programmers of the "Arduino as ISP" type, the port (serial.port) is often also needed
  if (!serialPort.empty()) {
    args += " -p " + ShellQuote(serialPort);
  }

  // Programmer: if set, add it
  if (!programmer.empty()) {
    args += " --programmer " + ShellQuote(programmer);
  }

  std::thread([this, weak, args]() {
    this->RunCliStreaming(args, weak, "burn-bootloader");
  }).detach();
}

void ArduinoCli::InitAttachedBoard() {
  if (sketchPath.empty() || m_cli.empty()) {
    return;
  }

  fqbn.clear();
  serialPort.clear();
  programmer.clear();

  // arduino-cli board attach "<sketch>" --format json
  std::ostringstream args;
  args << " board attach "
       << ShellQuote(sketchPath)
       << " --format json";

  std::string cmd = GetCliBaseCommand() + " " + args.str();

  std::string output;
  int rc = ExecuteCommand(cmd, output);

  if (rc != 0 || output.empty()) {
    APP_DEBUG_LOG("InitAttachedBoard: attach failed (rc=%d), out='%s'",
                  rc, output.c_str());
    return;
  }

  try {
    json j = json::parse(output);

    // FQBN
    if (j.contains("fqbn") && j["fqbn"].is_string()) {
      fqbn = j["fqbn"].get<std::string>();
    }

    // Serial port
    if (j.contains("port")) {
      const auto &jp = j["port"];

      if (jp.is_string()) {
        serialPort = jp.get<std::string>();
      } else if (jp.is_object() &&
                 jp.contains("address") &&
                 jp["address"].is_string()) {
        serialPort = jp["address"].get<std::string>();
      }
    }

    // Programmer
    if (j.contains("programmer") && j["programmer"].is_string()) {
      programmer = j["programmer"].get<std::string>();
    }

    APP_DEBUG_LOG("CLI: InitAttachedBoard: fqbn='%s', port='%s', programmer='%s'",
                  fqbn.c_str(), serialPort.c_str(), programmer.c_str());

  } catch (const std::exception &e) {
    wxLogWarning(wxT("CLI: InitAttachedBoard: JSON parse error: %s"),
                 wxString::FromUTF8(e.what()));
  }
}

ArduinoCli::ArduinoCli(const std::string &sketchPath_, const std::string &cliPath_)
    : m_hasResolveLibrariesCache(false), fqbn(""), sketchPath(sketchPath_), m_cli(cliPath_), serialPort() {

  InitAttachedBoard();
}
