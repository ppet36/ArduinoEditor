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

#pragma once

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/config.h>
#include <wx/frame.h>
#include <wx/listctrl.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ard_cli.hpp"

static constexpr int ID_PROCESS_INSTALL_CORE = wxID_HIGHEST + 3100;

class ArduinoEditorFrame;
struct EditorSettings;
struct EditorColorScheme;

class ArduinoCoreManagerFrame : public wxFrame {
public:
  ArduinoCoreManagerFrame(wxWindow *parent,
                          ArduinoCli *cli,
                          wxConfigBase *config,
                          const wxString &initialType);
  ~ArduinoCoreManagerFrame();

  void RefreshCores();

  void ApplySettings(const EditorSettings &settings);

  void StartInstallCore(const ArduinoCoreInfo &core, const std::string &version);
  void StartUninstallCore(const ArduinoCoreInfo &core);

private:
  void BuildUi();
  void InitData();
  void ApplyFilter();
  void UpdateColumnHeaders();
  void UpdateStateColors(const ArduinoCoreInfo &core, long item, const EditorColorScheme &colors);

  bool MatchesType(const ArduinoCoreInfo &core, const wxString &type) const;
  bool MatchesSearch(const ArduinoCoreInfo &core, const wxString &text) const;

  // ---- event handlers ----
  void OnClose(wxCloseEvent &evt);
  void OnTypeChanged(wxCommandEvent &evt);
  void OnSearchText(wxCommandEvent &evt);
  void OnSearchTimer(wxTimerEvent &evt);
  void OnItemActivated(wxListEvent &evt);
  void OnColClick(wxListEvent &evt);
  void OnListContextMenu(wxContextMenuEvent &evt);
  void OnListInstallLatest(wxCommandEvent &evt);
  void OnListInstallVersion(wxCommandEvent &evt);
  void OnListUninstall(wxCommandEvent &evt);
  void OnBottomUpdateIndex(wxCommandEvent &evt);

  ArduinoCli *m_cli = nullptr;
  wxConfigBase *m_config = nullptr;

  wxChoice *m_typeChoice = nullptr;
  wxTextCtrl *m_searchCtrl = nullptr;
  wxListCtrl *m_listCtrl = nullptr;
  wxButton *m_bottomUpdateBtn = nullptr;
  wxButton *m_bottomCloseBtn = nullptr;

  wxTimer m_searchTimer;

  std::vector<ArduinoCoreInfo> m_allCores;

  int m_sortColumn = 0;
  bool m_sortAscending = true;
  wxString m_colLabels[4];

  std::vector<int> m_filteredIndices;
  int m_contextCoreIndex = -1;
  std::map<int, std::string> m_versionMenuMap;
};
