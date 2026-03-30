#include "file_change_monitor.hpp"
#include <filesystem>

namespace fs = std::filesystem;

wxDEFINE_EVENT(EVT_FILE_MONITOR_CHANGED, wxThreadEvent);

namespace {

static uint64_t ToUint64(const fs::file_time_type::duration &value) {
  return static_cast<uint64_t>(value.count());
}

} // namespace

FileChangeMonitor::FileChangeMonitor(wxEvtHandler *owner, int pollIntervalMs)
    : m_owner(owner), m_timer(this) {
  Bind(wxEVT_TIMER, &FileChangeMonitor::OnTimer, this, m_timer.GetId());
  m_timer.Start(pollIntervalMs);
}

void FileChangeMonitor::WatchFile(const std::string &path) {
  Entry entry;
  entry.path = path;
  entry.isDirectory = false;
  entry.recursive = false;
  entry.snapshot = CaptureSnapshot(entry);
  m_entries[NormalizeKey(path)] = entry;
}

void FileChangeMonitor::WatchDirectory(const std::string &path, bool recursive) {
  Entry entry;
  entry.path = path;
  entry.isDirectory = true;
  entry.recursive = recursive;
  entry.snapshot = CaptureSnapshot(entry);
  m_entries[NormalizeKey(path)] = entry;
}

void FileChangeMonitor::Unwatch(const std::string &path) {
  m_entries.erase(NormalizeKey(path));
}

void FileChangeMonitor::Clear() {
  m_entries.clear();
}

void FileChangeMonitor::SyncPath(const std::string &path) {
  auto it = m_entries.find(NormalizeKey(path));
  if (it == m_entries.end()) {
    return;
  }

  it->second.snapshot = CaptureSnapshot(it->second);
}

FileChangeMonitor::Snapshot FileChangeMonitor::CaptureSnapshot(const Entry &entry) {
  Snapshot snapshot;

  std::error_code ec;
  const fs::path path = fs::u8path(entry.path);

  snapshot.exists = fs::exists(path, ec);
  if (ec || !snapshot.exists) {
    snapshot.exists = false;
    return snapshot;
  }

  snapshot.isDirectory = fs::is_directory(path, ec);
  if (ec) {
    return Snapshot{};
  }

  uint64_t sig = 0;
  HashCombine(sig, snapshot.exists ? 1ull : 0ull);
  HashCombine(sig, snapshot.isDirectory ? 1ull : 0ull);

  if (!snapshot.isDirectory) {
    const auto size = fs::file_size(path, ec);
    if (!ec) {
      HashCombine(sig, static_cast<uint64_t>(size));
    }

    const auto writeTime = fs::last_write_time(path, ec);
    if (!ec) {
      HashCombine(sig, ToUint64(writeTime.time_since_epoch()));
    }

    snapshot.signature = sig;
    return snapshot;
  }

  auto addEntry = [&](const fs::directory_entry &dirEntry) {
    const fs::path rel = dirEntry.path().lexically_relative(path);
    HashCombine(sig, HashString(rel.u8string()));

    std::error_code itemEc;
    const bool isDir = dirEntry.is_directory(itemEc);
    if (!itemEc) {
      HashCombine(sig, isDir ? 1ull : 0ull);
    }

    const auto writeTime = dirEntry.last_write_time(itemEc);
    if (!itemEc) {
      HashCombine(sig, ToUint64(writeTime.time_since_epoch()));
    }

    if (!isDir) {
      const auto size = dirEntry.file_size(itemEc);
      if (!itemEc) {
        HashCombine(sig, static_cast<uint64_t>(size));
      }
    }
  };

  if (entry.recursive) {
    fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
      addEntry(*it);
    }
  } else {
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
      addEntry(*it);
    }
  }

  snapshot.signature = sig;
  return snapshot;
}

uint64_t FileChangeMonitor::HashString(const std::string &value) {
  return static_cast<uint64_t>(std::hash<std::string>{}(value));
}

void FileChangeMonitor::HashCombine(uint64_t &seed, uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

std::string FileChangeMonitor::NormalizeKey(const std::string &path) {
  std::error_code ec;
  return fs::weakly_canonical(fs::u8path(path), ec).u8string();
}

void FileChangeMonitor::PostChange(const Entry &entry, FileChangeKind kind) {
  if (!m_owner) {
    return;
  }

  wxThreadEvent evt(EVT_FILE_MONITOR_CHANGED);
  evt.SetString(wxString::FromUTF8(entry.path));
  evt.SetInt(static_cast<int>(kind));
  evt.SetExtraLong(entry.isDirectory ? 1 : 0);
  wxPostEvent(m_owner, evt);
}

void FileChangeMonitor::OnTimer(wxTimerEvent &event) {
  for (auto &item : m_entries) {
    Entry &entry = item.second;
    const Snapshot current = CaptureSnapshot(entry);

    if (current.exists == entry.snapshot.exists &&
        current.isDirectory == entry.snapshot.isDirectory &&
        current.signature == entry.snapshot.signature) {
      continue;
    }

    FileChangeKind kind = FileChangeKind::Updated;
    if (!entry.snapshot.exists && current.exists) {
      kind = FileChangeKind::Created;
    } else if (entry.snapshot.exists && !current.exists) {
      kind = FileChangeKind::Deleted;
    }

    entry.snapshot = current;
    PostChange(entry, kind);
  }

  event.Skip();
}
