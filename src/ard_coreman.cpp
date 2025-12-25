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

#include "ard_coreman.hpp"

#include "ard_cli.hpp"
#include "ard_coreinfo.hpp"
#include "ard_ed_frm.hpp"
#include "ard_setdlg.hpp"
#include "utils.hpp"

#include <algorithm>
#include <functional>
#include <set>

#include <wx/msgdlg.h>
#include <wx/richmsgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

enum {
  ID_CORE_TYPE_CHOICE = wxID_HIGHEST + 3000,
  ID_CORE_SEARCH_CTRL,
  ID_CORE_LIST_CTRL,
  ID_CORE_SEARCH_TIMER,
  ID_CORE_MENU_INSTALL_LATEST,
  ID_CORE_MENU_INSTALL_VERSION_BASE,
  ID_CORE_MENU_UNINSTALL,
  ID_CORE_BTN_UPDATE,
  ID_CORE_BTN_CLOSE
};

ArduinoCoreManagerFrame::ArduinoCoreManagerFrame(wxWindow *parent,
                                                 ArduinoCli *cli,
                                                 wxConfigBase *config,
                                                 const wxString &initialType)
    : wxFrame(parent, wxID_ANY, _("Arduino Cores"),
              wxDefaultPosition, wxSize(800, 600),
              wxDEFAULT_FRAME_STYLE | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT),
      m_cli(cli),
      m_config(config),
      m_searchTimer(this, ID_CORE_SEARCH_TIMER) {

  BuildUi();

  Bind(wxEVT_CLOSE_WINDOW, &ArduinoCoreManagerFrame::OnClose, this);
  m_typeChoice->Bind(wxEVT_CHOICE, &ArduinoCoreManagerFrame::OnTypeChanged, this);
  m_searchCtrl->Bind(wxEVT_TEXT, &ArduinoCoreManagerFrame::OnSearchText, this);
  Bind(wxEVT_TIMER, &ArduinoCoreManagerFrame::OnSearchTimer, this, ID_CORE_SEARCH_TIMER);
  m_listCtrl->Bind(wxEVT_LIST_ITEM_ACTIVATED, &ArduinoCoreManagerFrame::OnItemActivated, this);
  m_listCtrl->Bind(wxEVT_LIST_COL_CLICK, &ArduinoCoreManagerFrame::OnColClick, this);
  m_listCtrl->Bind(wxEVT_CONTEXT_MENU, &ArduinoCoreManagerFrame::OnListContextMenu, this);
  m_bottomUpdateBtn->Bind(wxEVT_BUTTON, &ArduinoCoreManagerFrame::OnBottomUpdateIndex, this);
  m_bottomCloseBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { SaveWindowSize(wxT("CoreManager"), this, m_config); Hide(); });

  Bind(wxEVT_MENU, &ArduinoCoreManagerFrame::OnListInstallLatest, this,
       ID_CORE_MENU_INSTALL_LATEST);
  Bind(wxEVT_MENU, &ArduinoCoreManagerFrame::OnListInstallVersion, this,
       ID_CORE_MENU_INSTALL_VERSION_BASE,
       ID_CORE_MENU_INSTALL_VERSION_BASE + 2000);
  Bind(wxEVT_MENU, &ArduinoCoreManagerFrame::OnListUninstall, this,
       ID_CORE_MENU_UNINSTALL);

  // Initial type
  if (m_typeChoice) {
    int idx = m_typeChoice->FindString(initialType);
    if (idx == wxNOT_FOUND)
      idx = 0;
    m_typeChoice->SetSelection(idx);
  }

  InitData();
}

ArduinoCoreManagerFrame::~ArduinoCoreManagerFrame() {
  m_searchTimer.Stop();
}

void ArduinoCoreManagerFrame::BuildUi() {
  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  // top row: Type, Search
  {
    auto *row = new wxBoxSizer(wxHORIZONTAL);

    row->Add(new wxStaticText(this, wxID_ANY, _("Type:")),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_typeChoice = new wxChoice(this, ID_CORE_TYPE_CHOICE);
    m_typeChoice->Append(_("All"));
    m_typeChoice->Append(_("Updatable"));
    m_typeChoice->Append(_("Installed"));
    m_typeChoice->SetSelection(0);
    row->Add(m_typeChoice, 0, wxRIGHT, 10);

    row->AddStretchSpacer(1);

    row->Add(new wxStaticText(this, wxID_ANY, _("Search:")),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_searchCtrl = new wxTextCtrl(this, ID_CORE_SEARCH_CTRL);
    m_searchCtrl->SetHint(_("id / maintainer / board / website"));
    row->Add(m_searchCtrl, 2, wxEXPAND);

    topSizer->Add(row, 0, wxALL | wxEXPAND, 8);
  }

  // list
  {
    m_listCtrl = new wxListCtrl(this, ID_CORE_LIST_CTRL,
                                wxDefaultPosition, wxDefaultSize,
                                wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SUNKEN);

    m_listCtrl->SetImageList(CreateListCtrlSortIndicatorImageList(m_listCtrl->GetForegroundColour()), wxIMAGE_LIST_SMALL);

    // Name (id), Version, Maintainer, Boards
    m_colLabels[0] = _("Core");
    m_colLabels[1] = _("Version");
    m_colLabels[2] = _("Maintainer");
    m_colLabels[3] = _("Boards");

    m_listCtrl->AppendColumn(m_colLabels[0], wxLIST_FORMAT_LEFT, 220);
    m_listCtrl->AppendColumn(m_colLabels[1], wxLIST_FORMAT_LEFT, 90);
    m_listCtrl->AppendColumn(m_colLabels[2], wxLIST_FORMAT_LEFT, 180);
    m_listCtrl->AppendColumn(m_colLabels[3], wxLIST_FORMAT_LEFT, 260);

    topSizer->Add(m_listCtrl, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);
  }

  // bottom line: on the left "Update index", on the right Close
  {
    auto *bottomSizer = new wxBoxSizer(wxHORIZONTAL);

    m_bottomUpdateBtn = new wxButton(this, ID_CORE_BTN_UPDATE, _("Update index"));
    bottomSizer->Add(m_bottomUpdateBtn, 0, wxLEFT | wxBOTTOM, 8);

    bottomSizer->AddStretchSpacer(1);

    m_bottomCloseBtn = new wxButton(this, ID_CORE_BTN_CLOSE, _("Close"));
    bottomSizer->Add(m_bottomCloseBtn, 0, wxRIGHT | wxBOTTOM, 8);

    topSizer->Add(bottomSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 0);
  }

  SetSizer(topSizer);

  if (!LoadWindowSize(wxT("CoreManager"), this, m_config)) {
    Centre();
  }
}

void ArduinoCoreManagerFrame::InitData() {
  m_allCores = m_cli->GetCores();

  if (m_allCores.empty()) {
    if (m_listCtrl) {
      m_listCtrl->Freeze();
      m_listCtrl->DeleteAllItems();
      m_listCtrl->InsertItem(0, _("No cores (boards) available"));
      m_listCtrl->Thaw();
    }
    return;
  }

  ApplyFilter();
}

void ArduinoCoreManagerFrame::ApplyFilter() {
  if (!m_listCtrl)
    return;

  APP_DEBUG_LOG("m_allCores.size=%d", (int)m_allCores.size());

  wxString typeSel = m_typeChoice ? m_typeChoice->GetStringSelection() : _("All");
  wxString searchText = m_searchCtrl->GetValue();

  m_listCtrl->Freeze();
  m_listCtrl->DeleteAllItems();
  m_filteredIndices.clear();

  // 1) selection according to filters
  for (size_t i = 0; i < m_allCores.size(); ++i) {
    const auto &core = m_allCores[i];

    if (!MatchesType(core, typeSel))
      continue;
    if (!MatchesSearch(core, searchText))
      continue;

    m_filteredIndices.push_back((int)i);
  }

  // 2) sort by column
  std::sort(m_filteredIndices.begin(), m_filteredIndices.end(),
            [&](int ia, int ib) {
              const auto &A = m_allCores[(size_t)ia];
              const auto &B = m_allCores[(size_t)ib];

              std::string sa, sb;

              switch (m_sortColumn) {
                case 0:
                  sa = A.id;
                  break;
                case 1:
                  sa = A.latestVersion;
                  break;
                case 2:
                  sa = A.maintainer;
                  break;
                case 3: {
                  // boards as a string (only for sort)
                  for (const auto &rel : A.releases) {
                    for (const auto &b : rel.boards) {
                      if (!sa.empty())
                        sa += ", ";
                      sa += b.name;
                    }
                  }
                  break;
                }
                default:
                  sa = A.id;
                  break;
              }

              switch (m_sortColumn) {
                case 0:
                  sb = B.id;
                  break;
                case 1:
                  sb = B.latestVersion;
                  break;
                case 2:
                  sb = B.maintainer;
                  break;
                case 3: {
                  for (const auto &rel : B.releases) {
                    for (const auto &b : rel.boards) {
                      if (!sb.empty())
                        sb += ", ";
                      sb += b.name;
                    }
                  }
                  break;
                }
                default:
                  sb = B.id;
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

  // 3) filling the list
  for (size_t row = 0; row < m_filteredIndices.size(); ++row) {
    int coreIndex = m_filteredIndices[row];
    const auto &core = m_allCores[(size_t)coreIndex];

    wxString idStr = wxString::FromUTF8(core.id.c_str());
    wxString verStr = wxString::FromUTF8(core.latestVersion.c_str());
    wxString maintStr = wxString::FromUTF8(core.maintainer.c_str());

    // boards (only names from the latest release, if it exists)
    wxString boardsStr;
    for (const auto &rel : core.releases) {
      if (rel.version == core.latestVersion) {
        for (size_t bi = 0; bi < rel.boards.size(); ++bi) {
          if (bi)
            boardsStr << wxT(", ");
          boardsStr << wxString::FromUTF8(rel.boards[bi].name.c_str());
        }
        break;
      }
    }

    long item = m_listCtrl->InsertItem((long)row, idStr);
    m_listCtrl->SetItem(item, 1, verStr);
    m_listCtrl->SetItem(item, 2, maintStr);
    m_listCtrl->SetItem(item, 3, boardsStr);

    m_listCtrl->SetItemData(item, (long)coreIndex);

    UpdateStateColors(core, item, colors);
  }

  // auto-size
  for (int col = 0; col < 4; ++col) {
    m_listCtrl->SetColumnWidth(col, wxLIST_AUTOSIZE_USEHEADER);
  }

  m_listCtrl->Thaw();

  UpdateColumnHeaders();
}

void ArduinoCoreManagerFrame::UpdateColumnHeaders() {
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

bool ArduinoCoreManagerFrame::MatchesType(const ArduinoCoreInfo &core,
                                          const wxString &type) const {
  if (type.IsEmpty() || type == _("All"))
    return true;

  bool installed = !core.installedVersion.empty();

  if (type == _("Installed"))
    return installed;

  if (type == _("Updatable")) {
    if (!installed)
      return false;

    if (core.installedVersion.empty() || core.latestVersion.empty())
      return false;

    int cmp = CompareVersions(core.installedVersion, core.latestVersion);
    if (cmp >= 0)
      return false;

    return true;
  }

  return true;
}

bool ArduinoCoreManagerFrame::MatchesSearch(const ArduinoCoreInfo &core,
                                            const wxString &text) const {
  if (text.empty())
    return true;

  std::string needle = ToLower(std::string(text.utf8_str()));
  std::string haystack;

  auto add = [&](const std::string &s) {
    if (!s.empty()) {
      haystack += s;
      haystack += ' ';
    }
  };

  add(core.id);
  add(core.maintainer);
  add(core.email);
  add(core.website);
  add(core.installedVersion);
  add(core.latestVersion);

  for (const auto &rel : core.releases) {
    add(rel.name);
    for (const auto &b : rel.boards) {
      add(b.name);
      add(b.fqbn);
    }
  }

  haystack = ToLower(haystack);
  return haystack.find(needle) != std::string::npos;
}

// ---- Handlers ----

void ArduinoCoreManagerFrame::OnClose(wxCloseEvent &evt) {
  SaveWindowSize(wxT("CoreManager"), this, m_config);

  Hide();
  evt.Veto();
}

void ArduinoCoreManagerFrame::OnTypeChanged(wxCommandEvent &evt) {
  (void)evt;
  ApplyFilter();
}

void ArduinoCoreManagerFrame::OnSearchText(wxCommandEvent &evt) {
  (void)evt;
  m_searchTimer.Start(300, true);
}

void ArduinoCoreManagerFrame::OnSearchTimer(wxTimerEvent &evt) {
  (void)evt;
  ApplyFilter();
}

void ArduinoCoreManagerFrame::OnItemActivated(wxListEvent &evt) {
  long row = evt.GetIndex();
  if (row < 0)
    return;

  long coreIndex = m_listCtrl->GetItemData(row);
  if (coreIndex < 0 || coreIndex >= (long)m_allCores.size())
    return;

  const auto &core = m_allCores[(size_t)coreIndex];

  wxString typeLabel = m_typeChoice
                           ? m_typeChoice->GetStringSelection()
                           : _("All");

  ArduinoCoreDetailDialog dlg(this, core, m_config, typeLabel);
  dlg.ShowModal();
}

void ArduinoCoreManagerFrame::OnColClick(wxListEvent &evt) {
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

void ArduinoCoreManagerFrame::OnListContextMenu(wxContextMenuEvent &WXUNUSED(evt)) {
  if (!m_listCtrl)
    return;

  long item = m_listCtrl->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (item == -1) {
    return;
  }

  long coreIndex = m_listCtrl->GetItemData(item);
  if (coreIndex < 0 || coreIndex >= (long)m_allCores.size())
    return;

  m_contextCoreIndex = (int)coreIndex;
  m_versionMenuMap.clear();

  const auto &core = m_allCores[(size_t)coreIndex];

  wxMenu menu;
  wxMenu *installSub = new wxMenu;

  installSub->Append(ID_CORE_MENU_INSTALL_LATEST, _("Latest"));
  installSub->AppendSeparator();

  int baseId = ID_CORE_MENU_INSTALL_VERSION_BASE;
  for (size_t i = 0; i < core.availableVersions.size(); ++i) {
    int id = baseId + (int)i;
    wxString v = wxString::FromUTF8(core.availableVersions[i].c_str());
    installSub->Append(id, v);
    m_versionMenuMap[id] = core.availableVersions[i];
  }

  menu.AppendSubMenu(installSub, _("Install"));

  bool installed = !core.installedVersion.empty();
  if (installed) {
    menu.AppendSeparator();
    menu.Append(ID_CORE_MENU_UNINSTALL, _("Uninstall"));
  }

  m_listCtrl->PopupMenu(&menu);
}

void ArduinoCoreManagerFrame::OnListInstallLatest(wxCommandEvent &evt) {
  (void)evt;

  if (m_contextCoreIndex < 0 ||
      m_contextCoreIndex >= (int)m_allCores.size())
    return;

  const auto &core = m_allCores[(size_t)m_contextCoreIndex];

  StartInstallCore(core, std::string{});
}

void ArduinoCoreManagerFrame::OnListInstallVersion(wxCommandEvent &evt) {
  int id = evt.GetId();

  if (m_contextCoreIndex < 0 ||
      m_contextCoreIndex >= (int)m_allCores.size())
    return;

  auto it = m_versionMenuMap.find(id);
  if (it == m_versionMenuMap.end())
    return;

  const auto &core = m_allCores[(size_t)m_contextCoreIndex];
  const std::string &ver = it->second;

  StartInstallCore(core, ver);
}

void ArduinoCoreManagerFrame::OnListUninstall(wxCommandEvent &evt) {
  (void)evt;

  if (m_contextCoreIndex < 0 ||
      m_contextCoreIndex >= (int)m_allCores.size())
    return;

  const auto &core = m_allCores[(size_t)m_contextCoreIndex];

  if (core.installedVersion.empty())
    return;

  StartUninstallCore(core);
}

void ArduinoCoreManagerFrame::OnBottomUpdateIndex(wxCommandEvent &evt) {
  (void)evt;

  ArduinoEditorFrame *frame = wxDynamicCast(GetParent(), ArduinoEditorFrame);
  if (!frame)
    return;

  if (frame->CanPerformAction(coreupdateindex, true)) {
    m_cli->UpdateCoreIndexAsync(frame);
  }
}

void ArduinoCoreManagerFrame::RefreshCores() {
  if (!m_cli) {
    return;
  }

  m_allCores = m_cli->GetCores();
  if (m_allCores.empty()) {
    return;
  }

  ApplyFilter();
}

void ArduinoCoreManagerFrame::UpdateStateColors(const ArduinoCoreInfo &core, long item, const EditorColorScheme &colors) {
  // colors: Installed / Updatable
  bool installed = !core.installedVersion.empty();
  bool isUpdatable = false;

  if (installed &&
      !core.installedVersion.empty() &&
      !core.latestVersion.empty()) {
    if (CompareVersions(core.installedVersion, core.latestVersion) < 0) {
      isUpdatable = true;
    }
  }

  if (isUpdatable) {
    m_listCtrl->SetItemBackgroundColour(item, colors.updatable);
  } else if (installed) {
    m_listCtrl->SetItemBackgroundColour(item, colors.installed);
  }
}

void ArduinoCoreManagerFrame::ApplySettings(const EditorSettings &settings) {
  EditorColorScheme colors = settings.GetColors();

  long itemIndex = -1;

  while ((itemIndex = m_listCtrl->GetNextItem(itemIndex, wxLIST_NEXT_ALL, wxLIST_STATE_DONTCARE)) != wxNOT_FOUND) {
    long coreIndex = m_listCtrl->GetItemData(itemIndex);
    if (coreIndex < 0 || coreIndex >= (long)m_allCores.size())
      continue;

    const auto &core = m_allCores[(size_t)coreIndex];

    UpdateStateColors(core, itemIndex, colors);
  }
}

void ArduinoCoreManagerFrame::StartInstallCore(const ArduinoCoreInfo &core,
                                               const std::string &version) {
  if (!m_cli)
    return;

  ArduinoEditorFrame *frame = wxDynamicCast(GetParent(), ArduinoEditorFrame);
  if (!frame)
    return;

  std::string id = core.id;
  if (!version.empty()) {
    id += "@";
    id += version; // arduino-cli core install arduino:samd@1.8.6
  }

  std::vector<std::string> ids;
  ids.push_back(id);

  if (frame->CanPerformAction(coreinstall, true)) {
    m_cli->InstallCoresAsync(ids, frame);
  }
}

void ArduinoCoreManagerFrame::StartUninstallCore(const ArduinoCoreInfo &core) {
  if (!m_cli)
    return;

  wxString msg;
  msg << _("Do you really want to uninstall core:\n");
  msg << wxString::FromUTF8(core.id.c_str());
  if (!core.installedVersion.empty()) {
    msg << wxT(" (") << _("installed ") << wxString::FromUTF8(core.installedVersion.c_str()) << wxT(")");
  }
  msg << wxT("?");

  wxRichMessageDialog dlg(this, msg, _("Uninstall core"),
                          wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
  if (dlg.ShowModal() != wxID_YES)
    return;

  ArduinoEditorFrame *frame = wxDynamicCast(GetParent(), ArduinoEditorFrame);
  if (!frame)
    return;

  std::vector<std::string> ids;
  ids.push_back(core.id);

  if (frame->CanPerformAction(coreremove, true)) {
    m_cli->UninstallCoresAsync(ids, frame);
  }
}
