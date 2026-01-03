#include "ard_diagview.hpp"

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/menu.h>
#include <wx/window.h>

#include "ard_edit.hpp"
#include "utils.hpp"

wxDEFINE_EVENT(EVT_ARD_DIAG_JUMP, ArduinoDiagnosticsActionEvent);
wxDEFINE_EVENT(EVT_ARD_DIAG_SOLVE_AI, ArduinoDiagnosticsActionEvent);

ArduinoDiagnosticsView::ArduinoDiagnosticsView(wxWindow *parent, wxConfigBase *config)
    : wxPanel(parent, wxID_ANY), m_config(config), m_aiEnabled(false) {
  auto *sizer = new wxBoxSizer(wxVERTICAL);

  m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                          wxLC_REPORT | wxLC_SINGLE_SEL);

  m_list->InsertColumn(0, wxEmptyString);
  m_list->InsertColumn(1, _("File"));
  m_list->InsertColumn(2, _("Line"));
  m_list->InsertColumn(3, _("Column"));
  m_list->InsertColumn(4, _("Message"));

  EditorSettings settings;
  settings.Load(m_config);
  EditorColorScheme colors = settings.GetColors();
  UpdateColors(colors);

  AiSettings aiSettings;
  aiSettings.Load(m_config);
  ApplySettings(aiSettings);

  sizer->Add(m_list, 1, wxEXPAND);
  SetSizer(sizer);

  m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &ArduinoDiagnosticsView::OnItemActivated, this);
  m_list->Bind(wxEVT_CONTEXT_MENU, &ArduinoDiagnosticsView::OnContextMenu, this);

  Bind(wxEVT_SYS_COLOUR_CHANGED, &ArduinoDiagnosticsView::OnSysColourChanged, this);
}

void ArduinoDiagnosticsView::ApplySettings(const EditorSettings &settings) {
  EditorColorScheme colors = settings.GetColors();
  UpdateColors(colors);
  SetDiagnostics(m_current);
}

void ArduinoDiagnosticsView::UpdateColors(const EditorColorScheme &colors) {
  int fontSize = 15;

  auto *newList = new wxImageList(fontSize, fontSize, false);

  wxBitmap bmpErr = ArduinoEditor::MakeCircleBitmap(fontSize, colors.error, wxColour(0, 0, 0), 0);
  wxBitmap bmpWarn = ArduinoEditor::MakeCircleBitmap(fontSize, colors.warning, wxColour(0, 0, 0), 0);
  wxBitmap bmpNote = ArduinoEditor::MakeCircleBitmap(fontSize, colors.note, wxColour(0, 0, 0), 0);

  int imgErr = newList->Add(bmpErr);
  int imgWarn = newList->Add(bmpWarn);
  int imgNote = newList->Add(bmpNote);

  m_list->AssignImageList(newList, wxIMAGE_LIST_SMALL);

  m_imgList = newList;
  m_imgError = imgErr;
  m_imgWarning = imgWarn;
  m_imgNote = imgNote;

  m_list->SetColumnWidth(0, fontSize + 8);
}

void ArduinoDiagnosticsView::ApplySettings(const AiSettings &settings) {
  m_aiEnabled = settings.enabled;
}

void ArduinoDiagnosticsView::ShowMessage(const wxString &message) {
  m_current.clear();

  m_list->Freeze();
  m_list->DeleteAllItems();

  long idx = m_list->InsertItem(0, wxEmptyString, -1);

  m_list->SetItem(idx, 1, wxEmptyString);
  m_list->SetItem(idx, 2, wxEmptyString);
  m_list->SetItem(idx, 3, wxEmptyString);
  m_list->SetItem(idx, 4, message);

  m_list->SetItemData(idx, -1);

  m_list->SetColumnWidth(0, 24);
  m_list->SetColumnWidth(1, wxLIST_AUTOSIZE_USEHEADER);
  m_list->SetColumnWidth(2, wxLIST_AUTOSIZE_USEHEADER);
  m_list->SetColumnWidth(3, wxLIST_AUTOSIZE_USEHEADER);
  m_list->SetColumnWidth(4, wxLIST_AUTOSIZE);

  m_list->Thaw();
}

bool ArduinoDiagnosticsView::GetDiagnosticsAt(const std::string &filename, unsigned row, unsigned col,
                                              ArduinoParseError &outDiagnostic) {
  if (filename.empty() || m_current.empty())
    return false;

  auto normalizePath = [&](const std::string &in) -> std::string {
    return NormalizeFilename(m_sketchRoot, in);
  };

  auto endsWithPath = [](const std::string &whole, const std::string &tail) -> bool {
    if (tail.empty())
      return false;
    if (whole.size() < tail.size())
      return false;
    if (whole.compare(whole.size() - tail.size(), tail.size(), tail) != 0)
      return false;

    // Ensure we cut on a path boundary if there are extra chars before the match.
    if (whole.size() == tail.size())
      return true;

    const char prev = whole[whole.size() - tail.size() - 1];
    return prev == '/';
  };

  const std::string want = normalizePath(filename);

  auto fileMatches = [&](const std::string &diagFile) -> bool {
    if (diagFile.empty())
      return false;

    const std::string have = normalizePath(diagFile);
    if (have == want)
      return true;

    // Tolerant match: allow "absolute vs relative" differences.
    // (e.g. have ends with want, or want ends with have)
    return endsWithPath(have, want) || endsWithPath(want, have);
  };

  auto matches = [&](const ArduinoParseError &e) -> bool {
    if (!fileMatches(e.file))
      return false;
    if (row != 0 && e.line != row)
      return false;
    if (col != 0 && e.column != col)
      return false;
    return true;
  };

  auto findFirst = [&](auto &&self, const ArduinoParseError &e) -> const ArduinoParseError * {
    if (matches(e))
      return &e;
    for (const auto &ch : e.childs) {
      if (const ArduinoParseError *hit = self(self, ch))
        return hit;
    }
    return nullptr;
  };

  for (const auto &e : m_current) {
    if (const ArduinoParseError *hit = findFirst(findFirst, e)) {
      outDiagnostic = *hit;
      return true;
    }
  }

  return false;
}

void ArduinoDiagnosticsView::SetDiagnostics(const std::vector<ArduinoParseError> &diags) {
  m_current = diags;

  m_list->Freeze();
  m_list->DeleteAllItems();

  long row = 0;
  for (size_t i = 0; i < m_current.size(); ++i) {
    const auto &e = m_current[i];

    wxString file = wxString::FromUTF8(
        m_sketchRoot.empty()
            ? e.file
            : DiagnosticsFilename(m_sketchRoot, e.file));

    wxString lineStr = wxString::Format(wxT("%u"), (unsigned)e.line);
    wxString colStr = wxString::Format(wxT("%u"), (unsigned)e.column);
    wxString msg = wxString::FromUTF8(e.message.c_str());

    int image = -1;
    switch (e.severity) {
      case CXDiagnostic_Error:
      case CXDiagnostic_Fatal:
        image = m_imgError;
        break;
      case CXDiagnostic_Warning:
        image = m_imgWarning;
        break;
      case CXDiagnostic_Note:
        image = m_imgNote;
        break;
      default:
        image = -1;
        break;
    }

    long idx = m_list->InsertItem(row, wxEmptyString, image);

    m_list->SetItem(idx, 1, file);
    m_list->SetItem(idx, 2, lineStr);
    m_list->SetItem(idx, 3, colStr);
    m_list->SetItem(idx, 4, msg);

    m_list->SetItemData(idx, (long)i);

    ++row;
  }

  m_list->SetColumnWidth(0, 24);
  m_list->SetColumnWidth(1, wxLIST_AUTOSIZE);
  m_list->SetColumnWidth(2, wxLIST_AUTOSIZE_USEHEADER);
  m_list->SetColumnWidth(3, wxLIST_AUTOSIZE_USEHEADER);
  m_list->SetColumnWidth(4, wxLIST_AUTOSIZE);

  m_list->Thaw();
}

long ArduinoDiagnosticsView::GetSelectedRow() const {
  if (!m_list)
    return wxNOT_FOUND;
  return m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
}

void ArduinoDiagnosticsView::OnSysColourChanged(wxSysColourChangedEvent &event) {
  EditorSettings settings;
  settings.Load(m_config);

  ApplySettings(settings);

  event.Skip();
}

wxString ArduinoDiagnosticsView::GetRowText(long row) const {
  if (!m_list || row < 0 || row >= m_list->GetItemCount()) {
    return wxEmptyString;
  }

  auto getCol = [this, row](int col) {
    wxListItem info;
    info.SetId(row);
    info.SetColumn(col);
    info.SetMask(wxLIST_MASK_TEXT);
    if (m_list->GetItem(info)) {
      return info.GetText();
    }
    return wxString();
  };

  wxString file = getCol(1);
  wxString line = getCol(2);
  wxString column = getCol(3);
  wxString msg = getCol(4);

  if (file.IsEmpty() && line.IsEmpty() && column.IsEmpty()) {
    return msg;
  }

  wxString text = file;
  if (!line.IsEmpty()) {
    text << wxT(":") << line;
    if (!column.IsEmpty()) {
      text << wxT(":") << column;
    }
  }

  if (!msg.IsEmpty()) {
    if (!text.IsEmpty()) {
      text << wxT(": ");
    }
    text << msg;
  }

  return text;
}

void ArduinoDiagnosticsView::CopySelected() {
  long sel = GetSelectedRow();
  if (sel == wxNOT_FOUND)
    return;

  wxString text = GetRowText(sel);
  if (text.IsEmpty())
    return;

  if (wxTheClipboard->Open()) {
    wxTheClipboard->SetData(new wxTextDataObject(text));
    wxTheClipboard->Close();
  }
}

void ArduinoDiagnosticsView::CopyAll() {
  if (!m_list)
    return;

  int count = m_list->GetItemCount();
  if (count <= 0)
    return;

  wxString all;
  for (int i = 0; i < count; ++i) {
    wxString rowText = GetRowText(i);
    if (!rowText.IsEmpty()) {
      all << rowText << wxT("\n");
    }
  }

  all.Trim(true).Trim(false);
  if (all.IsEmpty())
    return;

  if (wxTheClipboard->Open()) {
    wxTheClipboard->SetData(new wxTextDataObject(all));
    wxTheClipboard->Close();
  }
}

void ArduinoDiagnosticsView::RequestSolveAi() {
  if (!m_aiEnabled || !m_list)
    return;

  long row = GetSelectedRow();
  if (row == wxNOT_FOUND)
    return;

  long data = m_list->GetItemData(row);
  if (data < 0)
    return;

  size_t idx = (size_t)data;
  if (idx >= m_current.size())
    return;

  const auto &e = m_current[idx];

  JumpTarget tgt;
  tgt.file = e.file;
  tgt.line = (int)e.line;
  tgt.column = (int)e.column;

  ArduinoDiagnosticsActionEvent ev(EVT_ARD_DIAG_SOLVE_AI, GetId());
  ev.SetEventObject(this);
  ev.SetJumpTarget(tgt);
  ev.SetDiagnostic(e);

  wxPostEvent(GetParent(), ev);
}

void ArduinoDiagnosticsView::OnItemActivated(wxListEvent &event) {
  long row = event.GetIndex();
  if (row < 0)
    return;

  long data = m_list->GetItemData(row);
  if (data < 0)
    return;

  size_t idx = (size_t)data;
  if (idx >= m_current.size())
    return;

  const auto &e = m_current[idx];

  JumpTarget tgt;
  tgt.file = e.file;
  tgt.line = (int)e.line;
  tgt.column = (int)e.column;

  ArduinoDiagnosticsActionEvent ev(EVT_ARD_DIAG_JUMP, GetId());
  ev.SetEventObject(this);
  ev.SetJumpTarget(tgt);
  ev.SetDiagnostic(e);

  wxWindow *top = wxGetTopLevelParent(this);
  if (top)
    wxPostEvent(top, ev);
  else
    wxPostEvent(GetParent(), ev);
}

void ArduinoDiagnosticsView::OnContextMenu(wxContextMenuEvent &evt) {
  if (!m_list)
    return;

  long selRow = GetSelectedRow();
  if (selRow == wxNOT_FOUND) {
    return;
  }
  long data = m_list->GetItemData(selRow);
  if (data < 0) {
    return;
  }

  wxPoint screenPt = evt.GetPosition();
  if (screenPt == wxDefaultPosition) {
    screenPt = m_list->GetScreenPosition();
    wxSize size = m_list->GetSize();
    screenPt.x += size.x / 2;
    screenPt.y += size.y / 2;
  }

  wxPoint clientPt = m_list->ScreenToClient(screenPt);

  wxMenu menu;

  int idCopy = wxWindow::NewControlId();
  int idCopyAll = wxWindow::NewControlId();
  int idSolveAi = wxWindow::NewControlId();

  menu.Append(idCopy, _("Copy problem"));
  menu.Append(idCopyAll, _("Copy all problems"));

  if (m_aiEnabled) {
    menu.AppendSeparator();
    menu.Append(idSolveAi, _("Solve error with AI"));
  }

  menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { CopySelected(); }, idCopy);
  menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { CopyAll(); }, idCopyAll);
  if (m_aiEnabled) {
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { RequestSolveAi(); }, idSolveAi);
  }

  m_list->PopupMenu(&menu, clientPt);
}
