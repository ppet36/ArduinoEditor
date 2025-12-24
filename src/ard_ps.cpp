#include "ard_ps.hpp"
#include "ard_ai.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

static inline bool StartsWith(const std::string &s, const char *pfx) {
  const size_t n = std::strlen(pfx);
  return s.size() >= n && std::equal(pfx, pfx + n, s.begin());
}

static inline void TrimBoth(wxString &s) {
  s.Trim(true);
  s.Trim(false);
}

static inline bool IsSpaceChar(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline bool IsPunctNoSpace(char c) {
  switch (c) {
    case '(':
    case ')':
    case '{':
    case '}':
    case '[':
    case ']':
    case ';':
    case ',':
    case ':':
      return true;
    default:
      return false;
  }
}

// ---------- Working-file rebuild/apply (stateful) ----------

void ArduinoPatchStraightener::RebuildFileLines(ArduinoPatchStraightener::FileLines &fl, const wxString &code) {
  fl.text = ArduinoPatchStraightener::NormalizeNewlinesToLF(code);

  fl.rawLines.clear();
  ArduinoPatchStraightener::SplitLinesLF(fl.text, fl.rawLines);

  fl.normLines.clear();
  fl.positions.clear();

  fl.normLines.reserve(fl.rawLines.size());
  for (size_t i = 0; i < fl.rawLines.size(); ++i) {
    const std::string norm = ArduinoPatchStraightener::NormalizeLineKey(fl.rawLines[i]);
    fl.normLines.push_back(norm);
    fl.positions[norm].push_back((int)i);
  }
}

void ArduinoPatchStraightener::ApplyHunkToWorkingFile(ArduinoPatchStraightener::FileLines &fl, const AiPatchHunk &h) {
  // Semantics:
  // - if h.toLine == 0 => insertion at h.fromLine (1-based), no deletion
  // - else replace inclusive [fromLine..toLine]
  std::vector<wxString> src = fl.rawLines;

  wxString repl = NormalizeNewlinesToLF(h.replacement);
  std::vector<wxString> replLines;
  SplitLinesLF(repl, replLines);

  const int n = (int)src.size();

  int from0 = 0;
  int to0 = -1;

  if (h.toLine == 0) {
    from0 = (h.fromLine > 0) ? (h.fromLine - 1) : 0;
    to0 = from0 - 1;
  } else {
    from0 = (h.fromLine > 0) ? (h.fromLine - 1) : 0;
    to0 = (h.toLine > 0) ? (h.toLine - 1) : (from0 - 1);
  }

  // clamp
  from0 = std::max(0, std::min(from0, n)); // allow insert at end
  to0 = std::max(-1, std::min(to0, n - 1));
  if (to0 < from0 - 1)
    to0 = from0 - 1;

  std::vector<wxString> out;
  out.reserve(src.size() + replLines.size());

  for (int i = 0; i < from0 && i < n; ++i)
    out.push_back(src[i]);
  for (auto &s : replLines)
    out.push_back(s);
  for (int i = to0 + 1; i < n; ++i)
    out.push_back(src[i]);

  RebuildFileLines(fl, JoinLinesLF(out));
}

// ---------- Loose normalization for matching (alignment scoring) ----------

std::string ArduinoPatchStraightener::NormalizeLineKeyLoose(const wxString &line) {
  wxString s = line;
  s.Replace(wxT("\t"), wxT(" "));
  TrimBoth(s);

  std::string u = wxToStd(s);
  if (u.empty())
    return u;

  // Collapse spaces OUTSIDE string literals, and remove spaces around (){}[];,: outside strings
  std::string out;
  out.reserve(u.size());

  bool inStr = false;
  char quote = 0;
  bool esc = false;
  bool pendingSpace = false;

  auto flushSpaceIfNeeded = [&]() {
    if (pendingSpace) {
      // don't emit space before punctuation
      if (!out.empty() && IsPunctNoSpace(out.back())) {
        pendingSpace = false;
        return;
      }
      out.push_back(' ');
      pendingSpace = false;
    }
  };

  for (size_t i = 0; i < u.size(); ++i) {
    char c = u[i];

    if (!inStr) {
      if (c == '"' || c == '\'') {
        flushSpaceIfNeeded();
        inStr = true;
        quote = c;
        esc = false;
        out.push_back(c);
        continue;
      }

      if (IsSpaceChar(c)) {
        pendingSpace = true;
        continue;
      }

      if (IsPunctNoSpace(c)) {
        // drop pending space before punct
        pendingSpace = false;
        // also drop space before punct already in output
        if (!out.empty() && out.back() == ' ')
          out.pop_back();
        out.push_back(c);
        continue;
      }

      // normal char
      flushSpaceIfNeeded();
      out.push_back(c);
    } else {
      // inside string: keep verbatim
      out.push_back(c);
      if (esc) {
        esc = false;
        continue;
      }
      if (c == '\\') {
        esc = true;
        continue;
      }
      if (c == quote) {
        inStr = false;
        continue;
      }
    }
  }

  // trim trailing space
  while (!out.empty() && out.back() == ' ')
    out.pop_back();

  return out;
}

// ---------- Alignment: find best source interval to replace with patchLines ----------

int ArduinoPatchStraightener::MatchScoreLine(const std::string &pStrict,
                                             const std::string &pLoose,
                                             const std::string &sStrict,
                                             const std::string &sLoose,
                                             int minStrongLen) {
  if (pStrict.empty() || sStrict.empty())
    return -4;

  const bool strictEq = (pStrict == sStrict);
  const bool looseEq = (!strictEq && !pLoose.empty() && pLoose == sLoose);

  if (!strictEq && !looseEq)
    return -5;

  const bool strong = IsStrongNormalizedLine(sStrict, minStrongLen) && IsStrongNormalizedLine(pStrict, minStrongLen);

  const bool weak = IsWeakNormalizedLine(sStrict) || IsWeakNormalizedLine(pStrict);

  if (strictEq) {
    if (strong)
      return 10;
    if (weak)
      return 3;
    return 6;
  } else { // looseEq
    if (strong)
      return 7;
    if (weak)
      return 2;
    return 4;
  }
}

ArduinoPatchStraightener::AlignResult ArduinoPatchStraightener::FindBestReplaceIntervalByAlignment(const ArduinoPatchStraightener::FileLines &file,
                                                                                                   const std::vector<wxString> &patchLines,
                                                                                                   int hintFrom1,
                                                                                                   int hintTo1,
                                                                                                   const ArduinoPatchStraightener::Options &opt) {
  AlignResult ar;

  const int fileLen = (int)file.rawLines.size();
  const int n = (int)patchLines.size();
  if (fileLen <= 0) {
    // empty file => pure insert at 1
    ar.ok = true;
    ar.isInsert = true;
    ar.insertAt1 = 1;
    ar.score = 1;
    return ar;
  }
  if (n <= 0) {
    // empty replacement => deletion; keep hint if valid else no-op
    if (hintFrom1 >= 1 && hintTo1 >= hintFrom1 && hintTo1 <= fileLen) {
      ar.ok = true;
      ar.from1 = hintFrom1;
      ar.to1 = hintTo1;
      ar.score = 1;
    }
    return ar;
  }

  // Window around hint, fallback to whole file
  int lo0 = 0;
  int hi0 = fileLen - 1;
  const bool hintValid = (hintFrom1 >= 1 && hintTo1 >= hintFrom1 && hintTo1 <= fileLen);

  if (hintValid) {
    lo0 = std::max(0, (hintFrom1 - 1) - opt.windowAroundOriginal);
    hi0 = std::min(fileLen - 1, (hintTo1 - 1) + opt.windowAroundOriginal);
  }

  const int m = hi0 - lo0 + 1;
  if (m <= 0)
    return ar;

  // Precompute patch keys
  std::vector<std::string> pStrict(n), pLoose(n);
  for (int i = 0; i < n; ++i) {
    pStrict[i] = ArduinoPatchStraightener::NormalizeLineKey(patchLines[i]);
    pLoose[i] = NormalizeLineKeyLoose(patchLines[i]);
  }

  // Precompute source loose keys for window; strict already exists in file.normLines
  std::vector<std::string> sLoose(m);
  for (int j = 0; j < m; ++j) {
    sLoose[j] = NormalizeLineKeyLoose(file.rawLines[lo0 + j]);
  }

  // Smith–Waterman DP
  const int GAP = -6;

  std::vector<int> prev(m + 1, 0), cur(m + 1, 0);
  std::vector<uint8_t> dir((n + 1) * (m + 1), 0); // 0 stop, 1 diag, 2 up, 3 left

  int best = 0;
  int bestI = 0, bestJ = 0;

  int hintMid0 = (hintValid ? ((hintFrom1 - 1) + (hintTo1 - 1)) / 2 : (lo0 + hi0) / 2);

  for (int i = 1; i <= n; ++i) {
    cur[0] = 0;
    for (int j = 1; j <= m; ++j) {
      const int ms = MatchScoreLine(
          pStrict[i - 1], pLoose[i - 1],
          file.normLines[lo0 + (j - 1)], sLoose[j - 1],
          opt.minStrongLineLen);

      int diag = prev[j - 1] + ms;
      int up = prev[j] + GAP;
      int left = cur[j - 1] + GAP;

      int v = 0;
      uint8_t d = 0;
      if (diag >= up && diag >= left && diag > 0) {
        v = diag;
        d = 1;
      } else if (left >= up && left > 0) {
        v = left;
        d = 3;
      } else if (up > 0) {
        v = up;
        d = 2;
      }

      cur[j] = v;
      dir[i * (m + 1) + j] = d;

      if (v > best) {
        best = v;
        bestI = i;
        bestJ = j;
      } else if (v == best && v > 0) {
        // tie-break: closer to hint mid
        int srcPos0 = lo0 + (j - 1);
        int bestSrc0 = lo0 + (bestJ - 1);
        if (std::abs(srcPos0 - hintMid0) < std::abs(bestSrc0 - hintMid0)) {
          bestI = i;
          bestJ = j;
        }
      }
    }
    std::swap(prev, cur);
  }

  if (best <= 0)
    return ar;

  // Traceback
  int i = bestI, j = bestJ;
  int minSrc0 = std::numeric_limits<int>::max();
  int maxSrc0 = std::numeric_limits<int>::min();

  int jStart = j;

  int steps = 0;
  while (i > 0 && j > 0) {
    uint8_t d = dir[i * (m + 1) + j];
    if (d == 0)
      break;

    if (d == 1) { // diag consumes source line j-1
      int src0 = lo0 + (j - 1);
      minSrc0 = std::min(minSrc0, src0);
      maxSrc0 = std::max(maxSrc0, src0);
      --i;
      --j;
    } else if (d == 3) { // left consumes source line j-1 (gap in patch => deletion)
      int src0 = lo0 + (j - 1);
      minSrc0 = std::min(minSrc0, src0);
      maxSrc0 = std::max(maxSrc0, src0);
      --j;
    } else { // up consumes patch only (insertion)
      --i;
    }

    if (++steps > (n + m + 20))
      break; // safety
  }

  // insertion point (boundary) – j is now start column
  jStart = j;

  ar.ok = true;
  ar.score = best;

  if (maxSrc0 < minSrc0) {
    // pure insertion
    ar.isInsert = true;
    int insert0 = lo0 + jStart;
    insert0 = std::max(0, std::min(insert0, fileLen)); // allow at end
    ar.insertAt1 = insert0 + 1;
    return ar;
  }

  ar.from1 = minSrc0 + 1;
  ar.to1 = maxSrc0 + 1;
  return ar;
}

ArduinoPatchStraightener::Options::Options()
    : windowAroundOriginal(260),
      minStrongLineLen(10),
      dropNoopHunks(true) {}

ArduinoPatchStraightener::ArduinoPatchStraightener(const std::string &sketchPath, const std::vector<AiPatchHunk> &hunks,
                                                   const std::vector<SketchFileBuffer> &buffers)
    : ArduinoPatchStraightener(sketchPath, hunks, buffers, Options()) {}

ArduinoPatchStraightener::ArduinoPatchStraightener(const std::string &sketchPath, const std::vector<AiPatchHunk> &hunks,
                                                   const std::vector<SketchFileBuffer> &buffers,
                                                   Options opt)
    : m_opt(std::move(opt)), m_hunks(hunks), m_sketchPath(sketchPath) {
  m_stats.hunksIn = (int)m_hunks.size();

  // Build file cache from buffers
  for (const auto &b : buffers) {
    FileLines fl;
    fl.text = wxString::FromUTF8(b.code.c_str());
    fl.text = NormalizeNewlinesToLF(fl.text);
    SplitLinesLF(fl.text, fl.rawLines);

    fl.normLines.reserve(fl.rawLines.size());
    for (size_t i = 0; i < fl.rawLines.size(); ++i) {
      const std::string norm = NormalizeLineKey(fl.rawLines[i]);
      fl.normLines.push_back(norm);
      fl.positions[norm].push_back((int)i);
    }

    const std::string key = NormalizePathKey(b.filename);
    m_files[key] = std::move(fl);
  }
}

void ArduinoPatchStraightener::Calculate() {
  m_result.clear();
  m_result.reserve(m_hunks.size());

  APP_DEBUG_LOG("PatchStraightener: Calculate begin, hunks=%d files=%zu",
                (int)m_hunks.size(), (size_t)m_files.size());

  for (auto h : m_hunks) {
    const std::string patchFile = wxToStd(h.file);
    const std::string fkey = NormalizePathKey(patchFile);

    auto it = m_files.find(fkey);
    if (it == m_files.end()) {
      m_stats.hunksSkippedNoFile++;
      APP_DEBUG_LOG("PatchStraightener: file not found: %s (from=%d to=%d)",
                    fkey.c_str(), h.fromLine, h.toLine);
      m_result.push_back(h);
      continue;
    }

    FileLines &file = it->second;
    APP_DEBUG_LOG("PatchStraightener: hunk file=%s origRange=%d-%d fileLines=%d replBytes=%zu",
                  fkey.c_str(), h.fromLine, h.toLine, (int)file.rawLines.size(),
                  (size_t)wxToStd(h.replacement).size());

    if (!TryStraightenOne(h, file, fkey)) {
      APP_DEBUG_LOG("PatchStraightener: straighten failed, keeping as-is (file=%s)", fkey.c_str());
      m_result.push_back(h);
      continue;
    }

    if (h.file.IsEmpty()) {
      APP_DEBUG_LOG("PatchStraightener: hunk dropped as noop (file=%s)", fkey.c_str());
      continue;
    }

    m_result.push_back(h);

    // critical: mutate working content so next hunks in same file align to updated code
    ApplyHunkToWorkingFile(file, h);

    APP_DEBUG_LOG("PatchStraightener: applied-to-working file=%s finalRange=%d-%d newFileLines=%d",
                  fkey.c_str(), h.fromLine, h.toLine, (int)file.rawLines.size());
  }

  m_stats.hunksOut = (int)m_result.size();
  APP_DEBUG_LOG("PatchStraightener: Calculate end, outHunks=%d remapped=%d droppedNoop=%d skippedNoFile=%d skippedLowConf=%d",
                m_stats.hunksOut, m_stats.hunksRemapped, m_stats.hunksDroppedNoop,
                m_stats.hunksSkippedNoFile, m_stats.hunksSkippedLowConfidence);
}

std::string ArduinoPatchStraightener::NormalizePathKey(const std::string &ps) {
  std::string out;

  std::string s = NormalizeFilename(m_sketchPath, ps);

  out.reserve(s.size());
  char prev = 0;

  for (char c : s) {
    char x = c;
    if (x == '\\')
      x = '/';
    if (x == '/' && prev == '/')
      continue; // collapse //
    out.push_back(x);
    prev = x;
  }

  // drop trailing "/"
  while (!out.empty() && out.back() == '/')
    out.pop_back();

  // drop leading "./"
  if (StartsWith(out, "./"))
    out.erase(0, 2);

  return out;
}

wxString ArduinoPatchStraightener::NormalizeNewlinesToLF(const wxString &s) {
  wxString out;
  out.reserve(s.size());

  for (size_t i = 0; i < s.size(); ++i) {
    const wxChar ch = s[i];
    if (ch == '\r') {
      // if \r\n -> skip \r, let \n be appended
      if (i + 1 < s.size() && s[i + 1] == '\n')
        continue;
      out.Append(wxASCII_STR('\n'));
    } else {
      out.Append(ch);
    }
  }
  return out;
}

void ArduinoPatchStraightener::SplitLinesLF(const wxString &text, std::vector<wxString> &outLines) {
  outLines.clear();
  wxString cur;
  cur.reserve(128);

  for (size_t i = 0; i < text.size(); ++i) {
    const wxChar ch = text[i];
    if (ch == '\n') {
      outLines.push_back(cur);
      cur.clear();
    } else {
      cur.Append(ch);
    }
  }
  // last line (even if empty)
  outLines.push_back(cur);
}

std::string ArduinoPatchStraightener::NormalizeLineKey(const wxString &line) {
  // For matching: trim both sides, keep internal spaces as-is.
  // Also replace tabs with 2 spaces to reduce trivial mismatches.
  wxString s = line;

  s.Replace(wxT("\t"), wxT("  "));
  TrimBoth(s);

  return wxToStd(s);
}

bool ArduinoPatchStraightener::IsWeakNormalizedLine(const std::string &norm) {
  if (norm.empty())
    return true;

  // Pure punctuation / braces / separators are weak anchors
  bool hasAlnum = false;
  for (char c : norm) {
    if (std::isalnum((unsigned char)c)) {
      hasAlnum = true;
      break;
    }
  }
  if (!hasAlnum)
    return true;

  // Comment-only lines are weak
  if (StartsWith(norm, "//"))
    return true;

  // Very short is weak
  if ((int)norm.size() <= 2)
    return true;

  return false;
}

bool ArduinoPatchStraightener::IsStrongNormalizedLine(const std::string &norm, int minLen) {
  if (IsWeakNormalizedLine(norm))
    return false;
  if ((int)norm.size() < minLen)
    return false;
  return true;
}

int ArduinoPatchStraightener::LineWeight(const std::string &norm, int minLen) {
  // weight used in chain optimization
  if (norm.empty())
    return 0;
  if (IsWeakNormalizedLine(norm))
    return 1; // weak anchor
  if (IsStrongNormalizedLine(norm, minLen))
    return 3; // strong anchor
  return 2;   // medium
}

bool ArduinoPatchStraightener::TryStraightenOne(AiPatchHunk &h, FileLines &file, const std::string &fkey) {
  // Parse replacement into lines
  wxString repl = NormalizeNewlinesToLF(h.replacement);
  std::vector<wxString> patchLines;
  SplitLinesLF(repl, patchLines);

  auto LooksLikeArduinoSkeleton = [&](const FileLines &f) -> bool {
    // very light check: file contains setup & loop headers (strict key)
    bool hasSetup = false, hasLoop = false;
    for (const auto &k : f.normLines) {
      if (k.find("void setup") != std::string::npos)
        hasSetup = true;
      if (k.find("void loop") != std::string::npos)
        hasLoop = true;
    }
    return hasSetup && hasLoop;
  };

  auto PatchContainsSetupLoop = [&](const std::vector<wxString> &pl) -> bool {
    bool hasSetup = false, hasLoop = false;
    for (auto &w : pl) {
      std::string k = NormalizeLineKey(w);
      if (k.find("void setup") != std::string::npos)
        hasSetup = true;
      if (k.find("void loop") != std::string::npos)
        hasLoop = true;
    }
    return hasSetup && hasLoop;
  };

  const int fileLen = (int)file.rawLines.size();
  const int patchLen = (int)patchLines.size();
  const bool origRangeValid = (h.fromLine >= 1 && h.toLine >= h.fromLine && h.toLine <= fileLen);
  const int origLen = origRangeValid ? (h.toLine - h.fromLine + 1) : 0;

  // Promote to full-file replace if patch is large relative to file and looks like full program.
  // This fixes empty-sketch cases where local alignment would pick only "void setup()" and leave skeleton tail.
  const bool fileIsSmall = (fileLen <= 60);
  const bool patchIsMuchLarger = (patchLen >= std::max(12, fileLen + 6)) || (patchLen >= fileLen * 2);
  const bool modelRangeTiny = (!origRangeValid) || (origLen <= 2);
  const bool hasSetupLoopBothSides = LooksLikeArduinoSkeleton(file) && PatchContainsSetupLoop(patchLines);

  if (fileIsSmall && patchIsMuchLarger && modelRangeTiny && hasSetupLoopBothSides) {
    APP_DEBUG_LOG("PatchStraightener: promote to WHOLE-FILE replace (fileLen=%d patchLen=%d orig=%d-%d)",
                  fileLen, patchLen, h.fromLine, h.toLine);

    h.fromLine = 1;
    h.toLine = fileLen; // replace whole file
    h.replacement = JoinLinesLF(patchLines);
    m_stats.hunksRemapped++;
    return true;
  }

  const bool origValid = (h.fromLine >= 1 && h.toLine >= h.fromLine && h.toLine <= fileLen);

  // Alignment-based range pick
  AlignResult ar = FindBestReplaceIntervalByAlignment(file, patchLines, h.fromLine, h.toLine, m_opt);

  if (!ar.ok) {
    m_stats.hunksSkippedLowConfidence++;
    APP_DEBUG_LOG("PatchStraightener: align failed (file=%s)", fkey.c_str());
    return false;
  }

  int newFrom = h.fromLine;
  int newTo = h.toLine;

  if (ar.isInsert) {
    newFrom = ar.insertAt1;
    newTo = 0; // insertion
  } else {
    newFrom = ar.from1;
    newTo = ar.to1;
  }

  // IMPORTANT: if model provided a valid range and alignment is reasonably strong,
  // do not SHRINK the original (prevents leaving tail braces behind).
  if (origValid && ar.score >= 12) {
    // Only clamp if we're not wildly far (otherwise model range was garbage)
    if (std::abs(newFrom - h.fromLine) <= m_opt.windowAroundOriginal * 2 &&
        std::abs((newTo == 0 ? newFrom : newTo) - h.toLine) <= m_opt.windowAroundOriginal * 2) {
      if (newTo != 0) {
        newFrom = std::min(newFrom, h.fromLine);
        newTo = std::max(newTo, h.toLine);
      }
    }
  }

  const bool didRemap = (newFrom != h.fromLine) || (newTo != h.toLine);

  APP_DEBUG_LOG("PatchStraightener: pre-align fileLen=%d patchLines=%d origRangeValid=%d",
                fileLen, (int)patchLines.size(), (int)origRangeValid);

  if (origRangeValid && newTo != 0) {
    // nikdy nenechávej tail původního souboru
    newFrom = std::min(newFrom, h.fromLine);
    newTo = std::max(newTo, h.toLine);
  }

  if (h.toLine > 0 && ar.isInsert) {
    APP_DEBUG_LOG("PatchStraightener: alignment suggested INSERT but model asked REPLACE (%d-%d) => keep model range",
                  h.fromLine, h.toLine);
    newFrom = h.fromLine;
    newTo = h.toLine;
  }

  auto IsFuncHeader = [&](const std::string &k) -> bool {
    // jednoduché: obsahuje '(' i ')' a končí '{' a neobsahuje ';'
    if (k.find('(') == std::string::npos || k.find(')') == std::string::npos)
      return false;
    if (k.find(';') != std::string::npos)
      return false;
    auto t = k;
    while (!t.empty() && isspace((unsigned char)t.back()))
      t.pop_back();
    return !t.empty() && t.back() == '{';
  };

  std::string replFirstStrong;
  for (auto &pl : patchLines) {
    auto k = NormalizeLineKey(pl);
    if (IsStrongNormalizedLine(k, m_opt.minStrongLineLen)) {
      replFirstStrong = k;
      break;
    }
  }

  if (!replFirstStrong.empty() && IsFuncHeader(replFirstStrong) && newFrom > 1) {
    const std::string above = file.normLines[newFrom - 2];
    if (above == replFirstStrong) {
      APP_DEBUG_LOG("PatchStraightener: pulling range up to include func header '%s'", replFirstStrong.c_str());
      newFrom -= 1;
    }
  }

  APP_DEBUG_LOG("PatchStraightener: align score=%d file=%s orig=%d-%d => new=%d-%d patchLines=%d fileLines=%d",
                ar.score, fkey.c_str(), h.fromLine, h.toLine, newFrom, newTo,
                (int)patchLines.size(), fileLen);

  // No-op dropping (safe and simple)
  if (m_opt.dropNoopHunks) {
    if (newTo != 0 && newFrom >= 1 && newTo >= newFrom && newTo <= (int)file.rawLines.size()) {
      const int segLen = newTo - newFrom + 1;
      if (segLen == (int)patchLines.size()) {
        bool allSame = true;
        for (int i = 0; i < segLen; ++i) {
          const std::string a = NormalizeLineKey(patchLines[i]);
          const std::string b = file.normLines[(newFrom - 1) + i];
          if (a != b) {
            allSame = false;
            break;
          }
        }
        if (allSame) {
          h.file.Clear();
          m_stats.hunksDroppedNoop++;
          return true;
        }
      }
    }
  }

  if (didRemap)
    m_stats.hunksRemapped++;

  h.fromLine = newFrom;
  h.toLine = newTo;
  h.replacement = JoinLinesLF(patchLines);
  return true;
}

wxString ArduinoPatchStraightener::JoinLinesLF(const std::vector<wxString> &lines) {
  wxString out;
  // preserve typical style: join with \n; do not force trailing newline
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i)
      out << wxT("\n");
    out << lines[i];
  }
  return out;
}

int ArduinoPatchStraightener::BraceDeltaLines(const std::vector<wxString> &lines) {
  int delta = 0;
  bool inBlockComment = false;

  for (const auto &wx : lines) {
    std::string s = wxToStd(wx);
    bool inStr = false;
    char quote = 0;
    bool esc = false;

    for (size_t i = 0; i < s.size(); ++i) {
      char c = s[i];
      char n = (i + 1 < s.size()) ? s[i + 1] : '\0';

      if (inBlockComment) {
        if (c == '*' && n == '/') {
          inBlockComment = false;
          ++i;
        }
        continue;
      }

      if (!inStr) {
        if (c == '/' && n == '*') {
          inBlockComment = true;
          ++i;
          continue;
        }
        if (c == '/' && n == '/')
          break; // line comment
        if (c == '"' || c == '\'') {
          inStr = true;
          quote = c;
          esc = false;
          continue;
        }
        if (c == '{')
          ++delta;
        else if (c == '}')
          --delta;
      } else {
        if (esc) {
          esc = false;
          continue;
        }
        if (c == '\\') {
          esc = true;
          continue;
        }
        if (c == quote) {
          inStr = false;
          continue;
        }
      }
    }
  }

  return delta;
}
