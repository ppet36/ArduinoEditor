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
    // LCS ignores "weak" lines (empty / braces), so they may appear as delete+add
    // even if they are identical. Here we pair them locally at the start/end of spans,
    // without them acting as global anchors.

    auto trimKey = [](wxString s) -> wxString {
      s.Trim(true).Trim(false);
      s.Replace(wxT("\r"), wxEmptyString);
      return s;
    };

    int i = aFrom;
    int j = bFrom;

    // 1) Pair identical weak lines from the beginning of the span
    while (i < aTo && j < bTo) {
      const wxString ka = trimKey(a[i]);
      const wxString kb = trimKey(b[j]);
      if (IsWeakLine(ka) && IsWeakLine(kb) && ka == kb) {
        r.left.push_back(a[i]);
        r.right.push_back(b[j]);
        ++i;
        ++j;
      } else {
        break;
      }
    }

    // 2) Pair identical weak lines from the end of the span
    int ii = aTo - 1;
    int jj = bTo - 1;
    std::vector<std::pair<wxString, wxString>> tail;

    while (ii >= i && jj >= j) {
      const wxString ka = trimKey(a[ii]);
      const wxString kb = trimKey(b[jj]);
      if (IsWeakLine(ka) && IsWeakLine(kb) && ka == kb) {
        tail.emplace_back(a[ii], b[jj]);
        --ii;
        --jj;
      } else {
        break;
      }
    }

    // 3) The rest: deletions
    for (int x = i; x <= ii; ++x) {
      r.left.push_back(a[x]);
      r.right.push_back(wxEmptyString);
    }

    // 4) The rest: insertions
    for (int y = j; y <= jj; ++y) {
      r.left.push_back(wxEmptyString);
      r.right.push_back(b[y]);
    }

    // 5) And finally, the paired tail (reverse the order, we collected from the back)
    for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
      r.left.push_back(it->first);
      r.right.push_back(it->second);
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
