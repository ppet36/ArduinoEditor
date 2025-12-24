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
