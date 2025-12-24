#include "lcs.hpp"
#include "ard_diff.hpp"
#include <algorithm>
#include <unordered_map>
#include <utility>

static bool IsWeakLine(const wxString &s) {
  wxString t = s;
  t.Trim(true).Trim(false);
  return t.empty() || t == wxT("}") || t == wxT("{") || t == wxT("};");
}

static wxString NormalizeForCompare(wxString s) {
  s.Trim(true);
  s.Replace(wxT("\r"), wxEmptyString);

  if (IsWeakLine(s)) {
    // empty key => never match (we'll enforce that in comparisons)
    return wxEmptyString;
  }

  return s;
}

ArduinoLcsDiffAligner::AlignedBlocks ArduinoLcsDiffAligner::AlignByPadding(const std::vector<wxString> &a,
                                                                           const std::vector<wxString> &b) {
  AlignedBlocks r;
  const size_t maxLen = std::max(a.size(), b.size());
  r.left = a;
  r.right = b;
  r.left.resize(maxLen, wxEmptyString);
  r.right.resize(maxLen, wxEmptyString);
  return r;
}

std::vector<int>
ArduinoLcsDiffAligner::LcsScoreForward(const std::vector<wxString> &a, int a0, int a1,
                                       const std::vector<wxString> &b, int b0, int b1) {
  const int n = a1 - a0;
  const int m = b1 - b0;

  std::vector<int> prev(m + 1, 0), cur(m + 1, 0);
  for (int i = 0; i < n; ++i) {
    cur[0] = 0;
    const wxString ai = NormalizeForCompare(a[a0 + i]);
    for (int j = 0; j < m; ++j) {
      const wxString bj = NormalizeForCompare(b[b0 + j]);
      const bool match = !ai.empty() && (ai == bj);
      if (match)
        cur[j + 1] = prev[j] + 1;
      else
        cur[j + 1] = std::max(cur[j], prev[j + 1]);
    }
    std::swap(prev, cur);
  }
  return prev;
}

std::vector<int>
ArduinoLcsDiffAligner::LcsScoreReverse(const std::vector<wxString> &a, int a0, int a1,
                                       const std::vector<wxString> &b, int b0, int b1) {
  const int n = a1 - a0;
  const int m = b1 - b0;

  std::vector<int> prev(m + 1, 0), cur(m + 1, 0);
  for (int i = 0; i < n; ++i) {
    cur[0] = 0;
    const wxString ai = NormalizeForCompare(a[a1 - 1 - i]);
    for (int j = 0; j < m; ++j) {
      const wxString bj = NormalizeForCompare(b[b1 - 1 - j]);
      const bool match = !ai.empty() && (ai == bj);
      if (match)
        cur[j + 1] = prev[j] + 1;
      else
        cur[j + 1] = std::max(cur[j], prev[j + 1]);
    }
    std::swap(prev, cur);
  }
  return prev;
}

void ArduinoLcsDiffAligner::Hirschberg(
    const std::vector<wxString> &a, int a0, int a1,
    const std::vector<wxString> &b, int b0, int b1,
    std::vector<std::pair<int, int>> &outMatches) {

  const int n = a1 - a0;
  const int m = b1 - b0;

  if (n <= 0 || m <= 0)
    return;

  if (n == 1) {
    const wxString needle = NormalizeForCompare(a[a0]);
    if (!needle.empty()) {
      for (int j = b0; j < b1; ++j) {
        if (needle == NormalizeForCompare(b[j])) {
          outMatches.emplace_back(a0, j);
          break;
        }
      }
    }
    return;
  }

  const int amid = a0 + n / 2;

  const auto leftScore = LcsScoreForward(a, a0, amid, b, b0, b1);
  const auto rightScore = LcsScoreReverse(a, amid, a1, b, b0, b1);

  int bestK = 0;
  int bestVal = -1;
  for (int k = 0; k <= m; ++k) {
    const int v = leftScore[k] + rightScore[m - k];
    if (v > bestVal) {
      bestVal = v;
      bestK = k;
    }
  }

  const int bmid = b0 + bestK;

  Hirschberg(a, a0, amid, b, b0, bmid, outMatches);
  Hirschberg(a, amid, a1, b, bmid, b1, outMatches);
}

ArduinoLcsDiffAligner::AlignedBlocks
ArduinoLcsDiffAligner::BuildAlignedFromMatches(
    const std::vector<wxString> &a,
    const std::vector<wxString> &b,
    const std::vector<std::pair<int, int>> &matches) {

  AlignedBlocks r;
  r.left.reserve(a.size() + b.size());
  r.right.reserve(a.size() + b.size());

  int ai = 0;
  int bi = 0;

  auto emitUnmatched = [&](int aFrom, int aTo, int bFrom, int bTo) {
    // deletions
    for (int i = aFrom; i < aTo; ++i) {
      r.left.push_back(a[i]);
      r.right.push_back(wxEmptyString);
    }
    // insertions
    for (int j = bFrom; j < bTo; ++j) {
      r.left.push_back(wxEmptyString);
      r.right.push_back(b[j]);
    }
  };

  for (const auto &p : matches) {
    if (ai < p.first || bi < p.second)
      emitUnmatched(ai, p.first, bi, p.second);

    r.left.push_back(a[p.first]);
    r.right.push_back(b[p.second]);
    ai = p.first + 1;
    bi = p.second + 1;
  }

  if (ai < (int)a.size() || bi < (int)b.size())
    emitUnmatched(ai, (int)a.size(), bi, (int)b.size());

  return r;
}

ArduinoLcsDiffAligner::AlignedBlocks
ArduinoLcsDiffAligner::Align(const std::vector<wxString> &a,
                             const std::vector<wxString> &b) const {
  const uint64_t n = a.size();
  const uint64_t m = b.size();

  if (n * m > m_maxCells)
    return AlignByPadding(a, b);

  std::vector<std::pair<int, int>> matches;
  Hirschberg(a, 0, (int)a.size(), b, 0, (int)b.size(), matches);

  std::vector<std::pair<int, int>> filtered;
  filtered.reserve(matches.size());
  int lastA = -1, lastB = -1;
  for (const auto &p : matches) {
    if (p.first > lastA && p.second > lastB) {
      filtered.push_back(p);
      lastA = p.first;
      lastB = p.second;
    }
  }
  return BuildAlignedFromMatches(a, b, filtered);
}
