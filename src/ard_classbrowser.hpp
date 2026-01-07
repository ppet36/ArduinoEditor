#pragma once

#include <wx/imaglist.h>
#include <wx/panel.h>
// clang-format off
#include <wx/treectrl.h>
#include <wx/generic/treectlg.h>
// clang-format on

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ard_cc.hpp"
#include "ard_edit.hpp"
#include "ard_ev.hpp"

class ArduinoClassBrowserPanel : public wxPanel {
public:
  explicit ArduinoClassBrowserPanel(wxWindow *parent);
  ~ArduinoClassBrowserPanel() override;

  void SetCompletion(ArduinoCodeCompletion *cc);
  void SetCurrentEditor(ArduinoEditor *ed);

  void SetCurrentLine(int line);
  void SetCurrentLine(int line, int col);

  void Clear();

private:
  struct ItemData : public wxTreeItemData {
    int index = -1;
    int sortLine = 0;
    int sortCol = 0;
    std::string usr;

    ItemData(int idx, std::string u, int line, int col)
        : index(idx), sortLine(line), sortCol(col), usr(std::move(u)) {}
  };

  struct HoverCacheEntry {
    HoverInfo info;
    std::string tooltip; // precomputed info.ToHoverString()
  };

private:
  void RebuildTree();
  void BuildImageList();
  int KindToImage(CXCursorKind kind) const;

  void SetSymbols(const std::string &currentFile, std::vector<SymbolInfo> symbols);

  void StopHoverWorker();
  void StartHoverWorker(uint64_t gen, std::string file, std::string codeSnapshot, const std::vector<SketchFileBuffer> &files);
  void ApplyHoverInfo(uint64_t gen, int symbolIndex, const HoverInfo &info);
  wxString MakeBaseLabel(const SymbolInfo &s) const;
  wxString MakeLabelFromHover(const SymbolInfo &s, const HoverInfo &info) const;

  bool IsContainerKind(CXCursorKind kind) const;
  bool IsMacroKind(CXCursorKind kind) const;
  bool IsFunctionKind(CXCursorKind kind) const;

  int FindBestSymbolForLine(int line) const;
  int FindBestSymbolForPos(int line, int col) const;

  void OnItemActivated(wxTreeEvent &e);
  void OnTreeGetToolTip(wxTreeEvent &e);

  static uint64_t Hash64_FNV1a(const std::string &s);
  std::string MakeHoverKey(const SymbolInfo &s) const;

  bool IsSameEditorContext(ArduinoEditor *ed, const std::string &file, uint64_t snapshotHash) const;
  void ResetHoverCacheForNewContext(ArduinoEditor *ed, const std::string &file, uint64_t snapshotHash);

private:
  wxGenericTreeCtrl *m_tree = nullptr;
  wxImageList *m_images = nullptr;
  ArduinoEditor *m_cachedEditor = nullptr;
  std::string m_cachedFile;
  uint64_t m_cachedSnapshotHash = 0;
  bool m_forceFullRebuildNext = true;

  std::vector<std::string> m_tooltipByIndex;
  wxTreeItemId m_lastTipItem;

  std::mutex m_hoverCacheMutex;
  std::unordered_map<std::string, HoverCacheEntry> m_hoverCache;

  ArduinoCodeCompletion *m_completion = nullptr; // non-owning
  ArduinoEditor *m_editor = nullptr;             // non-owning

  std::atomic<uint64_t> m_generation{0};
  std::atomic_bool m_cancel{false};
  std::thread m_worker;

  std::string m_codeSnapshot; // UI-thread snapshot for hover worker

  std::string m_currentFile;
  std::vector<SymbolInfo> m_symbols;

  std::vector<wxTreeItemId> m_itemByIndex;

  bool m_internalSelect = false;
};
