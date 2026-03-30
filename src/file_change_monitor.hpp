#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <wx/event.h>
#include <wx/timer.h>

enum class FileChangeKind {
  Updated = 1,
  Deleted = 2,
  Created = 3
};

wxDECLARE_EVENT(EVT_FILE_MONITOR_CHANGED, wxThreadEvent);

class FileChangeMonitor : public wxEvtHandler {
public:
  explicit FileChangeMonitor(wxEvtHandler *owner, int pollIntervalMs = 1000);

  void WatchFile(const std::string &path);
  void WatchDirectory(const std::string &path, bool recursive = true);
  void Unwatch(const std::string &path);
  void Clear();
  void SyncPath(const std::string &path);

private:
  struct Snapshot {
    bool exists = false;
    bool isDirectory = false;
    uint64_t signature = 0;
  };

  struct Entry {
    std::string path;
    bool isDirectory = false;
    bool recursive = false;
    Snapshot snapshot;
  };

  wxEvtHandler *m_owner = nullptr;
  wxTimer m_timer;
  std::unordered_map<std::string, Entry> m_entries;

  static Snapshot CaptureSnapshot(const Entry &entry);
  static uint64_t HashString(const std::string &value);
  static void HashCombine(uint64_t &seed, uint64_t value);
  static std::string NormalizeKey(const std::string &path);

  void PostChange(const Entry &entry, FileChangeKind kind);
  void OnTimer(wxTimerEvent &event);
};
