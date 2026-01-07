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

    // Zobrazení jen typů (bez jména parametru) – to chceš.
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

std::string ArduinoClassBrowserPanel::MakeHoverKey(const SymbolInfo &s) const {
  int col = s.column > 0 ? s.column : 1;

  std::string key;
  key.reserve(s.name.size() + 64);
  key.append(std::to_string((int)s.kind)).append("|");
  key.append(std::to_string(s.line)).append("|");
  key.append(std::to_string(col)).append("|");
  key.append(s.name);
  return key;
}

bool ArduinoClassBrowserPanel::IsSameEditorContext(ArduinoEditor *ed, const std::string &file, uint64_t snapshotHash) const {
  return ed == m_cachedEditor && file == m_cachedFile && snapshotHash == m_cachedSnapshotHash;
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

int ArduinoClassBrowserPanel::KindToImage(CXCursorKind kind) const {
  if (IsMacroKind(kind))
    return 3;
  if (IsContainerKind(kind))
    return 2;
  if (IsFunctionKind(kind))
    return 0;
  return 1; // "variable / everything else"
}

bool ArduinoClassBrowserPanel::IsMacroKind(CXCursorKind kind) const {
  return kind == CXCursor_MacroDefinition;
}

bool ArduinoClassBrowserPanel::IsContainerKind(CXCursorKind kind) const {
  return (kind == CXCursor_ClassDecl ||
          kind == CXCursor_StructDecl ||
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
}

void ArduinoClassBrowserPanel::SetCompletion(ArduinoCodeCompletion *cc) {
  m_completion = cc;
}

static bool EndsWith(const std::string &s, const std::string &suf) {
  if (suf.empty())
    return true;
  if (s.size() < suf.size())
    return false;
  return s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

void ArduinoClassBrowserPanel::SetCurrentEditor(ArduinoEditor *ed) {
  StopHoverWorker();

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

  if (!IsSameEditorContext(m_editor, curFile, snapshotHash)) {
    ResetHoverCacheForNewContext(m_editor, curFile, snapshotHash);
  }

  std::vector<SketchFileBuffer> filesSnapshot;
  m_completion->CollectSketchFiles(filesSnapshot);

  auto all = m_completion->GetAllSymbols(curFile, m_codeSnapshot);

  std::vector<SymbolInfo> filtered;
  filtered.reserve(all.size());

  for (auto &s : all) {
    if (EndsWith(s.file, ".ino.hpp")) {
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
        // key: file|name|line|col (col can be 0, but it's still stable)
        std::string key;
        key.reserve(s.file.size() + s.name.size() + 64);
        key.append(s.file).append("|").append(s.name).append("|");
        key.append(std::to_string(s.line)).append("|").append(std::to_string(s.column));

        if (!seen.insert(key).second) {
          continue; // dup
        }
      }
      filtered.push_back(std::move(s));
    }

    symbols = std::move(filtered);
  }

  m_symbols = std::move(symbols);

  m_hoverKeyByIndex.clear();
  m_hoverKeyByIndex.reserve(m_symbols.size());
  for (const auto &s : m_symbols) {
    m_hoverKeyByIndex.push_back(MakeHoverKey(s));
  }

  RebuildTree();
}

void ArduinoClassBrowserPanel::RebuildTree() {
  auto isScope = [](const SymbolInfo &s) -> bool {
    return (s.bodyLineFrom > 0 && s.bodyLineTo >= s.bodyLineFrom);
  };

  auto posInBody = [&isScope](const SymbolInfo &scope, int line, int col) -> bool {
    if (!isScope(scope))
      return false;

    // Column can be unknown (0). BodyColFrom/To can also be 0.
    auto afterStart = [&]() -> bool {
      if (line > scope.bodyLineFrom)
        return true;
      if (line < scope.bodyLineFrom)
        return false;
      // same line
      if (scope.bodyColFrom <= 0 || col <= 0)
        return true;
      return col >= scope.bodyColFrom;
    };

    auto beforeEnd = [&]() -> bool {
      if (line < scope.bodyLineTo)
        return true;
      if (line > scope.bodyLineTo)
        return false;
      // same line
      if (scope.bodyColTo <= 0 || col <= 0)
        return true;
      return col <= scope.bodyColTo;
    };

    return afterStart() && beforeEnd();
  };

  auto scopeSpanKey = [](const SymbolInfo &s) -> long long {
    // small spans first (innermost)
    long long dl = (long long)s.bodyLineTo - (long long)s.bodyLineFrom;
    long long dc = (long long)s.bodyColTo - (long long)s.bodyColFrom;
    if (dc < 0)
      dc = 0;
    return dl * 100000LL + dc;
  };

  m_tree->Freeze();
  m_tree->DeleteAllItems();

  wxTreeItemId root = m_tree->AddRoot(wxT("root"));
  m_itemByIndex.assign(m_symbols.size(), wxTreeItemId());

  m_tooltipByIndex.assign(m_symbols.size(), std::string());
  m_lastTipItem = wxTreeItemId();
  m_tree->SetToolTip((wxToolTip *)nullptr);

  const int n = (int)m_symbols.size();
  std::vector<int> scopes;
  scopes.reserve(n);

  // collect all scopes (anything with valid body range)
  for (int i = 0; i < n; i++) {
    if (isScope(m_symbols[i])) {
      scopes.push_back(i);
    }
  }

  // sort scopes by size (innermost first)
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

  // for every symbol, pick the innermost scope that contains its declaration position
  std::vector<int> parentIdx(n, -1);
  for (int i = 0; i < n; i++) {
    const auto &sym = m_symbols[i];

    if (sym.kind == CXCursor_ParmDecl) {
      continue;
    }

    for (int sc : scopes) {
      if (sc == i)
        continue;

      const auto &scope = m_symbols[sc];

      if (posInBody(scope, sym.line, sym.column)) {
        parentIdx[i] = sc;
        break; // first match = innermost (scopes are sorted by span)
      }
    }
  }

  // ------------------------------------------------------------
  // Special case: EnumConstantDecl doesn't have body-range.
  // Group enum constants under the nearest preceding EnumDecl
  // within the same immediate scope (class/function/namespace...).
  // ------------------------------------------------------------
  for (int i = 0; i < n; i++) {
    const auto &sym = m_symbols[i];
    if (sym.kind != CXCursor_EnumConstantDecl)
      continue;

    const int scopeParent = parentIdx[i]; // např. class/function scope (ne enum!)
    int bestEnum = -1;

    for (int j = i - 1; j >= 0; j--) {
      const auto &cand = m_symbols[j];
      if (cand.kind != CXCursor_EnumDecl)
        continue;

      // musí být ve stejném souboru a ve stejném "nad-scope"
      if (cand.file != sym.file)
        continue;

      if (parentIdx[j] != scopeParent)
        continue;

      // nejbližší EnumDecl nad konstantou
      bestEnum = j;
      break;
    }

    if (bestEnum >= 0) {
      parentIdx[i] = bestEnum;
    }
  }

  // create tree items in file order (m_symbols are already sorted by line/col in SetSymbols)
  std::vector<wxTreeItemId> itemByIndex(n, wxTreeItemId());

  for (int i = 0; i < n; i++) {
    const auto &s = m_symbols[i];
    if (s.kind == CXCursor_ParmDecl)
      continue;

    wxTreeItemId parentItem = root;

    int p = parentIdx[i];
    if (p >= 0 && p < n && itemByIndex[p].IsOk()) {
      parentItem = itemByIndex[p];
    }

    wxString label = MakeBaseLabel(s);

    HoverCacheEntry cached;
    bool hasCached = false;

    if (i >= 0 && i < (int)m_hoverKeyByIndex.size()) {
      std::lock_guard<std::mutex> lk(m_hoverCacheMutex);
      auto it = m_hoverCache.find(m_hoverKeyByIndex[i]);
      if (it != m_hoverCache.end()) {
        cached = it->second; // kopie je OK, HoverInfo je malý struct se stringy
        hasCached = true;
      }
    }

    if (hasCached) {
      wxString enriched = MakeLabelFromHover(s, cached.info);
      if (!enriched.empty())
        label = enriched;

      if (i >= 0 && i < (int)m_tooltipByIndex.size() && !cached.tooltip.empty()) {
        m_tooltipByIndex[i] = cached.tooltip;
      }
    }

    int img = KindToImage(s.kind);

    wxTreeItemId item = m_tree->AppendItem(parentItem, label, img, img, new ItemData(i));
    itemByIndex[i] = item;
    m_itemByIndex[i] = item;
  }

  m_tree->Expand(root);
  m_tree->Thaw();
}

wxString ArduinoClassBrowserPanel::MakeBaseLabel(const SymbolInfo &s) const {
  if (!s.display.empty())
    return wxString::FromUTF8(s.display);
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
  if (symbolIndex < 0 || symbolIndex >= (int)m_tooltipByIndex.size())
    return;

  wxTreeItemId item = m_itemByIndex[symbolIndex];
  if (!item.IsOk())
    return;

  // tooltip
  {
    std::string hover = info.ToHoverString();
    if (!hover.empty()) {

      if (symbolIndex >= 0 && symbolIndex < (int)m_hoverKeyByIndex.size()) {
        HoverCacheEntry e;
        e.info = info;
        e.tooltip = info.ToHoverString();

        {
          std::lock_guard<std::mutex> lk(m_hoverCacheMutex);
          m_hoverCache[m_hoverKeyByIndex[symbolIndex]] = e;
        }

        if (!e.tooltip.empty()) {
          m_tooltipByIndex[symbolIndex] = e.tooltip;

          if (m_lastTipItem.IsOk() && m_lastTipItem == item) {
            m_tree->SetToolTip(wxString::FromUTF8(e.tooltip));
          }
        }
      }

      // if we are currently hovering over this item, update the tooltip immediately
      if (m_lastTipItem.IsOk() && m_lastTipItem == item) {
        if (!m_tooltipByIndex[symbolIndex].empty()) {
          m_tree->SetToolTip(wxString::FromUTF8(m_tooltipByIndex[symbolIndex]));
        } else {
          m_tree->SetToolTip((wxToolTip *)nullptr);
        }
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
      if (i >= 0 && i < (int)m_hoverKeyByIndex.size()) {
        const std::string &key = m_hoverKeyByIndex[i];
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
  if (line <= 0)
    return -1;

  int best = -1;
  int bestSpan = INT_MAX;

  for (int i = 0; i < (int)m_symbols.size(); i++) {
    const auto &s = m_symbols[i];

    if (s.bodyLineFrom > 0 && s.bodyLineTo >= s.bodyLineFrom) {
      if (line >= s.bodyLineFrom && line <= s.bodyLineTo) {
        int span = s.bodyLineTo - s.bodyLineFrom;
        if (span < bestSpan) {
          bestSpan = span;
          best = i;
        }
      }
    }
  }

  if (best < 0) {
    for (int i = 0; i < (int)m_symbols.size(); i++) {
      if (m_symbols[i].line == line) {
        best = i;
        break;
      }
    }
  }

  return best;
}

void ArduinoClassBrowserPanel::SetCurrentLine(int line) {
  if (m_internalSelect)
    return;

  int idx = FindBestSymbolForLine(line);
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
