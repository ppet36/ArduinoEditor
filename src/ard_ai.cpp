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

#include "ard_ai.hpp"
#include "ard_aipnl.hpp"
#include "ard_diff.hpp"
#include "ard_ed_frm.hpp"
#include "ard_edit.hpp"
#include "ard_ev.hpp"
#include "ard_indic.hpp"
#include "ard_pop.hpp"
#include "ard_ps.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <random>
#include <thread>
#include <wx/arrstr.h>
#include <wx/datetime.h>
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stc/stc.h>

using nlohmann::json;

static const int ID_PROCESS_AI_OPERATION = 666;

static wxString solveErrorSystemPrompt = wxT(
    "You are an expert C++/Arduino build assistant integrated into an IDE.\n"
    "You fix compiler errors by proposing minimal code patches.\n"
    "\n"
    "You have two options:\n"
    "1) If the information you received is enough to confidently fix the error,\n"
    "   respond ONLY with a patch in this format:\n"
    "\n"
    "*** BEGIN PATCH\n"
    "FILE: <sketch-relative path>\n"
    "RANGE: <from>-<to>\n"
    "REPLACE:\n"
    "<new code>\n"
    "ENDREPLACE\n"
    "*** END PATCH\n"
    "\n"
    "2) If you need more information (e.g. full includes list, where a symbol is declared,\n"
    "   project-wide search result, or a code range from a file), respond ONLY with one or more information requests:\n"
    "\n"
    "*** BEGIN INFO_REQUEST\n"
    "ID: <numeric id>\n"
    "TYPE: <includes | symbol_declaration | search | file_range>\n"
    "[additional fields depending on TYPE]\n"
    "*** END INFO_REQUEST\n"
    "\n"
    "For TYPE=file_range you MUST provide:\n"
    "  FILE: <file name as previously provided>\n"
    "  FROM_LINE: <1-based line number>\n"
    "  TO_LINE: <1-based line number, >= FROM_LINE>\n"
    "\n"
    "Never mix PATCH and INFO_REQUEST in one response.\n"
    "If you need information, you may emit MULTIPLE INFO_REQUEST blocks in a single response.\n"
    "Never output any commentary, explanation, or Markdown.\n"
    "If you already received one or more INFO_RESPONSE blocks with the information you asked for,"
    "you MUST now produce a PATCH and not ask for the same info again.\n");

static wxString interactiveChatSystemPrompt = wxT(
    "You are an expert Arduino/C++ assistant integrated into an IDE.\n"
    "\n"
    "You are talking to the user in a chat panel inside the Arduino Editor.\n"
    "You can do two categories of things:\n"
    "(A) When the user asks you to EXPLAIN something (e.g. 'explain'),\n"
    "    answer in natural language (Markdown is allowed) and DO NOT emit a PATCH.\n"
    "(B) When the user asks you to IMPLEMENT, GENERATE, CREATE, MODIFY, REFACTOR code or a sketch\n"
    "    (for example in Czech: 'implementuj', 'vytvoř', 'vygeneruj', 'přepiš', 'uprav'),\n"
    "    you MUST respond with a PATCH in the format below. Outside the PATCH block maybe explanation.\n"
    "(C) If you are unsure whether you have enough context, choose INFO_REQUEST.\n"
    "\n"
    "PATCH format (for editing code):\n"
    "*** BEGIN PATCH\n"
    "FILE: <sketch-relative path>\n"
    "RANGE: <from>-<to> (1-based line numbers)\n"
    "REPLACE:\n"
    "<new code>\n"
    "ENDREPLACE\n"
    "*** END PATCH\n"
    "\n"
    "INFO_REQUEST format (for asking the IDE for context):\n"
    "*** BEGIN INFO_REQUEST\n"
    "ID: <numeric id>\n"
    "TYPE: <file_range | includes | symbol_declaration | search>\n"
    "[additional fields depending on TYPE]\n"
    "*** END INFO_REQUEST\n"
    "\n"
    "For TYPE=file_range you MUST provide:\n"
    "  FILE: <file name as previously provided>\n"
    "  FROM_LINE: <1-based line number>\n"
    "  TO_LINE: <1-based line number, >= FROM_LINE>\n"
    "\n"
    "Rules:\n"
    "- Output MUST be either a PATCH or an INFO_REQUEST, never both.\n"
    "- If the user asks to create/modify/refactor code, you MUST respond with a PATCH, unless you first need more context.\n"
    "- If you need more context to produce a correct PATCH, you MUST respond with INFO_REQUEST (TYPE=file_range) and nothing else.\n"
    "- Do NOT ask the user to paste files or code. If you need code, use INFO_REQUEST (TYPE=file_range).\n"
    "- Before producing a PATCH for any file (including CURRENT_FILE), you MUST have received numbered INFO_RESPONSE content (TYPE=file_range) covering the lines you will edit.\n"
    "- When you decide you need file_range, request ALL required files/ranges in the SAME response (batch your INFO_REQUEST blocks).\n"
    "- After you receive INFO_RESPONSE blocks, you MUST use them to produce a PATCH in your next response (do not request the same file_range again unless you believe the file changed).\n"
    "- When the user asks only for explanation, answer in Markdown and do NOT emit a PATCH.\n");

static wxString optimizeFunctionSystemPrompt = wxT(
    "You are an expert C++/Arduino performance and code-quality engineer integrated into an IDE.\n"
    "\n"
    "Your task is to refactor and optimize a single function or method while strictly preserving its\n"
    "observable behavior and public interface.\n"
    "\n"
    "Goals (in this order):\n"
    "1) Keep the behavior EXACTLY the same (including edge cases and error handling).\n"
    "2) Improve readability and maintainability of the code.\n"
    "3) Where safe, reduce CPU time, memory usage and unnecessary allocations.\n"
    "\n"
    "You will receive information about the current file, the target function/method and a numbered\n"
    "code context. You are allowed to ask for more information if needed.\n"
    "\n"
    "You have two options:\n"
    "1) If the information you received is enough to confidently refactor/optimize the function,\n"
    "   respond ONLY with a patch in this format:\n"
    "\n"
    "*** BEGIN PATCH\n"
    "FILE: <sketch-relative path>\n"
    "RANGE: <from>-<to>\n"
    "REPLACE:\n"
    "<new code>\n"
    "ENDREPLACE\n"
    "*** END PATCH\n"
    "\n"
    "The RANGE must use 1-based line numbers from the numbered code context you received.\n"
    "You will also receive BODY_LINES: A-B for the target function/method.\n"
    "When you emit a PATCH for this task, you MUST set RANGE EXACTLY to A-B.\n"
    "Do NOT narrow the range to a subset of the body and do NOT extend it beyond A-B.\n"
    "This range already covers the full function/method body including its closing brace.\n"
    "\n"
    "Do not change the function signature unless absolutely necessary.\n"
    "\n"
    "2) If you need more information (e.g. full includes list, where a symbol is declared,\n"
    "   project-wide search result, or a code range from a file), respond ONLY with one or more information requests:\n"
    "\n"
    "*** BEGIN INFO_REQUEST\n"
    "ID: <numeric id>\n"
    "TYPE: <includes | symbol_declaration | search | file_range>\n"
    "[additional fields depending on TYPE]\n"
    "*** END INFO_REQUEST\n"
    "\n"
    "For TYPE=file_range you MUST provide:\n"
    "  FILE: <file name as previously provided>\n"
    "  FROM_LINE: <1-based line number>\n"
    "  TO_LINE: <1-based line number, >= FROM_LINE>\n"
    "\n"
    "Rules:\n"
    "- Never mix PATCH and INFO_REQUEST in one response.\n"
    "- Never output any commentary, explanation, or Markdown outside of PATCH/INFO_REQUEST blocks.\n"
    "- If you already received one or more INFO_RESPONSE blocks with the information you asked for,\n"
    "  you MUST now produce a PATCH and not ask for the same info again.\n");

// --------- AI persistence helpers ------------

static std::string NowUtcIso8601() {
  wxDateTime now = wxDateTime::UNow();
  return wxToStd(now.FormatISOCombined('T')) + "Z";
}

static std::string MakeSessionId() {
  wxDateTime now = wxDateTime::UNow();
  wxString base = now.Format(wxT("%Y%m%d_%H%M%S"));

  // short random suffix
  std::random_device rd;
  uint32_t r = rd();
  wxString suf = wxString::Format(wxT("%06x"), (unsigned)(r & 0xFFFFFF));

  return wxToStd(base + wxT("_") + suf);
}

static wxFileName GetAiRootDir(const std::string &sketchRoot) {
  wxFileName fn(wxString::FromUTF8(sketchRoot), wxEmptyString);
  fn.AppendDir(wxT(".ardedit"));
  fn.AppendDir(wxT("ai"));
  return fn;
}

static wxFileName GetAiSessionsDir(const std::string &sketchRoot) {
  wxFileName fn = GetAiRootDir(sketchRoot);
  fn.AppendDir(wxT("sessions"));
  return fn;
}

static bool EnsureDir(const wxFileName &dir) {
  wxFileName d = dir;
  return d.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
}

static wxFileName GetIndexJsonPath(const std::string &sketchRoot) {
  wxFileName fn = GetAiRootDir(sketchRoot);
  fn.SetFullName(wxT("index.json"));
  return fn;
}

static wxFileName GetSessionJsonlPath(const std::string &sketchRoot, const std::string &sessionId) {
  wxFileName fn = GetAiSessionsDir(sketchRoot);
  fn.SetFullName(wxString::FromUTF8(sessionId) + wxT(".jsonl"));
  return fn;
}

static json LoadIndexJson(const std::string &sketchRoot) {
  wxFileName p = GetIndexJsonPath(sketchRoot);
  if (!p.FileExists()) {
    return json{{"version", 1}, {"sessions", json::array()}};
  }
  wxFFile f(p.GetFullPath(), wxT("rb"));
  if (!f.IsOpened()) {
    return json{{"version", 1}, {"sessions", json::array()}};
  }
  wxString content;
  f.ReadAll(&content);
  try {
    return json::parse(wxToStd(content));
  } catch (...) {
    return json{{"version", 1}, {"sessions", json::array()}};
  }
}

static void SaveIndexJsonAtomic(const std::string &sketchRoot, const json &j) {
  wxFileName p = GetIndexJsonPath(sketchRoot);
  wxFileName tmp = p;
  tmp.SetFullName(p.GetName() + wxT(".tmp"));

  wxFFile f(tmp.GetFullPath(), wxT("wb"));
  if (!f.IsOpened())
    return;

  std::string dumped = j.dump(2);
  f.Write(wxString::FromUTF8(dumped));
  f.Close();

  // atomic-ish replace
  if (wxFileExists(p.GetFullPath())) {
    wxRemoveFile(p.GetFullPath());
  }
  wxRenameFile(tmp.GetFullPath(), p.GetFullPath(), true);
}

static void UpdateSessionTitleInIndex(const std::string &sketchRoot, const std::string &sessionId, const std::string &title) {
  if (sessionId.empty())
    return;

  json idx = LoadIndexJson(sketchRoot);
  if (!idx.contains("sessions") || !idx["sessions"].is_array())
    return;

  bool changed = false;

  for (auto &s : idx["sessions"]) {
    if (!s.is_object())
      continue;
    if (s.value("id", "") == sessionId) {
      s["title"] = title;
      changed = true;
      break;
    }
  }

  if (changed) {
    SaveIndexJsonAtomic(sketchRoot, idx);
  }
}

static void AppendJsonlLine(const wxFileName &path, const json &line) {
  wxFFile f(path.GetFullPath(), wxT("ab"));
  if (!f.IsOpened())
    return;

  std::string dumped = line.dump();
  f.Write(wxString::FromUTF8(dumped));
  f.Write(wxT("\n"));
  f.Close();
}

// --------------

wxString GetContextAroundLine(wxStyledTextCtrl *stc, int centerLine, int radius) {
  int lineCount = stc->GetLineCount();
  int fromLine = std::max(0, centerLine - radius);
  int toLine = std::min(lineCount - 1, centerLine + radius);

  int startPos = stc->PositionFromLine(fromLine);
  int endPos = stc->GetLineEndPosition(toLine);

  return stc->GetTextRange(startPos, endPos);
}

// ---
static wxString GetNumberedContext(wxStyledTextCtrl *stc, int fromLine, int toLine, wxString *outRawNoNumbers = nullptr) {
  wxString out;
  wxString raw;

  const int maxLine = stc->GetLineCount() - 1;
  const int fl = std::max(0, fromLine);
  const int tl = std::min(maxLine, toLine);

  for (int line = fl; line <= tl; ++line) {
    wxString text = stc->GetLine(line);
    text.Replace(wxT("\r"), wxEmptyString);
    if (text.EndsWith(wxT("\n"))) {
      text.RemoveLast();
    }

    // raw
    raw << text << wxT("\n");

    // numbered
    out << wxString::Format(wxT("%d: %s\n"), line + 1, text);
  }

  if (outRawNoNumbers) {
    *outRawNoNumbers = raw;
  }
  return out;
}

static wxString GetNumberedContextAroundLine(wxStyledTextCtrl *stc, int centerLine, int radius, wxString *outRawNoNumbers = nullptr) {
  return GetNumberedContext(stc, centerLine - radius, centerLine + radius, outRawNoNumbers);
}

// ---
static wxString GetNumberedContext(const wxString &allText, int fromLine, int toLine, wxString *outRawNoNumbers = nullptr) {
  wxString out;
  wxString raw;

  // Count lines similarly to wxStyledTextCtrl (newline-delimited; trailing '\n' => last empty line exists)
  int lineCount = 1;
  for (size_t i = 0; i < allText.length(); ++i) {
    if (allText[i] == wxT('\n')) {
      ++lineCount;
    }
  }

  const int maxLine = lineCount - 1; // 0-based
  const int fl = std::max(0, fromLine);
  const int tl = std::min(maxLine, toLine);

  if (fl > tl) {
    if (outRawNoNumbers) {
      outRawNoNumbers->clear();
    }
    return out;
  }

  int line = 0;
  size_t start = 0;

  // Iterate over lines by scanning '\n'
  for (size_t i = 0; i <= allText.length(); ++i) {
    const bool atEnd = (i == allText.length());
    const bool isEol = (!atEnd && allText[i] == wxT('\n'));
    if (!atEnd && !isEol) {
      continue;
    }

    const size_t end = i; // excluding '\n'
    if (line >= fl && line <= tl) {
      wxString text = allText.Mid(start, end - start);

      // match STC variant behavior: remove all '\r' and strip trailing '\n' (there is no '\n' here)
      text.Replace(wxT("\r"), wxEmptyString);

      // raw
      raw << text << wxT("\n");

      // numbered
      out << wxString::Format(wxT("%d: %s\n"), line + 1, text);
    }

    if (line > tl) {
      break;
    }

    ++line;
    start = i + 1; // after '\n' (or length+1 at end, but loop ends anyway)
  }

  if (outRawNoNumbers) {
    *outRawNoNumbers = raw;
  }
  return out;
}

static wxString GetNumberedContextAroundLine(const wxString &allText, int centerLine, int radius, wxString *outRawNoNumbers = nullptr) {
  return GetNumberedContext(allText, centerLine - radius, centerLine + radius, outRawNoNumbers);
}

wxString GetMethodImplementationWithPadding(wxStyledTextCtrl *stc, const SymbolInfo &info, int paddingLines) {
  wxString resl;

  if ((info.bodyLineFrom < 1) || (info.bodyLineTo < 1)) {
    return resl;
  }

  int start = (info.bodyLineFrom - 1) - paddingLines - 1;
  if (start < 0) {
    start = 0;
  }

  int lineCount = stc->GetLineCount();

  int end = (info.bodyLineTo - 1) + paddingLines;
  if (end >= lineCount) {
    end = lineCount - 1;
  }

  int startPos = stc->PositionFromLine(start);
  int endPos = stc->GetLineEndPosition(end);

  return stc->GetTextRange(startPos, endPos);
}

static bool HasComment(wxStyledTextCtrl *stc, int declarationLine) {
  if (!stc || declarationLine <= 0) {
    return false;
  }

  const int lineCount = stc->GetLineCount();
  const int decl0 = declarationLine - 1; // 0-based

  if (decl0 < 0 || decl0 >= lineCount) {
    return false;
  }

  // 1) trailing Doxygen comment on the same line as the declaration (e.g. "///<")
  {
    wxString declLine = stc->GetLine(decl0);
    wxString trimmed = TrimCopy(declLine);
    if (trimmed.Contains(wxT("///<")) || trimmed.Contains(wxT("//!<"))) {
      return true;
    }
  }

  // 2) search upwards from the line above the declaration
  int line = decl0 - 1;
  int maxLookbackLines = 8;
  bool insideBlock = false;

  while (line >= 0 && maxLookbackLines-- > 0) {
    wxString raw = stc->GetLine(line);
    wxString trimmed = TrimCopy(raw);

    if (trimmed.IsEmpty()) {
      break;
    }

    // single-line Doxygen comments
    if (trimmed.StartsWith(wxT("///")) || trimmed.StartsWith(wxT("//!"))) {
      return true;
    }

    // block Doxygen comments
    if (!insideBlock) {
      // we are not "in the block" yet, we are looking for the end of the block
      if (trimmed.Contains(wxT("*/"))) {
        insideBlock = true;

        // short block on one line: "/** ... */"
        if (trimmed.Find(wxT("/**")) != wxNOT_FOUND ||
            trimmed.Find(wxT("/*!")) != wxNOT_FOUND) {
          return true;
        }

        --line;
        continue;
      }

      // the case where the "/** ..." block starts directly above the declaration
      if (trimmed.StartsWith(wxT("/**")) || trimmed.StartsWith(wxT("/*!"))) {
        return true;
      }

      // anything other than a comment (//, /*, * inside a block) will terminate the search
      if (!trimmed.StartsWith(wxT("//")) &&
          !trimmed.StartsWith(wxT("/*")) &&
          !trimmed.StartsWith(wxT("*"))) {
        break;
      }
    } else {
      // we are above the line with "*/" (still inside the block when viewed from the bottom up)
      if (trimmed.Find(wxT("/**")) != wxNOT_FOUND ||
          trimmed.Find(wxT("/*!")) != wxNOT_FOUND) {
        // we found the beginning of the Doxygen block
        return true;
      }

      // if we encounter a plain "/*" without the second asterisk/exclamation mark,
      // we treat it as a non-documentation block and end
      int idx = trimmed.Find(wxT("/*"));
      if (idx != wxNOT_FOUND) {
        break;
      }
    }

    --line;
  }

  return false;
}

static std::string CanonicalizeForChecksum(const std::string &s) {
  std::string out;
  out.reserve(s.size());

  // 1) remove CR
  for (char c : s) {
    if (c != '\r')
      out.push_back(c);
  }

  // 2) drop trailing LF(s)
  while (!out.empty() && out.back() == '\n') {
    out.pop_back();
  }

  return out;
}

static uint64_t ChecksumText(const std::string &s) {
  const std::string norm = CanonicalizeForChecksum(s);
  return Fnv1a64(reinterpret_cast<const uint8_t *>(norm.data()), norm.size());
}

// returns spaces or tabs at beginning of line
static wxString GetLineIndent(const wxString &line) {
  wxString indent;
  for (wxUniChar ch : line) {
    if (ch == ' ' || ch == '\t') {
      indent.Append(ch);
    } else {
      break;
    }
  }
  return indent;
}

static bool LooksLikeCodeFence(const wxString &input) {
  wxString t = input;
  t.Trim(true).Trim(false);
  return t.StartsWith(wxT("```")) || t.StartsWith(wxT("~~~"));
}

// wxString::Find() supports searching from the end only for single characters.
// For substrings we implement a tiny rfind using wxString::find().
static int RFindSubstr(const wxString &text, const wxString &sub) {
  if (sub.IsEmpty())
    return wxNOT_FOUND;

  size_t last = wxString::npos;
  size_t pos = text.find(sub);
  while (pos != wxString::npos) {
    last = pos;
    pos = text.find(sub, pos + 1);
  }

  return (last == wxString::npos) ? wxNOT_FOUND : (int)last;
}

// strips chat code fence  e.q.: ```cpp ... ```  (also supports ~~~)
static wxString StripCodeFence(const wxString &input) {
  wxString text = input;
  text.Trim(true).Trim(false);

  wxString fence;
  if (text.StartsWith(wxT("```"))) {
    fence = wxT("```");
  } else if (text.StartsWith(wxT("~~~"))) {
    fence = wxT("~~~");
  } else {
    return text;
  }

  // drop the opening fence line (```cpp, ~~~cpp, etc.)
  int firstNewline = text.Find(wxT("\n"));
  if (firstNewline != wxNOT_FOUND) {
    text = text.Mid(firstNewline + 1);
  } else {
    // fence only, no content
    return wxString();
  }

  // Prefer a fence that starts on a new line, from the end.
  int lastFenceNl = RFindSubstr(text, wxT("\n") + fence);
  if (lastFenceNl != wxNOT_FOUND) {
    text = text.Left(lastFenceNl);
  } else {
    // fallback: last occurrence of fence anywhere
    int lastFence = RFindSubstr(text, fence);
    if (lastFence != wxNOT_FOUND) {
      text = text.Left(lastFence);
    }
  }

  text.Trim(true).Trim(false);
  return text;
}

// reformats plain text to valid Doxygen comment e.q.: /** ... */
static wxString NormalizeDocComment(const wxString &raw) {
  wxString doc = raw;
  doc.Trim(true).Trim(false);
  if (doc.IsEmpty()) {
    return wxString();
  }

  if (doc.StartsWith(wxT("/**"))) {
    if (!doc.Contains(wxT("*/"))) {
      doc.Append(wxT("\n */"));
    }
    return doc;
  }

  wxArrayString lines = wxSplit(doc, '\n', '\0');
  wxString result;
  result << wxT("/**\n");
  for (auto &line : lines) {
    wxString t = line;
    t.Trim(true).Trim(false);
    if (!t.IsEmpty()) {
      result << wxT(" * ") << t << wxT("\n");
    } else {
      result << wxT(" *\n");
    }
  }
  result << wxT(" */");
  return result;
}

// adds indent to any line in text
static wxString IndentMultiline(const wxString &text, const wxString &indent) {
  wxArrayString lines = wxSplit(text, '\n', '\0');
  wxString result;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      result << wxT("\n");
    }
    if (!lines[i].IsEmpty()) {
      result << indent << lines[i];
    } else {
      result << indent;
    }
  }
  return result;
}

static bool ParseRangeLine(const wxString &line, int &from, int &to) {
  // expect "RANGE: a-b"
  wxString tmp = line;
  tmp.Trim(true).Trim(false);
  if (!tmp.StartsWith(wxT("RANGE:"))) {
    return false;
  }
  tmp = tmp.Mid(wxString(wxT("RANGE:")).length());
  tmp.Trim(true).Trim(false);

  int dashPos = tmp.Find('-');
  if (dashPos == wxNOT_FOUND) {
    return false;
  }

  wxString left = tmp.SubString(0, dashPos - 1);
  wxString right = tmp.SubString(dashPos + 1, tmp.length() - 1);
  left.Trim(true).Trim(false);
  right.Trim(true).Trim(false);

  long a = 0;
  long b = 0;
  if (!left.ToLong(&a) || !right.ToLong(&b)) {
    return false;
  }

  // only base check
  if (a < 0 || b < a) {
    return false;
  }

  from = static_cast<int>(a);
  to = static_cast<int>(b);
  return true;
}

static wxString UtcToLocal(const std::string &createdUtc) {
  wxString iso = wxString::FromUTF8(createdUtc);
  iso.Trim(true).Trim(false);

  if (iso.EndsWith(wxT("Z"))) {
    iso.RemoveLast(); // strip UTC designator
  }

  wxDateTime dt;
  if (!dt.ParseISOCombined(iso) || !dt.IsValid()) {
    return wxString::FromUTF8(createdUtc);
  }

  // IMPORTANT: The parsed value is treated as "local" by wxDateTime.
  // We want to interpret it as UTC without shifting the clock value.
  // So: rebuild dt from the same Y/M/D/H/M/S but tagged as UTC.
  wxDateTime utc(
      dt.GetDay(),
      dt.GetMonth(),
      dt.GetYear(),
      dt.GetHour(),
      dt.GetMinute(),
      dt.GetSecond(),
      dt.GetMillisecond());

  // Now convert UTC -> local for display
  wxDateTime local = utc.ToTimezone(wxDateTime::Local);

  return local.FormatDate() + wxT(" ") + local.FormatTime();
}

wxString AiChatSessionInfo::GetCreatedDateByLocale() const {
  return UtcToLocal(createdUtc);
}

wxString AiChatSessionInfo::GetTitle() const {
  wxString t = wxString::FromUTF8(title);
  t.Trim(true).Trim(false);
  if (!t.IsEmpty()) {
    return t;
  }
  return GetCreatedDateByLocale();
}

wxString AiChatUiItem::GetCreatedDateByLocale() const {
  if (tsUtc.empty()) {
    return wxEmptyString;
  }

  return UtcToLocal(tsUtc);
}

wxString AiChatUiItem::GetTokenInfo() const {
  wxString tokens;
  if ((inputTokens > -1) && (outputTokens > -1) && (totalTokens > -1)) {
    tokens = wxString::Format(_("%s (in: %d, out: %d, tot: %d) tokens"), wxString::FromUTF8(model), inputTokens, outputTokens, totalTokens);
  }
  return tokens;
}

// -------------------------------------------------------------------------------
ArduinoAiActions::ArduinoAiActions(ArduinoEditor *editor)
    : m_currentAction(Action::None), m_editor(editor), m_docCommentTargetLine(-1) {

  // events
  Bind(wxEVT_AI_SIMPLE_CHAT_SUCCESS, &ArduinoAiActions::OnAiSimpleChatSuccess, this);
  Bind(wxEVT_AI_SIMPLE_CHAT_ERROR, &ArduinoAiActions::OnAiSimpleChatError, this);
  Bind(wxEVT_AI_SUMMARIZATION_UPDATED, &ArduinoAiActions::OnAiSummarizationUpdated, this);
}

ArduinoAiActions::~ArduinoAiActions() {
  if (m_client) {
    delete m_client;
    m_client = nullptr;
  }
}

AiClient *ArduinoAiActions::GetClient() {
  if (m_client) {
    return m_client;
  }

  if (m_editor) {
    return (m_client = new AiClient(m_editor->m_aiSettings));
  }

  return nullptr;
}

void ArduinoAiActions::SetCurrentEditor(ArduinoEditor *editor) {
  m_editor = editor;
}

AiSettings ArduinoAiActions::GetSettings() const {
  return m_editor->m_aiSettings;
}

void ArduinoAiActions::RebuildProject() {
  if (m_editor) {
    m_editor->GetOwnerFrame()->RebuildProject(/*withClean=*/true);
  }
}

bool ArduinoAiActions::StartAction(Action action) {
  if (action == m_currentAction) {
    return true;
  }

  if (m_currentAction != Action::None) {
    m_editor->ModalMsgDialog(_("Another AI action is running."),
                             _("Arduino Editor AI"), wxOK | wxICON_INFORMATION);
    return false;
  }

  if (!m_editor || !m_editor->m_aiSettings.enabled) {
    m_editor->ModalMsgDialog(
        _("AI is disabled. Enable it in Settings -> AI."),
        _("Arduino Editor AI"),
        wxOK | wxICON_INFORMATION);
    return false;
  }

  m_currentAction = action;

  // inform indicator at main frame
  m_editor->GetOwnerFrame()->StartProcess(wxT("AI model thinking..."), ID_PROCESS_AI_OPERATION, ArduinoActivityState::Background);
  return true;
}

void ArduinoAiActions::StopCurrentAction() {
  m_editor->GetOwnerFrame()->StopProcess(ID_PROCESS_AI_OPERATION);
  m_currentAction = Action::None;
}

// AI selection explain
void ArduinoAiActions::ExplainSelection() {
  auto *client = GetClient();
  if (!client) {
    return;
  }

  if (!StartAction(Action::ExplainSelection)) {
    return;
  }

  wxStyledTextCtrl *stc = GetStc();

  int selStart = stc->GetSelectionStart();
  int selEnd = stc->GetSelectionEnd();
  if (selStart == selEnd) {
    m_editor->ModalMsgDialog(
        _("Please select some code you want to explain."),
        _("AI explain selection"),
        wxOK | wxICON_INFORMATION);
    StopCurrentAction();
    return;
  }

  wxString selected = stc->GetTextRange(selStart, selEnd);
  wxString trimmed = selected;
  trimmed.Trim(true).Trim(false);
  if (trimmed.IsEmpty()) {
    m_editor->ModalMsgDialog(
        _("The selected text is empty or whitespace only."),
        _("AI explain selection"),
        wxOK | wxICON_INFORMATION);
    StopCurrentAction();
    return;
  }

  wxString systemPrompt =
      _("You are an expert Arduino/C++ assistant integrated into an IDE. "
        "Explain the selected code snippet in a clear and concise way. "
        "Focus on what it does, important details, side-effects and possible pitfalls. "
        "Answer in English.");

  wxString userPrompt;
  userPrompt << _("Please explain the following C++/Arduino code snippet:\n\n```cpp\n");
  userPrompt << selected;
  userPrompt << _("\n```");

  if (!client->SimpleChatAsync(systemPrompt, userPrompt, this)) {
    StopCurrentAction();

    m_editor->ModalMsgDialog(
        _("Failed to start AI request."),
        _("AI explain selection"),
        wxOK | wxICON_ERROR);
  }
}

bool ArduinoAiActions::StartDocCommentForSymbol(const SymbolInfo &info) {
  wxStyledTextCtrl *stc = GetStc();
  if (!stc) {
    return false;
  }

  auto *client = GetClient();
  if (!client) {
    return false;
  }

  // 1-based
  int line = info.line;
  if (line <= 0) {
    return false;
  }

  bool isMethod = (info.kind == CXCursor_CXXMethod ||
                   info.kind == CXCursor_Constructor ||
                   info.kind == CXCursor_Destructor ||
                   info.kind == CXCursor_FunctionDecl ||
                   info.kind == CXCursor_FunctionTemplate ||
                   info.kind == CXCursor_ClassDecl ||
                   info.kind == CXCursor_StructDecl ||
                   info.kind == CXCursor_ClassTemplate ||
                   info.kind == CXCursor_EnumDecl);

  bool isVariable = (info.kind == CXCursor_VarDecl);

  wxString symbolInfo = wxString::FromUTF8(info.display);

  wxString codeContext;
  if (isVariable) {
    codeContext = GetContextAroundLine(stc, line - 1 /*0 based*/, 10);
  } else if (isMethod) {
    codeContext = GetMethodImplementationWithPadding(stc, info, 5);
  } else {
    return false;
  }

  if (codeContext.IsEmpty()) {
    return false;
  }

  wxString systemPrompt =
      _("You are an expert C++/Arduino assistant integrated into an IDE.\n"
        "You will receive:\n"
        "1) A single C++ declaration of the symbol that should be documented.\n"
        "2) Additional surrounding code for context.\n"
        "You MUST generate a concise Doxygen-style documentation comment for that symbol only.\n"
        "Rules:\n"
        "- Output ONLY a C++ documentation comment block starting with \"/**\" and ending with \"*/\".\n"
        "- Do not include the declaration or definition itself.\n"
        "- Use clear English sentences.\n"
        "- For functions/methods, include @param and @return tags where appropriate.\n"
        "- Do not add any text outside the comment block.");

  wxString userPrompt;
  userPrompt << wxString::Format(
      _("Generate a documentation comment for the symbol %s in the following C++/Arduino code:\n\n```cpp\n"),
      symbolInfo);
  userPrompt << codeContext;
  userPrompt << _("\n```");

  APP_DEBUG_LOG("AI: userPrompt:\n%s", wxToStd(userPrompt).c_str());

  m_docCommentTargetLine = line;

  if (!client->SimpleChatAsync(systemPrompt, userPrompt, this)) {
    m_docCommentTargetLine = -1;
    return false;
  }

  return true;
}

void ArduinoAiActions::GenerateDocCommentForSymbol() {
  auto *completion = m_editor->completion;
  if (!completion || !completion->IsTranslationUnitValid()) {
    return;
  }

  if (!StartAction(Action::GenerateDocComment)) {
    return;
  }

  int line = 0;
  int column = 0;
  m_editor->GetCurrentCursor(line, column); // 1-based

  if (HasComment(GetStc(), line)) {
    StopCurrentAction();

    m_editor->ModalMsgDialog(_("This symbol is already documented."), _("AI generate documentation"), wxOK | wxICON_INFORMATION);
    return;
  }

  std::string code = wxToStd(m_editor->m_editor->GetText());

  SymbolInfo info;
  if (!completion->GetSymbolInfo(m_editor->m_filename, code, line, column, info)) {
    m_editor->ModalMsgDialog(_("No symbol under the cursor."),
                             _("AI generate documentation"),
                             wxOK | wxICON_INFORMATION);
    StopCurrentAction();
    return;
  }

  if (!StartDocCommentForSymbol(info)) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("No suitable type or context for documentation at the cursor."),
        _("AI generate documentation"),
        wxOK | wxICON_INFORMATION);
  }
}

void ArduinoAiActions::InsertAiGeneratedDocComment(const wxString &reply) {
  if (reply.IsEmpty()) {
    m_docCommentTargetLine = -1;
    return;
  }

  wxStyledTextCtrl *stc = m_editor->m_editor;
  if (!stc) {
    m_docCommentTargetLine = -1;
    return;
  }

  wxString strippedReply = StripCodeFence(reply);

  wxString doc = NormalizeDocComment(strippedReply);
  if (doc.IsEmpty()) {
    m_docCommentTargetLine = -1;
    return;
  }

  int lineCount = stc->GetLineCount();

  int targetLine1 = m_docCommentTargetLine;
  m_docCommentTargetLine = -1;

  if (targetLine1 < 1 || targetLine1 > lineCount) {
    // fallback - current cursor
    int curLine = 0;
    int curCol = 0;
    m_editor->GetCurrentCursor(curLine, curCol); // 1-based
    if (curLine < 1)
      curLine = 1;
    if (curLine > lineCount)
      curLine = lineCount;
    targetLine1 = curLine;
  }

  // STC using 0-based index
  int targetLine0 = targetLine1 - 1;

  wxString declLine = stc->GetLine(targetLine0);
  wxString indent = GetLineIndent(declLine);

  wxString indentedDoc = IndentMultiline(doc, indent);
  indentedDoc << wxT("\n");

  int insertPos = stc->PositionFromLine(targetLine0);

  stc->BeginUndoAction();
  stc->InsertText(insertPos, indentedDoc);
  stc->EndUndoAction();
}

void ArduinoAiActions::FinalizeBatchDocComments(Action WXUNUSED(action)) {
  if (m_pendingDocComments.empty()) {
    m_editor->ModalMsgDialog(
        _("No documentation comments to insert."),
        _("AI generate documentation"),
        wxOK | wxICON_INFORMATION);
    m_docBatchSymbols.clear();
    m_docBatchIndex = 0;
    return;
  }

  wxStyledTextCtrl *stc = GetStc();
  if (!stc) {
    m_pendingDocComments.clear();
    m_docBatchSymbols.clear();
    m_docBatchIndex = 0;
    return;
  }

  // --- Try search for original symbols ---
  std::vector<SymbolInfo> currentSyms;
  ArduinoCodeCompletion *completion = m_editor->completion;
  if (completion && completion->IsTranslationUnitValid()) {
    std::string code = wxToStd(m_editor->m_editor->GetText());
    currentSyms = completion->GetAllSymbols(m_editor->GetFilePath(), code);
  }

  struct Insertion {
    int line;       // 1-based
    wxString reply; // plain AI answer
  };
  std::vector<Insertion> insertions;

  for (const auto &pd : m_pendingDocComments) {
    int targetLine = pd.symbol.line; // fallback

    // search match in currentSyms
    const SymbolInfo *best = nullptr;
    int bestScore = INT_MAX;

    for (const auto &s : currentSyms) {
      // same name
      if (!pd.symbol.name.empty() && pd.symbol.name != s.name) {
        continue;
      }
      // same kind
      if (pd.symbol.kind != s.kind) {
        continue;
      }
      // if we have file
      if (!pd.symbol.file.empty() && !s.file.empty() && pd.symbol.file != s.file) {
        continue;
      }

      int score = std::abs((int)s.line - (int)pd.symbol.line);
      if (score < bestScore) {
        bestScore = score;
        best = &s;
      }
    }

    if (best) {
      targetLine = best->line;
    }

    if (targetLine <= 0) {
      continue;
    }

    Insertion ins;
    ins.line = targetLine;
    ins.reply = pd.reply;
    insertions.push_back(std::move(ins));
  }

  if (insertions.empty()) {
    size_t total = m_pendingDocComments.size();
    m_pendingDocComments.clear();
    m_docBatchSymbols.clear();
    m_docBatchIndex = 0;

    wxString msg;
    msg.Printf(_("Documentation was generated, but no suitable insertion points were found (%zu symbol(s))."),
               total);
    m_editor->ModalMsgDialog(
        msg,
        _("AI generate documentation"),
        wxOK | wxICON_WARNING);
    return;
  }

  // sort descending
  std::sort(insertions.begin(), insertions.end(),
            [](const Insertion &a, const Insertion &b) {
              return a.line > b.line;
            });

  int lineCount = stc->GetLineCount();

  stc->BeginUndoAction();

  for (const auto &ins : insertions) {
    if (ins.line < 1 || ins.line > lineCount) {
      continue;
    }

    wxString strippedReply = StripCodeFence(ins.reply);
    wxString doc = NormalizeDocComment(strippedReply);
    if (doc.IsEmpty()) {
      continue;
    }

    int targetLine1 = ins.line;
    if (targetLine1 < 1)
      targetLine1 = 1;
    if (targetLine1 > lineCount)
      targetLine1 = lineCount;

    int targetLine0 = targetLine1 - 1;

    wxString declLine = stc->GetLine(targetLine0);
    wxString indent = GetLineIndent(declLine);

    wxString indentedDoc = IndentMultiline(doc, indent);
    indentedDoc << wxT("\n");

    int insertPos = stc->PositionFromLine(targetLine0);
    stc->InsertText(insertPos, indentedDoc);
  }

  stc->EndUndoAction();

  size_t total = m_pendingDocComments.size();

  m_pendingDocComments.clear();
  m_docBatchSymbols.clear();
  m_docBatchIndex = 0;

  wxString msg;
  msg.Printf(_("Documentation generated for %zu symbol(s)."), total);
  m_editor->ModalMsgDialog(
      msg,
      _("AI generate documentation"),
      wxOK | wxICON_INFORMATION);
}

void ArduinoAiActions::GenerateDocCommentsForCurrentFile() {
  auto *completion = m_editor->completion;
  if (!completion || !completion->IsTranslationUnitValid()) {
    return;
  }

  if (!StartAction(Action::GenerateDocCommentsInFile)) {
    return;
  }

  std::string code = wxToStd(m_editor->m_editor->GetText());

  std::vector<SymbolInfo> allSyms =
      completion->GetAllSymbols(m_editor->GetFilePath(), code);

  wxStyledTextCtrl *stc = GetStc();
  if (!stc) {
    StopCurrentAction();
    return;
  }

  m_docBatchSymbols.clear();
  m_docBatchIndex = 0;

  for (const auto &s : allSyms) {
    bool isFuncOrMethod =
        (s.kind == CXCursor_CXXMethod ||
         s.kind == CXCursor_Constructor ||
         s.kind == CXCursor_Destructor ||
         s.kind == CXCursor_FunctionDecl ||
         s.kind == CXCursor_FunctionTemplate);

    if (!isFuncOrMethod) {
      continue;
    }

    // only syms with body
    if (s.bodyLineFrom <= 0 || s.bodyLineTo <= 0) {
      continue;
    }

    // check current file
    if (s.file != m_editor->GetFilePath()) {
      continue;
    }

    if (HasComment(stc, s.line)) {
      continue;
    }

    m_docBatchSymbols.push_back(s);
  }

  if (m_docBatchSymbols.empty()) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("No functions or methods found that need documentation."),
        _("AI generate documentation"),
        wxOK | wxICON_INFORMATION);
    return;
  }

  // Run first symbol in batch
  if (!StartDocCommentForSymbol(m_docBatchSymbols[0])) {
    StopCurrentAction();
    m_docBatchSymbols.clear();
    m_editor->ModalMsgDialog(
        _("Failed to start AI request."),
        _("AI generate documentation"),
        wxOK | wxICON_ERROR);
    return;
  }

  m_docBatchIndex = 0;
}

void ArduinoAiActions::GenerateDocCommentsForCurrentClass() {
  auto *completion = m_editor->completion;
  if (!completion || !completion->IsTranslationUnitValid()) {
    return;
  }

  if (!StartAction(Action::GenerateDocCommentsInClass)) {
    return;
  }

  int line = 0, column = 0;
  m_editor->GetCurrentCursor(line, column);

  std::string code = wxToStd(m_editor->m_editor->GetText());

  SymbolInfo classInfo;
  if (!completion->GetSymbolInfo(m_editor->m_filename, code, line, column, classInfo)) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("No symbol under the cursor."),
        _("AI generate documentation"),
        wxOK | wxICON_INFORMATION);
    return;
  }

  if (!(classInfo.kind == CXCursor_ClassDecl ||
        classInfo.kind == CXCursor_StructDecl ||
        classInfo.kind == CXCursor_ClassTemplate)) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("Cursor is not on a class or struct declaration."),
        _("AI generate documentation"),
        wxOK | wxICON_INFORMATION);
    return;
  }

  if (classInfo.bodyLineFrom <= 0 || classInfo.bodyLineTo <= 0) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("Class body range is not available."),
        _("AI generate documentation"),
        wxOK | wxICON_INFORMATION);
    return;
  }

  std::vector<SymbolInfo> allSyms =
      completion->GetAllSymbols(m_editor->GetFilePath(), code);

  wxStyledTextCtrl *stc = GetStc();
  if (!stc) {
    StopCurrentAction();
    return;
  }

  m_docBatchSymbols.clear();
  m_docBatchIndex = 0;

  for (const auto &s : allSyms) {
    bool isMethod =
        (s.kind == CXCursor_CXXMethod ||
         s.kind == CXCursor_Constructor ||
         s.kind == CXCursor_Destructor);

    if (!isMethod) {
      continue;
    }

    if (s.line <= 0) {
      continue;
    }

    // symbol must have body
    if (s.line < classInfo.bodyLineFrom || s.line > classInfo.bodyLineTo) {
      continue;
    }

    if (s.bodyLineFrom <= 0 || s.bodyLineTo <= 0) {
      continue;
    }

    if (s.file != m_editor->GetFilePath()) {
      continue;
    }

    if (HasComment(stc, s.line)) {
      continue;
    }

    m_docBatchSymbols.push_back(s);
  }

  if (m_docBatchSymbols.empty()) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("No methods found in the current class that need documentation."),
        _("AI generate documentation"),
        wxOK | wxICON_INFORMATION);
    return;
  }

  if (!StartDocCommentForSymbol(m_docBatchSymbols[0])) {
    StopCurrentAction();
    m_docBatchSymbols.clear();
    m_editor->ModalMsgDialog(
        _("Failed to start AI request."),
        _("AI generate documentation"),
        wxOK | wxICON_ERROR);
    return;
  }

  m_docBatchIndex = 0;
}

void ArduinoAiActions::OptimizeFunctionOrMethod() {
  auto *completion = m_editor->completion;
  if (!completion || !completion->IsTranslationUnitValid()) {
    return;
  }

  auto *client = GetClient();
  if (!client) {
    return;
  }

  if (!StartAction(Action::OptimizeFunctionOrMethod)) {
    return;
  }

  wxStyledTextCtrl *stc = GetStc();
  if (!stc) {
    StopCurrentAction();
    return;
  }

  int line = 0;
  int column = 0;
  m_editor->GetCurrentCursor(line, column); // 1-based

  std::string code = wxToStd(m_editor->m_editor->GetText());

  SymbolInfo info;
  if (!completion->GetSymbolInfo(m_editor->GetFilePath(), code, line, column, info)) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("No symbol under the cursor."),
        _("AI optimize function or method"),
        wxOK | wxICON_INFORMATION);
    return;
  }

  bool isFuncOrMethod =
      (info.kind == CXCursor_CXXMethod ||
       info.kind == CXCursor_Constructor ||
       info.kind == CXCursor_Destructor ||
       info.kind == CXCursor_FunctionDecl ||
       info.kind == CXCursor_FunctionTemplate);

  if (!isFuncOrMethod) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("Cursor is not on a function or method declaration."),
        _("AI optimize function or method"),
        wxOK | wxICON_INFORMATION);
    return;
  }

  if (info.bodyLineFrom <= 0 || info.bodyLineTo <= 0) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("Function or method body range is not available."),
        _("AI optimize function or method"),
        wxOK | wxICON_INFORMATION);
    return;
  }

  // Build numbered context around the body (a few lines of padding around it).
  const int lineCount = stc->GetLineCount();
  int fromLine = std::max(1, info.bodyLineFrom - 5);
  int toLine = std::min(lineCount, info.bodyLineTo + 5);

  wxString numberedContext, raw;
  for (int l = fromLine; l <= toLine; ++l) {
    wxString lineText = stc->GetLine(l - 1);
    lineText.Replace(wxT("\r"), wxEmptyString);
    if (lineText.EndsWith(wxT("\n"))) {
      lineText.RemoveLast();
    }
    raw << lineText << wxT("\n");
    numberedContext << wxString::Format(wxT("%d: %s\n"), l, lineText);
  }

  wxString symbolInfo = FormatSymbolInfoForAi(info);

  wxString basename = wxString::FromUTF8(m_editor->GetFileName()); // relative

  wxString userPrompt;
  userPrompt << wxT("CURRENT_FILE: ") << basename << wxT("\n");
  userPrompt << wxT("CURRENT_BOARD_FQBN: ") << wxString::FromUTF8(m_editor->arduinoCli->GetFQBN()) << wxT("\n\n");
  userPrompt << wxT("TARGET_SYMBOL_INFO:\n");
  userPrompt << symbolInfo << wxT("\n");
  userPrompt << wxString::Format(wxT("BODY_LINES: %d-%d\n\n"),
                                 info.bodyLineFrom, info.bodyLineTo);
  userPrompt << wxT("CODE_CONTEXT (with 1-based line numbers):\n");
  userPrompt << wxT("```cpp\n");
  userPrompt << numberedContext;
  userPrompt << wxT("```\n");

  APP_DEBUG_LOG("AI OptimizeFunctionOrMethod: userPrompt:\n%s",
                wxToStd(userPrompt).c_str());

  // Prepare solve session
  m_solveSession = SolveSession{};
  m_solveSession.action = Action::OptimizeFunctionOrMethod;
  m_solveSession.basename = basename;
  m_solveSession.iteration = 0;
  m_solveSession.maxIterations = m_editor->m_aiSettings.maxIterations;
  m_solveSession.finished = false;
  m_solveSession.transcript = userPrompt;
  m_solveSession.bodyFromLine = info.bodyLineFrom;
  m_solveSession.bodyToLine = info.bodyLineTo;
  m_solveSession.seen.AddSeen(wxString::FromUTF8(m_editor->GetFileName()), fromLine, toLine, ChecksumText(wxToStd(stc->GetText())));
  m_editor->GetOwnerFrame()->CollectEditorSources(m_solveSession.workingFiles);

  if (!client->SimpleChatAsync(optimizeFunctionSystemPrompt, userPrompt, this)) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("Failed to start AI request."),
        _("AI optimize function or method"),
        wxOK | wxICON_ERROR);
    return;
  }
}

// ======
void ArduinoAiActions::SolveProjectError(const ArduinoParseError &compilerError) {
  auto *client = GetClient();
  if (!client) {
    return;
  }

  if (!StartAction(Action::SolveProjectError)) {
    return;
  }

  // copy
  ArduinoParseError error = compilerError;

  // It is theoretically possible that the error will be handled in a different ArduinoEditor.
  // So here we make sure that the correct editor is selected and the model is given the correct context.

  // We move to error location
  JumpTarget errorLocation;
  errorLocation.file = error.file;
  errorLocation.line = error.line;
  errorLocation.column = error.column;
  // Ensure editor selected
  m_editor->GetOwnerFrame()->HandleGoToLocation(errorLocation);

  ArduinoEditor *errorEditor = m_editor->GetOwnerFrame()->GetCurrentEditor();
  wxStyledTextCtrl *stc = errorEditor->m_editor;

  // ArduinoParseError::ToString contains absolute path so strip it
  std::string basename = errorEditor->GetFileName(); // relative
  error.file = basename;
  wxString wxBasename = wxString::FromUTF8(basename);

  // Error context
  const int radius = 15;
  wxString raw;
  int err0 = (error.line > 0) ? (error.line - 1) : 0;
  wxString numberedContext = GetNumberedContextAroundLine(stc, err0, radius, &raw);

  wxString userPrompt;
  userPrompt << wxT("CURRENT_FILE: ") << wxBasename << wxT("\n\n");
  userPrompt << wxT("CURRENT_BOARD_FQBN: ") << wxString::FromUTF8(m_editor->arduinoCli->GetFQBN()) << wxT("\n");
  userPrompt << wxT("COMPILER_ERROR(S):\n");
  userPrompt << wxString::FromUTF8(error.ToString()) << wxT("\n\n");
  userPrompt << wxString::Format(wxT("CURSOR_LINE: %d\n\n"), error.line);
  userPrompt << wxT("CODE_CONTEXT (with 1-based line numbers):\n");
  userPrompt << wxT("```cpp\n");
  userPrompt << numberedContext;
  userPrompt << wxT("```\n");

  m_solveSession = SolveSession{};
  m_solveSession.action = Action::SolveProjectError;
  m_solveSession.compilerError = error;
  m_solveSession.basename = wxBasename;
  m_solveSession.iteration = 0;
  m_solveSession.maxIterations = m_editor->m_aiSettings.maxIterations;
  m_solveSession.finished = false;
  m_solveSession.transcript = userPrompt;
  m_editor->GetOwnerFrame()->CollectEditorSources(m_solveSession.workingFiles);

  if (!raw.IsEmpty()) {
    m_solveSession.seen.AddSeen(wxBasename, std::max(1, (int)error.line - radius), std::min(stc->GetLineCount(), (int)error.line + radius), ChecksumText(wxToStd(stc->GetText())));
  }

  if (!client->SimpleChatAsync(solveErrorSystemPrompt, userPrompt, this)) {
    StopCurrentAction();

    m_editor->ModalMsgDialog(
        _("Failed to start AI request."),
        _("AI solve project errors"),
        wxOK | wxICON_ERROR);
  }
}

wxString ArduinoAiActions::GenerateSessionTitleIfRequested(const wxString &userText) {
  if (!m_editor)
    return wxEmptyString;

  const AiSettings &st = m_editor->m_aiSettings;

  // Only when history is enabled (title lives in index.json)
  if (!st.storeChatHistory)
    return wxEmptyString;

  // Mode
  if (st.summarizeChatSessionMode == noSumarization)
    return wxEmptyString;

  // ---- Heuristic title (always available) ----
  wxString t = userText;
  t.Replace(wxT("\r"), wxEmptyString);
  t.Trim(true).Trim(false);

  // take first non-empty line
  wxArrayString lines = wxSplit(t, '\n', '\0');
  wxString first;
  for (auto &l : lines) {
    wxString x = l;
    x.Trim(true).Trim(false);
    if (!x.IsEmpty()) {
      first = x;
      break;
    }
  }
  if (first.IsEmpty()) {
    first = t;
  }

  // collapse whitespace
  auto collapseWs = [](wxString s) {
    wxString out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (wxUniChar ch : s) {
      bool isSpace = (ch == ' ' || ch == '\t' || ch == '\n');
      if (isSpace) {
        if (!prevSpace)
          out << wxT(' ');
        prevSpace = true;
      } else {
        out << ch;
        prevSpace = false;
      }
    }
    out.Trim(true).Trim(false);
    return out;
  };

  first = collapseWs(first);

  // strip leading "please/hi" fluff a bit
  wxString low = first.Lower();
  if (low.StartsWith(_("please ")))
    first = first.Mid(7);
  if (low.StartsWith(_("pls ")))
    first = first.Mid(4);

  // limit length
  const int kMax = 64;
  if ((int)first.size() > kMax) {
    first = first.Mid(0, kMax - 1);
    first.Trim(true).Trim(false);
    first << wxT("…");
  }

  wxString heuristicTitle = first;
  heuristicTitle.Trim(true).Trim(false);

  // ---- AI summarization (async) ----
  if (st.summarizeChatSessionMode != aiModelSumarization) {
    return heuristicTitle;
  }

  // Need an open session to attach the result to
  if (!m_chatSessionOpen || m_chatSessionId.empty()) {
    return heuristicTitle;
  }

  // Fire-and-forget background request; we keep heuristic title now.
  const std::string sessionId = m_chatSessionId;
  const std::string sketchRoot = GetSketchRoot();

  // Copy text to thread
  wxString textForAi = userText;
  textForAi.Trim(true).Trim(false);
  if (textForAi.size() > 1200) {
    textForAi = textForAi.Mid(0, 1200) + wxT("...");
  }

  std::thread([this, st, sketchRoot, sessionId, textForAi]() mutable {
    // Build a short prompt
    wxString systemPrompt = wxT(
        "You are an assistant embedded in an IDE. "
        "Generate a short, descriptive chat session title based ONLY on the user's first message. "
        "Rules: output ONLY the title, no quotes, no punctuation at the end, max 60 characters.");

    wxString userPrompt;
    userPrompt << wxT("USER_FIRST_MESSAGE:\n") << textForAi;

    wxString title;

    try {
      // New client with thread lifetime
      AiClient client(st);

      wxString resp;
      if (client.SimpleChat(systemPrompt, userPrompt, resp)) {
        title = resp;
      }
    } catch (...) {
      // ignore
    }

    title.Replace(wxT("\r"), wxEmptyString);
    title.Replace(wxT("\n"), wxT(" "));
    title.Trim(true).Trim(false);

    if (title.IsEmpty())
      return;

    // hard cap
    if ((int)title.size() > 60) {
      title = title.Mid(0, 59);
      title.Trim(true).Trim(false);
      title << wxT("...");
    }

    wxThreadEvent evt(wxEVT_AI_SUMMARIZATION_UPDATED);
    evt.SetString(wxString::FromUTF8(sessionId) + wxT("\n") + title);
    wxPostEvent(this, evt);
  }).detach();

  return heuristicTitle;
}

void ArduinoAiActions::OnAiSummarizationUpdated(wxThreadEvent &event) {
  if (!m_editor || !m_editor->m_aiSettings.storeChatHistory)
    return;

  wxString payload = event.GetString();
  payload.Trim(true).Trim(false);
  if (payload.IsEmpty())
    return;

  wxString sessionLine = payload.BeforeFirst('\n');
  wxString titleLine = payload.AfterFirst('\n');

  sessionLine.Trim(true).Trim(false);
  titleLine.Trim(true).Trim(false);

  if (sessionLine.IsEmpty() || titleLine.IsEmpty())
    return;

  UpdateSessionTitleInIndex(GetSketchRoot(), wxToStd(sessionLine), wxToStd(titleLine));

  // Notify origin (panel) to refresh / update choice label
  if (m_origin) {
    wxThreadEvent evt(wxEVT_AI_SUMMARIZATION_UPDATED);
    // Keep it simple: "sessionId\ntitle"
    evt.SetString(sessionLine + wxT("\n") + titleLine);
    wxPostEvent(m_origin, evt);
  }
}

bool ArduinoAiActions::StartInteractiveChat(const wxString &userText, wxEvtHandler *origin) {
  m_origin = origin;

  auto *client = GetClient();
  if (!client) {
    return false;
  }

  if (!StartAction(Action::InteractiveChat)) {
    return false;
  }

  wxStyledTextCtrl *stc = GetStc();
  if (!stc) {
    StopCurrentAction();
    return false;
  }

  m_interactiveChatPayload.Clear();

  // current fn
  wxString basename = wxString::FromUTF8(m_editor->GetFileName()); // relative

  // cursor (1-based)
  int line = 0;
  int column = 0;
  m_editor->GetCurrentCursor(line, column);

  // current editor context
  wxString fileContext;
  wxString raw;

  // if there is a selection, it will be context
  int selStart = stc->GetSelectionStart();
  int selEnd = stc->GetSelectionEnd();

  int seenIntervalFrom, seenIntervalTo;

  if (selStart != selEnd) {
    int firstSelLine = stc->LineFromPosition(selStart);
    int lastPos = std::max(selStart, selEnd - 1);
    int lastSelLine = stc->LineFromPosition(lastPos);

    fileContext = GetNumberedContext(stc, firstSelLine, lastSelLine, &raw);

    seenIntervalFrom = firstSelLine + 1;
    seenIntervalTo = lastSelLine + 1;
  } else {
    // otherwise context around cursor is used
    if (line > 0) {
      int cursor0 = (line > 0) ? (line - 1) : 0;
      int numLines = 200;
      fileContext = GetNumberedContextAroundLine(stc, cursor0, numLines, &raw);
      seenIntervalFrom = std::max(1, line - numLines);
      seenIntervalTo = std::min(stc->GetLineCount(), line + numLines);
    } else {
      seenIntervalFrom = seenIntervalTo = -1; // not used
    }
  }

  wxString ephemeralPrompt;

  ephemeralPrompt << wxString::Format(wxT("CURSOR_LINE: %d\nCURSOR_COLUMN: %d\n\n"), line, column);

  if (!fileContext.IsEmpty()) {
    ephemeralPrompt << wxT("CODE_CONTEXT (with 1-based line numbers):\n```cpp\n");
    ephemeralPrompt << fileContext;
    ephemeralPrompt << wxT("```\n\n");
  }

  m_solveSession = SolveSession{};
  m_solveSession.action = Action::InteractiveChat;
  m_solveSession.basename = basename;
  m_solveSession.iteration = 0;
  m_solveSession.maxIterations = m_editor->m_aiSettings.maxIterations;
  m_solveSession.finished = false;
  if (!raw.IsEmpty()) {
    m_solveSession.seen.AddSeen(basename, seenIntervalFrom, seenIntervalTo, ChecksumText(wxToStd(stc->GetText())));
  }
  m_editor->GetOwnerFrame()->CollectEditorSources(m_solveSession.workingFiles);

  if (!m_chatTranscript.IsEmpty()) {
    m_chatTranscript << wxT("\n\n");
  }
  m_chatTranscript << wxT("USER_MESSAGE:\n") << userText;

  if (!m_chatActive) {
    // Chat start
    m_chatActive = true;

    // persistence: new session + store initial prompt (contains first USER_MESSAGE)
    EnsureChatSessionStarted(userText, /*sessionTitle=*/wxEmptyString);

    // Now that session exists (m_chatSessionId is known), we can generate title.
    wxString sessionTitle = GenerateSessionTitleIfRequested(userText);
    sessionTitle.Trim(true).Trim(false);

    if (!sessionTitle.IsEmpty() && m_editor && m_editor->m_aiSettings.storeChatHistory) {
      UpdateSessionTitleInIndex(GetSketchRoot(), m_chatSessionId, wxToStd(sessionTitle));
    }
  }

  AppendChatEvent("user", userText);

  wxString promptForModel = BuildPromptForModel(m_chatTranscript, ephemeralPrompt, wxEmptyString);

  if (!client->SimpleChatAsync(interactiveChatSystemPrompt, promptForModel, this)) {
    StopCurrentAction();
    m_editor->ModalMsgDialog(
        _("Failed to start AI request."),
        _("AI interactive chat"),
        wxOK | wxICON_ERROR);
    return false;
  }
  return true;
}

// --- chat persistence ---
bool ArduinoAiActions::EnsureChatSessionStarted(const wxString &initialPromptForModel, const wxString &sessionTitle) {
  if (!m_editor || !m_editor->m_aiSettings.storeChatHistory)
    return false;
  if (m_chatSessionOpen)
    return true;

  const std::string sketchRoot = GetSketchRoot();
  if (sketchRoot.empty())
    return false;

  wxFileName root = GetAiRootDir(sketchRoot);
  wxFileName sessions = GetAiSessionsDir(sketchRoot);
  if (!EnsureDir(root) || !EnsureDir(sessions))
    return false;

  m_chatSessionId = MakeSessionId();
  wxFileName sessionPath = GetSessionJsonlPath(sketchRoot, m_chatSessionId);

  // meta + initial prompt
  json meta = {
      {"t", "meta"},
      {"v", 1},
      {"createdUtc", NowUtcIso8601()},
      {"model", wxToStd(m_editor->m_aiSettings.model)},
      {"endpoint", wxToStd(m_editor->m_aiSettings.endpointUrl)}};
  AppendJsonlLine(sessionPath, meta);

  json init = {
      {"t", "initial_prompt"},
      {"tsUtc", NowUtcIso8601()},
      {"model", wxToStd(m_editor->m_aiSettings.model)},
      {"text", wxToStd(initialPromptForModel)}};
  AppendJsonlLine(sessionPath, init);

  // update index.json
  json idx = LoadIndexJson(sketchRoot);
  json entry = {
      {"id", m_chatSessionId},
      {"createdUtc", NowUtcIso8601()},
      {"messageCount", 0},
      {"title", wxToStd(sessionTitle)}};
  idx["sessions"].push_back(entry);
  SaveIndexJsonAtomic(sketchRoot, idx);

  m_chatSessionOpen = true;
  return true;
}

void ArduinoAiActions::ResetInteractiveChat() {
  m_chatTranscript.Clear();
  m_solveSession.seen.Reset();
  m_chatActive = false;

  // also reset persistence session state
  m_chatSessionId.clear();
  m_chatSessionOpen = false;
}

std::vector<AiChatSessionInfo> ArduinoAiActions::ListStoredChatSessions() const {
  std::vector<AiChatSessionInfo> out;
  if (!m_editor || !m_editor->m_aiSettings.storeChatHistory)
    return out;

  const std::string sketchRoot = GetSketchRoot();
  json idx = LoadIndexJson(sketchRoot);
  if (!idx.contains("sessions") || !idx["sessions"].is_array())
    return out;

  for (const auto &s : idx["sessions"]) {
    if (!s.is_object())
      continue;
    AiChatSessionInfo i;
    i.id = s.value("id", "");
    i.createdUtc = s.value("createdUtc", "");
    i.messageCount = s.value("messageCount", 0);
    i.title = s.value("title", "");
    if (!i.id.empty())
      out.push_back(std::move(i));
  }

  // newest first (sessionId starts with YYYYMMDD_HHMMSS)
  std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
    return a.id > b.id;
  });
  return out;
}

bool ArduinoAiActions::LoadChatSession(const std::string &sessionId,
                                       wxString &outTranscript) const {
  outTranscript.Clear();

  if (!m_editor || !m_editor->m_aiSettings.storeChatHistory)
    return false;

  const std::string sketchRoot = GetSketchRoot();
  wxFileName p = GetSessionJsonlPath(sketchRoot, sessionId);
  if (!p.FileExists())
    return false;

  wxFFile f(p.GetFullPath(), wxT("rb"));
  if (!f.IsOpened())
    return false;

  wxString content;
  f.ReadAll(&content);

  wxArrayString lines = wxSplit(content, '\n', '\0');

  wxString transcript;
  wxString md;

  bool haveInitial = false;

  for (auto &line : lines) {
    wxString t = line;
    t.Trim(true).Trim(false);
    if (t.IsEmpty())
      continue;

    json ev;
    try {
      ev = json::parse(wxToStd(t));
    } catch (...) {
      continue;
    }

    std::string typ = ev.value("t", "");
    std::string text = ev.value("text", "");

    if (typ == "initial_prompt") {
      transcript = wxString::FromUTF8(text);
      haveInitial = true;

      // UI: first user message is already inside initial_prompt,
      // we won't try to extract it perfectly; keep history minimal:
      md << _("(Loaded previous session context.)\n\n");
      continue;
    }

    if (!haveInitial) {
      // without initial_prompt we cannot safely reconstruct transcript
      continue;
    }

    if (typ == "user") {
      wxString wxText = wxString::FromUTF8(text);
      transcript << wxT("\n\nUSER_MESSAGE:\n") << wxText;

      md << wxT("**You:**\n\n") << wxText << wxT("\n\n");
    } else if (typ == "assistant") {
      wxString wxText = wxString::FromUTF8(text);
      transcript << wxT("\n\nASSISTANT:\n") << wxText;

      md << wxText << wxT("\n\n");
    } else if (typ == "info_request") {
      wxString wxText = wxString::FromUTF8(text);
      transcript << wxT("\n\n*** BEGIN INFO_REQUEST_FROM_ASSISTANT\n")
                 << wxText
                 << wxT("\n*** END INFO_REQUEST_FROM_ASSISTANT\n");
    }
  }

  if (transcript.IsEmpty())
    return false;

  wxString finalTranscript = transcript;

  // When floating window is enabled, restore only the last "user -> patch" window
  // into m_chatTranscript. UI history (LoadChatSessionUi) still shows everything.
  TrimTranscriptToLastPatchWindow(finalTranscript);

  outTranscript = finalTranscript;
  return true;
}

bool ArduinoAiActions::LoadChatSessionUi(const std::string &sessionId, std::vector<AiChatUiItem> &outItems) {
  outItems.clear();

  if (!m_editor || !m_editor->m_aiSettings.storeChatHistory)
    return false;

  const std::string sketchRoot = GetSketchRoot();
  wxFileName p = GetSessionJsonlPath(sketchRoot, sessionId);
  if (!p.FileExists())
    return false;

  wxFFile f(p.GetFullPath(), wxT("rb"));
  if (!f.IsOpened())
    return false;

  wxString content;
  f.ReadAll(&content);
  wxArrayString lines = wxSplit(content, '\n', '\0');

  bool haveAny = false;

  for (auto &line : lines) {
    wxString t = line;
    t.Trim(true).Trim(false);
    if (t.IsEmpty())
      continue;

    json ev;
    try {
      ev = json::parse(wxToStd(t));
    } catch (...) {
      continue;
    }

    std::string typ = ev.value("t", "");
    std::string text = ev.value("text", "");
    std::string ts = ev.value("tsUtc", "");
    std::string model = ev.value("model", "");
    int inTok = ev.value("inputTokens", -1);
    int outTok = ev.value("outputTokens", -1);
    int totTok = ev.value("totalTokens", -1);

    if (typ == "user") {
      AiChatUiItem it;
      it.role = "user";
      it.text = wxString::FromUTF8(text);
      it.tsUtc = ts;
      it.model = model;
      it.inputTokens = inTok;
      it.outputTokens = outTok;
      it.totalTokens = totTok;
      outItems.push_back(std::move(it));
      haveAny = true;
    } else if (typ == "assistant_raw") {
      const std::string sketchPath = GetSketchRoot();

      wxString reply = wxString::FromUTF8(text);
      wxString vis, payload;

      std::vector<AiPatchHunk> patches;
      AiInfoRequest infoRequest;

      if (ParseAiPatch(reply, patches, &payload)) {
        vis.Append(payload);
        for (auto &patch : patches) {
          std::string bn = StripFilename(sketchPath, wxToStd(patch.file));
          wxString basename = wxString::FromUTF8(bn);
          vis.Append(wxString::Format(_("\n\n<patch into file %s (%d-%d)>\n"), basename, patch.fromLine, patch.toLine));
        }
      } else {
        std::vector<AiInfoRequest> reqs;
        if (ParseAiInfoRequests(reply, reqs, &payload)) {
          vis.Append(payload);
          for (const auto &r : reqs) {
            vis.Append(wxString::Format(_("\n\n<info with type %s requested>\n"), r.type));
          }
        } else {
          vis = wxString::FromUTF8(text); // plain model response
        }
      }

      AiChatUiItem it;
      it.role = "assistant";
      it.text = vis;
      it.tsUtc = ts;
      it.model = model;
      it.inputTokens = inTok;
      it.outputTokens = outTok;
      it.totalTokens = totTok;
      outItems.push_back(std::move(it));
      haveAny = true;
    }

    // ignore everything else for UI bubbles (meta/initial_prompt/info_response etc.)
  }

  return haveAny;
}

void ArduinoAiActions::RestoreInteractiveChatFromTranscript(const wxString &transcript, const std::string &sessionIdToContinue) {
  // Basic restore of model context
  m_chatTranscript = transcript;
  m_chatTranscript.Trim(true).Trim(false);
  m_chatActive = !m_chatTranscript.IsEmpty();

  // Also restore session persistence state (only if the feature is enabled)
  if (!m_editor || !m_editor->m_aiSettings.storeChatHistory) {
    // If persistence is disabled, do not keep any session state.
    m_chatSessionId.clear();
    m_chatSessionOpen = false;
    return;
  }

  // If caller wants to continue an existing stored session, keep it open
  if (!sessionIdToContinue.empty()) {
    m_chatSessionId = sessionIdToContinue;
    m_chatSessionOpen = true;
  } else {
    // No session requested -> next interactive chat will create a new session on first message
    m_chatSessionId.clear();
    m_chatSessionOpen = false;
  }

  // Optional sanity check: ensure the sessions directory exists, but do not create anything here.
}

wxString ArduinoAiActions::HandleInfoRequest(const AiInfoRequest &req) {

  if (req.type == wxT("includes")) {

    std::string code = GetCurrentCode();

    wxString includes;
    wxArrayString lines = wxSplit(wxString::FromUTF8(code), '\n', '\0');
    for (size_t i = 0; i < lines.size(); ++i) {
      wxString line = lines[i];
      wxString trimmed = line;
      trimmed.Trim(true).Trim(false);
      if (trimmed.StartsWith(wxT("#include"))) {
        includes << line;
        if (!line.EndsWith(wxT("\n"))) {
          includes << wxT("\n");
        }
      }
    }

    wxString resp;
    resp << wxT("*** BEGIN INFO_RESPONSE\n");
    resp << wxT("ID: ") << req.id << wxT("\n");
    resp << wxT("TYPE: includes\n");
    if (!req.file.IsEmpty()) {
      resp << wxT("FILE: ") << req.file << wxT("\n");
    }
    resp << wxT("CONTENT:\n");
    resp << includes;
    resp << wxT("*** END INFO_RESPONSE\n");
    return resp;

  } else if (req.type == wxT("symbol_declaration")) {
    // all symbols from current translation unit
    std::vector<SymbolInfo> allSyms = m_editor->completion->GetAllSymbols();

    wxArrayString symsInfos;
    wxString needle = req.symbol;
    needle.Trim(true).Trim(false);

    // filter by name
    for (auto &s : allSyms) {
      wxString sName = wxString::FromUTF8(s.name);

      if (sName.IsSameAs(needle)) {
        // Exact match - return one info
        symsInfos.Clear();
        symsInfos.Add(FormatSymbolInfoForAi(s));
        break;
      }

      if (sName.Contains(needle)) {
        symsInfos.Add(FormatSymbolInfoForAi(s));
      }
    }

    // context construction
    wxString content;
    if (symsInfos.IsEmpty()) {
      content << wxT("NO_MATCH_FOR_SYMBOL: ") << needle << wxT("\n");
    } else {
      // Max. 10 symbols
      for (size_t i = 0, n = std::min((int)symsInfos.GetCount(), 10); i < n; ++i) {
        content << symsInfos[i];
        if (i + 1 < n) {
          content << wxT("\n");
        }
      }
    }

    // wrap into response block
    wxString resp;
    resp << wxT("*** BEGIN INFO_RESPONSE\n");
    resp << wxT("ID: ") << req.id << wxT("\n");
    resp << wxT("TYPE: symbol_declaration\n");
    resp << wxT("SYMBOL: ") << needle << wxT("\n");
    resp << wxT("CONTENT:\n");
    resp << content;
    resp << wxT("*** END INFO_RESPONSE\n");

    return resp;

  } else if (req.type == wxT("search")) {

    wxString needleWx = req.query;
    needleWx.Trim(true).Trim(false);
    if (needleWx.IsEmpty()) {
      return wxString();
    }

    const std::string needle = wxToStd(needleWx);

    wxString content;
    int matchCount = 0;

    for (const auto &b : m_solveSession.workingFiles) {
      const std::string text = b.code;

      // Precompute line starts for fast line/col computations
      std::vector<size_t> lineStarts;
      lineStarts.reserve(128);
      lineStarts.push_back(0);
      for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
          lineStarts.push_back(i + 1);
        }
      }

      size_t fromPos = 0;
      while (fromPos < text.size()) {
        size_t pos = text.find(needle, fromPos);
        if (pos == std::string::npos) {
          break;
        }

        // line0 = last lineStart <= pos
        auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), pos);
        size_t line0 = (it == lineStarts.begin()) ? 0 : (size_t)((it - lineStarts.begin()) - 1);

        size_t col0 = pos - lineStarts[line0];

        int line1 = (int)line0 + 1; // 1-based
        int col1 = (int)col0 + 1;   // 1-based

        size_t lineStart = lineStarts[line0];
        size_t lineEnd = text.size();
        if (line0 + 1 < lineStarts.size()) {
          // points to next line start (after '\n')
          lineEnd = lineStarts[line0 + 1];
        }

        // Extract line text (trim \r and \n)
        std::string lineText = text.substr(lineStart, lineEnd - lineStart);
        TrimInPlace(lineText);

        if (matchCount > 0) {
          content << wxT("\n");
        }

        // AI block
        const std::string rel = StripFilename(GetSketchRoot(), b.filename);
        content << wxT("MATCH:\n");
        content << wxT("  FILE: ") << wxString::FromUTF8(rel) << wxT("\n");
        content << wxT("  LINE: ") << line1 << wxT("\n");
        content << wxT("  COLUMN: ") << col1 << wxT("\n");
        content << wxT("  CONTEXT: ") << wxString::FromUTF8(lineText) << wxT("\n");

        matchCount++;

        // move search index after current match
        fromPos = pos + needle.length();

        // Limit results for extreme situations
        if (matchCount >= 100) {
          content << wxT("\nMAX_MATCHES_REACHED: 100\n");
          break;
        }
      }

      if (matchCount >= 100) {
        break;
      }
    }

    if (matchCount == 0) {
      content << wxT("NO_MATCH_FOR_QUERY: ") << needleWx << wxT("\n");
    }

    wxString resp;
    resp << wxT("*** BEGIN INFO_RESPONSE\n");
    resp << wxT("ID: ") << req.id << wxT("\n");
    resp << wxT("TYPE: search\n");
    if (!req.kind.IsEmpty()) {
      resp << wxT("KIND: ") << req.kind << wxT("\n");
    }
    resp << wxT("QUERY: ") << needleWx << wxT("\n");
    resp << wxT("CONTENT:\n");
    resp << content;
    resp << wxT("*** END INFO_RESPONSE\n");

    return resp;

  } else if (req.type == wxT("file_range")) {

    // base input check
    if (req.file.IsEmpty() || req.fromLine <= 0 || req.toLine < req.fromLine) {
      return wxString();
    }

    const SketchFileBuffer *buf = FindBufferWithFile(wxToStd(req.file));

    if (!buf) {
      wxString resp;
      resp << wxT("*** BEGIN INFO_RESPONSE\n");
      resp << wxT("ID: ") << req.id << wxT("\n");
      resp << wxT("TYPE: file_range\n");
      resp << wxT("FILE: ") << req.file << wxT("\n");
      resp << wxT("FROM_LINE: 0\n");
      resp << wxT("TO_LINE: 0\n");
      resp << wxT("CONTENT:\n");
      resp << wxT("FILE_NOT_FOUND\n");
      resp << wxT("*** END INFO_RESPONSE\n");
      return resp;
    }

    const std::string &text = buf->code;

    // Build line starts
    std::vector<size_t> lineStarts;
    lineStarts.reserve(128);
    lineStarts.push_back(0);
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n') {
        lineStarts.push_back(i + 1);
      }
    }

    int lineCount = (int)lineStarts.size();
    int fromLine = std::max(1, req.fromLine);
    int toLine = std::min(lineCount, req.toLine);

    wxString content, raw;

    for (int l = fromLine; l <= toLine; ++l) {
      size_t idx0 = (size_t)(l - 1);
      size_t start = lineStarts[idx0];
      size_t end = text.size();
      if (idx0 + 1 < lineStarts.size()) {
        end = lineStarts[idx0 + 1];
      }

      std::string lineTextStd = text.substr(start, end - start);
      // remove line endings
      while (!lineTextStd.empty() && (lineTextStd.back() == '\n' || lineTextStd.back() == '\r')) {
        lineTextStd.pop_back();
      }

      wxString lineText = wxString::FromUTF8(lineTextStd);
      raw << lineText << wxT("\n"); // no numbering
      content << wxString::Format(wxT("%d: %s\n"), l, lineText);
    }

    std::string basename = StripFilename(GetSketchRoot(), wxToStd(req.file));

    m_solveSession.seen.AddSeen(wxString::FromUTF8(basename), fromLine, toLine, ChecksumText(text));

    wxString resp;
    resp << wxT("*** BEGIN INFO_RESPONSE\n");
    resp << wxT("ID: ") << req.id << wxT("\n");
    resp << wxT("TYPE: file_range\n");
    resp << wxT("FILE: ") << req.file << wxT("\n");
    resp << wxT("FROM_LINE: ") << fromLine << wxT("\n");
    resp << wxT("TO_LINE: ") << toLine << wxT("\n");
    resp << wxT("CONTENT:\n");
    resp << content;
    resp << wxT("*** END INFO_RESPONSE\n");

    return resp;
  }

  return wxString();
}

bool ArduinoAiActions::ParseAiPatch(const wxString &rawPatch, std::vector<AiPatchHunk> &out, wxString *payload) {
  out.clear();

  if (payload) {
    payload->Clear();
  }

  wxString text = StripCodeFence(rawPatch);
  wxArrayString lines = wxSplit(text, '\n', '\0');

  AiPatchHunk current;
  bool inPatch = false;
  bool inReplace = false;

  auto flushCurrent = [&]() {
    // If the model wrapped the replacement in a Markdown code fence, strip it here
    // to avoid compiler-error ping-pong iterations.
    if (!current.replacement.IsEmpty() && LooksLikeCodeFence(current.replacement)) {
      current.replacement = StripCodeFence(current.replacement);
    }
    // we accept any non-negative range, we solve the logic only when applying
    bool hasRange =
        (current.fromLine >= 0) &&
        (current.toLine >= current.fromLine);
    bool hasFile = !current.file.IsEmpty();
    bool hasReplacement = !current.replacement.IsEmpty();

    if (hasFile && hasRange && hasReplacement) {
      out.push_back(current);
    }
    current = AiPatchHunk{};
  };

  for (size_t i = 0; i < lines.size(); ++i) {
    wxString line = lines[i];
    wxString trimmed = line;
    trimmed.Trim(true).Trim(false);

    if (trimmed.StartsWith(wxT("*** BEGIN PATCH")) || trimmed.StartsWith(wxT("PATCH"))) {
      // if by chance the previous block was not closed correctly,
      // we will try to flush the current one
      if (inPatch) {
        if (inReplace) {
          inReplace = false;
        }
        flushCurrent();
      }
      inPatch = true;
      inReplace = false;
      continue;
    }

    if (!inPatch) {
      if (payload) {
        payload->Append(line);
      }
      continue;
    }

    if (trimmed.StartsWith(wxT("*** END PATCH"))) {
      if (inReplace) {
        inReplace = false;
      }
      flushCurrent();
      inPatch = false;
      continue;
    }

    if (trimmed.StartsWith(wxT("FILE:"))) {
      flushCurrent();

      wxString fileName = trimmed.Mid(wxString(wxT("FILE:")).length());
      fileName.Trim(true).Trim(false);
      current.file = fileName;
      continue;
    }

    if (trimmed.StartsWith(wxT("RANGE:"))) {
      // If we already collected a full hunk for the same FILE (i.e. we have replacement),
      // start a new hunk but keep the FILE name.
      if (!current.file.IsEmpty() && !current.replacement.IsEmpty()) {
        wxString keepFile = current.file;
        flushCurrent();
        current.file = keepFile;
      }

      int from = 0, to = 0;
      if (ParseRangeLine(trimmed, from, to)) {
        current.fromLine = from;
        current.toLine = to;
      }
      continue;
    }

    if (trimmed.StartsWith(wxT("REPLACE:"))) {
      inReplace = true;
      current.replacement.clear();
      continue;
    }

    if (trimmed.StartsWith(wxT("ENDREPLACE"))) {
      // Strip possible fenced block immediately when REPLACE ends.
      if (!current.replacement.IsEmpty() && LooksLikeCodeFence(current.replacement)) {
        current.replacement = StripCodeFence(current.replacement);
      }
      inReplace = false;
      continue;
    }

    if (inReplace) {
      current.replacement << line;
      if (i + 1 < lines.size()) {
        current.replacement << wxT("\n");
      }
    }
  }

  // if the last PATCH block did not have an explicit *** END PATCH
  if (inPatch) {
    if (inReplace) {
      inReplace = false;
    }
    flushCurrent();
  }

  return !out.empty();
}

// Applies a multi-file AI patch into in-memory buffers (UTF-8).
// Returns true if at least one hunk was applied.
bool ArduinoAiActions::ApplyAiPatchToFiles(const std::vector<AiPatchHunk> &patches) {

  auto computeLineStarts = [](const std::string &text, std::vector<size_t> &starts) {
    starts.clear();
    starts.reserve(256);
    starts.push_back(0);

    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n') {
        // next line starts after '\n'
        starts.push_back(i + 1);
      }
    }
  };

  // Convert 1-based (fromLine, toLine) to [startPos, endPos) in bytes.
  auto rangeToByteOffsets = [](const std::vector<size_t> &lineStarts,
                               const std::string &text,
                               int fromLine,
                               int toLine,
                               bool emptyRange,
                               size_t &outStart,
                               size_t &outEnd) -> bool {
    if (emptyRange) {
      outStart = 0;
      outEnd = 0;
      return true;
    }

    if (fromLine < 1) {
      return false;
    }
    if (toLine < fromLine) {
      return false;
    }

    const int lineCount = (int)lineStarts.size(); // lines are 1..lineCount

    if (fromLine > lineCount) {
      // start beyond EOF => nothing to apply
      return false;
    }

    // start at beginning of (fromLine-1)
    outStart = lineStarts[(size_t)(fromLine - 1)];

    // end at beginning of (toLine) if exists, else EOF
    if (toLine < lineCount) {
      outEnd = lineStarts[(size_t)toLine];
    } else {
      outEnd = text.size();
    }

    if (outEnd < outStart) {
      return false;
    }
    return true;
  };

  // Group hunks by file (normalized)
  std::string sketchRoot = GetSketchRoot();
  std::unordered_map<std::string, std::vector<AiPatchHunk>> byFile;
  byFile.reserve(patches.size());

  for (const auto &h : patches) {
    if (h.file.IsEmpty()) {
      continue;
    }
    std::string path = NormalizeFilename(sketchRoot, wxToStd(h.file));
    if (path.empty()) {
      continue;
    }
    byFile[path].push_back(h);
  }

  if (byFile.empty()) {
    APP_DEBUG_LOG("AI: No files for patch collected!");
    return false;
  }

  bool anyApplied = false;

  for (auto &kv : byFile) {
    auto &hun = kv.second;

    SketchFileBuffer *buf = FindBufferWithFile(kv.first, /*allowCreate=*/true);
    if (!buf) {
      continue;
    }

    // Apply from end to avoid shifting
    std::sort(hun.begin(), hun.end(),
              [](const AiPatchHunk &a, const AiPatchHunk &b) {
                return a.fromLine > b.fromLine;
              });

    std::string &text = buf->code;

    // Precompute line starts once per file (but must refresh after each replace if ranges might depend on updated text).
    // Because we apply from end, we can recompute each time cheaply and stay correct even if earlier hunks change line structure.
    for (const auto &h : hun) {
      const bool emptyRange = (h.toLine == 0);

      std::vector<size_t> lineStarts;
      computeLineStarts(text, lineStarts);

      size_t startPos = 0, endPos = 0;
      if (!rangeToByteOffsets(lineStarts, text, h.fromLine, h.toLine, emptyRange, startPos, endPos)) {
        // nothing / invalid range
        continue;
      }

      // Replacement text in UTF-8
      std::string repl = wxToStd(h.replacement);

      if (startPos > text.size() || endPos > text.size() || startPos > endPos) {
        continue;
      }

      APP_DEBUG_LOG("AI: replace range %zu - %zu", startPos, endPos);
      APP_TRACE_LOG("AI: replacement text:\n%s", repl.c_str());

      text.replace(startPos, endPos - startPos, repl);
      anyApplied = true;
    }
  }

  if (!anyApplied) {
    APP_DEBUG_LOG("AI: No hunks applied (ranges out of file bounds or empty patch).");
  }

  return anyApplied;
}

static bool AiIsInfoMarker(const wxString &trimmedLine, const wxString &tokenA, const wxString &tokenB) {
  wxString t = trimmedLine;
  t.MakeUpper();

  if (!t.StartsWith(wxT("***"))) {
    return false;
  }

  return (t.Find(tokenA) != wxNOT_FOUND) && (t.Find(tokenB) != wxNOT_FOUND);
}

bool ArduinoAiActions::ParseAiInfoRequests(const wxString &raw,
                                           std::vector<AiInfoRequest> &out,
                                           wxString *payload) {
  out.clear();
  if (payload)
    payload->clear();

  wxString text = StripCodeFence(raw);
  wxArrayString lines = wxSplit(text, '\n', '\0');

  bool inReq = false;
  AiInfoRequest cur;

  auto flushCur = [&]() {
    if (!cur.type.IsEmpty()) {
      out.push_back(cur);
    }
    cur = AiInfoRequest{};
  };

  for (size_t i = 0; i < lines.size(); ++i) {
    wxString line = lines[i];

    wxString trimmed = line;
    trimmed.Trim(true).Trim(false);

    if (!inReq) {
      if (AiIsInfoMarker(trimmed, wxT("BEGIN"), wxT("INFO_REQUEST"))) {
        inReq = true;
        cur = AiInfoRequest{};
        cur.rawBlock.clear();
        cur.rawBlock << line << wxT("\n");
      } else {
        // Only outside blocks goes into payload (including text between blocks).
        if (payload) {
          (*payload) << line;
          if (i + 1 < lines.size()) {
            (*payload) << wxT("\n");
          }
        }
      }
      continue;
    }

    // inReq
    cur.rawBlock << line << wxT("\n");

    if (AiIsInfoMarker(trimmed, wxT("END"), wxT("INFO_REQUEST"))) {
      inReq = false;
      flushCur();
      continue;
    }

    if (trimmed.StartsWith(wxT("ID:"))) {
      wxString val = trimmed.Mid(3);
      val.Trim(true).Trim(false);
      long id = 0;
      if (val.ToLong(&id))
        cur.id = (int)id;
      continue;
    }

    if (trimmed.StartsWith(wxT("TYPE:"))) {
      wxString val = trimmed.Mid(5);
      val.Trim(true).Trim(false);
      cur.type = val;
      continue;
    }

    if (trimmed.StartsWith(wxT("FILE:"))) {
      wxString val = trimmed.Mid(5);
      val.Trim(true).Trim(false);
      cur.file = val;
      continue;
    }

    if (trimmed.StartsWith(wxT("SYMBOL:"))) {
      wxString val = trimmed.Mid(7);
      val.Trim(true).Trim(false);
      cur.symbol = val;
      continue;
    }

    if (trimmed.StartsWith(wxT("QUERY:")) || trimmed.StartsWith(wxT("TERM:")) || trimmed.StartsWith(wxT("PATTERN:"))) {
      wxString val = trimmed.Mid(6);
      val.Trim(true).Trim(false);
      cur.query = val;
      continue;
    }

    if (trimmed.StartsWith(wxT("KIND:"))) {
      wxString val = trimmed.Mid(5);
      val.Trim(true).Trim(false);
      cur.kind = val;
      continue;
    }

    if (trimmed.StartsWith(wxT("FROM_LINE:"))) {
      wxString val = trimmed.Mid(10);
      val.Trim(true).Trim(false);
      long num = 0;
      if (val.ToLong(&num))
        cur.fromLine = (int)num;
      continue;
    }

    if (trimmed.StartsWith(wxT("TO_LINE:"))) {
      wxString val = trimmed.Mid(8);
      val.Trim(true).Trim(false);
      long num = 0;
      if (val.ToLong(&num))
        cur.toLine = (int)num;
      continue;
    }
  }

  // If the model forgot the END marker, still flush if we have something.
  if (inReq) {
    flushCur();
  }

  return !out.empty();
}

wxString ArduinoAiActions::FormatSymbolInfoForAi(const SymbolInfo &s) {
  wxString out;

  out << wxT("=== BEGIN SYMBOL_INFO ===\n");

  if (!s.name.empty()) {
    out << wxT("NAME: ") << wxString::FromUTF8(s.name) << wxT("\n");
  }
  if (!s.display.empty()) {
    out << wxT("DISPLAY: ") << wxString::FromUTF8(s.display) << wxT("\n");
  }
  if (!s.file.empty()) {
    out << wxT("FILE: ") << wxString::FromUTF8(StripFilename(GetSketchRoot(), s.file)) << wxT("\n"); // only base filename from sketchdir
  }
  if (s.line > 0) {
    out << wxT("LINE: ") << s.line << wxT("\n");
  }
  if (s.column > 0) {
    out << wxT("COLUMN: ") << s.column << wxT("\n");
  }

  // KIND - we just print it as a string because CXCursorKind is an enum
  out << wxT("KIND: ") << wxString::FromUTF8(ArduinoCodeCompletion::GetKindSpelling(s.kind)) << wxT("\n");

  for (const auto &p : s.parameters) {
    out << wxT("\nPARAMETER:\n");
    if (!p.name.empty()) {
      out << wxT("  NAME: ") << wxString::FromUTF8(p.name) << wxT("\n");
    }
    if (!p.type.empty()) {
      out << wxT("  TYPE: ") << wxString::FromUTF8(p.type) << wxT("\n");
    }
  }

  // BODY (if exists)
  if (s.bodyLineFrom > 0 && s.bodyLineTo > 0) {
    out << wxT("\nBODY_RANGE:\n");
    out << wxT("  FROM: ") << s.bodyLineFrom << wxT(":") << s.bodyColFrom << wxT("\n");
    out << wxT("  TO: ") << s.bodyLineTo << wxT(":") << s.bodyColTo << wxT("\n");
  }

  out << wxT("=== END SYMBOL_INFO ===\n");

  return out;
}

// --- POLICY ENFORCEMENT: if model patches an existing file without requesting file_range first, force a retry ---
bool ArduinoAiActions::CheckModelQueriedFile(const std::vector<AiPatchHunk> &patches) {

  ArduinoEditorFrame *frame = m_editor->GetOwnerFrame();
  AiClient *client = GetClient();

  // Gather offending hunks (we will send file_range for each).
  std::vector<AiPatchHunk> offenders;

  for (const auto &h : patches) {
    if (h.file.IsEmpty())
      continue;

    APP_DEBUG_LOG("AI: checking file %s and range %d-%d for model memory...", wxToStd(h.file).c_str(), h.fromLine, h.toLine);

    std::string basename = StripFilename(GetSketchRoot(), wxToStd(h.file));

    ArduinoEditor *edi = frame->FindEditorWithFile(basename, /*allowCreate=*/true);
    if (!edi) {
      // probably new file
      continue;
    }

    if (!m_solveSession.seen.HasSeen(wxString::FromUTF8(basename), h.fromLine, h.toLine, ChecksumText(wxToStd(edi->m_editor->GetText())))) {
      offenders.push_back(h);
    }
  }

  if (!offenders.empty()) {
    // Build forced INFO_RESPONSE blocks for the requested ranges.
    wxString forced;
    forced << wxT("POLICY: You attempted to patch an existing file without requesting TYPE=file_range first.\n");
    forced << wxT("POLICY: Use the numbered lines provided below and try again.\n");
    forced << wxT("POLICY: Output ONLY a PATCH.\n\n");

    // 1) Merge requested ranges per file (dedupe offenders by filename)
    std::unordered_map<wxString, std::pair<int, int>, wxStringHash, wxStringEqual> merged;
    merged.reserve(offenders.size());

    for (const auto &h : offenders) {
      int fromLine = h.fromLine;
      int toLine = h.toLine;

      // fallback when model provides nonsense
      if (fromLine <= 0 || toLine < fromLine) {
        fromLine = 1;
        toLine = 160;
      } else {
        // Enforce a minimum useful context window for forced file_range,
        // otherwise small models will only be able to patch 1-1.
        const int kMinLines = 120;

        if (toLine - fromLine + 1 < kMinLines) {
          // Prefer includes/top-of-file for compiler fixes
          fromLine = 1;
          toLine = fromLine + kMinLines - 1;
        }
      }

      auto it = merged.find(h.file);
      if (it == merged.end()) {
        merged.emplace(h.file, std::make_pair(fromLine, toLine));
      } else {
        it->second.first = std::min(it->second.first, fromLine);
        it->second.second = std::max(it->second.second, toLine);
      }
    }

    // 2) Emit exactly one file_range per file
    int forcedId = 9000; // unique IDs for this forced block
    for (const auto &kv : merged) {
      const wxString &file = kv.first;
      int fromLine = kv.second.first;
      int toLine = kv.second.second;

      // Clamp to real line count if editor is open
      // (prevents "1-160" when file has only 20 lines, and improves token usage)
      auto *ed = frame->FindEditorWithFile(wxToStd(file), /*allowCreate=*/false);
      if (ed) {
        const int maxLine = ed->m_editor->GetLineCount(); // 0-based count, but we use 1-based in protocol
        if (maxLine > 0) {
          fromLine = std::max(1, std::min(fromLine, maxLine));
          toLine = std::max(fromLine, std::min(toLine, maxLine));
        }
      }

      AiInfoRequest req;
      req.id = forcedId++;
      req.type = wxT("file_range");
      req.file = file;
      req.fromLine = fromLine;
      req.toLine = toLine;

      wxString resp = HandleInfoRequest(req);
      if (!resp.IsEmpty()) {
        forced << resp << wxT("\n");
      }
    }

    // Add to the correct transcript and ask the model to try again.
    if (m_solveSession.action == Action::InteractiveChat) {
      // Persistence (optional): store what happened
      AppendChatEvent("assistant", wxT("<patch rejected by IDE policy>"));
      AppendChatEvent("info_response", forced);

      AppendAssistantNote(AssistantNoteKind::PatchRejected, wxT("Missing required file_range for modified file."));
      AppendAssistantNote(AssistantNoteKind::RetryRequested, wxT("Please reapply the patch using the provided INFO_RESPONSE blocks."));

      wxString retryPrompt = BuildPromptForModel(m_chatTranscript, wxEmptyString, forced);

      // Count an iteration (one more model roundtrip).
      m_solveSession.iteration++;
      if (CheckNumberOfIterations()) {
        client->SimpleChatAsync(interactiveChatSystemPrompt, retryPrompt, this);
      } else {
        return true;
      }
    } else if (m_solveSession.action == Action::SolveProjectError) {
      AppendAssistantNote(AssistantNoteKind::PatchRejected,
                          wxT("Missing required file_range for modified file."));
      AppendAssistantNote(AssistantNoteKind::RetryRequested,
                          wxT("Please reapply the patch using the provided INFO_RESPONSE blocks."));

      wxString retryPrompt = BuildPromptForModel(m_solveSession.transcript, wxEmptyString, forced);

      // Count an iteration (one more model roundtrip).
      m_solveSession.iteration++;
      if (CheckNumberOfIterations()) {
        client->SimpleChatAsync(solveErrorSystemPrompt, retryPrompt, this);
      } else {
        return true;
      }
    } else if (m_solveSession.action == Action::OptimizeFunctionOrMethod) {
      AppendAssistantNote(AssistantNoteKind::PatchRejected,
                          wxT("Missing required file_range for modified file."));
      AppendAssistantNote(AssistantNoteKind::RetryRequested,
                          wxT("Please reapply the patch using the provided INFO_RESPONSE blocks."));

      wxString retryPrompt = BuildPromptForModel(m_solveSession.transcript, wxEmptyString, forced);

      // Count an iteration (one more model roundtrip).
      m_solveSession.iteration++;
      if (CheckNumberOfIterations()) {
        client->SimpleChatAsync(optimizeFunctionSystemPrompt, retryPrompt, this);
      } else {
        return true;
      }
    }

    // UI bubble summary
    m_interactiveChatPayload.Append(_("\n\n<patch rejected: file_range required>\n"));

    return false; // expecting next AI response
  }

  return true;
}

bool ArduinoAiActions::CheckNumberOfIterations() {
  // check iterations (per roundtrip, not per block)
  if (m_solveSession.iteration < m_solveSession.maxIterations)
    return true;

  wxMessageDialog dlg(
      m_editor,
      _("AI reached the maximum number of iterations.\n\t\n"
        "Do you want to continue?"),
      _("Arduino Editor AI"),
      wxYES_NO | wxICON_WARNING | wxNO_DEFAULT);

  dlg.SetYesNoLabels(_("Continue"), _("Stop"));

  if (dlg.ShowModal() == wxID_YES) {
    // Allow more iterations for this run.
    m_solveSession.iteration = 0;
    return true;
  }

  m_solveSession.finished = true;
  StopCurrentAction();
  SendDoneEventToOrigin();
  return false;
}

void ArduinoAiActions::SendDoneEventToOrigin() {
  if (m_origin != nullptr) {
    wxThreadEvent evt(wxEVT_AI_SIMPLE_CHAT_SUCCESS);
    evt.SetString(m_interactiveChatPayload);
    evt.SetPayload(m_solveSession.tokenTotals);
    wxPostEvent(m_origin, evt);
  }
}

void ArduinoAiActions::AppendAssistantNote(AssistantNoteKind kind,
                                           const wxString &detail,
                                           const std::vector<AiPatchHunk> *patches,
                                           const std::vector<AiInfoRequest> *reqs) {
  wxString &tr =
      (m_solveSession.action == Action::InteractiveChat)
          ? m_chatTranscript
          : m_solveSession.transcript;

  tr << wxT("\n\nASSISTANT:\n");

  switch (kind) {
    case AssistantNoteKind::PatchApplied: {
      tr << wxT("\n<patch applied>\n");
      if (patches) {
        for (const auto &p : *patches) {
          tr << wxT("- FILE: ") << p.file
             << wxT(" RANGE: ") << p.fromLine << wxT("-") << p.toLine << wxT("\n");
        }
      }
      break;
    }

    case AssistantNoteKind::PatchRejected: {
      tr << wxT("\n<patch rejected by IDE policy>\n");
      if (!detail.IsEmpty()) {
        tr << wxT("REASON: ") << detail << wxT("\n");
      }
      break;
    }

    case AssistantNoteKind::InfoResponsesProvided: {
      tr << wxT("INFO_RESPONSES provided");
      if (reqs && !reqs->empty()) {
        tr << wxT(" for request IDs: ");
        for (size_t i = 0; i < reqs->size(); ++i) {
          tr << (*reqs)[i].id;
          if (i + 1 < reqs->size())
            tr << wxT(", ");
        }
      }
      tr << wxT(".\n");
      tr << wxT("INSTRUCTION: Use the provided numbered lines for PATCH RANGE.\n");
      break;
    }

    case AssistantNoteKind::RetryRequested: {
      tr << wxT("<retry requested>\n");
      if (!detail.IsEmpty()) {
        tr << detail << wxT("\n");
      }
      break;
    }
  }
}

void ArduinoAiActions::AppendAssistantPlaintextToTranscript(const wxString &text) {
  wxString t = text;
  t.Trim(true).Trim(false);
  if (t.IsEmpty()) {
    return;
  }

  wxString &tr =
      (m_solveSession.action == Action::InteractiveChat)
          ? m_chatTranscript
          : m_solveSession.transcript;

  tr << wxT("\n\nASSISTANT:\n") << t;

  // persistence (jsonl)
  AppendChatEvent("assistant", t);
}

void ArduinoAiActions::TrimTranscriptToLastPatchWindow(wxString &transcript) const {
  if (!m_floatingWindow) {
    return;
  }

  transcript.Trim(true).Trim(false);
  if (transcript.IsEmpty()) {
    return;
  }

  APP_DEBUG_LOG("AI: trimming context, len=%u", transcript.Length());

  const wxString patchMarker = wxT("<patch applied>");
  const wxString userMarker = wxT("USER_MESSAGE");

  // 1) Find last "<patch applied>"
  int lastPatchPos = transcript.find(patchMarker);
  if (lastPatchPos == wxNOT_FOUND) {
    APP_DEBUG_LOG("AI: no <patch applied> marker found, nothing to trim.");
    return;
  }

  // But find() returns first occurrence → we need the LAST one
  int searchFrom = lastPatchPos;
  while (true) {
    int next = transcript.find(patchMarker, searchFrom + 1);
    if (next == wxNOT_FOUND) {
      break;
    }
    searchFrom = next;
  }
  lastPatchPos = searchFrom;

  APP_DEBUG_LOG("AI: last <patch applied> at %d", lastPatchPos);

  // 2) Walk backwards line-by-line to find last USER_MESSAGE
  int userPos = wxNOT_FOUND;

  wxArrayString lines = wxSplit(transcript, '\n', '\0');
  size_t patchLineIndex = 0;

  // First find which line contains the patch marker
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].Contains(patchMarker)) {
      patchLineIndex = i;
    }
  }

  // Walk backwards from that line
  for (int i = (int)patchLineIndex; i >= 0; --i) {
    wxString l = lines[i];
    l.Trim(true).Trim(false);

    if (l.StartsWith(userMarker)) {
      // Recompute char offset of this line
      size_t off = 0;
      for (int j = 0; j < i; ++j) {
        off += lines[j].Length() + 1; // + '\n'
      }
      userPos = (int)off;
      break;
    }
  }

  if (userPos == wxNOT_FOUND) {
    APP_DEBUG_LOG("AI: USER_MESSAGE marker not found, trimming from start.");
    userPos = 0;
  }

  wxString newTranscript = transcript.Mid(userPos);
  newTranscript.Trim(true).Trim(false);

  APP_DEBUG_LOG("AI: transcript trimmed %u -> %u",
                transcript.Length(), newTranscript.Length());

  transcript = newTranscript;
}

// Returns true if no further action expected
bool ArduinoAiActions::ApplyAiModelSolution(const wxString &reply) {
  if (reply.IsEmpty()) {
    return true;
  }

  auto *client = GetClient();
  int inTok = -1, outTok = -1, totTok = -1;
  if (client) {
    inTok = client->GetLastInputTokens();
    outTok = client->GetLastOutputTokens();
    totTok = client->GetLastTotalTokens();
  } else {
    return true;
  }

  // Sum token usage across the whole iterative solve session.
  if (m_solveSession.action != Action::None) {
    m_solveSession.tokenTotals.Add(inTok, outTok, totTok);
  }

  if (m_solveSession.action == Action::InteractiveChat) {
    AppendChatEvent("assistant_raw", reply, inTok, outTok, totTok);
  }

  const std::string sketchPath = GetSketchRoot();

  wxString assistantText;
  std::vector<AiInfoRequest> infoRequests;
  std::vector<AiPatchHunk> patches;

  if (ParseAiPatch(reply, patches, &assistantText)) {
    assistantText.Trim(true).Trim(false);
    if (!assistantText.IsEmpty()) {
      m_interactiveChatPayload.Append(assistantText);
      m_interactiveChatPayload.Append(wxT("\n\n"));
    }

    for (auto &p : patches) {
      if (p.file.IsEmpty())
        continue;
      std::string rel = StripFilename(GetSketchRoot(), wxToStd(p.file));
      p.file = wxString::FromUTF8(rel);
    }

    // 0) normalize FILE names from the model for single-file actions
    if (!m_solveSession.basename.IsEmpty() &&
        (m_solveSession.action == Action::OptimizeFunctionOrMethod ||
         m_solveSession.action == Action::SolveProjectError)) {

      auto getLastComponent = [](const wxString &path) -> wxString {
        int slashPos = path.Find('/', true);
        int backslashPos = path.Find('\\', true);
        int pos = wxMax(slashPos, backslashPos);
        if (pos == wxNOT_FOUND) {
          return path;
        }
        return path.Mid(pos + 1);
      };

      wxString sessionPath = m_solveSession.basename;
      wxString sessionBase = getLastComponent(sessionPath);

      for (auto &patch : patches) {
        if (patch.file.IsEmpty()) {
          continue;
        }

        // Already matches exactly
        if (patch.file == sessionPath) {
          continue;
        }

        wxString fileBase = getLastComponent(patch.file);

        // If the last component (filename) matches,
        // but the path is different, we align FILE to session.basename.
        if (fileBase.IsSameAs(sessionBase)) {
          patch.file = sessionPath;
        }
      }
    }

    // For OptimizeFunctionOrMethod, enforce the body range for patches
    if (m_solveSession.action == Action::OptimizeFunctionOrMethod &&
        m_solveSession.bodyFromLine > 0 &&
        m_solveSession.bodyToLine > 0) {

      for (auto &patch : patches) {
        // Only for the target file of this session
        if (patch.file == m_solveSession.basename) {
          // If the model's range is fully inside the known body range,
          // snap it to exactly that body range.
          if (patch.fromLine >= m_solveSession.bodyFromLine &&
              patch.toLine <= m_solveSession.bodyToLine) {
            patch.fromLine = m_solveSession.bodyFromLine;
            patch.toLine = m_solveSession.bodyToLine;
          }
        }
      }
    }

    if (GetSettings().forceModelQueryRange) {
      if (!CheckModelQueriedFile(patches)) {
        // either waiting for retry OR we finished due to iteration limit
        return m_solveSession.finished ? true : false;
      }

      // IMPORTANT: if policy/iteration logic marked the session as finished,
      // do not continue to show a diff for the current (possibly invalid) patch.
      if (m_solveSession.finished) {
        return true;
      }
    }

    const std::vector<SketchFileBuffer> &files = m_solveSession.workingFiles;

    ArduinoPatchStraightener ps(sketchPath, patches, files);

    ps.Calculate();

    patches = ps.GetResult();

    if (!ApplyAiPatchToFiles(patches)) {
      m_editor->ModalMsgDialog(
          _("AI patch does not apply to the current file."),
          _("Arduino Editor AI"));
      return true; // No further action expected
    }

    const wxString evidence = BuildAppliedPatchEvidence(patches, /*extraContextLines=*/5, /*maxTotalLines=*/150);
    AppendAssistantPlaintextToTranscript(evidence);

    m_solveSession.assistantPatchExplanation = assistantText;

    Bind(EVT_DIAGNOSTICS_UPDATED, &ArduinoAiActions::OnDiagnosticsUpdated, this);
    m_editor->completion->RefreshProjectDiagnosticsAsync(m_solveSession.workingFiles, this);
    return false;

  } else {
    if (!ParseAiInfoRequests(reply, infoRequests, &assistantText)) {
      m_interactiveChatPayload.Append(reply);

      // store into model transcript so it stays in context
      AppendAssistantPlaintextToTranscript(reply);

      return true;
    }

    assistantText.Trim(true).Trim(false);
    if (!assistantText.IsEmpty()) {
      m_interactiveChatPayload.Append(assistantText);
      m_interactiveChatPayload.Append(wxT("\n\n"));

      // Keep assistant plaintext in transcript (helps continuity)
      AppendAssistantPlaintextToTranscript(assistantText);
    }

    // UI summary: one line per request
    for (const auto &req : infoRequests) {
      m_interactiveChatPayload.Append(
          wxString::Format(_("<info with type %s requested>\n"), req.type));
    }

    // Produce responses for ALL requests
    wxString allResponses;
    for (const auto &req : infoRequests) {
      wxString one = HandleInfoRequest(req);
      if (one.IsEmpty()) {
        one << wxT("*** BEGIN INFO_RESPONSE\n");
        one << wxT("ID: ") << req.id << wxT("\n");
        one << wxT("TYPE: ") << req.type << wxT("\n");
        one << wxT("BAD_PARAMETERS_SEE_SPEC\n");
        one << wxT("*** END INFO_RESPONSE\n");
      }

      if (m_solveSession.action == Action::InteractiveChat) {
        AppendChatEvent("info_response", one);
      }

      allResponses << one << wxT("\n");
    }

    auto appendInfoRequestMarker = [&](wxString &tr, const std::vector<AiInfoRequest> &reqs) {
      tr << wxT("\n\nINFO_REQUESTS (assistant asked):\n");

      for (const auto &r : reqs) {
        tr << wxString::Format(wxT("- [id=%d] %s"), r.id, r.type);

        if (!r.file.IsEmpty()) {
          tr << wxT(" file=") << r.file;
        }
        if (r.fromLine > 0 || r.toLine > 0) {
          tr << wxString::Format(wxT(" range=%d-%d"), r.fromLine, r.toLine);
        }
        if (!r.query.IsEmpty()) {
          tr << wxT(" query=\"") << r.query << wxT("\"");
        }
        tr << wxT("\n");
      }

      if (!m_fullInfoRequest) {
        tr << wxT("NOTE: Requested info was provided out-of-band (not stored in transcript).\n");
        tr << wxT("INSTRUCTION: Treat the INFO_RESPONSES block in the next message as authoritative.\n");
      } else {
        tr << wxT("NOTE: Requested info was appended into transcript (full context mode).\n");
      }
    };

    wxString outOfBand;
    if (!m_fullInfoRequest) {
      outOfBand << wxT("\n*** BEGIN INFO_RESPONSES\n");
      outOfBand << allResponses;
      outOfBand << wxT("\n*** END INFO_RESPONSES\n");
      outOfBand << wxT("INSTRUCTION: Use line numbers from INFO_RESPONSE CONTENT for RANGE.\n");
    }

    if (m_solveSession.action == Action::SolveProjectError) {
      wxString newTranscript;
      newTranscript << m_solveSession.transcript;
      appendInfoRequestMarker(newTranscript, infoRequests);
      m_solveSession.transcript = newTranscript;

      if (m_fullInfoRequest) {
        // Persist into transcript so the model keeps the full context for refactoring / multi-step work.
        m_solveSession.transcript << wxT("\n\n") << allResponses;

        // Optional: store in JSONL as well so session replay contains full context.
        AppendChatEvent("info_response", allResponses);
      }

      AppendAssistantNote(AssistantNoteKind::InfoResponsesProvided, wxEmptyString, nullptr, &infoRequests);

      wxString promptForModel = BuildPromptForModel(m_solveSession.transcript, wxEmptyString, outOfBand);

      m_solveSession.iteration++;
      if (CheckNumberOfIterations()) {
        client->SimpleChatAsync(solveErrorSystemPrompt, promptForModel, this);
      } else {
        return true;
      }

    } else if (m_solveSession.action == Action::InteractiveChat) {
      appendInfoRequestMarker(m_chatTranscript, infoRequests);
      AppendAssistantNote(AssistantNoteKind::InfoResponsesProvided, wxEmptyString, nullptr, &infoRequests);

      if (m_fullInfoRequest) {
        m_chatTranscript << wxT("\n\n") << allResponses;

        AppendChatEvent("info_response", allResponses);
      }

      wxString promptForModel = BuildPromptForModel(m_chatTranscript, wxEmptyString, outOfBand);

      m_solveSession.iteration++;
      if (CheckNumberOfIterations()) {
        client->SimpleChatAsync(interactiveChatSystemPrompt, promptForModel, this);
      } else {
        return true;
      }

    } else if (m_solveSession.action == Action::OptimizeFunctionOrMethod) {
      wxString newTranscript;
      newTranscript << m_solveSession.transcript;
      appendInfoRequestMarker(newTranscript, infoRequests);
      m_solveSession.transcript = newTranscript;

      AppendAssistantNote(AssistantNoteKind::InfoResponsesProvided, wxEmptyString, nullptr, &infoRequests);

      if (m_fullInfoRequest) {
        m_solveSession.transcript << wxT("\n\n") << allResponses;
        AppendChatEvent("info_response", allResponses);
      }

      wxString promptForModel = BuildPromptForModel(m_solveSession.transcript, wxEmptyString, outOfBand);

      m_solveSession.iteration++;
      if (CheckNumberOfIterations()) {
        client->SimpleChatAsync(optimizeFunctionSystemPrompt, promptForModel, this);
      } else {
        return true;
      }
    }

    return false; // expecting next AI response
  }
}

// -------------------------------------------------------------------------------
std::string ArduinoAiActions::GetSketchRoot() const {
  if (!m_editor)
    return std::string();

  return m_editor->arduinoCli->GetSketchPath();
}

std::string ArduinoAiActions::GetCurrentCode() {
  if (m_solveSession.basename.empty() || m_solveSession.workingFiles.size() < 1) {
    return m_editor->GetText();
  } else {
    std::string sketchRoot = GetSketchRoot();
    std::string normName = NormalizeFilename(sketchRoot, wxToStd(m_solveSession.basename));

    for (auto &buff : m_solveSession.workingFiles) {
      if (normName == NormalizeFilename(sketchRoot, buff.filename)) {
        return buff.code;
      }
    }

    return {};
  }
}

SketchFileBuffer *ArduinoAiActions::FindBufferWithFile(const std::string &filename, bool allowCreate) {
  std::string sketchRoot = GetSketchRoot();
  std::string normName = NormalizeFilename(sketchRoot, filename);
  for (auto &buff : m_solveSession.workingFiles) {
    const std::string bufName = NormalizeFilename(sketchRoot, buff.filename);
    if (normName == bufName) {
      return (SketchFileBuffer *)&buff;
    }
  }

  if (allowCreate) {
    SketchFileBuffer buff;
    buff.filename = normName;
    buff.code.clear();
    m_solveSession.workingFiles.push_back(std::move(buff));
    return &m_solveSession.workingFiles.back();
  } else {
    return nullptr;
  }
}

wxStyledTextCtrl *ArduinoAiActions::GetStc() {
  return m_editor->m_editor;
}

// *** event handlers ***
void ArduinoAiActions::OnAiSimpleChatSuccess(wxThreadEvent &event) {
  if (!m_editor) {
    return;
  }

  wxString reply = TrimCopy(event.GetString());
  if (reply.IsEmpty()) {
    StopCurrentAction();
    return;
  }

  Action action = m_currentAction;

  switch (action) {
    case Action::ExplainSelection: {

      StopCurrentAction();

      AiExplainPopup dlg(m_editor->GetOwnerFrame(), m_editor->m_config, reply);
      dlg.ShowModal();
      break;
    }

    case Action::GenerateDocComment: {
      StopCurrentAction();
      InsertAiGeneratedDocComment(reply);
      break;
    }

    case Action::SolveProjectError: {
      bool done = ApplyAiModelSolution(reply);
      if (!done) {
        return;
      }

      m_solveSession.finished = true;
      StopCurrentAction();
      RebuildProject();
      break;
    }

    case Action::InteractiveChat: {
      bool done = ApplyAiModelSolution(reply);
      if (!done) {
        return;
      }

      SendDoneEventToOrigin();

      m_solveSession.finished = true;
      StopCurrentAction();
      break;
    }

    case Action::GenerateDocCommentsInFile:
    case Action::GenerateDocCommentsInClass: {
      // 1) Save AI repply to current symbol
      if (m_docBatchIndex < m_docBatchSymbols.size()) {
        PendingDocComment pd;
        pd.symbol = m_docBatchSymbols[m_docBatchIndex];
        pd.reply = reply;
        m_pendingDocComments.push_back(std::move(pd));
      }

      // 2) Move in batch
      m_docBatchIndex++;
      if (m_docBatchIndex < m_docBatchSymbols.size()) {
        StartAction(action);
        if (!StartDocCommentForSymbol(m_docBatchSymbols[m_docBatchIndex])) {
          StopCurrentAction();
          m_editor->ModalMsgDialog(
              _("Failed to start AI request for next symbol."),
              _("AI generate documentation"),
              wxOK | wxICON_ERROR);
        }
        return;
      }

      // 3) All responses collected -> apply
      FinalizeBatchDocComments(action);
      StopCurrentAction();
      RebuildProject();
      break;
    }

    case Action::OptimizeFunctionOrMethod: {
      bool done = ApplyAiModelSolution(reply);
      if (!done) {
        return;
      }

      m_solveSession.finished = true;
      StopCurrentAction();
      RebuildProject();
      break;
    }

    case Action::None:
    default:
      StopCurrentAction();
      if (!reply.IsEmpty()) {
        m_editor->ModalMsgDialog(
            reply,
            _("Arduino Editor AI"),
            wxOK | wxICON_INFORMATION);
      }
      break;
  }
}

void ArduinoAiActions::OnAiSimpleChatError(wxThreadEvent &event) {
  if (!m_editor) {
    return;
  }

  wxString err = event.GetString();
  if (err.IsEmpty()) {
    err = _("Unknown AI error.");
  }

  Action action = m_currentAction;
  StopCurrentAction();

  switch (action) {
    case Action::ExplainSelection:
      m_editor->ModalMsgDialog(
          _("AI request failed:\n") + err,
          _("AI explain selection"),
          wxOK | wxICON_ERROR);
      break;
    case Action::GenerateDocComment:
      m_editor->ModalMsgDialog(
          _("AI request failed:\n") + err,
          _("AI generate documentation"),
          wxOK | wxICON_ERROR);
      break;
    case Action::SolveProjectError:
      m_editor->ModalMsgDialog(
          _("AI request failed:\n") + err,
          _("AI solve project errors"),
          wxOK | wxICON_ERROR);
      break;
    case Action::InteractiveChat:
      AppendChatEvent("error", err);
      if (m_origin) {
        wxThreadEvent evt(wxEVT_AI_SIMPLE_CHAT_ERROR);
        evt.SetString(err);
        wxPostEvent(m_origin, evt);
      } else {
        m_editor->ModalMsgDialog(
            _("AI request failed:\n") + err,
            _("AI interactive chat"),
            wxOK | wxICON_ERROR);
      }
      break;
    case Action::GenerateDocCommentsInFile:
    case Action::GenerateDocCommentsInClass:
      m_editor->ModalMsgDialog(
          _("AI request failed:\n") + err,
          _("AI generate documentation"),
          wxOK | wxICON_ERROR);
      break;
    case Action::OptimizeFunctionOrMethod:
      m_editor->ModalMsgDialog(
          _("AI request failed:\n") + err,
          _("AI optimize function or method"),
          wxOK | wxICON_ERROR);
      break;
    case Action::None:
    default:
      m_editor->ModalMsgDialog(
          _("AI request failed:\n") + err,
          _("Arduino Editor AI"),
          wxOK | wxICON_ERROR);
      break;
  }
}

void ArduinoAiActions::OnDiagnosticsUpdated(wxThreadEvent &evt) {
  APP_DEBUG_LOG("AI: OnDiagnosticsUpdated()");

  Unbind(EVT_DIAGNOSTICS_UPDATED, &ArduinoAiActions::OnDiagnosticsUpdated, this);

  if (!m_editor) {
    return;
  }

  auto *frame = m_editor->GetOwnerFrame();
  if (!frame) {
    return;
  }

  std::string sketchRoot = GetSketchRoot();
  auto errors = evt.GetPayload<std::vector<ArduinoParseError>>();

  const ArduinoParseError *best = nullptr;
  for (const auto &e : errors) {
    if (e.severity < CXDiagnostic_Error) {
      continue;
    }

    if (!best) {
      best = &e;
      continue;
    }

    if (e.line < best->line) {
      best = &e;
      continue;
    }
    if (e.line > best->line) {
      continue;
    }

    if (e.column < best->column) {
      best = &e;
      continue;
    }
  }

  SketchFileBuffer *buf = nullptr;
  if (best) {
    buf = FindBufferWithFile(best->file);
  }

  if (!best || !buf) {
    APP_DEBUG_LOG("AI: no errors after AI patch...");

    std::vector<SketchFileBuffer> oldFiles;
    frame->CollectEditorSources(oldFiles);

    ArduinoDiffDialog dd(frame, oldFiles, m_solveSession.workingFiles, m_editor->arduinoCli, m_editor->m_config, m_solveSession.assistantPatchExplanation);
    int resl = dd.ShowModal();
    if (resl != wxID_OK) {
      m_interactiveChatPayload.Append(_("\n\n<patch rejected>"));
      AppendChatEvent("assistant", wxT("<patch rejected>"));
      AppendAssistantNote(AssistantNoteKind::PatchRejected, dd.GetAdditionalInfo());

      m_solveSession.finished = true;
      SendDoneEventToOrigin();
      StopCurrentAction();
      return;
    }

    // Apply workingFiles to live files
    for (const auto &newSfb : m_solveSession.workingFiles) {
      std::string filename = NormalizeFilename(sketchRoot, newSfb.filename);
      std::string basename = StripFilename(sketchRoot, filename);

      ArduinoEditor *pathEd = frame->FindEditorWithFile(filename, /*allowCreate=*/true);

      if (!pathEd) {
        // new file
        frame->CreateNewSketchFile(wxString::FromUTF8(filename));

        pathEd = frame->GetCurrentEditor();

        if (!pathEd || (pathEd->GetFilePath() != filename)) {
          m_editor->ModalMsgDialog(_("New sketch file can not be created!"), _("Arduino Editor AI"));
          continue;
        }
      }

      pathEd->SetText(newSfb.code);

      // 3) summary to chat
      m_interactiveChatPayload.Append(
          wxString::Format(_("\n\n<patch applied into file %s>\n"), wxString::FromUTF8(basename)));
    }

    AppendAssistantNote(AssistantNoteKind::PatchApplied, wxEmptyString);

    // persistent history
    wxString msg = wxT("\n<patch applied>\n");
    AppendChatEvent("assistant", msg);

    // Floating window cleanup for interactive chat:
    // after applying a patch we drop older history and keep only the
    // last "USER_MESSAGE -> PATCH" window in m_chatTranscript.
    if (m_solveSession.action == Action::InteractiveChat) {
      TrimTranscriptToLastPatchWindow(m_chatTranscript);
    }

    m_solveSession.finished = true;
    SendDoneEventToOrigin();
    StopCurrentAction();

    frame->RebuildProject();
  } else {

    m_solveSession.iteration++;

    if (!CheckNumberOfIterations()) {
      StopCurrentAction();
      return;
    }

    auto *client = GetClient();
    if (!client) {
      StopCurrentAction();
      return;
    }

    // Solve errors
    TrimTranscriptToLastPatchWindow(m_chatTranscript);

    std::string basename = StripFilename(sketchRoot, best->file);
    wxString ctx = GetNumberedContextAroundLine(wxString::FromUTF8(buf->code), (int)best->line - 1, 150);

    m_solveSession.basename = wxString::FromUTF8(basename);

    wxString diagMsg;
    diagMsg << wxT("The IDE reports compiler errors after applying the previous patch.\n");
    diagMsg << wxT("COMPILER_ERROR:\n") << wxString::FromUTF8(best->ToString()) << wxT("\n");

    diagMsg << wxT("\nCODE_CONTEXT (with 1-based line numbers):\n```cpp\n");
    diagMsg << ctx;
    diagMsg << wxT("```\n");

    // Persist into transcript so INFO_REQUEST rounds don't lose the error.
    if (m_solveSession.action == Action::InteractiveChat) {
      if (!m_chatTranscript.IsEmpty()) {
        m_chatTranscript << wxT("\n\n");
      }
      m_chatTranscript << wxT("USER_MESSAGE:\n") << diagMsg;

      wxString chatMsg = _("<IDE forced error solving>");
      AppendChatEvent("user", chatMsg);

      m_interactiveChatPayload.Append(wxT("\n"));
      m_interactiveChatPayload.Append(chatMsg);

      wxString promptForModel = BuildPromptForModel(m_chatTranscript, wxEmptyString, wxEmptyString);
      if (!client->SimpleChatAsync(solveErrorSystemPrompt, promptForModel, this)) {
        StopCurrentAction();

        m_editor->ModalMsgDialog(_("Failed to start AI request."), _("AI solve project errors"));
      }

    } else {
      if (!m_solveSession.transcript.IsEmpty()) {
        m_solveSession.transcript << wxT("\n\n");
      }
      m_solveSession.transcript << wxT("USER_MESSAGE:\n") << diagMsg;

      wxString promptForModel = BuildPromptForModel(m_solveSession.transcript, wxEmptyString, wxEmptyString);

      if (!client->SimpleChatAsync(solveErrorSystemPrompt, promptForModel, this)) {
        StopCurrentAction();

        m_editor->ModalMsgDialog(_("Failed to start AI request."), _("AI solve project errors"));
      }
    }
  }
}

void ArduinoAiActions::AppendChatEvent(const char *type, const wxString &text, int inputTokens, int outputTokens, int totalTokens) {
  if (!m_editor || !m_editor->m_aiSettings.storeChatHistory)
    return;
  if (!m_chatSessionOpen || m_chatSessionId.empty())
    return;

  const std::string sketchRoot = GetSketchRoot();
  wxFileName sessionPath = GetSessionJsonlPath(sketchRoot, m_chatSessionId);

  json ev = {
      {"t", type},
      {"tsUtc", NowUtcIso8601()},
      {"model", wxToStd(m_editor->m_aiSettings.model)},
      {"text", wxToStd(text)}};

  if (inputTokens >= 0)
    ev["inputTokens"] = inputTokens;
  if (outputTokens >= 0)
    ev["outputTokens"] = outputTokens;
  if (totalTokens >= 0)
    ev["totalTokens"] = totalTokens;

  AppendJsonlLine(sessionPath, ev);

  // bump messageCount in index only for user/assistant (no meta/info)
  if (strcmp(type, "user") == 0 || strcmp(type, "assistant") == 0) {
    json idx = LoadIndexJson(sketchRoot);
    for (auto &s : idx["sessions"]) {
      if (s.is_object() && s.value("id", "") == m_chatSessionId) {
        int c = s.value("messageCount", 0);
        s["messageCount"] = c + 1;
        break;
      }
    }
    SaveIndexJsonAtomic(sketchRoot, idx);
  }
}

wxString ArduinoAiActions::GetPromptCurrentFile() const {
  // Prefer session basename if set (SolveProjectError / Optimize... typically set it).
  if (!m_solveSession.basename.IsEmpty()) {
    return m_solveSession.basename;
  }

  if (m_editor) {
    // Editor stores relative filename (sketch-relative) in GetFileName()
    return wxString::FromUTF8(m_editor->GetFileName());
  }

  return wxEmptyString;
}

wxString ArduinoAiActions::BuildStablePromptHeader() const {
  wxString h;

  const wxString curFile = GetPromptCurrentFile();
  if (!curFile.IsEmpty()) {
    h << wxT("CURRENT_FILE: ") << curFile << wxT("\n");
  }

  if (m_editor) {
    h << wxT("CURRENT_BOARD_FQBN: ")
      << wxString::FromUTF8(m_editor->arduinoCli->GetFQBN())
      << wxT("\n");
  }

  return h;
}

wxString ArduinoAiActions::BuildPromptForModel(const wxString &baseTranscript,
                                               const wxString &extraEphemeral,
                                               const wxString &outOfBandBlocks) const {
  wxString p = baseTranscript;

  // Always keep stable header present in every roundtrip.
  wxString stable = BuildStablePromptHeader();
  if (!stable.IsEmpty()) {
    p << wxT("\n\n") << stable;
  }

  if (!extraEphemeral.IsEmpty()) {
    p << wxT("\n") << extraEphemeral;
  }

  if (!outOfBandBlocks.IsEmpty()) {
    p << wxT("\n") << outOfBandBlocks;
  }

  return p;
}

wxString ArduinoAiActions::BuildAppliedPatchEvidence(const std::vector<AiPatchHunk> &patches, int extraContextLines, int maxTotalLines) {
  wxString out;
  out << wxT("\n*** BEGIN APPLIED_PATCH_EVIDENCE\n");
  out << wxT("NOTE: This is the post-apply state of touched ranges (with 1-based line numbers).\n");

  int emittedLines = 0;

  for (const auto &p : patches) {
    if (p.file.IsEmpty() || p.fromLine <= 0 || p.toLine <= 0) {
      continue;
    }

    const SketchFileBuffer *wf = FindBufferWithFile(wxToStd(p.file));
    if (!wf) {
      continue;
    }

    const wxString allText = wxString::FromUTF8(wf->code);

    const int from0 = std::max(0, (int)p.fromLine - 1 - extraContextLines);
    const int to0 = std::max(0, (int)p.toLine - 1 + extraContextLines);

    wxString snippet = GetNumberedContext(allText, from0, to0);

    // crude global cap (line counting by '\n')
    int linesHere = 0;
    for (wxUniChar c : snippet)
      if (c == '\n')
        ++linesHere;
    if (emittedLines + linesHere > maxTotalLines) {
      out << wxString::Format(wxT("\nFILE: %s RANGE: %d-%d\n<evidence truncated due to size>\n"),
                              p.file, p.fromLine, p.toLine);
      break;
    }

    out << wxString::Format(wxT("\nFILE: %s RANGE: %d-%d\n```cpp\n"),
                            p.file, p.fromLine, p.toLine);
    out << snippet;
    out << wxT("```\n");

    emittedLines += linesHere;
  }

  out << wxT("*** END APPLIED_PATCH_EVIDENCE\n");
  return out;
}
