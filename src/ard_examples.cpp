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

#include "ard_examples.hpp"

#include "ard_ap.hpp"
#include "ard_libinfo.hpp"
#include "ard_setdlg.hpp"
#include "main.hpp"
#include "utils.hpp"
#include <algorithm>
#include <filesystem>
#include <set>
#include <system_error>
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

namespace {
namespace fs = std::filesystem;

wxColour BlendColor(const wxColour &a, const wxColour &b, double t) {
  t = std::clamp(t, 0.0, 1.0);
  auto lerp = [t](unsigned char x, unsigned char y) -> unsigned char {
    return (unsigned char)std::lround((1.0 - t) * (double)x + t * (double)y);
  };
  return wxColour(lerp(a.Red(), b.Red()),
                  lerp(a.Green(), b.Green()),
                  lerp(a.Blue(), b.Blue()));
}

std::string BuildPlatformLabel(const std::string &platformPath,
                               const wxString &roleLabel) {
  if (platformPath.empty()) {
    return wxToStd(roleLabel);
  }

  fs::path p(platformPath);
  std::string version = p.filename().string();
  std::string arch = p.parent_path().filename().string();
  std::string vendor;

  fs::path parent = p.parent_path().parent_path();
  if (!parent.empty()) {
    vendor = parent.filename().string();
  }

  std::string label = wxToStd(roleLabel);
  if (!vendor.empty() && !arch.empty()) {
    label += " (" + vendor + ":" + arch + ")";
  } else if (!arch.empty()) {
    label += " (" + arch + ")";
  }

  if (!version.empty() && version != arch) {
    label += " " + version;
  }

  return label;
}
} // namespace

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
  std::vector<SourceRow> libraryRows;
  libraryRows.reserve(libs.size());

  auto addPlatformRow = [this](const std::string &platformPath,
                               const wxString &roleLabel) {
    if (platformPath.empty()) {
      return;
    }

    SourceRow row;
    row.isPlatform = true;
    row.name = "[Platform] " + BuildPlatformLabel(platformPath, roleLabel);
    row.version = fs::path(platformPath).filename().string();
    row.maintainer = wxToStd(roleLabel);
    row.location = platformPath;
    row.examples = CollectPlatformExamples(platformPath);

    if (!row.examples.empty()) {
      m_allLibRows.push_back(std::move(row));
    }
  };

  addPlatformRow(m_cli->GetCorePlatformPath(), _("Core platform"));

  const std::string platformPath = m_cli->GetPlatformPath();
  if (platformPath != m_cli->GetCorePlatformPath()) {
    addPlatformRow(platformPath, _("Board platform"));
  }

  for (const auto &lib : libs) {
    if (lib.latest.examples.empty())
      continue;

    SourceRow row;
    row.lib = &lib;
    row.name = lib.name;
    row.version = lib.latest.version;
    row.maintainer = lib.latest.maintainer;
    row.location = lib.latest.website;
    row.examples = lib.latest.examples;
    libraryRows.push_back(std::move(row));
  }

  std::sort(libraryRows.begin(), libraryRows.end(),
            [](const SourceRow &a, const SourceRow &b) {
              const std::string an = ToLower(a.name);
              const std::string bn = ToLower(b.name);
              if (an != bn) {
                return an < bn;
              }
              return a.name < b.name;
            });

  for (auto &row : libraryRows) {
    m_allLibRows.push_back(std::move(row));
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

  for (const auto &source : m_allLibRows) {
    wxString name = wxString::FromUTF8(source.name.c_str());
    wxString ver = wxString::FromUTF8(source.version.c_str());
    wxString maint = wxString::FromUTF8(source.maintainer.c_str());
    wxString url = wxString::FromUTF8(source.location.c_str());

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

    if (source.isPlatform) {
      const wxColour listBg = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);
      const wxColour highlight = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
      const wxColour listFg = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT);
      const double tint = IsDarkMode() ? 0.32 : 0.20;
      const double fgTint = IsDarkMode() ? 0.10 : 0.18;
      m_libList->SetItemBackgroundColour(idx, BlendColor(listBg, highlight, tint));
      m_libList->SetItemTextColour(idx, BlendColor(listFg, highlight, fgTint));
    }

    m_libRows.push_back(source);
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

void ArduinoExamplesFrame::PopulateExamplesForSource(const SourceRow *source) {
  m_exampleList->Freeze();
  m_exampleList->DeleteAllItems();
  m_exampleRows.clear();

  if (!source) {
    m_exampleList->Thaw();
    return;
  }

  long row = 0;
  for (const auto &p : source->examples) {
    ExampleRow exRow;
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

std::vector<std::string> ArduinoExamplesFrame::CollectPlatformExamples(const std::string &platformPath) const {
  std::vector<std::string> out;
  if (platformPath.empty()) {
    return out;
  }

  std::set<std::string> unique;
  std::error_code ec;

  auto collectFromRoot = [&](const fs::path &root) {
    if (root.empty() || !fs::exists(root, ec) || !fs::is_directory(root, ec)) {
      return;
    }

    fs::recursive_directory_iterator it(root, ec), end;
    while (!ec && it != end) {
      if (!it->is_directory(ec)) {
        it.increment(ec);
        continue;
      }

      const fs::path dirPath = it->path();
      bool hasSketch = false;

      for (const auto &entry : fs::directory_iterator(dirPath, ec)) {
        if (ec) {
          break;
        }

        if (!entry.is_regular_file(ec)) {
          continue;
        }

        const std::string ext = entry.path().extension().string();
        if (ext == ".ino" || ext == ".pde" || ext == ".cpp") {
          hasSketch = true;
          break;
        }
      }

      if (hasSketch) {
        unique.insert(dirPath.string());
        it.disable_recursion_pending();
      }

      it.increment(ec);
    }
  };

  collectFromRoot(fs::path(platformPath) / "examples");

  fs::path librariesRoot = fs::path(platformPath) / "libraries";
  if (fs::exists(librariesRoot, ec) && fs::is_directory(librariesRoot, ec)) {
    for (const auto &dirEntry : fs::directory_iterator(librariesRoot, ec)) {
      if (ec) {
        break;
      }
      if (!dirEntry.is_directory(ec)) {
        continue;
      }
      collectFromRoot(dirEntry.path() / "examples");
    }
  }

  out.assign(unique.begin(), unique.end());
  return out;
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

  const SourceRow *source = &m_libRows[(size_t)idx];
  PopulateExamplesForSource(source);
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

  const SourceRow &source = m_libRows[(size_t)idx];
  if (!source.lib || !m_cli)
    return;

  ArduinoLibraryDetailDialog::ShowInstalledLibraryInfo(this, source.lib, m_config, m_cli);
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
