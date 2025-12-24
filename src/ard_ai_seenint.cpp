#include "ard_ai_seenint.hpp"

void AiSeenIntervals::Reset() {
  m_files.clear();
}

void AiSeenIntervals::ClearFile(const wxString &file) {
  auto it = m_files.find(file);
  if (it != m_files.end()) {
    m_files.erase(it);
  }
}

bool AiSeenIntervals::NormalizeRange(int &fromLine, int &toLine) {
  // Allow "empty range" sentinel used by some flows (0-0).
  if (fromLine == 0 && toLine == 0) {
    return true;
  }

  // Require positive 1-based lines.
  if (fromLine < 1 || toLine < 1) {
    return false;
  }

  if (toLine < fromLine) {
    std::swap(fromLine, toLine);
  }
  return true;
}

void AiSeenIntervals::MergeRanges(std::vector<Range> &ranges) {
  if (ranges.empty())
    return;

  std::sort(ranges.begin(), ranges.end(),
            [](const Range &a, const Range &b) {
              if (a.from != b.from)
                return a.from < b.from;
              return a.to < b.to;
            });

  std::vector<Range> out;
  out.reserve(ranges.size());

  Range cur = ranges[0];
  for (size_t i = 1; i < ranges.size(); ++i) {
    const Range &r = ranges[i];

    // Overlap or touching -> merge (touching because line intervals are inclusive)
    if (r.from <= cur.to + 1) {
      if (r.to > cur.to)
        cur.to = r.to;
    } else {
      out.push_back(cur);
      cur = r;
    }
  }
  out.push_back(cur);

  ranges.swap(out);
}

void AiSeenIntervals::AddSeen(const wxString &file, int fromLine, int toLine, uint64_t stamp) {
  if (file.IsEmpty())
    return;

  if (!NormalizeRange(fromLine, toLine)) {
    return;
  }

  FileState &st = m_files[file];

  // If caller provides stamp, invalidate stored ranges when it changes.
  if (stamp != 0) {
    if (st.stamp == 0) {
      st.stamp = stamp;
    } else if (st.stamp != stamp) {
      st.stamp = stamp;
      st.mergedRanges.clear();
    }
  }

  // Ignore empty range for "seen"; it doesn't convey any actual lines.
  if (fromLine == 0 && toLine == 0) {
    return;
  }

  st.mergedRanges.push_back({fromLine, toLine});
  MergeRanges(st.mergedRanges);
}

bool AiSeenIntervals::HasSeen(const wxString &file, int fromLine, int toLine, uint64_t stamp) const {
  if (file.IsEmpty())
    return false;

  if (!NormalizeRange(fromLine, toLine)) {
    return false;
  }

  // By default: empty patch range means "pure insertion / new file content",
  // you typically don't want to block it on "seen lines".
  if (fromLine == 0 && toLine == 0) {
    return true;
  }

  auto it = m_files.find(file);
  if (it == m_files.end()) {
    return false;
  }

  const FileState &st = it->second;

  if (stamp != 0 && st.stamp != 0 && st.stamp != stamp) {
    return false;
  }

  // mergedRanges is sorted & non-overlapping.
  // Find the interval that could contain fromLine.
  for (const Range &r : st.mergedRanges) {
    if (fromLine < r.from) {
      return false; // because sorted, no later range can cover fromLine
    }
    if (fromLine >= r.from && fromLine <= r.to) {
      return (toLine <= r.to);
    }
  }
  return false;
}

wxString AiSeenIntervals::DumpForFile(const wxString &file) const {
  auto it = m_files.find(file);
  if (it == m_files.end())
    return wxT("<none>");

  const FileState &st = it->second;
  wxString s;
  s << wxT("stamp=") << (unsigned long long)st.stamp << wxT(" ranges=");
  for (size_t i = 0; i < st.mergedRanges.size(); ++i) {
    const auto &r = st.mergedRanges[i];
    if (i)
      s << wxT(",");
    s << wxT("[") << r.from << wxT("-") << r.to << wxT("]");
  }
  return s;
}
