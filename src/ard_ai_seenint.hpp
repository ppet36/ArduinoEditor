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

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>
#include <wx/string.h>

// Tracks which line intervals of which files were shown to the model.
// Optionally tracks a "stamp" (checksum/revision) per file to invalidate ranges after edits.
class AiSeenIntervals {
public:
  struct Range {
    int from = 0; // 1-based inclusive
    int to = 0;   // 1-based inclusive
  };

  AiSeenIntervals() = default;

  // Clears everything.
  void Reset();

  // Clears a single file.
  void ClearFile(const wxString &file);

  // Pump a seen interval. Ignores nonsensical intervals.
  // If stamp is non-zero, it is used to invalidate old ranges when stamp changes.
  void AddSeen(const wxString &file, int fromLine, int toLine, uint64_t stamp = 0);

  // Returns true if [fromLine, toLine] is fully covered by seen intervals.
  // If stamp is non-zero and doesn't match stored stamp, returns false.
  // Special case: empty range (0-0) returns true by default (useful for new file insertions).
  bool HasSeen(const wxString &file, int fromLine, int toLine, uint64_t stamp = 0) const;

  // Useful for debugging/logging
  wxString DumpForFile(const wxString &file) const;

private:
  struct FileState {
    uint64_t stamp = 0;              // 0 = unknown/unused
    std::vector<Range> mergedRanges; // sorted, non-overlapping
  };

  static bool NormalizeRange(int &fromLine, int &toLine);
  static void MergeRanges(std::vector<Range> &ranges);

  std::map<wxString, FileState> m_files;
};
