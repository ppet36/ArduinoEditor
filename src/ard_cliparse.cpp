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

#include "ard_cliparse.hpp"
#include "ard_cc.hpp"
#include "utils.hpp"
#include <clang-c/Index.h>

#include <algorithm>
#include <cctype>

static inline bool IsAllDigits(const std::string &s) {
  if (s.empty())
    return false;
  for (unsigned char c : s)
    if (!std::isdigit(c))
      return false;
  return true;
}

static std::string BaseNameLower(std::string p) {
  // strip trailing spaces
  TrimInPlace(p);

  // last path component
  size_t s1 = p.find_last_of('/');
  size_t s2 = p.find_last_of('\\');
  size_t s = (s1 == std::string::npos) ? s2 : (s2 == std::string::npos ? s1 : std::max(s1, s2));
  std::string b = (s == std::string::npos) ? p : p.substr(s + 1);

  // lowercase
  std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return b;
}

static ArduinoParseError MakeNote(const std::string &file, unsigned line, unsigned col, std::string msg) {
  ArduinoParseError e;
  e.file = file;
  e.line = line;
  e.column = col;
  e.message = std::move(msg);
  e.severity = CXDiagnostic_Note;
  return e;
}

// Parse "file:123:(...)" -> file + line (no column)
static bool ParseLdFileLineBeforeParen(const std::string &s, std::string &outFile, unsigned &outLine) {
  // find ":(" which usually starts section like ":(.text...)" / ":(.literal...)"
  size_t colonParen = s.find(":(");
  if (colonParen == std::string::npos)
    return false;

  // digits directly before colonParen
  size_t p = colonParen;
  if (p == 0)
    return false;

  size_t d = p;
  while (d > 0 && std::isdigit((unsigned char)s[d - 1]))
    --d;

  if (d == p)
    return false; // no digits

  if (d == 0 || s[d - 1] != ':')
    return false; // not "...:<digits>:("

  size_t colonBeforeDigits = d - 1;
  std::string file = s.substr(0, colonBeforeDigits);
  TrimInPlace(file);

  std::string num = s.substr(d, p - d);
  if (!IsAllDigits(num))
    return false;

  outFile = std::move(file);
  outLine = (unsigned)std::stoul(num);
  return true;
}

static size_t FindFirstLdKeywordPos(const std::string &s) {
  // extend as needed
  const char *keys[] = {
      "undefined reference to",
      "multiple definition of",
      "cannot find",
  };

  size_t best = std::string::npos;
  for (auto k : keys) {
    size_t p = s.find(k);
    if (p != std::string::npos && (best == std::string::npos || p < best))
      best = p;
  }
  return best;
}

// Returns true if line is ld-like and was consumed.
// outIsMain=true  => emit as main error (file/line filled when possible)
// outIsMain=false => keep as pending note (child of next main error / collect2)
static bool TryParseLdLikeLine(const std::string &lineRaw, ArduinoParseError &outErr, bool &outIsMain) {
  std::string s = lineRaw;
  if (!s.empty() && s.back() == '\r')
    s.pop_back();

  std::string work = s;

  // Optional "…/ld: " prefix
  size_t sep = s.find(": ");
  if (sep != std::string::npos) {
    std::string tool = s.substr(0, sep);
    std::string bn = BaseNameLower(tool);
    if (bn == "ld" || bn == "ld.exe") {
      work = s.substr(sep + 2);
      TrimInPlace(work);
    }
  }

  // ld context line: "<obj>: in function `...':"
  {
    size_t pos = work.find(": in function `");
    if (pos != std::string::npos) {
      std::string f = work.substr(0, pos);
      std::string msg = work.substr(pos + 2); // skip ": "
      TrimInPlace(f);
      TrimInPlace(msg);

      outErr = MakeNote(f, 0, 0, msg);
      outIsMain = false; // pending note for next undefined reference
      return true;
    }
  }

  // Actual ld error-ish lines
  size_t kw = FindFirstLdKeywordPos(work);
  if (kw == std::string::npos)
    return false;

  // "file:line:(section): undefined reference ..."
  std::string srcFile;
  unsigned srcLine = 0;
  if (ParseLdFileLineBeforeParen(work, srcFile, srcLine)) {
    // Keep section info: "(.text....): undefined reference ..."
    size_t lp = work.find(":(");
    std::string msg = (lp != std::string::npos) ? work.substr(lp + 1) : work.substr(kw);
    TrimInPlace(msg);

    outErr.file = std::move(srcFile);
    outErr.line = srcLine;
    outErr.column = 0;
    outErr.message = std::move(msg);
    outErr.severity = CXDiagnostic_Error;
    outErr.childs.clear();

    outIsMain = true;
    return true;
  }

  // "…cpp.o:(section): undefined reference ..."  -> keep as note (child)
  if (work.find("undefined reference to") != std::string::npos ||
      work.find("multiple definition of") != std::string::npos) {

    // try set file to object path (before ":(")
    size_t cp = work.find(":(");
    std::string f;
    if (cp != std::string::npos) {
      f = work.substr(0, cp);
      TrimInPlace(f);
    }

    size_t lp = work.find(":(");
    std::string msg = (lp != std::string::npos) ? work.substr(lp + 1) : work.substr(kw);
    TrimInPlace(msg);

    outErr = MakeNote(f, 0, 0, msg);
    outIsMain = false;
    return true;
  }

  // "cannot find ..." etc – no source location -> this *is* a main error
  {
    std::string msg = work.substr(kw);
    TrimInPlace(msg);

    outErr.file.clear();
    outErr.line = 0;
    outErr.column = 0;
    outErr.message = std::move(msg);
    outErr.severity = CXDiagnostic_Error;
    outErr.childs.clear();

    outIsMain = true;
    return true;
  }
}

static bool ParseFileLineColTail(std::string tail, std::string &outFile, unsigned &outLine, unsigned &outCol) {
  TrimInPlace(tail);
  while (!tail.empty() && tail.back() == ':')
    tail.pop_back();
  TrimInPlace(tail);

  if (tail.empty())
    return false;

  auto last = tail.rfind(':');
  if (last == std::string::npos)
    return false;

  std::string a = tail.substr(0, last);
  std::string b = tail.substr(last + 1);
  TrimInPlace(a);
  TrimInPlace(b);
  if (!IsAllDigits(b))
    return false;

  auto prev = a.rfind(':');
  if (prev != std::string::npos) {
    std::string maybeLine = a.substr(prev + 1);
    std::string maybeFile = a.substr(0, prev);
    TrimInPlace(maybeLine);
    TrimInPlace(maybeFile);

    if (IsAllDigits(maybeLine)) {
      outFile = maybeFile;
      outLine = (unsigned)std::stoul(maybeLine);
      outCol = (unsigned)std::stoul(b);
      return true;
    }
  }

  outFile = a;
  outLine = (unsigned)std::stoul(b);
  outCol = 0;
  return true;
}

static CXDiagnosticSeverity MapSeverity(const std::string &sev) {
  if (sev == "warning")
    return CXDiagnostic_Warning;
  if (sev == "note")
    return CXDiagnostic_Note;
  if (sev == "fatal error")
    return CXDiagnostic_Fatal;
  return CXDiagnostic_Error;
}

struct ParsedDiag {
  bool ok = false;
  std::string file;
  unsigned line = 0;
  unsigned col = 0;
  std::string sevText; // "error", "warning", "note", "fatal error"
  std::string msg;
};

static ParsedDiag TryParseGccClangDiagLine(const std::string &lineRaw) {
  ParsedDiag out;
  std::string s = lineRaw;
  if (!s.empty() && s.back() == '\r')
    s.pop_back();

  struct Marker {
    const char *text;
    const char *sev;
  };
  static const Marker markers[] = {
      {": fatal error:", "fatal error"},
      {": error:", "error"},
      {": warning:", "warning"},
      {": note:", "note"},
  };

  size_t bestPos = std::string::npos;
  const Marker *best = nullptr;
  for (const auto &m : markers) {
    size_t p = s.find(m.text);
    if (p != std::string::npos && (bestPos == std::string::npos || p < bestPos)) {
      bestPos = p;
      best = &m;
    }
  }
  if (!best)
    return out;

  std::string loc = s.substr(0, bestPos);
  std::string msg = s.substr(bestPos + std::strlen(best->text));
  TrimInPlace(loc);
  TrimInPlace(msg);

  std::string file;
  unsigned ln = 0, col = 0;
  if (ParseFileLineColTail(loc, file, ln, col)) {
    out.ok = true;
    out.file = file;
    out.line = ln;
    out.col = col;
    out.sevText = best->sev;
    out.msg = msg;
    return out;
  }

  out.ok = true;
  out.file.clear();
  out.line = 0;
  out.col = 0;
  out.sevText = best->sev;
  out.msg = msg.empty() ? s : msg;
  return out;
}

std::vector<ArduinoParseError> ArduinoCliOutputParser::ParseCliOutput(const std::string &fullText) {
  std::vector<ArduinoParseError> out;
  std::vector<ArduinoParseError> pendingNotes;

  ArduinoParseError *lastMain = nullptr;

  size_t i = 0;
  while (i < fullText.size()) {
    size_t eol = fullText.find('\n', i);
    if (eol == std::string::npos)
      eol = fullText.size();
    std::string line = fullText.substr(i, eol - i);
    i = (eol < fullText.size()) ? (eol + 1) : eol;

    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    {
      auto pos = line.find(": In function ");
      if (pos != std::string::npos) {
        std::string file = line.substr(0, pos);
        std::string msg = line.substr(pos + 2); // skip ": "
        TrimInPlace(file);
        TrimInPlace(msg);
        pendingNotes.push_back(MakeNote(file, 0, 0, msg));
        continue;
      }
    }

    if (startsWithCaseInsensitive(line, "In file included from ")) {
      std::string tail = line.substr(std::strlen("In file included from "));
      std::string f;
      unsigned ln = 0, col = 0;
      if (ParseFileLineColTail(tail, f, ln, col)) {
        pendingNotes.push_back(MakeNote(f, ln, col, "Included from here"));
        continue;
      }
    }
    {
      std::string trimmed = line;
      TrimInPlace(trimmed);
      if (startsWithCaseInsensitive(trimmed, "from ")) {
        std::string tail = trimmed.substr(5);
        std::string f;
        unsigned ln = 0, col = 0;
        if (ParseFileLineColTail(tail, f, ln, col)) {
          pendingNotes.push_back(MakeNote(f, ln, col, "Included from here"));
          continue;
        }
      }
    }

    // ---- ld / linker diagnostics (undefined reference, in function, etc.) ----
    {
      ArduinoParseError ldErr;
      bool ldIsMain = false;
      if (TryParseLdLikeLine(line, ldErr, ldIsMain)) {
        if (ldIsMain) {
          if (!pendingNotes.empty()) {
            ldErr.childs.insert(ldErr.childs.end(), pendingNotes.begin(), pendingNotes.end());
            pendingNotes.clear();
          }
          out.push_back(std::move(ldErr));
          lastMain = &out.back();
        } else {
          // keep as note for the next main linker error (or will attach to collect2 error)
          pendingNotes.push_back(std::move(ldErr));
        }
        continue;
      }
    }

    ParsedDiag d = TryParseGccClangDiagLine(line);
    if (d.ok) {
      ArduinoParseError e;
      e.file = d.file;
      e.line = d.line;
      e.column = d.col;
      e.message = d.msg;
      e.severity = MapSeverity(d.sevText);

      const bool isNote = (e.severity == CXDiagnostic_Note);

      if (!isNote) {
        if (!pendingNotes.empty()) {
          e.childs.insert(e.childs.end(), pendingNotes.begin(), pendingNotes.end());
          pendingNotes.clear();
        }
        out.push_back(std::move(e));
        lastMain = &out.back();
      } else {
        if (lastMain)
          lastMain->childs.push_back(std::move(e));
        else
          pendingNotes.push_back(std::move(e));
      }
      continue;
    }
  }

  return out;
}
