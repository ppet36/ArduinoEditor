// ard_diff.cpp
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

#include "ard_diff.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>

#include <wx/button.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>

// -------------------- ctor --------------------

ArduinoDiffDialog::ArduinoDiffDialog(wxWindow *parent,
                                     const std::vector<SketchFileBuffer> &buffersOld,
                                     const std::vector<SketchFileBuffer> &buffersNew,
                                     ArduinoCli *cli,
                                     wxConfigBase *config,
                                     const wxString &aiComment)
    : wxDialog(parent,
               wxID_ANY,
               _("Review AI Changes"),
               wxDefaultPosition,
               wxSize(1100, 700),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_cli(cli), m_config(config), m_aiComment(aiComment) {

  // 1) read divider position first (before creating splitters)
  if (m_config) {
    long v = -1;
    if (m_config->Read(wxT("DiffDialog/DivPos"), &v) && v > 0) {
      m_diffDivPos = (int)v;
    }
  }

  BuildUi(buffersOld, buffersNew);

  if (!LoadWindowSize(wxT("DiffDialog"), this, m_config)) {
    CentreOnParent();
  }
}

ArduinoDiffDialog::~ArduinoDiffDialog() {
  SaveWindowSize(wxT("DiffDialog"), this, m_config);
  if (m_diffDivPos > 0) {
    m_config->Write(wxT("DiffDialog/DivPos"), (long)m_diffDivPos);
  }
}

wxString ArduinoDiffDialog::GetAdditionalInfo() {
  // TODO
  // A wxTextCtrl should be added here, in which the user can write a response
  // to the model patch and return the model patch with an explanation.
  return wxEmptyString;
}

// -------------------- UI build --------------------

void ArduinoDiffDialog::BuildUi(const std::vector<SketchFileBuffer> &buffersOld,
                                const std::vector<SketchFileBuffer> &buffersNew) {
  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  // AI comment, if any
  wxString c = m_aiComment;
  c.Trim(true).Trim(false);
  if (!c.IsEmpty()) {
    m_aiPane = new wxCollapsiblePane(this, wxID_ANY, _("AI explanation"),
                                     wxDefaultPosition, wxDefaultSize,
                                     wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);

    auto *paneWin = m_aiPane->GetPane();
    auto *ps = new wxBoxSizer(wxVERTICAL);

    m_aiMd = new ArduinoMarkdownPanel(paneWin, wxID_ANY);
    m_aiMd->SetMinSize(wxSize(-1, 110)); // stejně “kompaktní” jako dřív

    m_aiMd->AppendMarkdown(c, AiMarkdownRole::Assistant);
    m_aiMd->Render(false);

    ps->Add(m_aiMd, 1, wxEXPAND | wxALL, 6);
    paneWin->SetSizer(ps);

    m_aiPane->Collapse(false);
    topSizer->Add(m_aiPane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    m_aiPane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent &e) {
      Layout();
      if (m_notebook)
        Layout();
      e.Skip();
    });
  }

  // Notebook
  m_notebook = new wxNotebook(this, wxID_ANY);
  topSizer->Add(m_notebook, 1, wxEXPAND | wxALL, 8);

  // Buttons
  auto *btnSizer = new wxStdDialogButtonSizer();
  btnSizer->AddButton(new wxButton(this, wxID_OK, _("Apply")));
  btnSizer->AddButton(new wxButton(this, wxID_CANCEL, _("Cancel")));
  btnSizer->Realize();
  topSizer->Add(btnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  SetSizer(topSizer);

  m_views.clear();
  m_changedBuffersNew.clear();

  // Build lookups for old/new buffers by normalized filename.
  std::unordered_map<std::wstring, wxString> mapKeyToOldText;
  std::unordered_map<std::wstring, wxString> mapKeyToNewText;
  std::unordered_map<std::wstring, size_t> mapKeyToNewIndex;

  mapKeyToOldText.reserve(buffersOld.size() * 2);
  mapKeyToNewText.reserve(buffersNew.size() * 2);
  mapKeyToNewIndex.reserve(buffersNew.size() * 2);

  for (const auto &b : buffersOld) {
    const wxString fn = wxString::FromUTF8(b.filename.c_str());
    const wxString key = NormalizeKey(fn);
    mapKeyToOldText[key.ToStdWstring()] = wxString::FromUTF8(b.code.c_str());
  }

  for (size_t i = 0; i < buffersNew.size(); ++i) {
    const auto &b = buffersNew[i];
    const wxString fn = wxString::FromUTF8(b.filename.c_str());
    const wxString key = NormalizeKey(fn);
    const std::wstring wk = key.ToStdWstring();
    mapKeyToNewText[wk] = wxString::FromUTF8(b.code.c_str());
    mapKeyToNewIndex[wk] = i;
  }

  // Determine tab order: prefer new buffers order, then any old-only (deleted) files.
  std::vector<wxString> fileOrder;
  std::set<std::wstring> seen;

  fileOrder.reserve(mapKeyToNewText.size() + mapKeyToOldText.size());

  for (const auto &b : buffersNew) {
    const wxString fn = wxString::FromUTF8(b.filename.c_str());
    const wxString key = NormalizeKey(fn);
    const std::wstring wk = key.ToStdWstring();
    if (seen.insert(wk).second) {
      fileOrder.push_back(fn);
    }
  }

  for (const auto &b : buffersOld) {
    const wxString fn = wxString::FromUTF8(b.filename.c_str());
    const wxString key = NormalizeKey(fn);
    const std::wstring wk = key.ToStdWstring();
    if (seen.insert(wk).second) {
      fileOrder.push_back(fn);
    }
  }

  // Create tabs
  for (const auto &fileKey : fileOrder) {
    const wxString normKey = NormalizeKey(fileKey);
    const std::wstring wk = normKey.ToStdWstring();

    auto itOld = mapKeyToOldText.find(wk);
    auto itNew = mapKeyToNewText.find(wk);

    if (itOld == mapKeyToOldText.end() && itNew == mapKeyToNewText.end()) {
      continue;
    }

    m_views.emplace_back();
    FileViewData &v = m_views.back();

    v.fileKey = fileKey;
    v.resolvedKey = normKey;
    v.isNewFile = (itOld == mapKeyToOldText.end() && itNew != mapKeyToNewText.end());
    v.isDeletedFile = (itOld != mapKeyToOldText.end() && itNew == mapKeyToNewText.end());

    // Skip unchanged existing files (no changes in content).
    if (!v.isNewFile && !v.isDeletedFile) {
      const wxString oldTxt = (itOld != mapKeyToOldText.end()) ? itOld->second : wxString();
      const wxString newTxt = (itNew != mapKeyToNewText.end()) ? itNew->second : wxString();
      if (CanonicalizeEol(oldTxt) == CanonicalizeEol(newTxt)) {
        m_views.pop_back();
        continue;
      }
    }

    if (v.isNewFile) {
      v.newText = itNew->second;
      CreateNewFileTab(m_notebook, v, v.newText);

      // New file => changed
      auto itIdx = mapKeyToNewIndex.find(wk);
      if (itIdx != mapKeyToNewIndex.end()) {
        m_changedBuffersNew.push_back(buffersNew[itIdx->second]);
      }
    } else {
      v.oldText = (itOld != mapKeyToOldText.end()) ? itOld->second : wxString();
      v.newText = (itNew != mapKeyToNewText.end()) ? itNew->second : wxString();

      CreateExistingFileTab(m_notebook, v, v.oldText, v.newText);
      BindScrollSync(v);

      // Modified file => changed (deleted files have no new buffer)
      if (!v.isDeletedFile) {
        auto itIdx = mapKeyToNewIndex.find(wk);
        if (itIdx != mapKeyToNewIndex.end()) {
          m_changedBuffersNew.push_back(buffersNew[itIdx->second]);
        }
      }
    }
  }

  if (m_notebook->GetPageCount() == 0) {
    // Graceful empty state
    auto *p = new wxPanel(m_notebook);
    auto *s = new wxBoxSizer(wxVERTICAL);
    s->Add(new wxStaticText(p, wxID_ANY, _("No changes to review.")), 0, wxALL, 12);
    p->SetSizer(s);
    m_notebook->AddPage(p, _("Empty"), true);
  }
}

wxPanel *ArduinoDiffDialog::CreateExistingFileTab(wxNotebook *nb,
                                                  FileViewData &v,
                                                  const wxString &oldText,
                                                  const wxString &newText) {
  auto *panel = new wxPanel(nb);
  auto *root = new wxBoxSizer(wxVERTICAL);

  // Optional title line
  {
    wxString title;
    if (v.isDeletedFile) {
      title = wxString::Format(_("Deleted file: %s"), v.fileKey);
    } else {
      title = wxString::Format(_("File: %s"), v.fileKey);
    }
    root->Add(new wxStaticText(panel, wxID_ANY, title), 0, wxLEFT | wxRIGHT | wxTOP, 6);
  }

  // Split view
  auto *splitter = new wxSplitterWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxSP_LIVE_UPDATE | wxSP_3D);
  splitter->SetMinimumPaneSize(100);

  auto *leftPanel = new wxPanel(splitter);
  auto *rightPanel = new wxPanel(splitter);

  auto *leftSizer = new wxBoxSizer(wxVERTICAL);
  auto *rightSizer = new wxBoxSizer(wxVERTICAL);

  leftSizer->Add(new wxStaticText(leftPanel, wxID_ANY, _("Original")), 0, wxALL, 4);
  rightSizer->Add(new wxStaticText(rightPanel, wxID_ANY, v.isDeletedFile ? _("Deleted") : _("Modified")), 0, wxALL, 4);

  v.oldText = oldText;
  v.newText = newText;

  v.left = new wxStyledTextCtrl(leftPanel, wxID_ANY);
  v.right = new wxStyledTextCtrl(rightPanel, wxID_ANY);

  SetupStyledTextCtrl(v.left, m_config);
  SetupStyledTextCtrl(v.right, m_config);

  SetupDiffIndicators(v.left);
  SetupDiffIndicators(v.right);

  UpdateExistingTabView(v);

  SetupReadOnlyDiffCtrl(v.left);
  SetupReadOnlyDiffCtrl(v.right);

  leftSizer->Add(v.left, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
  rightSizer->Add(v.right, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

  leftPanel->SetSizer(leftSizer);
  rightPanel->SetSizer(rightSizer);

  int initialPos = splitter->GetSize().GetWidth() / 2;
  if (m_diffDivPos > 0) {
    initialPos = m_diffDivPos;
  }

  splitter->SplitVertically(leftPanel, rightPanel, initialPos);
  splitter->SetSashPosition(initialPos, true);
  splitter->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [this](wxSplitterEvent &e) {
    m_diffDivPos = e.GetSashPosition();
    e.Skip();
  });

  root->Add(splitter, 1, wxEXPAND | wxALL, 6);

  // Checkbox full file
  v.chkShowFull = new wxCheckBox(panel, wxID_ANY, _("Show full file"));
  v.chkShowFull->SetValue(m_showFullFile);
  root->Add(v.chkShowFull, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

  v.chkShowFull->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &e) {
    m_showFullFile = e.IsChecked();

    for (auto &vv : m_views) {
      if (vv.chkShowFull && vv.chkShowFull->GetValue() != m_showFullFile) {
        vv.chkShowFull->SetValue(m_showFullFile);
      }
    }

    for (auto &vv : m_views) {
      if (!vv.isNewFile && vv.left && vv.right) {
        UpdateExistingTabView(vv);
      }
    }
  });

  panel->SetSizer(root);

  std::string sketchPath = m_cli->GetSketchPath();

  std::string pageTitle = StripFilename(sketchPath, wxToStd(v.fileKey));

  nb->AddPage(panel, wxString::FromUTF8(pageTitle), nb->GetPageCount() == 0);

  return panel;
}

wxPanel *ArduinoDiffDialog::CreateNewFileTab(wxNotebook *nb,
                                             FileViewData &v,
                                             const wxString &newText) {
  auto *panel = new wxPanel(nb);
  auto *root = new wxBoxSizer(wxVERTICAL);

  {
    wxString title = wxString::Format(_("New file: %s"), v.fileKey);
    root->Add(new wxStaticText(panel, wxID_ANY, title), 0, wxLEFT | wxRIGHT | wxTOP, 6);
  }

  v.right = new wxStyledTextCtrl(panel, wxID_ANY);
  SetupStyledTextCtrl(v.right, m_config);

  v.right->SetText(newText);
  v.right->EmptyUndoBuffer();

  SetupReadOnlyDiffCtrl(v.right);

  root->Add(v.right, 1, wxEXPAND | wxALL, 6);

  panel->SetSizer(root);

  std::string sketchPath = m_cli->GetSketchPath();

  std::string pageTitle = StripFilename(sketchPath, wxToStd(v.fileKey));

  nb->AddPage(panel, wxString::FromUTF8(pageTitle), nb->GetPageCount() == 0);

  return panel;
}

void ArduinoDiffDialog::SetupReadOnlyDiffCtrl(wxStyledTextCtrl *stc) {
  stc->SetReadOnly(true);
  stc->SetCaretLineVisible(false);
  stc->SetUseHorizontalScrollBar(true);
  stc->SetUseVerticalScrollBar(true);
}

// -------------------- scroll sync --------------------

void ArduinoDiffDialog::BindScrollSync(FileViewData &v) {
  if (!v.left || !v.right) {
    return;
  }

  FileViewData *pv = &v;
  wxStyledTextCtrl *left = v.left;
  wxStyledTextCtrl *right = v.right;

  auto syncFromTo = [pv](wxStyledTextCtrl *from, wxStyledTextCtrl *to) {
    if (!pv || !from || !to)
      return;
    if (pv->syncing)
      return;

    pv->syncing = true;

    const int first = from->GetFirstVisibleLine();
    const int xOff = from->GetXOffset();

    to->SetFirstVisibleLine(first);
    to->SetXOffset(xOff);

    pv->syncing = false;
  };

  auto scheduleSync = [syncFromTo](wxStyledTextCtrl *from, wxStyledTextCtrl *to) {
    if (!from || !to)
      return;
    from->CallAfter([syncFromTo, from, to]() { syncFromTo(from, to); });
  };

  left->Bind(wxEVT_STC_UPDATEUI,
             [syncFromTo, left, right](wxEvent &) {
               syncFromTo(left, right);
             });

  right->Bind(wxEVT_STC_UPDATEUI,
              [syncFromTo, left, right](wxEvent &) {
                syncFromTo(right, left);
              });

  left->Bind(wxEVT_SCROLLBAR,
             [scheduleSync, left, right](wxEvent &e) {
               e.Skip();
               scheduleSync(left, right);
             });

  right->Bind(wxEVT_SCROLLBAR,
              [scheduleSync, left, right](wxEvent &e) {
                e.Skip();
                scheduleSync(right, left);
              });

  left->Bind(wxEVT_MOUSEWHEEL,
             [scheduleSync, left, right](wxEvent &e) {
               e.Skip();
               scheduleSync(left, right);
             });

  right->Bind(wxEVT_MOUSEWHEEL,
              [scheduleSync, left, right](wxEvent &e) {
                e.Skip();
                scheduleSync(right, left);
              });
}

// -------------------- helpers: key normalization --------------------

wxString ArduinoDiffDialog::NormalizeKey(const wxString &path) {
  return wxString::FromUTF8(NormalizeFilename(m_cli->GetSketchPath(), wxToStd(path)));
}

wxString ArduinoDiffDialog::CanonicalizeEol(wxString s) {
  s.Replace(wxT("\r\n"), wxT("\n"));
  s.Replace(wxT("\r"), wxT("\n"));
  return s;
}

// -------------------- helpers: lines --------------------

std::vector<wxString> ArduinoDiffDialog::SplitLinesKeepLogical(const wxString &text) {
  // Split into "logical lines" without keeping '\n' in the elements.
  // Works for \n and \r\n.
  std::vector<wxString> out;
  out.reserve(256);

  wxString s = text;
  s.Replace(wxT("\r\n"), wxT("\n"));
  s.Replace(wxT("\r"), wxT("\n"));

  wxString current;
  for (wxUniChar c : s) {
    if (c == '\n') {
      out.push_back(current);
      current.clear();
    } else {
      current.Append(c);
    }
  }
  out.push_back(current);
  return out;
}

wxString ArduinoDiffDialog::JoinLines(const std::vector<wxString> &lines) {
  wxString out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size()) {
      out += wxT("\n");
    }
  }
  return out;
}

// -------------------- existing file aligned view --------------------

void ArduinoDiffDialog::UpdateExistingTabView(FileViewData &v) {
  wxString left, right;

  if (m_showFullFile) {
    BuildAlignedExistingFileView(v.oldText, v.newText, left, right);
  } else {
    BuildContextAlignedExistingFileView(v.oldText, v.newText, m_contextLines, left, right);
  }

  const auto kinds = ComputeLineKindsFromAlignedText(left, right);

  // preserve scroll position
  const int leftFirst = v.left ? v.left->GetFirstVisibleLine() : 0;
  const int rightFirst = v.right ? v.right->GetFirstVisibleLine() : 0;
  const int leftX = v.left ? v.left->GetXOffset() : 0;
  const int rightX = v.right ? v.right->GetXOffset() : 0;

  if (v.left) {
    v.left->SetReadOnly(false);
    v.left->SetText(left);
    ApplyLineIndicators(v.left, kinds);
    v.left->EmptyUndoBuffer();
    v.left->SetReadOnly(true);
    v.left->SetFirstVisibleLine(leftFirst);
    v.left->SetXOffset(leftX);
  }

  if (v.right) {
    v.right->SetReadOnly(false);
    v.right->SetText(right);
    ApplyLineIndicators(v.right, kinds);
    v.right->EmptyUndoBuffer();
    v.right->SetReadOnly(true);
    v.right->SetFirstVisibleLine(rightFirst);
    v.right->SetXOffset(rightX);
  }
}

void ArduinoDiffDialog::BuildContextAlignedExistingFileView(
    const wxString &oldText,
    const wxString &newText,
    int contextLines,
    wxString &outLeft,
    wxString &outRight) {

  wxString fullLeft, fullRight;
  BuildAlignedExistingFileView(oldText, newText, fullLeft, fullRight);

  auto leftLines = SplitLinesKeepLogical(fullLeft);
  auto rightLines = SplitLinesKeepLogical(fullRight);

  const int n = std::min<int>((int)leftLines.size(), (int)rightLines.size());
  if (n <= 0) {
    outLeft = fullLeft;
    outRight = fullRight;
    return;
  }

  std::vector<int> changed;
  changed.reserve(64);
  for (int i = 0; i < n; ++i) {
    if (leftLines[(size_t)i] != rightLines[(size_t)i]) {
      changed.push_back(i);
    }
  }

  if (changed.empty()) {
    outLeft = fullLeft;
    outRight = fullRight;
    return;
  }

  struct Block {
    int start = 0;
    int end = 0;
  };
  std::vector<Block> blocks;

  for (int idx : changed) {
    const int bs = std::max(0, idx - contextLines);
    const int be = std::min(n - 1, idx + contextLines);

    if (!blocks.empty() && bs <= blocks.back().end + 1) {
      blocks.back().end = std::max(blocks.back().end, be);
    } else {
      Block b;
      b.start = bs;
      b.end = be;
      blocks.push_back(b);
    }
  }

  std::vector<wxString> leftOut;
  std::vector<wxString> rightOut;

  for (size_t bi = 0; bi < blocks.size(); ++bi) {
    const auto &b = blocks[bi];

    wxString hdr = wxString::Format(_("-------- change block #%zu (aligned lines %d-%d) --------"),
                                    bi + 1, b.start + 1, b.end + 1);
    leftOut.push_back(hdr);
    rightOut.push_back(hdr);

    for (int ln = b.start; ln <= b.end; ++ln) {
      leftOut.push_back(leftLines[(size_t)ln]);
      rightOut.push_back(rightLines[(size_t)ln]);
    }

    if (bi + 1 < blocks.size()) {
      leftOut.push_back(wxEmptyString);
      rightOut.push_back(wxEmptyString);
    }
  }

  outLeft = JoinLines(leftOut);
  outRight = JoinLines(rightOut);
}

void ArduinoDiffDialog::BuildAlignedExistingFileView(
    const wxString &oldText,
    const wxString &newText,
    wxString &outOldAligned,
    wxString &outNewAligned) {
  auto oldLines = SplitLinesKeepLogical(oldText);
  auto newLines = SplitLinesKeepLogical(newText);

  auto aligned = m_lcsAligner.Align(oldLines, newLines);
  outOldAligned = JoinLines(aligned.left);
  outNewAligned = JoinLines(aligned.right);
}

// line highlight
void ArduinoDiffDialog::SetupDiffIndicators(wxStyledTextCtrl *stc) {
  // TODO: should be linked to EditorSettings
  // Modified
  stc->IndicatorSetStyle(20, wxSTC_INDIC_STRAIGHTBOX);
  stc->IndicatorSetForeground(20, wxColour(255, 230, 150));
  stc->IndicatorSetAlpha(20, 90);

  // Added
  stc->IndicatorSetStyle(21, wxSTC_INDIC_STRAIGHTBOX);
  stc->IndicatorSetForeground(21, wxColour(180, 255, 180));
  stc->IndicatorSetAlpha(21, 90);

  // Removed
  stc->IndicatorSetStyle(22, wxSTC_INDIC_STRAIGHTBOX);
  stc->IndicatorSetForeground(22, wxColour(255, 180, 180));
  stc->IndicatorSetAlpha(22, 90);

  // Header
  stc->IndicatorSetStyle(23, wxSTC_INDIC_STRAIGHTBOX);
  stc->IndicatorSetForeground(23, wxColour(220, 220, 220));
  stc->IndicatorSetAlpha(23, 60);
}

void ArduinoDiffDialog::ApplyLineIndicators(wxStyledTextCtrl *stc, const std::vector<DiffLineKind> &kinds) {
  // clear
  const int len = stc->GetTextLength();
  for (int id : {20, 21, 22, 23}) {
    stc->SetIndicatorCurrent(id);
    stc->IndicatorClearRange(0, len);
  }

  const int lineCount = stc->GetLineCount();
  const int n = std::min<int>((int)kinds.size(), lineCount);

  for (int i = 0; i < n; ++i) {
    int indic = -1;
    switch (kinds[i]) {
      case DiffLineKind::Same:
        continue;
      case DiffLineKind::Added:
        indic = 21;
        break;
      case DiffLineKind::Removed:
        indic = 22;
        break;
      case DiffLineKind::Modified:
        indic = 20;
        break;
      case DiffLineKind::Header:
        indic = 23;
        break;
    }

    const int start = stc->PositionFromLine(i);
    const int end = stc->GetLineEndPosition(i);
    const int len = std::max(0, end - start);
    if (len <= 0)
      continue;

    stc->SetIndicatorCurrent(indic);
    stc->IndicatorFillRange(start, len);
  }
}

static bool IsHeaderLine(const wxString &s) {
  return s.StartsWith(wxT("-------- "));
}

std::vector<DiffLineKind> ArduinoDiffDialog::ComputeLineKindsFromAlignedText(const wxString &leftAligned, const wxString &rightAligned) {
  std::vector<wxString> L = SplitLinesKeepLogical(leftAligned);
  std::vector<wxString> R = SplitLinesKeepLogical(rightAligned);

  const size_t n = std::max(L.size(), R.size());
  std::vector<DiffLineKind> out;
  out.reserve(n);

  for (size_t i = 0; i < n; ++i) {
    wxString a = (i < L.size()) ? L[i] : wxString();
    wxString b = (i < R.size()) ? R[i] : wxString();

    if (IsHeaderLine(a) || IsHeaderLine(b)) {
      out.push_back(DiffLineKind::Header);
      continue;
    }

    if (a == b)
      out.push_back(DiffLineKind::Same);
    else if (a.empty() && !b.empty())
      out.push_back(DiffLineKind::Added);
    else if (!a.empty() && b.empty())
      out.push_back(DiffLineKind::Removed);
    else
      out.push_back(DiffLineKind::Modified);
  }
  return out;
}
