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

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <vector>
#include <wx/string.h>

// LCS-based aligner for side-by-side diff views.
// Takes two sequences of logical lines and produces two aligned sequences
// of equal length, inserting empty lines where needed.
class ArduinoLcsDiffAligner {
public:
  struct AlignedBlocks {
    std::vector<wxString> left;
    std::vector<wxString> right;
  };

  // Guard against pathological cases (O(n*m)).
  void SetMaxCellWork(uint64_t maxCells) { m_maxCells = maxCells; }

  AlignedBlocks Align(const std::vector<wxString> &a,
                      const std::vector<wxString> &b) const;

private:
  uint64_t m_maxCells = 100'000ULL;

  static AlignedBlocks AlignByPadding(const std::vector<wxString> &a,
                                      const std::vector<wxString> &b);

  static std::vector<int> LcsScoreForward(const std::vector<wxString> &a, int a0, int a1,
                                          const std::vector<wxString> &b, int b0, int b1);
  static std::vector<int> LcsScoreReverse(const std::vector<wxString> &a, int a0, int a1,
                                          const std::vector<wxString> &b, int b0, int b1);

  static void Hirschberg(const std::vector<wxString> &a, int a0, int a1,
                         const std::vector<wxString> &b, int b0, int b1,
                         std::vector<std::pair<int, int>> &outMatches);

  static AlignedBlocks BuildAlignedFromMatches(
      const std::vector<wxString> &a,
      const std::vector<wxString> &b,
      const std::vector<std::pair<int, int>> &matches);
};
