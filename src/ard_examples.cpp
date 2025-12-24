#include "ard_examples.hpp"

#include "ard_libinfo.hpp"
#include "ard_setdlg.hpp"
#include "main.hpp"
#include "utils.hpp"
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/richmsgdlg.h>
#include <wx/wx.h>

enum {
  ID_EXAMPLE_INSTALL = wxID_HIGHEST + 5000,
  ID_LIB_FILTER_TIMER
};

ArduinoExamplesFrame::ArduinoExamplesFrame(wxWindow *parent, ArduinoCli *cli, wxConfigBase *config)
    : wxFrame(parent,
              wxID_ANY,
              _("Arduino Library Examples"),
              wxDefaultPosition,
              wxSize(900, 600),
              wxDEFAULT_FRAME_STYLE | wxRESIZE_BORDER),
      m_cli(cli), m_config(config), m_filterTimer(this, ID_LIB_FILTER_TIMER) {
  CreateLayout();
  BindEvents();
  LoadLayout();
  RefreshLibraries();
}

void ArduinoExamplesFrame::CreateLayout() {
  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  // Main horizontal splitter: libraries on the left, examples + preview on the right
  m_mainSplitter = new wxSplitterWindow(this, wxID_ANY,
                                        wxDefaultPosition, wxDefaultSize,
                                        wxSP_LIVE_UPDATE | wxSP_3D);

  // Left panel - list of libraries
  wxPanel *leftPanel = new wxPanel(m_mainSplitter);
  auto *leftSizer = new wxBoxSizer(wxVERTICAL);

  m_libFilterCtrl = new wxTextCtrl(leftPanel, wxID_ANY);
  m_libFilterCtrl->SetHint(_("Filter libraries..."));
  leftSizer->Add(m_libFilterCtrl, 0, wxEXPAND | wxALL, 4);

  m_libList = new wxListCtrl(
      leftPanel, wxID_ANY,
      wxDefaultPosition, wxDefaultSize,
      wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);

  m_libList->InsertColumn(0, _("Library"));
  m_libList->InsertColumn(1, _("Version"));
  m_libList->InsertColumn(2, _("Maintainer"));
  m_libList->InsertColumn(3, _("URL"));

  leftSizer->Add(m_libList, 1, wxEXPAND | wxALL, 4);
  leftPanel->SetSizer(leftSizer);

  // Right panel - vertical splitter: examples list on top, preview at the bottom
  m_rightSplitter = new wxSplitterWindow(m_mainSplitter, wxID_ANY,
                                         wxDefaultPosition, wxDefaultSize,
                                         wxSP_LIVE_UPDATE | wxSP_3D);

  wxPanel *topRightPanel = new wxPanel(m_rightSplitter);
  auto *topRightSizer = new wxBoxSizer(wxVERTICAL);

  m_exampleList = new wxListCtrl(
      topRightPanel, wxID_ANY,
      wxDefaultPosition, wxDefaultSize,
      wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);

  m_exampleList->InsertColumn(0, _("Example"));
  m_exampleList->InsertColumn(1, _("Path"));

  topRightSizer->Add(m_exampleList, 1, wxEXPAND | wxALL, 4);
  topRightPanel->SetSizer(topRightSizer);

  wxPanel *bottomRightPanel = new wxPanel(m_rightSplitter);
  auto *bottomRightSizer = new wxBoxSizer(wxVERTICAL);

  m_preview = new wxStyledTextCtrl(bottomRightPanel, wxID_ANY);
  m_preview->SetReadOnly(true);

  SetupStyledTextCtrl(m_preview, m_config);

  bottomRightSizer->Add(m_preview, 1, wxEXPAND | wxALL, 4);
  bottomRightPanel->SetSizer(bottomRightSizer);

  // Split on the right (examples / preview)
  m_rightSplitter->SplitHorizontally(topRightPanel, bottomRightPanel, 200);

  // Split left/right
  m_mainSplitter->SplitVertically(leftPanel, m_rightSplitter, 280);

  topSizer->Add(m_mainSplitter, 1, wxEXPAND);
  SetSizer(topSizer);
}

void ArduinoExamplesFrame::BindEvents() {
  m_libList->Bind(wxEVT_LIST_ITEM_SELECTED, &ArduinoExamplesFrame::OnLibraryItemSelected, this);

  m_libList->Bind(wxEVT_LIST_ITEM_ACTIVATED, &ArduinoExamplesFrame::OnLibraryItemActivated, this);

  m_exampleList->Bind(wxEVT_LIST_ITEM_SELECTED, &ArduinoExamplesFrame::OnExampleItemSelected, this);

  m_exampleList->Bind(wxEVT_LIST_ITEM_ACTIVATED, &ArduinoExamplesFrame::OnExampleItemActivated, this);

  m_exampleList->Bind(wxEVT_CONTEXT_MENU, &ArduinoExamplesFrame::OnExampleContextMenu, this);

  Bind(wxEVT_MENU, &ArduinoExamplesFrame::OnInstallExample, this, ID_EXAMPLE_INSTALL);

  Bind(wxEVT_CLOSE_WINDOW, &ArduinoExamplesFrame::OnClose, this);

  if (m_libFilterCtrl) {
    m_libFilterCtrl->Bind(wxEVT_TEXT, &ArduinoExamplesFrame::OnFilterText, this);
  }
  Bind(wxEVT_TIMER, &ArduinoExamplesFrame::OnFilterTimer, this, ID_LIB_FILTER_TIMER);
}

void ArduinoExamplesFrame::RefreshLibraries() {
  PopulateLibraries();
}

void ArduinoExamplesFrame::PopulateLibraries() {
  m_libList->Freeze();
  m_libList->DeleteAllItems();

  m_libRows.clear();
  m_allLibRows.clear();

  if (!m_cli) {
    m_libList->Thaw();
    return;
  }

  const auto &libs = m_cli->GetInstalledLibraries();

  for (const auto &lib : libs) {
    if (lib.latest.examples.empty())
      continue;

    m_allLibRows.push_back(&lib);
  }

  m_libList->Thaw();

  // apply the current filter
  wxString filter;
  if (m_libFilterCtrl) {
    filter = m_libFilterCtrl->GetValue();
  }
  ApplyLibraryFilter(filter);

  // simultaneously clear the right part
  m_exampleList->DeleteAllItems();
  m_exampleRows.clear();
  m_preview->SetReadOnly(false);
  m_preview->SetText(wxEmptyString);
  m_preview->SetReadOnly(true);
}

void ArduinoExamplesFrame::ApplyLibraryFilter(const wxString &filter) {
  wxString f = filter.Lower();

  m_libList->Freeze();
  m_libList->DeleteAllItems();
  m_libRows.clear();

  long row = 0;

  for (const auto *lib : m_allLibRows) {
    if (!lib)
      continue;

    wxString name = wxString::FromUTF8(lib->name.c_str());
    wxString ver = wxString::FromUTF8(lib->latest.version.c_str());
    wxString maint = wxString::FromUTF8(lib->latest.maintainer.c_str());
    wxString url = wxString::FromUTF8(lib->latest.website.c_str());

    if (!f.empty()) {
      wxString joined = name + wxT(" ") + ver + wxT(" ") + maint + wxT(" ") + url;
      if (!joined.Lower().Contains(f)) {
        continue; // does not match the filter
      }
    }

    long idx = m_libList->InsertItem(row, name);
    m_libList->SetItem(idx, 1, ver);
    m_libList->SetItem(idx, 2, maint);
    m_libList->SetItem(idx, 3, url);

    m_libRows.push_back(lib);
    ++row;
  }

  m_libList->SetColumnWidth(0, wxLIST_AUTOSIZE_USEHEADER);
  m_libList->SetColumnWidth(1, wxLIST_AUTOSIZE_USEHEADER);
  m_libList->SetColumnWidth(2, wxLIST_AUTOSIZE_USEHEADER);
  m_libList->SetColumnWidth(3, wxLIST_AUTOSIZE_USEHEADER);

  m_libList->Thaw();

  // after changing the filter, we clear the selection and the right part
  m_exampleList->DeleteAllItems();
  m_exampleRows.clear();
  m_preview->SetReadOnly(false);
  m_preview->SetText(wxEmptyString);
  m_preview->SetReadOnly(true);
}

void ArduinoExamplesFrame::PopulateExamplesForLibrary(const ArduinoLibraryInfo *lib) {
  m_exampleList->Freeze();
  m_exampleList->DeleteAllItems();
  m_exampleRows.clear();

  if (!lib) {
    m_exampleList->Thaw();
    return;
  }

  long row = 0;
  for (const auto &p : lib->latest.examples) {
    ExampleRow exRow;
    exRow.lib = lib;
    exRow.examplePath = p;

    wxString pathWx = wxString::FromUTF8(p.c_str());

    // Example name = last part of the path
    // /.../Bounce2/examples/bounce_basic  ->  "bounce_basic"
    wxString displayName = pathWx.AfterLast(wxFileName::GetPathSeparator());
    if (displayName.empty()) {
      displayName = pathWx; // fallback
    }

    long idx = m_exampleList->InsertItem(row, displayName);
    m_exampleList->SetItem(idx, 1, pathWx); // second column = full path

    m_exampleRows.push_back(exRow);
    ++row;
  }

  m_exampleList->SetColumnWidth(0, wxLIST_AUTOSIZE_USEHEADER);
  m_exampleList->SetColumnWidth(1, wxLIST_AUTOSIZE_USEHEADER);

  m_exampleList->Thaw();

  m_preview->SetReadOnly(false);
  m_preview->SetText(wxEmptyString);
  m_preview->SetReadOnly(true);
}

void ArduinoExamplesFrame::LoadExamplePreview(const std::string &examplePath) {
  wxString dirWx = wxString::FromUTF8(examplePath.c_str());

  // we expect that examplePath is the directory where the .ino is located
  if (!wxDirExists(dirWx)) {
    // fallback: if it is a direct file, try to load it
    wxFileName fn(dirWx);
    if (!fn.FileExists()) {
      m_preview->SetReadOnly(false);
      m_preview->SetText(_("// Example not found:\n// ") + dirWx);
      m_preview->SetReadOnly(true);
      return;
    }

    wxFFile f(dirWx, wxT("r"));
    if (!f.IsOpened()) {
      m_preview->SetReadOnly(false);
      m_preview->SetText(_("// Cannot open file:\n// ") + dirWx);
      m_preview->SetReadOnly(true);
      return;
    }

    wxString content;
    f.ReadAll(&content, wxConvUTF8);
    f.Close();

    m_preview->SetReadOnly(false);
    m_preview->SetText(content);
    m_preview->EmptyUndoBuffer();
    m_preview->SetReadOnly(true);
    return;
  }

  // Find the first *.ino in the directory
  wxDir dir(dirWx);
  if (!dir.IsOpened()) {
    m_preview->SetReadOnly(false);
    m_preview->SetText(_("// Cannot open directory:\n// ") + dirWx);
    m_preview->SetReadOnly(true);
    return;
  }

  wxString fname;
  bool cont = dir.GetFirst(&fname, wxT("*.ino"), wxDIR_FILES);
  if (!cont) {
    // fallback - no .ino, let's try .cpp instead
    cont = dir.GetFirst(&fname, wxT("*.cpp"), wxDIR_FILES);
  }

  if (!cont) {
    m_preview->SetReadOnly(false);
    m_preview->SetText(_("// No .ino (or .cpp) file found in:\n// ") + dirWx);
    m_preview->SetReadOnly(true);
    return;
  }

  wxFileName inoFn(dirWx, fname);
  wxString inoPath = inoFn.GetFullPath();

  wxFFile f(inoPath, wxT("r"));
  if (!f.IsOpened()) {
    m_preview->SetReadOnly(false);
    m_preview->SetText(_("// Cannot open file:\n// ") + inoPath);
    m_preview->SetReadOnly(true);
    return;
  }

  wxString content;
  f.ReadAll(&content, wxConvUTF8);
  f.Close();

  m_preview->SetReadOnly(false);
  m_preview->SetText(content);
  m_preview->EmptyUndoBuffer();
  m_preview->GotoLine(0);
  m_preview->SetReadOnly(true);
}

void ArduinoExamplesFrame::OnLibraryItemSelected(wxListEvent &evt) {
  long idx = evt.GetIndex();
  if (idx < 0 || (size_t)idx >= m_libRows.size())
    return;

  const ArduinoLibraryInfo *lib = m_libRows[(size_t)idx];
  PopulateExamplesForLibrary(lib);
}

void ArduinoExamplesFrame::OnExampleItemSelected(wxListEvent &evt) {
  long idx = evt.GetIndex();
  if (idx < 0 || (size_t)idx >= m_exampleRows.size())
    return;

  const auto &row = m_exampleRows[(size_t)idx];
  LoadExamplePreview(row.examplePath);
}

void ArduinoExamplesFrame::OnExampleItemActivated(wxListEvent &WXUNUSED(evt)) {
  InstallSelectedExample();
}

void ArduinoExamplesFrame::LoadLayout() {
  if (!m_config)
    return;

  LoadWindowSize(wxT("Examples"), this, m_config);

  long sashMain, sashRight;
  if (m_config->Read(wxT("Examples/MainSash"), &sashMain)) {
    m_mainSplitter->SetSashPosition((int)sashMain);
  }
  if (m_config->Read(wxT("Examples/RightSash"), &sashRight)) {
    m_rightSplitter->SetSashPosition((int)sashRight);
  }
}

void ArduinoExamplesFrame::OnFilterText(wxCommandEvent &WXUNUSED(evt)) {
  m_filterTimer.Start(500, wxTIMER_ONE_SHOT);
}

void ArduinoExamplesFrame::OnFilterTimer(wxTimerEvent &WXUNUSED(evt)) {
  wxString filter;
  if (m_libFilterCtrl) {
    filter = m_libFilterCtrl->GetValue();
  }
  ApplyLibraryFilter(filter);
}

void ArduinoExamplesFrame::SaveLayout() {
  if (!m_config)
    return;

  if (m_mainSplitter) {
    m_config->Write(wxT("Examples/MainSash"), (long)m_mainSplitter->GetSashPosition());
  }
  if (m_rightSplitter) {
    m_config->Write(wxT("Examples/RightSash"), (long)m_rightSplitter->GetSashPosition());
  }

  SaveWindowSize(wxT("Examples"), this, m_config);
}

void ArduinoExamplesFrame::OnLibraryItemActivated(wxListEvent &evt) {
  long idx = evt.GetIndex();
  if (idx < 0 || (size_t)idx >= m_libRows.size())
    return;

  const ArduinoLibraryInfo *lib = m_libRows[(size_t)idx];
  if (!lib || !m_cli)
    return;

  ArduinoLibraryDetailDialog::ShowInstalledLibraryInfo(this, lib, m_config, m_cli);
}

void ArduinoExamplesFrame::OnExampleContextMenu(wxContextMenuEvent &WXUNUSED(evt)) {
  if (!m_exampleList || m_exampleRows.empty())
    return;

  long sel = m_exampleList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (sel == -1)
    return; // nothing selected, nothing offered

  wxMenu menu;
  menu.Append(ID_EXAMPLE_INSTALL, _("Install..."));
  m_exampleList->PopupMenu(&menu);
}

void ArduinoExamplesFrame::InstallSelectedExample() {
  if (!m_config)
    return;

  long idx = m_exampleList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (idx < 0 || (size_t)idx >= m_exampleRows.size())
    return;

  const auto &row = m_exampleRows[(size_t)idx];

  wxString srcDir = wxString::FromUTF8(row.examplePath.c_str());
  if (!wxDirExists(srcDir)) {
    ModalMsgDialog(_("Example directory does not exist:\n") + srcDir, _("Install example"));
    return;
  }

  // Derive the default sketch name from the last directory
  wxFileName fn;
  fn.AssignDir(srcDir);
  wxString defaultName;
  const wxArrayString &dirs = fn.GetDirs();
  if (!dirs.IsEmpty()) {
    defaultName = dirs.Last();
  } else {
    defaultName = _("example");
  }

  wxTextEntryDialog dlg(this,
                        _("Enter sketch name:"),
                        _("Install example"),
                        defaultName);

  if (dlg.ShowModal() != wxID_OK)
    return;

  wxString sketchName = dlg.GetValue();
  sketchName.Trim(true).Trim(false);

  if (sketchName.empty()) {
    ModalMsgDialog(_("Sketch name cannot be empty."));
    return;
  }

  // must not contain separators
  if (sketchName.Find('/') != wxNOT_FOUND || sketchName.Find('\\') != wxNOT_FOUND) {
    ModalMsgDialog(_("Sketch name must not contain path separators."));
    return;
  }

  wxString sketchesDir;
  if (!m_config->Read(wxT("SketchesDir"), &sketchesDir) || sketchesDir.empty()) {
    ModalMsgDialog(_("Sketches directory is not known."));
    return;
  }

  wxFileName destFn(sketchesDir, sketchName);
  wxString destPath = destFn.GetFullPath();

  if (wxDirExists(destPath) || wxFileExists(destPath)) {
    ModalMsgDialog(_("Target sketch directory already exists:\n") + destPath);
    return;
  }

  if (!CopyDirRecursive(srcDir, destPath)) {
    ModalMsgDialog(_("Failed to copy example to:\n") + destPath);
    return;
  }

  // Done - let's open a new sketch
  ArduinoEditApp &app = wxGetApp();
  app.OpenSketch(destPath);
}

void ArduinoExamplesFrame::OnInstallExample(wxCommandEvent &WXUNUSED(evt)) {
  InstallSelectedExample();
}

int ArduinoExamplesFrame::ModalMsgDialog(const wxString &message, const wxString &caption, int styles) {
  wxRichMessageDialog dlg(this, message, caption, styles);
  return dlg.ShowModal();
}

void ArduinoExamplesFrame::OnClose(wxCloseEvent &evt) {
  SaveLayout();
  evt.Skip(); // let the frame be destroyed normally (and let the parent nullify the pointer)
}
