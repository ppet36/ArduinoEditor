#include "ard_classbrowser.hpp"
#include "ard_diagview.hpp"
#include "utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <thread>
#include <unordered_set>

static std::string BuildFunctionLabelFromHover(const HoverInfo &info) {
  if (info.name.empty())
    return {};

  std::string out;
  out.reserve(info.name.size() + 64);

  out.append(info.name);
  out.push_back('(');

  bool first = true;
  for (const auto &p : info.parameters) {
    std::string t = p.type;
    TrimInPlace(t);

    if (t.empty())
      continue;

    if (!first)
      out.append(", ");
    first = false;

    out.append(t);
  }

  out.push_back(')');

  std::string ret = info.type;
  TrimInPlace(ret);

  if (!ret.empty() && ret != "void") {
    out.append(" -> ").append(ret);
  }

  return out;
}

// -------------------------------------------------------------------------

ArduinoClassBrowserPanel::ArduinoClassBrowserPanel(wxWindow *parent)
    : wxPanel(parent) {

  m_tree = new wxGenericTreeCtrl(
      this,
      wxID_ANY,
      wxDefaultPosition,
      wxDefaultSize,
      wxTR_HAS_BUTTONS |
          wxTR_LINES_AT_ROOT |
          wxTR_HIDE_ROOT |
          wxTR_SINGLE);

  KillUnfocusedColor(m_tree);

  auto *sizer = new wxBoxSizer(wxVERTICAL);
  sizer->Add(m_tree, 1, wxEXPAND);
  SetSizer(sizer);

  BuildImageList();

  Clear();

  m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &ArduinoClassBrowserPanel::OnItemActivated, this);
  m_tree->Bind(wxEVT_TREE_ITEM_GETTOOLTIP, &ArduinoClassBrowserPanel::OnTreeGetToolTip, this);
}

ArduinoClassBrowserPanel::~ArduinoClassBrowserPanel() {
  StopHoverWorker();
}

uint64_t ArduinoClassBrowserPanel::Hash64_FNV1a(const std::string &s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= (uint64_t)c;
    h *= 1099511628211ULL;
  }
  return h;
}

void ArduinoClassBrowserPanel::StopHoverWorker() {
  m_cancel.store(true, std::memory_order_relaxed);

  if (m_worker.joinable()) {
    m_worker.join();
  }

  m_cancel.store(false, std::memory_order_relaxed);
}

void ArduinoClassBrowserPanel::BuildImageList() {
  const int size = FromDIP(10);

  if (m_images) {
    m_tree->AssignImageList(nullptr);
    delete m_images;
    m_images = nullptr;
  }

  m_images = new wxImageList(size, size, true);

  // Todo EditorColors
  wxBitmap funcBmp = ArduinoEditor::MakeCircleBitmap(size, wxColour(80, 120, 255));   // blue
  wxBitmap varBmp = ArduinoEditor::MakeCircleBitmap(size, wxColour(100, 200, 120));   // green
  wxBitmap classBmp = ArduinoEditor::MakeCircleBitmap(size, wxColour(255, 180, 80));  // orange
  wxBitmap macroBmp = ArduinoEditor::MakeCircleBitmap(size, wxColour(180, 120, 200)); // purple

  m_images->Add(funcBmp);  // 0
  m_images->Add(varBmp);   // 1
  m_images->Add(classBmp); // 2
  m_images->Add(macroBmp); // 3

  m_tree->AssignImageList(m_images);
}

bool ArduinoClassBrowserPanel::IsMacroKind(CXCursorKind kind) const {
  return kind == CXCursor_MacroDefinition;
}

bool ArduinoClassBrowserPanel::IsContainerKind(CXCursorKind kind) const {
  return (kind == CXCursor_ClassDecl ||
          kind == CXCursor_StructDecl ||
          kind == CXCursor_EnumDecl ||
          kind == CXCursor_UnionDecl);
}

bool ArduinoClassBrowserPanel::IsFunctionKind(CXCursorKind kind) const {
  return (kind == CXCursor_FunctionDecl ||
          kind == CXCursor_CXXMethod ||
          kind == CXCursor_Constructor ||
          kind == CXCursor_Destructor ||
          kind == CXCursor_FunctionTemplate);
}

void ArduinoClassBrowserPanel::Clear() {
  m_currentFile.clear();
  m_symbols.clear();
  m_itemByIndex.clear();

  m_tooltipByIndex.clear();
  m_lastTipItem = wxTreeItemId();
  m_tree->SetToolTip((wxToolTip *)nullptr);

  m_tree->Freeze();
  m_tree->DeleteAllItems();
  m_tree->AddRoot(wxT("root"));
  m_tree->Thaw();

  m_forceFullRebuildNext = true;
}

void ArduinoClassBrowserPanel::SetCompletion(ArduinoCodeCompletion *cc) {
  m_completion = cc;
}

void ArduinoClassBrowserPanel::ResetHoverCacheForNewContext(ArduinoEditor *ed, const std::string &file, uint64_t snapshotHash) {
  {
    std::lock_guard<std::mutex> lk(m_hoverCacheMutex);
    m_hoverCache.clear();
  }
  m_cachedEditor = ed;
  m_cachedFile = file;
  m_cachedSnapshotHash = snapshotHash;
}

void ArduinoClassBrowserPanel::SetCurrentEditor(ArduinoEditor *ed) {
  StopHoverWorker();

  bool editorChanged = (m_editor != ed);

  m_editor = ed;

  const uint64_t gen = m_generation.fetch_add(1) + 1;

  if (!m_completion || !m_editor) {
    Clear();
    return;
  }

  int line = 0, column = 0;
  m_editor->GetCurrentCursor(line, column);

  const std::string sketchPath = m_completion->GetCli()->GetSketchPath();
  const std::string curFile = NormalizeFilename(sketchPath, m_editor->GetFilePath());
  m_codeSnapshot = m_editor->GetText(); // IMPORTANT: snapshot in UI thread

  const uint64_t snapshotHash = Hash64_FNV1a(m_codeSnapshot);

  if (editorChanged) {
    m_forceFullRebuildNext = true;
    ResetHoverCacheForNewContext(m_editor, curFile, snapshotHash);
  }

  std::vector<SketchFileBuffer> filesSnapshot;
  m_completion->CollectSketchFiles(filesSnapshot);

  auto all = m_completion->GetAllSymbols(curFile, m_codeSnapshot);

  std::vector<SymbolInfo> filtered;
  filtered.reserve(all.size());

  for (auto &s : all) {
    if (hasSuffix(s.file, ".ino.hpp")) {
      continue; // synthetic header
    }

    std::string symFile = NormalizeFilename(sketchPath, s.file);
    if (symFile == curFile) {
      filtered.push_back(std::move(s));
    }
  }

  SetSymbols(curFile, std::move(filtered));

  CallAfter([this, gen, line, curFile]() {
    if (gen != m_generation.load())
      return;
    SetCurrentLine(line);
  });

  // Start async hover enrichment (file+code snapshot are captured by value)
  StartHoverWorker(gen, curFile, m_codeSnapshot, filesSnapshot);
}

void ArduinoClassBrowserPanel::SetSymbols(const std::string &currentFile, std::vector<SymbolInfo> symbols) {
  m_currentFile = currentFile;

  APP_DEBUG_LOG("CB: SetSymbols(file=%s, sym.count=%zu)", currentFile.c_str(), symbols.size());

  std::sort(symbols.begin(), symbols.end(), [](const SymbolInfo &a, const SymbolInfo &b) {
    if (a.line != b.line)
      return a.line < b.line;
    return a.column < b.column;
  });

  // libclang sometimes returns EnumConstantDecl duplicately (typically 2x).
  // Dedup only for enum constants, so that the rest do not lose any "legit" duplicates.
  {
    std::unordered_set<std::string> seen;
    std::vector<SymbolInfo> filtered;
    filtered.reserve(symbols.size());

    for (auto &s : symbols) {
      if (s.kind == CXCursor_EnumConstantDecl) {
        if (!seen.insert(s.usr).second) {
          continue; // dup
        }
      }
      filtered.push_back(std::move(s));
    }

    symbols = std::move(filtered);
  }

  m_symbols = std::move(symbols);

  RebuildTree();
}

void ArduinoClassBrowserPanel::RebuildTree() {
  APP_DEBUG_LOG("CB: RebuildTree()");

  auto isScope = [](const SymbolInfo &s) -> bool {
    return (s.bodyLineFrom > 0 && s.bodyLineTo >= s.bodyLineFrom);
  };

  auto posInBody = [&isScope](const SymbolInfo &scope, int line, int col) -> bool {
    if (!isScope(scope))
      return false;

    auto afterStart = [&]() -> bool {
      if (line > scope.bodyLineFrom)
        return true;
      if (line < scope.bodyLineFrom)
        return false;
      if (scope.bodyColFrom <= 0 || col <= 0)
        return true;
      return col >= scope.bodyColFrom;
    };

    auto beforeEnd = [&]() -> bool {
      if (line < scope.bodyLineTo)
        return true;
      if (line > scope.bodyLineTo)
        return false;
      if (scope.bodyColTo <= 0 || col <= 0)
        return true;
      return col <= scope.bodyColTo;
    };

    return afterStart() && beforeEnd();
  };

  auto posLess = [](int l1, int c1, int l2, int c2) -> bool {
    if (l1 != l2)
      return l1 < l2;
    return c1 < c2;
  };

  auto findInsertBefore = [&](const wxTreeItemId &parent, int line, int col) -> wxTreeItemId {
    wxTreeItemIdValue cookie;
    wxTreeItemId child = m_tree->GetFirstChild(parent, cookie);
    while (child.IsOk()) {
      if (auto *d = dynamic_cast<ItemData *>(m_tree->GetItemData(child))) {
        if (posLess(line, col, d->sortLine, d->sortCol))
          return child;
      }
      child = m_tree->GetNextChild(parent, cookie);
    }
    return wxTreeItemId(); // invalid => Append
  };

  auto scopeSpanKey = [](const SymbolInfo &s) -> long long {
    long long dl = (long long)s.bodyLineTo - (long long)s.bodyLineFrom;
    long long dc = (long long)s.bodyColTo - (long long)s.bodyColFrom;
    if (dc < 0)
      dc = 0;
    return dl * 100000LL + dc;
  };

  auto getKey = [](const SymbolInfo &s) -> std::string {
    if (!s.usr.empty())
      return s.usr;

    std::string k;
    k.reserve(s.name.size() + s.file.size() + 64);
    k.append("fallback:");
    k.append(std::to_string((int)s.kind));
    k.push_back(':');
    k.append(s.file);
    k.push_back(':');
    k.append(s.name);
    k.push_back(':');
    k.append(std::to_string(s.line));
    k.push_back(':');
    k.append(std::to_string(s.column));
    return k;
  };

  auto imgFor = [this](const SymbolInfo &s) -> int {
    if (IsMacroKind(s.kind))
      return 3;
    if (IsContainerKind(s.kind))
      return 2;
    if (IsFunctionKind(s.kind))
      return 0;
    return 1;
  };

  auto computeParentIdx = [&](std::vector<int> &parentIdx) {
    const int n = (int)m_symbols.size();
    parentIdx.assign(n, -1);

    std::vector<int> scopes;
    scopes.reserve(n);

    for (int i = 0; i < n; i++) {
      if (isScope(m_symbols[i]))
        scopes.push_back(i);
    }

    std::sort(scopes.begin(), scopes.end(), [&](int a, int b) {
      const auto &A = m_symbols[a];
      const auto &B = m_symbols[b];
      long long sa = scopeSpanKey(A);
      long long sb = scopeSpanKey(B);
      if (sa != sb)
        return sa < sb;
      if (A.bodyLineFrom != B.bodyLineFrom)
        return A.bodyLineFrom < B.bodyLineFrom;
      return A.bodyColFrom < B.bodyColFrom;
    });

    for (int i = 0; i < n; i++) {
      const auto &sym = m_symbols[i];
      if (sym.kind == CXCursor_ParmDecl)
        continue;

      for (int sc : scopes) {
        if (sc == i)
          continue;

        const auto &scope = m_symbols[sc];
        if (posInBody(scope, sym.line, sym.column)) {
          parentIdx[i] = sc;
          break; // innermost
        }
      }
    }

    // EnumConstantDecl special-case
    for (int i = 0; i < n; i++) {
      const auto &sym = m_symbols[i];
      if (sym.kind != CXCursor_EnumConstantDecl)
        continue;

      const int scopeParent = parentIdx[i];
      int bestEnum = -1;

      for (int j = i - 1; j >= 0; j--) {
        const auto &cand = m_symbols[j];
        if (cand.kind != CXCursor_EnumDecl)
          continue;
        if (cand.file != sym.file)
          continue;
        if (parentIdx[j] != scopeParent)
          continue;

        bestEnum = j;
        break;
      }

      if (bestEnum >= 0)
        parentIdx[i] = bestEnum;
    }
  };

  // -----------------------------------------
  // Full rebuild (if needed)
  // -----------------------------------------
  auto doFullRebuild = [&]() {
    m_tree->Freeze();
    m_tree->DeleteAllItems();

    wxTreeItemId root = m_tree->AddRoot(wxT("root"));

    m_itemByIndex.assign(m_symbols.size(), wxTreeItemId());
    m_tooltipByIndex.assign(m_symbols.size(), std::string());
    m_lastTipItem = wxTreeItemId();
    m_tree->SetToolTip((wxToolTip *)nullptr);

    const int n = (int)m_symbols.size();

    std::vector<int> parentIdx;
    computeParentIdx(parentIdx);

    std::vector<wxTreeItemId> itemByIndex(n, wxTreeItemId());

    for (int i = 0; i < n; i++) {
      const auto &s = m_symbols[i];
      if (s.kind == CXCursor_ParmDecl)
        continue;

      wxTreeItemId parentItem = root;
      int p = parentIdx[i];
      if (p >= 0 && p < n && itemByIndex[p].IsOk())
        parentItem = itemByIndex[p];

      wxString label = MakeBaseLabel(s);

      // hover cache enrichment
      HoverCacheEntry cached;
      bool hasCached = false;
      const std::string key = getKey(s);
      if (!key.empty()) {
        std::lock_guard<std::mutex> lk(m_hoverCacheMutex);
        auto it = m_hoverCache.find(key);
        if (it != m_hoverCache.end()) {
          cached = it->second;
          hasCached = true;
        }
      }

      if (hasCached) {
        wxString enriched = MakeLabelFromHover(s, cached.info);
        if (!enriched.empty())
          label = enriched;

        if (!cached.tooltip.empty())
          m_tooltipByIndex[i] = cached.tooltip;
      }

      const int img = imgFor(s);
      wxTreeItemId item = m_tree->AppendItem(
          parentItem,
          label,
          img,
          img,
          new ItemData(i, getKey(s), s.line, s.column));

      itemByIndex[i] = item;
      m_itemByIndex[i] = item;
    }

    m_tree->Expand(root);
    m_tree->Thaw();
  };

  if (m_forceFullRebuildNext) {
    m_forceFullRebuildNext = false;
    doFullRebuild();
    return;
  }

  // -----------------------------------------
  // Diff rebuild
  // -----------------------------------------

  wxTreeItemId root = m_tree->GetRootItem();
  if (!root.IsOk()) {
    doFullRebuild();
    return;
  }

  // 1) remember expanded nodes
  std::unordered_set<std::string> expandedKeys;
  std::string selectedKey;

  {
    wxTreeItemId sel = m_tree->GetSelection();
    if (sel.IsOk()) {
      if (auto *d = dynamic_cast<ItemData *>(m_tree->GetItemData(sel))) {
        selectedKey = d->usr;
      }
    }

    auto collectExpanded = [&](auto &&self, const wxTreeItemId &item) -> void {
      if (!item.IsOk())
        return;

      if (item != root && m_tree->IsExpanded(item)) {
        if (auto *d = dynamic_cast<ItemData *>(m_tree->GetItemData(item))) {
          if (!d->usr.empty())
            expandedKeys.insert(d->usr);
        }
      }

      wxTreeItemIdValue cookie;
      wxTreeItemId child = m_tree->GetFirstChild(item, cookie);
      while (child.IsOk()) {
        self(self, child);
        child = m_tree->GetNextChild(item, cookie);
      }
    };

    collectExpanded(collectExpanded, root);
  }

  // 2) calculate parent for new symbols
  std::vector<int> parentIdx;
  computeParentIdx(parentIdx);

  // 3) prepare keys
  const int n = (int)m_symbols.size();
  std::unordered_set<std::string> keepKeys;
  keepKeys.reserve((size_t)n * 2);

  for (int i = 0; i < n; i++) {
    const auto &s = m_symbols[i];
    if (s.kind == CXCursor_ParmDecl)
      continue;
    keepKeys.insert(getKey(s));
  }

  // 4) prune
  m_tree->Freeze();

  m_itemByIndex.assign(m_symbols.size(), wxTreeItemId());
  m_tooltipByIndex.assign(m_symbols.size(), std::string());
  m_lastTipItem = wxTreeItemId();
  m_tree->SetToolTip((wxToolTip *)nullptr);

  {
    auto prune = [&](auto &&self, const wxTreeItemId &item) -> void {
      if (!item.IsOk())
        return;

      // children first (post-order)
      std::vector<wxTreeItemId> kids;
      kids.reserve(16);

      wxTreeItemIdValue cookie;
      wxTreeItemId child = m_tree->GetFirstChild(item, cookie);
      while (child.IsOk()) {
        kids.push_back(child);
        child = m_tree->GetNextChild(item, cookie);
      }

      for (auto &c : kids)
        self(self, c);

      if (item == root)
        return;

      auto *d = dynamic_cast<ItemData *>(m_tree->GetItemData(item));
      if (!d)
        return;

      if (!d->usr.empty() && keepKeys.find(d->usr) == keepKeys.end()) {
        m_tree->Delete(item);
      }
    };

    prune(prune, root);
  }

  // 5) map of existing items by USR
  std::unordered_map<std::string, wxTreeItemId> itemByKey;
  itemByKey.reserve(keepKeys.size() * 2);

  {
    auto indexExisting = [&](auto &&self, const wxTreeItemId &item) -> void {
      if (!item.IsOk())
        return;

      if (item != root) {
        if (auto *d = dynamic_cast<ItemData *>(m_tree->GetItemData(item))) {
          if (!d->usr.empty())
            itemByKey[d->usr] = item;
        }
      }

      wxTreeItemIdValue cookie;
      wxTreeItemId child = m_tree->GetFirstChild(item, cookie);
      while (child.IsOk()) {
        self(self, child);
        child = m_tree->GetNextChild(item, cookie);
      }
    };

    indexExisting(indexExisting, root);
  }

  auto getParentItem = [&](int idx) -> wxTreeItemId {
    int p = parentIdx[idx];
    if (p < 0 || p >= n)
      return root;

    const std::string pk = getKey(m_symbols[p]);
    auto it = itemByKey.find(pk);
    if (it != itemByKey.end() && it->second.IsOk())
      return it->second;

    return root;
  };

  auto computeLabelAndTooltip = [&](int i, wxString &outLabel, std::string &outTooltip) {
    const auto &s = m_symbols[i];

    outLabel = MakeBaseLabel(s);
    outTooltip.clear();

    const std::string key = getKey(s);
    if (!key.empty()) {
      HoverCacheEntry cached;
      bool hasCached = false;

      {
        std::lock_guard<std::mutex> lk(m_hoverCacheMutex);
        auto it = m_hoverCache.find(key);
        if (it != m_hoverCache.end()) {
          cached = it->second;
          hasCached = true;
        }
      }

      if (hasCached) {
        wxString enriched = MakeLabelFromHover(s, cached.info);
        if (!enriched.empty())
          outLabel = enriched;
        outTooltip = cached.tooltip;
      }
    }
  };

  // 6) create/update items for all symbols
  for (int i = 0; i < n; i++) {
    const auto &s = m_symbols[i];
    if (s.kind == CXCursor_ParmDecl)
      continue;

    const std::string key = getKey(s);
    wxTreeItemId parentItem = getParentItem(i);

    wxString label;
    std::string tooltip;
    computeLabelAndTooltip(i, label, tooltip);
    if (!tooltip.empty())
      m_tooltipByIndex[i] = tooltip;

    const int img = imgFor(s);

    wxTreeItemId item;
    auto it = itemByKey.find(key);
    if (it != itemByKey.end())
      item = it->second;

    // exists, but is under a different parent? -> delete + recreate
    if (item.IsOk()) {
      wxTreeItemId curParent = m_tree->GetItemParent(item);
      if (curParent.IsOk() && curParent != parentItem) {
        m_tree->Delete(item);
        item = wxTreeItemId();
        itemByKey.erase(key);
      }
    }

    if (!item.IsOk()) {
      wxTreeItemId before = findInsertBefore(parentItem, s.line, s.column);

      if (before.IsOk()) {
        item = m_tree->InsertItem(
            parentItem, before, label, img, img,
            new ItemData(i, key, s.line, s.column));
      } else {
        item = m_tree->AppendItem(
            parentItem, label, img, img,
            new ItemData(i, key, s.line, s.column));
      }

      itemByKey[key] = item;
    } else {
      // update text
      wxString old = m_tree->GetItemText(item);
      if (old != label)
        m_tree->SetItemText(item, label);

      // update image
      m_tree->SetItemImage(item, img, wxTreeItemIcon_Normal);
      m_tree->SetItemImage(item, img, wxTreeItemIcon_Selected);

      // update data
      if (auto *d = dynamic_cast<ItemData *>(m_tree->GetItemData(item))) {
        d->index = i;
        d->sortLine = s.line;
        d->sortCol = s.column;
        d->usr = key;
      } else {
        m_tree->SetItemData(item, new ItemData(i, key, s.line, s.column));
      }
    }

    m_itemByIndex[i] = item;
  }

  // 7) refresh expanded items
  for (const auto &k : expandedKeys) {
    auto it = itemByKey.find(k);
    if (it != itemByKey.end() && it->second.IsOk()) {
      m_tree->Expand(it->second);
    }
  }

  // 8) refresh selection
  if (!selectedKey.empty()) {
    auto it = itemByKey.find(selectedKey);
    if (it != itemByKey.end() && it->second.IsOk()) {
      m_internalSelect = true;
      m_tree->SelectItem(it->second);
      m_tree->EnsureVisible(it->second);
      m_internalSelect = false;
    }
  }

  m_tree->Thaw();
}

wxString ArduinoClassBrowserPanel::MakeBaseLabel(const SymbolInfo &s) const {
  if (!s.display.empty()) {
    return wxString::FromUTF8(s.display);
  }

  return wxString::FromUTF8(s.name);
}

wxString ArduinoClassBrowserPanel::MakeLabelFromHover(const SymbolInfo &s, const HoverInfo &info) const {
  if (IsFunctionKind(s.kind)) {
    std::string lbl = BuildFunctionLabelFromHover(info);
    if (!lbl.empty()) {
      return wxString::FromUTF8(lbl);
    }

    if (!info.signature.empty()) {
      return wxString::FromUTF8(info.signature);
    }
  }

  // Variables/fields -> show "name : type"
  if ((s.kind == CXCursor_VarDecl || s.kind == CXCursor_FieldDecl) && !info.type.empty()) {
    std::string txt = s.name;
    txt.append(" : ").append(info.type);
    return wxString::FromUTF8(txt);
  }

  // EnumDecl -> optionally show underlying type if it exists in hover
  if (s.kind == CXCursor_EnumDecl && !info.type.empty()) {
    std::string txt = s.name;
    txt.append(" : ").append(info.type);
    return wxString::FromUTF8(txt);
  }

  return MakeBaseLabel(s);
}

void ArduinoClassBrowserPanel::ApplyHoverInfo(uint64_t gen, int symbolIndex, const HoverInfo &info) {
  if (gen != m_generation.load())
    return;
  if (symbolIndex < 0 || symbolIndex >= (int)m_symbols.size())
    return;
  if (symbolIndex >= (int)m_itemByIndex.size())
    return;

  wxTreeItemId item = m_itemByIndex[symbolIndex];
  if (!item.IsOk())
    return;

  // tooltip
  {
    const std::string tip = info.ToHoverString();
    if (!tip.empty()) {
      m_tooltipByIndex[symbolIndex] = tip;

      const std::string &key = m_symbols[symbolIndex].usr;
      if (!key.empty()) {
        HoverCacheEntry e;
        e.info = info;
        e.tooltip = tip;

        std::lock_guard<std::mutex> lk(m_hoverCacheMutex);
        m_hoverCache[key] = std::move(e);
      }

      if (m_lastTipItem.IsOk() && m_lastTipItem == item) {
        m_tree->SetToolTip(wxString::FromUTF8(m_tooltipByIndex[symbolIndex]));
      }
    } else {
      if (m_lastTipItem.IsOk() && m_lastTipItem == item) {
        m_tree->SetToolTip((wxToolTip *)nullptr);
      }
    }
  }

  // label enrichment
  const SymbolInfo &s = m_symbols[symbolIndex];
  wxString newLabel = MakeLabelFromHover(s, info);

  if (!newLabel.empty()) {
    wxString old = m_tree->GetItemText(item);
    if (old != newLabel) {
      m_tree->SetItemText(item, newLabel);
    }
  }
}

void ArduinoClassBrowserPanel::StartHoverWorker(uint64_t gen, std::string file, std::string codeSnapshot, const std::vector<SketchFileBuffer> &files) {
  if (!m_completion)
    return;
  if (m_symbols.empty())
    return;

  m_cancel.store(false, std::memory_order_relaxed);

  m_worker = std::thread([this, gen, file = std::move(file), code = std::move(codeSnapshot), files = std::move(files)]() mutable {
    // batch UI updates to avoid spamming event loop
    std::vector<std::pair<int, HoverInfo>> batch;
    batch.reserve(8);

    auto flush = [&]() {
      if (batch.empty())
        return;
      auto send = std::move(batch);
      batch.clear();

      // UI thread apply
      CallAfter([this, gen, send = std::move(send)]() mutable {
        if (gen != m_generation.load())
          return;
        for (auto &it : send) {
          ApplyHoverInfo(gen, it.first, it.second);
        }
      });
    };

    for (int i = 0; i < (int)m_symbols.size(); i++) {
      if (m_cancel.load(std::memory_order_relaxed))
        return;
      if (gen != m_generation.load())
        return;

      const auto &s = m_symbols[i];

      // Skip items that typically don't have interesting hover, or have no position.
      if (s.line <= 0)
        continue;
      if (s.kind == CXCursor_ParmDecl)
        continue;

      // skip if hover in cache
      const std::string &key = s.usr;
      if (!key.empty()) {
        bool already = false;
        {
          std::lock_guard<std::mutex> lk(m_hoverCacheMutex);
          already = (m_hoverCache.find(key) != m_hoverCache.end());
        }
        if (already)
          continue;
      }

      HoverInfo hi;

      int col = s.column > 0 ? s.column : 1;
      if (!m_completion->GetHoverInfo(file, code, s.line, col, files, hi)) {
        continue;
      }

      batch.emplace_back(i, std::move(hi));
      if (batch.size() >= 8) {
        flush();
      }
    }

    flush();
  });
}

int ArduinoClassBrowserPanel::FindBestSymbolForLine(int line) const {
  return FindBestSymbolForPos(line, 0);
}

int ArduinoClassBrowserPanel::FindBestSymbolForPos(int line, int col) const {
  if (line <= 0)
    return -1;

  const int n = (int)m_symbols.size();

  int bestOnLine = -1;
  int bestDist = INT_MAX;

  for (int i = 0; i < n; i++) {
    const auto &s = m_symbols[i];
    if (s.kind == CXCursor_ParmDecl)
      continue;
    if (s.line != line)
      continue;

    if (s.column <= 0 || col <= 0) {
      return i;
    }

    int dist;
    if (s.column <= col) {
      dist = col - s.column;
    } else {
      dist = 1000000 + (s.column - col);
    }

    if (dist < bestDist) {
      bestDist = dist;
      bestOnLine = i;
    }
  }

  if (bestOnLine >= 0)
    return bestOnLine;

  auto containsPosInBody = [&](const SymbolInfo &scope) -> bool {
    if (!(scope.bodyLineFrom > 0 && scope.bodyLineTo >= scope.bodyLineFrom))
      return false;

    if (line < scope.bodyLineFrom || line > scope.bodyLineTo)
      return false;

    if (col <= 0 || scope.bodyColFrom <= 0 || scope.bodyColTo <= 0)
      return true;

    if (line == scope.bodyLineFrom && col < scope.bodyColFrom)
      return false;
    if (line == scope.bodyLineTo && col > scope.bodyColTo)
      return false;

    return true;
  };

  int bestScope = -1;
  int bestSpan = INT_MAX;

  for (int i = 0; i < n; i++) {
    const auto &s = m_symbols[i];
    if (!containsPosInBody(s))
      continue;

    int span = s.bodyLineTo - s.bodyLineFrom;
    if (span < bestSpan) {
      bestSpan = span;
      bestScope = i;
    }
  }

  if (bestScope >= 0)
    return bestScope;

  for (int i = 0; i < n; i++) {
    if (m_symbols[i].kind == CXCursor_ParmDecl)
      continue;
    if (m_symbols[i].line == line)
      return i;
  }

  return -1;
}

void ArduinoClassBrowserPanel::SetCurrentLine(int line) {
  SetCurrentLine(line, 0);
}

void ArduinoClassBrowserPanel::SetCurrentLine(int line, int col) {
  if (m_internalSelect)
    return;

  int idx = FindBestSymbolForPos(line, col);
  if (idx < 0 || idx >= (int)m_itemByIndex.size())
    return;

  wxTreeItemId item = m_itemByIndex[idx];
  if (!item.IsOk())
    return;

  m_internalSelect = true;
  m_tree->SelectItem(item);
  m_tree->EnsureVisible(item);
  m_internalSelect = false;
}

void ArduinoClassBrowserPanel::OnItemActivated(wxTreeEvent &e) {
  wxTreeItemId item = e.GetItem();
  if (!item.IsOk())
    return;

  auto *data = dynamic_cast<ItemData *>(m_tree->GetItemData(item));
  if (!data)
    return;

  int idx = data->index;
  if (idx < 0 || idx >= (int)m_symbols.size())
    return;

  const SymbolInfo &s = m_symbols[idx];

  JumpTarget tgt;
  tgt.file = s.file;
  tgt.line = s.line;
  tgt.column = s.column;

  ArduinoDiagnosticsActionEvent ev(EVT_ARD_DIAG_JUMP, GetId());
  ev.SetEventObject(this);
  ev.SetJumpTarget(tgt);
  wxPostEvent(GetParent(), ev);
}

void ArduinoClassBrowserPanel::OnTreeGetToolTip(wxTreeEvent &e) {
  wxTreeItemId item = e.GetItem();
  if (!item.IsOk()) {
    e.Skip();
    return;
  }

  auto *data = dynamic_cast<ItemData *>(m_tree->GetItemData(item));
  if (!data) {
    e.Skip();
    return;
  }

  const int idx = data->index;
  if (idx >= 0 && idx < (int)m_tooltipByIndex.size() && !m_tooltipByIndex[idx].empty()) {
    const wxString tip = wxString::FromUTF8(m_tooltipByIndex[idx]);
    e.SetToolTip(tip);
  } else {
    e.Skip();
  }
}
