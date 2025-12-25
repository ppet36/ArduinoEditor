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

#include "ard_libman.hpp"

#include "ard_cli.hpp"
#include "ard_ed_frm.hpp"
#include "ard_libinfo.hpp"
#include "ard_setdlg.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/richmsgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

enum {
  ID_LIB_TOPIC_CHOICE = wxID_HIGHEST + 2000,
  ID_LIB_TYPE_CHOICE,
  ID_LIB_SEARCH_CTRL,
  ID_LIB_LIST_CTRL,
  ID_LIB_POLL_TIMER,
  ID_LIB_SEARCH_TIMER,
  ID_LIB_MENU_INSTALL_LATEST,
  ID_LIB_MENU_INSTALL_VERSION_BASE,
  ID_LIB_MENU_INSTALL_SOURCE_GIT,
  ID_LIB_MENU_INSTALL_SOURCE_ZIP,
  ID_LIB_MENU_UPDATEINDEX,
  ID_LIB_BTN_INSTALL,
  ID_LIB_MENU_UNINSTALL,
  ID_LIB_BTN_CLOSE
};

// extract arch from FQBN "pkg:arch:board"
static std::string ExtractArchFromFqbn(const std::string &fqbn) {
  auto first = fqbn.find(':');
  if (first == std::string::npos)
    return {};

  auto second = fqbn.find(':', first + 1);
  if (second == std::string::npos)
    return {};

  return fqbn.substr(first + 1, second - first - 1);
}

ArduinoLibraryManagerFrame::ArduinoLibraryManagerFrame(wxWindow *parent, ArduinoCli *cli, const std::vector<ArduinoCoreBoard> &availableBoards, wxConfigBase *config, const wxString &initialType)
    : wxFrame(parent, wxID_ANY, _("Arduino Libraries"),
              wxDefaultPosition, wxSize(800, 600),
              wxDEFAULT_FRAME_STYLE | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT),
      m_cli(cli),
      m_config(config),
      m_searchTimer(this, ID_LIB_SEARCH_TIMER) {

  BuildUi();

  Bind(wxEVT_CLOSE_WINDOW, &ArduinoLibraryManagerFrame::OnClose, this);
  m_topicChoice->Bind(wxEVT_CHOICE, &ArduinoLibraryManagerFrame::OnTopicChanged, this);
  m_typeChoice->Bind(wxEVT_CHOICE, &ArduinoLibraryManagerFrame::OnTypeChanged, this);
  m_searchCtrl->Bind(wxEVT_TEXT, &ArduinoLibraryManagerFrame::OnSearchText, this);
  Bind(wxEVT_TIMER, &ArduinoLibraryManagerFrame::OnSearchTimer, this, ID_LIB_SEARCH_TIMER);
  m_listCtrl->Bind(wxEVT_LIST_ITEM_ACTIVATED, &ArduinoLibraryManagerFrame::OnItemActivated, this);
  m_listCtrl->Bind(wxEVT_LIST_COL_CLICK, &ArduinoLibraryManagerFrame::OnColClick, this);
  m_listCtrl->Bind(wxEVT_CONTEXT_MENU, &ArduinoLibraryManagerFrame::OnListContextMenu, this);
  m_bottomInstallBtn->Bind(wxEVT_BUTTON, &ArduinoLibraryManagerFrame::OnBottomInstallButton, this);
  m_bottomCloseBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Close(); });
  Bind(wxEVT_MENU, &ArduinoLibraryManagerFrame::OnListInstallLatest, this, ID_LIB_MENU_INSTALL_LATEST);
  Bind(wxEVT_MENU, &ArduinoLibraryManagerFrame::OnListInstallVersion, this, ID_LIB_MENU_INSTALL_VERSION_BASE, ID_LIB_MENU_INSTALL_VERSION_BASE + 2000);
  Bind(wxEVT_MENU, &ArduinoLibraryManagerFrame::OnBottomInstallSourceGit, this, ID_LIB_MENU_INSTALL_SOURCE_GIT);
  Bind(wxEVT_MENU, &ArduinoLibraryManagerFrame::OnBottomInstallSourceZip, this, ID_LIB_MENU_INSTALL_SOURCE_ZIP);
  Bind(wxEVT_MENU, &ArduinoLibraryManagerFrame::OnUpdateLibraryIndex, this, ID_LIB_MENU_UPDATEINDEX);
  Bind(wxEVT_MENU, &ArduinoLibraryManagerFrame::OnListUninstall, this, ID_LIB_MENU_UNINSTALL);

  // setting the initial "Type"
  if (m_typeChoice) {
    int idx = m_typeChoice->FindString(initialType);
    if (idx == wxNOT_FOUND)
      idx = 0;
    m_typeChoice->SetSelection(idx);
  }

  // loading position/size from config

  m_supportedArchitectures.clear();
  for (const auto &b : availableBoards) {
    std::string arch = ExtractArchFromFqbn(b.fqbn);
    if (!arch.empty()) {
      m_supportedArchitectures.insert(arch);
    }
  }

  InitData();
}

ArduinoLibraryManagerFrame::~ArduinoLibraryManagerFrame() {
  m_searchTimer.Stop();
}

void ArduinoLibraryManagerFrame::BuildUi() {
  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  // top row: Topic, Type, Search
  {
    auto *row = new wxBoxSizer(wxHORIZONTAL);

    row->Add(new wxStaticText(this, wxID_ANY, _("Topic:")),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_topicChoice = new wxChoice(this, ID_LIB_TOPIC_CHOICE);
    row->Add(m_topicChoice, 0, wxRIGHT, 10);

    row->Add(new wxStaticText(this, wxID_ANY, _("Type:")),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_typeChoice = new wxChoice(this, ID_LIB_TYPE_CHOICE);
    m_typeChoice->Append(_("All"));
    m_typeChoice->Append(_("Updatable"));
    m_typeChoice->Append(_("Installed"));
    m_typeChoice->SetSelection(0);
    row->Add(m_typeChoice, 0, wxRIGHT, 10);

    row->AddStretchSpacer(1);

    row->Add(new wxStaticText(this, wxID_ANY, _("Search:")),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_searchCtrl = new wxTextCtrl(this, ID_LIB_SEARCH_CTRL);
    m_searchCtrl->SetHint(_("author / maintainer / text / website"));
    row->Add(m_searchCtrl, 2, wxEXPAND);

    topSizer->Add(row, 0, wxALL | wxEXPAND, 8);
  }

  // list
  {
    m_listCtrl = new wxListCtrl(this, ID_LIB_LIST_CTRL,
                                wxDefaultPosition, wxDefaultSize,
                                wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SUNKEN);

    m_listCtrl->SetImageList(CreateListCtrlSortIndicatorImageList(m_listCtrl->GetForegroundColour()), wxIMAGE_LIST_SMALL);

    // order: Name, Category, Version, Maintainer
    m_colLabels[0] = _("Name");
    m_colLabels[1] = _("Category");
    m_colLabels[2] = _("Version");
    m_colLabels[3] = _("Maintainer");

    m_listCtrl->AppendColumn(m_colLabels[0], wxLIST_FORMAT_LEFT, 250);
    m_listCtrl->AppendColumn(m_colLabels[1], wxLIST_FORMAT_LEFT, 140);
    m_listCtrl->AppendColumn(m_colLabels[2], wxLIST_FORMAT_LEFT, 90);
    m_listCtrl->AppendColumn(m_colLabels[3], wxLIST_FORMAT_LEFT, 220);

    topSizer->Add(m_listCtrl, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);
  }

  // bottom line: Install (Git/Zip) on the left, Close on the right
  {
    auto *bottomSizer = new wxBoxSizer(wxHORIZONTAL);

    m_bottomInstallBtn = new wxButton(this, ID_LIB_BTN_INSTALL, _("Other..."));
    bottomSizer->Add(m_bottomInstallBtn, 0, wxLEFT | wxBOTTOM, 8);

    bottomSizer->AddStretchSpacer(1);

    m_bottomCloseBtn = new wxButton(this, ID_LIB_BTN_CLOSE, _("Close"));
    bottomSizer->Add(m_bottomCloseBtn, 0, wxRIGHT | wxBOTTOM, 8);

    topSizer->Add(bottomSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 0);
  }

  SetSizer(topSizer);

  if (!LoadWindowSize(wxT("LibraryManager"), this, m_config)) {
    Centre();
  }
}

void ArduinoLibraryManagerFrame::InitData() {
  m_allLibraries = m_cli->GetLibraries();
  m_installedLibraries = m_cli->GetInstalledLibraries();

  if (m_allLibraries.empty()) {
    // nothing has been loaded yet -> display "Loading..."
    if (m_listCtrl) {
      m_listCtrl->Freeze();
      m_listCtrl->DeleteAllItems();
      m_listCtrl->InsertItem(0, _("Loading libraries..."));
      m_listCtrl->Thaw();
    }
    return;
  }

  RebuildTopicChoices();
  ApplyFilter();
}

void ArduinoLibraryManagerFrame::RebuildTopicChoices() {
  std::set<std::string> topics;

  for (const auto &lib : m_allLibraries) {
    if (!MatchesArchitecture(lib))
      continue;

    if (!lib.latest.category.empty()) {
      topics.insert(lib.latest.category);
    }
  }

  m_topicChoice->Clear();
  m_topicChoice->Append(_("All topics"));
  for (const auto &t : topics) {
    m_topicChoice->Append(wxString::FromUTF8(t.c_str()));
  }
  m_topicChoice->SetSelection(0);
}

void ArduinoLibraryManagerFrame::ApplyFilter() {
  if (!m_listCtrl)
    return;

  APP_DEBUG_LOG("m_allLibraries.size=%d", m_allLibraries.size());
  APP_DEBUG_LOG("m_installedLibraries.size=%d", m_installedLibraries.size());

  wxString topicSel = m_topicChoice ? m_topicChoice->GetStringSelection() : _("All topics");
  wxString typeSel = m_typeChoice ? m_typeChoice->GetStringSelection() : _("All");
  wxString searchText = m_searchCtrl->GetValue();

  m_listCtrl->Freeze();
  m_listCtrl->DeleteAllItems();
  m_filteredIndices.clear();

  // 1) collect the indexes that pass the filters
  for (size_t i = 0; i < m_allLibraries.size(); ++i) {
    const auto &lib = m_allLibraries[i];

    if (!MatchesArchitecture(lib))
      continue;
    if (!MatchesTopic(lib, topicSel))
      continue;
    if (!MatchesType(lib, typeSel))
      continue;
    if (!MatchesSearch(lib, searchText))
      continue;
    if (!MatchesExplicitLib(lib))
      continue;

    m_filteredIndices.push_back((int)i);
  }

  // 2) sort according to m_sortColumn / m_sortAscending
  std::sort(m_filteredIndices.begin(), m_filteredIndices.end(),
            [&](int ia, int ib) {
              const auto &A = m_allLibraries[(size_t)ia];
              const auto &B = m_allLibraries[(size_t)ib];

              std::string sa, sb;

              switch (m_sortColumn) {
                case 0:
                  sa = A.name;
                  break;
                case 1:
                  sa = A.latest.category;
                  break;
                case 2:
                  sa = A.latest.version;
                  break;
                case 3:
                  sa = A.latest.maintainer;
                  break;
                default:
                  sa = A.name;
                  break;
              }

              switch (m_sortColumn) {
                case 0:
                  sb = B.name;
                  break;
                case 1:
                  sb = B.latest.category;
                  break;
                case 2:
                  sb = B.latest.version;
                  break;
                case 3:
                  sb = B.latest.maintainer;
                  break;
                default:
                  sb = B.name;
                  break;
              }

              if (sa == sb)
                return false;

              if (m_sortAscending)
                return sa < sb;
              else
                return sa > sb;
            });

  EditorSettings settings;
  settings.Load(m_config);
  EditorColorScheme colors = settings.GetColors();

  // 3) fill the list in sorted order
  for (size_t row = 0; row < m_filteredIndices.size(); ++row) {
    int libIndex = m_filteredIndices[row];
    const auto &lib = m_allLibraries[(size_t)libIndex];

    long item = m_listCtrl->InsertItem((long)row,
                                       wxString::FromUTF8(lib.name.c_str()));
    m_listCtrl->SetItem(item, 1,
                        wxString::FromUTF8(lib.latest.category.c_str()));
    m_listCtrl->SetItem(item, 2,
                        wxString::FromUTF8(lib.latest.version.c_str()));
    m_listCtrl->SetItem(item, 3,
                        wxString::FromUTF8(lib.latest.maintainer.c_str()));

    m_listCtrl->SetItemData(item, (long)libIndex);

    // --- color highlighting legacy / updatable ---
    UpdateStateColors(lib, item, colors);
  }

  // 4) auto-resize columns after filling (point 4)
  for (int col = 0; col < 4; ++col) {
    m_listCtrl->SetColumnWidth(col, wxLIST_AUTOSIZE_USEHEADER);
  }

  m_listCtrl->Thaw();

  UpdateColumnHeaders();
}

void ArduinoLibraryManagerFrame::UpdateColumnHeaders() {
  if (!m_listCtrl)
    return;

  for (int col = 0; col < 4; ++col) {
    wxListItem item;
    item.SetId(col);
    item.SetMask(wxLIST_MASK_TEXT | wxLIST_MASK_IMAGE);

    item.SetText(m_colLabels[col]);

    if (col == m_sortColumn) {
      item.SetImage(m_sortAscending ? IMLI_TREECTRL_ARROW_UP : IMLI_TREECTRL_ARROW_DOWN);
    } else {
      item.SetImage(IMLI_TREECTRL_ARROW_EMPTY);
    }

    m_listCtrl->SetColumn(col, item);
  }
}
bool ArduinoLibraryManagerFrame::MatchesArchitecture(const ArduinoLibraryInfo &lib) const {
  const auto &archs = lib.latest.architectures;

  if (archs.empty())
    return true; // better not to limit if nothing is specified

  // library for all architectures
  for (const auto &a : archs) {
    if (a == "*" || a == "all" || a == "any")
      return true;
  }

  if (m_supportedArchitectures.empty())
    return true;

  for (const auto &a : archs) {
    if (m_supportedArchitectures.count(a) > 0)
      return true;
  }

  return false;
}

bool ArduinoLibraryManagerFrame::MatchesTopic(const ArduinoLibraryInfo &lib,
                                              const wxString &topic) const {
  if (topic.empty() || topic == _("All topics"))
    return true;

  wxString cat = wxString::FromUTF8(lib.latest.category.c_str());
  return cat == topic;
}

bool ArduinoLibraryManagerFrame::MatchesType(const ArduinoLibraryInfo &lib,
                                             const wxString &type) const {
  // All (or empty) -> no restriction
  if (type.IsEmpty() || type == _("All"))
    return true;

  // we check if this library is installed and what version it has
  std::string installedVersion;
  bool installed = false;

  for (const auto &inst : m_installedLibraries) {
    if (inst.name == lib.name) { // match by name
      installed = true;
      installedVersion = inst.latest.version;
      break;
    }
  }

  if (type == _("Installed")) {
    // we want only those that are installed
    return installed;
  }

  if (type == _("Updatable")) {
    // Updatable = installed and latest > installed
    if (!installed)
      return false;

    const std::string &latestVer = lib.latest.version;

    if (latestVer.empty() || installedVersion.empty())
      return false; // we don't have info -> better not mark as updatable

    int cmp = CompareVersions(installedVersion, latestVer);
    if (cmp >= 0) {
      // installed == latest or installed > latest -> not updatable
      return false;
    }

    // latest > installed -> updatable
    return true;
  }

  // unknown type -> behave like All
  return true;
}

bool ArduinoLibraryManagerFrame::MatchesSearch(const ArduinoLibraryInfo &lib,
                                               const wxString &text) const {
  if (text.empty())
    return true;

  std::string needle = ToLower(std::string(text.utf8_str()));

  const auto &rel = lib.latest;

  std::string haystack;
  auto add = [&](const std::string &s) {
    if (!s.empty()) {
      haystack += s;
      haystack += ' ';
    }
  };

  add(lib.name);
  add(rel.author);
  add(rel.maintainer);
  add(rel.sentence);
  add(rel.paragraph);
  add(rel.website);

  haystack = ToLower(haystack);

  return haystack.find(needle) != std::string::npos;
}

bool ArduinoLibraryManagerFrame::MatchesExplicitLib(const ArduinoLibraryInfo &lib) const {
  if (m_explicitShowLibs.empty())
    return true;

  return std::any_of(m_explicitShowLibs.begin(), m_explicitShowLibs.end(),
                     [&](const ArduinoLibraryInfo &x) { return x.name == lib.name; });
}

// ---- Handlers ----

void ArduinoLibraryManagerFrame::OnShow(wxShowEvent &evt) {
  evt.Skip();

  if (!evt.IsShown())
    return;

  if (!m_refreshOnShow)
    return;

  m_refreshOnShow = false;

  CallAfter([this] {
    ApplyFilter();
  });
}

void ArduinoLibraryManagerFrame::OnClose(wxCloseEvent &evt) {
  SaveWindowSize(wxT("LibraryManager"), this, m_config);

  // Explicit libraries will be cleared on close
  m_explicitShowLibs.clear();
  m_refreshOnShow = true;

  Hide();
  evt.Veto();
}

void ArduinoLibraryManagerFrame::OnTopicChanged(wxCommandEvent &evt) {
  (void)evt;
  ApplyFilter();
}

void ArduinoLibraryManagerFrame::OnTypeChanged(wxCommandEvent &evt) {
  (void)evt;
  ApplyFilter();
}

void ArduinoLibraryManagerFrame::OnSearchText(wxCommandEvent &evt) {
  (void)evt;
  // debounce - restart the timer
  m_searchTimer.Start(300, true); // 300 ms since the last press
}

void ArduinoLibraryManagerFrame::OnSearchTimer(wxTimerEvent &evt) {
  (void)evt;
  ApplyFilter();
}

void ArduinoLibraryManagerFrame::OnItemActivated(wxListEvent &evt) {
  long row = evt.GetIndex();
  if (row < 0)
    return;

  long libIndex = m_listCtrl->GetItemData(row);
  if (libIndex < 0 || libIndex >= (long)m_allLibraries.size())
    return;

  const auto &lib = m_allLibraries[(size_t)libIndex];

  wxString typeLabel = m_typeChoice
                           ? m_typeChoice->GetStringSelection()
                           : _("All");

  const ArduinoLibraryInfo *installedInfo = nullptr;
  for (const auto &inst : m_installedLibraries) {
    if (inst.name == lib.name) {
      installedInfo = &inst;
      break;
    }
  }

  ArduinoLibraryDetailDialog dlg(this, lib, installedInfo, m_config, typeLabel);
  dlg.ShowModal();
}

void ArduinoLibraryManagerFrame::OnColClick(wxListEvent &evt) {
  int col = evt.GetColumn();
  if (col < 0)
    return;

  if (m_sortColumn == col) {
    m_sortAscending = !m_sortAscending;
  } else {
    m_sortColumn = col;
    m_sortAscending = true;
  }

  ApplyFilter();
  UpdateColumnHeaders();
}

void ArduinoLibraryManagerFrame::OnListContextMenu(wxContextMenuEvent &WXUNUSED(evt)) {
  if (!m_listCtrl)
    return;

  long item = m_listCtrl->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (item == -1) {
    // Nothing selected
    return;
  }

  long libIndex = m_listCtrl->GetItemData(item);
  if (libIndex < 0 || libIndex >= (long)m_allLibraries.size())
    return;

  m_contextLibIndex = (int)libIndex;
  m_versionMenuMap.clear();

  const auto &lib = m_allLibraries[(size_t)libIndex];

  wxMenu menu;
  wxMenu *installSub = new wxMenu;

  installSub->Append(ID_LIB_MENU_INSTALL_LATEST, _("Latest"));
  installSub->AppendSeparator();

  int baseId = ID_LIB_MENU_INSTALL_VERSION_BASE;
  for (size_t i = 0; i < lib.availableVersions.size(); ++i) {
    int id = baseId + (int)i;
    wxString v = wxString::FromUTF8(lib.availableVersions[i].c_str());
    installSub->Append(id, v);
    m_versionMenuMap[id] = lib.availableVersions[i];
  }

  menu.AppendSubMenu(installSub, _("Install"));

  // check whether this library is installed
  bool installed = false;
  for (const auto &inst : m_installedLibraries) {
    if (inst.name == lib.name) {
      installed = true;
      break;
    }
  }

  if (installed) {
    menu.AppendSeparator();
    menu.Append(ID_LIB_MENU_UNINSTALL, _("Uninstall"));
  }

  m_listCtrl->PopupMenu(&menu);
}

void ArduinoLibraryManagerFrame::OnListInstallLatest(wxCommandEvent &evt) {
  (void)evt;

  if (m_contextLibIndex < 0 ||
      m_contextLibIndex >= (int)m_allLibraries.size())
    return;

  const auto &lib = m_allLibraries[(size_t)m_contextLibIndex];

  // latest -> version empty (only name)
  StartInstallRepo(lib, std::string{});
}

void ArduinoLibraryManagerFrame::OnListInstallVersion(wxCommandEvent &evt) {
  int id = evt.GetId();

  if (m_contextLibIndex < 0 ||
      m_contextLibIndex >= (int)m_allLibraries.size())
    return;

  auto it = m_versionMenuMap.find(id);
  if (it == m_versionMenuMap.end())
    return;

  const auto &lib = m_allLibraries[(size_t)m_contextLibIndex];
  const std::string &ver = it->second;

  StartInstallRepo(lib, ver);
}

void ArduinoLibraryManagerFrame::OnListUninstall(wxCommandEvent &evt) {
  (void)evt;

  if (m_contextLibIndex < 0 ||
      m_contextLibIndex >= (int)m_allLibraries.size())
    return;

  const auto &lib = m_allLibraries[(size_t)m_contextLibIndex];

  // only installed
  bool installed = false;
  for (const auto &inst : m_installedLibraries) {
    if (inst.name == lib.name) {
      installed = true;
      break;
    }
  }
  if (!installed)
    return;

  StartUninstallRepo(lib);
}

void ArduinoLibraryManagerFrame::OnBottomInstallButton(wxCommandEvent &evt) {
  (void)evt;

  if (!m_bottomInstallBtn)
    return;

  wxMenu menu;
  menu.Append(ID_LIB_MENU_INSTALL_SOURCE_GIT, _("Git repository..."));
  menu.Append(ID_LIB_MENU_INSTALL_SOURCE_ZIP, _("Zip file..."));
  menu.AppendSeparator();
  menu.Append(ID_LIB_MENU_UPDATEINDEX, _("Update library index"));

  // position under the button
  wxPoint btnPos = m_bottomInstallBtn->GetPosition();
  wxSize btnSz = m_bottomInstallBtn->GetSize();

  wxPoint popupPos(btnPos.x, btnPos.y + btnSz.y);

  PopupMenu(&menu, popupPos);
}

void ArduinoLibraryManagerFrame::OnBottomInstallSourceGit(wxCommandEvent &evt) {
  (void)evt;

  wxTextEntryDialog dlg(this,
                        _("Enter Git repository URL (e.g. https://github.com/arduino-libraries/WiFi101.git or with #version):"),
                        _("Install library from Git"));

  if (dlg.ShowModal() != wxID_OK)
    return;

  wxString url = dlg.GetValue();
  url.Trim(true).Trim(false);

  if (url.empty())
    return;

  ArduinoLibraryInstallSpec spec;
  spec.source = ArduinoLibraryInstallSource::Git;
  spec.spec = wxToStd(url);

  std::vector<ArduinoLibraryInstallSpec> specs;
  specs.push_back(std::move(spec));

  StartInstallSpecs(specs);
}

void ArduinoLibraryManagerFrame::OnBottomInstallSourceZip(wxCommandEvent &evt) {
  (void)evt;

  wxFileDialog dlg(this,
                   _("Select library ZIP file"),
                   wxEmptyString,
                   wxEmptyString,
                   _("ZIP archives (*.zip)|*.zip|All files (*.*)|*.*"),
                   wxFD_OPEN | wxFD_FILE_MUST_EXIST);

  if (dlg.ShowModal() != wxID_OK)
    return;

  wxString path = dlg.GetPath();
  if (path.empty())
    return;

  ArduinoLibraryInstallSpec spec;
  spec.source = ArduinoLibraryInstallSource::Zip;
  spec.spec = wxToStd(path);

  std::vector<ArduinoLibraryInstallSpec> specs;
  specs.push_back(std::move(spec));

  StartInstallSpecs(specs);
}

void ArduinoLibraryManagerFrame::OnUpdateLibraryIndex(wxCommandEvent &evt) {
  (void)evt;

  ArduinoEditorFrame *frame = wxDynamicCast(GetParent(), ArduinoEditorFrame);
  if (!frame)
    return;

  if (frame->CanPerformAction(libupdateindex, true)) {
    m_cli->UpdateLibraryIndexAsync(frame);
  }
}

void ArduinoLibraryManagerFrame::RefreshLibraries() {
  if (!m_cli) {
    return;
  }

  m_allLibraries = m_cli->GetLibraries();
  if (m_allLibraries.empty()) {
    return;
  }

  RebuildTopicChoices();
  ApplyFilter();
}

void ArduinoLibraryManagerFrame::RefreshInstalledLibraries() {
  if (!m_cli) {
    return;
  }

  m_installedLibraries = m_cli->GetInstalledLibraries();
  if (m_installedLibraries.empty()) {
    return;
  }

  ApplyFilter();
}

void ArduinoLibraryManagerFrame::UpdateStateColors(const ArduinoLibraryInfo &lib, long item, const EditorColorScheme &colors) {
  bool installed = false;
  bool isLegacy = false;
  bool isUpdatable = false;
  std::string instVer;

  // find any installed variant
  for (const auto &inst : m_installedLibraries) {
    if (inst.name == lib.name) {
      installed = true;
      instVer = inst.latest.version;
      isLegacy = inst.latest.isLegacy;
      break;
    }
  }

  const std::string &latestVer = lib.latest.version;
  if (installed && !latestVer.empty() && !instVer.empty()) {
    if (CompareVersions(instVer, latestVer) < 0) {
      isUpdatable = true;
    }
  }

  // updatable has priority over legacy
  if (isUpdatable) {
    m_listCtrl->SetItemBackgroundColour(item, colors.updatable);
  } else if (isLegacy) {
    m_listCtrl->SetItemBackgroundColour(item, colors.deprecated);
  } else if (installed) {
    m_listCtrl->SetItemBackgroundColour(item, colors.installed);
  }
}

void ArduinoLibraryManagerFrame::ApplySettings(const EditorSettings &settings) {
  EditorColorScheme colors = settings.GetColors();

  long itemIndex = -1;

  while ((itemIndex = m_listCtrl->GetNextItem(itemIndex, wxLIST_NEXT_ALL, wxLIST_STATE_DONTCARE)) != wxNOT_FOUND) {
    long libIndex = m_listCtrl->GetItemData(itemIndex);
    if (libIndex < 0 || libIndex >= (long)m_allLibraries.size())
      continue;

    const auto &lib = m_allLibraries[(size_t)libIndex];

    UpdateStateColors(lib, itemIndex, colors);
  }
}

void ArduinoLibraryManagerFrame::RefreshAvailableBoards(const std::vector<ArduinoCoreBoard> &boards) {
  m_supportedArchitectures.clear();
  for (const auto &b : boards) {
    std::string arch = ExtractArchFromFqbn(b.fqbn);
    if (!arch.empty()) {
      m_supportedArchitectures.insert(arch);
    }
  }

  RefreshLibraries();
}

void ArduinoLibraryManagerFrame::ShowLibraries(const std::vector<ArduinoLibraryInfo> &libs) {
  m_explicitShowLibs = libs;

  if (m_topicChoice)
    m_topicChoice->SetSelection(0);

  if (m_typeChoice)
    m_typeChoice->SetSelection(0);

  if (m_searchCtrl)
    m_searchCtrl->SetValue(wxEmptyString);

  ApplyFilter();
}

void ArduinoLibraryManagerFrame::StartInstallSpecs(
    const std::vector<ArduinoLibraryInstallSpec> &specs) {
  if (!m_cli)
    return;

  if (specs.empty())
    return;

  ArduinoEditorFrame *frame = wxDynamicCast(GetParent(), ArduinoEditorFrame);
  if (!frame)
    return;

  if (frame->CanPerformAction(libinstall, true)) {
    m_cli->InstallLibrariesAsync(specs, frame);
  }
}

void ArduinoLibraryManagerFrame::StartInstallRepo(const ArduinoLibraryInfo &lib,
                                                  const std::string &version) {
  if (!m_cli)
    return;

  // ---- helper: finds the library in m_allLibraries by name ----
  auto findLibByName = [this](const std::string &name) -> const ArduinoLibraryInfo * {
    for (const auto &l : m_allLibraries) {
      if (l.name == name)
        return &l;
    }
    return nullptr;
  };

  // ---- helper: is the library already installed? ----
  auto isInstalled = [this](const std::string &name) -> bool {
    for (const auto &inst : m_installedLibraries) {
      if (inst.name == name)
        return true;
    }
    return false;
  };

  // ---- recursive collection of all *not installed* dependencies ----
  std::set<std::string> missingDeps;
  std::set<std::string> visiting;

  std::function<void(const ArduinoLibraryInfo &)> dfs;
  dfs = [&](const ArduinoLibraryInfo &l) {
    if (visiting.count(l.name))
      return;
    visiting.insert(l.name);

    const auto &deps = l.latest.dependencies;
    for (const auto &depName : deps) {
      if (depName.empty() || depName == l.name)
        continue;

      const ArduinoLibraryInfo *depLib = findLibByName(depName);
      if (!depLib)
        continue; // unknown dependency in the catalog

      if (!isInstalled(depName)) {
        missingDeps.insert(depName);
      }

      dfs(*depLib);
    }
  };

  // collect missing dependencies for the selected library
  dfs(lib);
  missingDeps.erase(lib.name); // to be sure, remove any potential self-dependency

  // ---- ask the user to confirm the installation of dependencies ----
  bool installDependencies = false;

  if (!missingDeps.empty()) {
    wxString msg;
    msg << _("Library '") << wxString::FromUTF8(lib.name.c_str())
        << _("' has missing dependencies:\n\n");

    for (const auto &d : missingDeps) {
      msg << wxT("  - ") << wxString::FromUTF8(d.c_str()) << wxT("\n");
    }

    msg << _("\nDo you want to install these dependencies as well?");

    wxRichMessageDialog dlg(this,
                            msg,
                            _("Install dependencies"),
                            wxYES_NO | wxYES_DEFAULT | wxCANCEL | wxICON_QUESTION);
    int resl = dlg.ShowModal();

    if (resl == wxID_CANCEL) {
      return;
    }

    installDependencies = (resl == wxID_YES);
  }

  // ---- building the list of installations ----
  std::vector<ArduinoLibraryInstallSpec> specs;

  // 1) main library
  {
    ArduinoLibraryInstallSpec spec;
    spec.source = ArduinoLibraryInstallSource::Repo;

    if (version.empty()) {
      // latest -> only the name
      spec.spec = lib.name;
    } else {
      // specific version -> Name@1.2.3
      spec.spec = lib.name + "@" + version;
    }

    specs.push_back(std::move(spec));
  }

  // 2) dependencies (if the user agreed)
  if (installDependencies) {
    for (const auto &depName : missingDeps) {
      const ArduinoLibraryInfo *depLib = findLibByName(depName);
      if (!depLib)
        continue;

      ArduinoLibraryInstallSpec depSpec;
      depSpec.source = ArduinoLibraryInstallSource::Repo;

      if (!depLib->latest.version.empty()) {
        depSpec.spec = depLib->name + "@" + depLib->latest.version;
      } else {
        depSpec.spec = depLib->name;
      }

      specs.push_back(std::move(depSpec));
    }
  }

  // nothing to install (should not happen, but just in case)
  if (specs.empty())
    return;

  StartInstallSpecs(specs);
}

void ArduinoLibraryManagerFrame::InstallLibrariesWithDeps(
    const std::vector<ArduinoLibraryInfo> &libs) {
  if (!m_cli)
    return;
  if (libs.empty())
    return;

  // ---- helper: finds the library in m_allLibraries by name ----
  auto findLibByName = [this](const std::string &name) -> const ArduinoLibraryInfo * {
    for (const auto &l : m_allLibraries) {
      if (l.name == name)
        return &l;
    }
    return nullptr;
  };

  // ---- helper: is the library already installed? ----
  auto isInstalled = [this](const std::string &name) -> bool {
    for (const auto &inst : m_installedLibraries) {
      if (inst.name == name)
        return true;
    }
    return false;
  };

  // names of root libraries (those provided by the user)
  std::set<std::string> rootNames;
  for (const auto &l : libs) {
    rootNames.insert(l.name);
  }

  // ---- recursive collection of all *not installed* dependencies for all roots ----
  std::set<std::string> missingDeps;

  std::function<void(const ArduinoLibraryInfo &, std::set<std::string> &)> dfs;
  dfs = [&](const ArduinoLibraryInfo &l, std::set<std::string> &visiting) {
    if (visiting.count(l.name))
      return;
    visiting.insert(l.name);

    const auto &deps = l.latest.dependencies;
    for (const auto &depName : deps) {
      if (depName.empty() || depName == l.name)
        continue;

      const ArduinoLibraryInfo *depLib = findLibByName(depName);
      if (!depLib)
        continue; // unknown dependency in the catalog

      // if it is not installed, add it to the set of missing ones
      if (!isInstalled(depName)) {
        missingDeps.insert(depName);
      }

      dfs(*depLib, visiting);
    }
  };

  for (const auto &lib : libs) {
    std::set<std::string> visiting;
    dfs(lib, visiting);
  }

  // we do not want to list roots as a "dependency" on themselves
  for (const auto &name : rootNames) {
    missingDeps.erase(name);
  }

  // ---- query the user whether to install dependencies ----
  bool installDependencies = false;

  if (!missingDeps.empty()) {
    wxString msg;
    msg << _("The selected libraries have missing dependencies:\n\n");

    for (const auto &d : missingDeps) {
      msg << wxT("  - ") << wxString::FromUTF8(d.c_str()) << wxT("\n");
    }

    msg << _("\nDo you want to install these dependencies as well?");

    wxRichMessageDialog dlg(this,
                            msg,
                            _("Install dependencies"),
                            wxYES_NO | wxYES_DEFAULT | wxCANCEL | wxICON_QUESTION);
    int resl = dlg.ShowModal();

    if (resl == wxID_CANCEL) {
      // user canceled the entire operation
      return;
    }

    installDependencies = (resl == wxID_YES);
  }

  // ---- building the list of installations ----
  std::vector<ArduinoLibraryInstallSpec> specs;

  // 1) main (user-selected) libraries - always the latest version
  for (const auto &lib : libs) {
    ArduinoLibraryInstallSpec spec;
    spec.source = ArduinoLibraryInstallSource::Repo;

    if (!lib.latest.version.empty()) {
      spec.spec = lib.name + "@" + lib.latest.version;
    } else {
      spec.spec = lib.name;
    }

    specs.push_back(std::move(spec));
  }

  // 2) dependencies (if the user agreed)
  if (installDependencies) {
    for (const auto &depName : missingDeps) {
      const ArduinoLibraryInfo *depLib = findLibByName(depName);
      if (!depLib)
        continue;

      ArduinoLibraryInstallSpec depSpec;
      depSpec.source = ArduinoLibraryInstallSource::Repo;

      if (!depLib->latest.version.empty()) {
        depSpec.spec = depLib->name + "@" + depLib->latest.version;
      } else {
        depSpec.spec = depLib->name;
      }

      specs.push_back(std::move(depSpec));
    }
  }

  if (specs.empty())
    return;

  StartInstallSpecs(specs);
}

void ArduinoLibraryManagerFrame::StartUninstallRepo(const ArduinoLibraryInfo &lib) {
  if (!m_cli)
    return;

  // ---- helper: finds the library in m_allLibraries by name ----
  auto findLibByName = [this](const std::string &name) -> const ArduinoLibraryInfo * {
    for (const auto &l : m_allLibraries) {
      if (l.name == name)
        return &l;
    }
    return nullptr;
  };

  // ---- determine which installed libraries depend (recursively) on lib.name ----
  std::vector<std::string> dependents;

  for (const auto &inst : m_installedLibraries) {
    if (inst.name == lib.name)
      continue; // this is the one we want to delete

    const ArduinoLibraryInfo *root = findLibByName(inst.name);
    if (!root)
      continue;

    std::set<std::string> visiting;
    bool usesTarget = false;

    std::function<void(const ArduinoLibraryInfo &)> dfs;
    dfs = [&](const ArduinoLibraryInfo &l) {
      if (usesTarget)
        return; // we already know that it depends

      if (visiting.count(l.name))
        return;
      visiting.insert(l.name);

      for (const auto &depName : l.latest.dependencies) {
        if (depName == lib.name) {
          usesTarget = true;
          return;
        }

        const ArduinoLibraryInfo *depLib = findLibByName(depName);
        if (depLib) {
          dfs(*depLib);
          if (usesTarget)
            return;
        }
      }
    };

    dfs(*root);

    if (usesTarget) {
      dependents.push_back(inst.name);
    }
  }

  // ---- confirmation dialog (with info about dependencies) ----
  wxString msg;

  if (!dependents.empty()) {
    msg << _("Other installed libraries depend on:\n  '")
        << wxString::FromUTF8(lib.name.c_str()) << wxT("':\n\n");

    for (const auto &d : dependents) {
      msg << wxT("  - ") << wxString::FromUTF8(d) << wxT("\n");
    }

    msg << _("\nUninstalling this library may break those libraries.\n");
    msg << _("\nDo you really want to uninstall it?");
  } else {
    msg << _("Do you really want to uninstall library:\n");
    msg << wxString::FromUTF8(lib.name) << wxT("?");
  }

  wxRichMessageDialog dlg(this, msg, _("Uninstall library"),
                          wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
  if (dlg.ShowModal() != wxID_YES)
    return;

  ArduinoEditorFrame *frame = wxDynamicCast(GetParent(), ArduinoEditorFrame);
  if (!frame)
    return;

  std::vector<std::string> names;
  names.push_back(lib.name);

  if (frame->CanPerformAction(libremove, true)) {
    m_cli->UninstallLibrariesAsync(names, frame);
  }
}
