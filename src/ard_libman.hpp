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

#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/config.h>
#include <wx/frame.h>
#include <wx/listctrl.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

class ArduinoCli;
struct ArduinoLibraryInfo;
struct ArduinoCoreBoard;
struct ArduinoLibraryInstallSpec;
struct EditorSettings;
struct EditorColorScheme;

class ArduinoLibraryManagerFrame : public wxFrame {
public:
  ArduinoLibraryManagerFrame(wxWindow *parent, ArduinoCli *cli, const std::vector<ArduinoCoreBoard> &availableBoards, wxConfigBase *config, const wxString &initialType);
  ~ArduinoLibraryManagerFrame() override;

  void RefreshLibraries();
  void RefreshInstalledLibraries();
  void RefreshAvailableBoards(const std::vector<ArduinoCoreBoard> &boards);

  void ShowLibraries(const std::vector<ArduinoLibraryInfo> &libs);

  void ApplySettings(const EditorSettings &settings);

  void StartInstallRepo(const ArduinoLibraryInfo &lib, const std::string &version);
  void StartUninstallRepo(const ArduinoLibraryInfo &lib);

  // Bulk installation of libraries (Repo) with dependency resolution.
  // - libs: list of libraries from the directory (m_allLibraries) that we want to install
  // - always the latest version is taken (lib.latest.version)
  void InstallLibrariesWithDeps(const std::vector<ArduinoLibraryInfo> &libs);

private:
  ArduinoCli *m_cli;
  wxConfigBase *m_config = nullptr;

  int m_sortColumn = 0;
  bool m_sortAscending = true;

  bool m_refreshOnShow = false;

  wxString m_colLabels[4];

  int m_contextLibIndex = -1;
  std::map<int, std::string> m_versionMenuMap;
  std::vector<ArduinoLibraryInfo> m_explicitShowLibs;

  wxChoice *m_topicChoice = nullptr;
  wxChoice *m_typeChoice = nullptr;
  wxTextCtrl *m_searchCtrl = nullptr;
  wxListCtrl *m_listCtrl = nullptr;
  wxButton *m_bottomInstallBtn = nullptr;
  wxButton *m_bottomCloseBtn = nullptr;
  wxTimer m_searchTimer;

  std::vector<ArduinoLibraryInfo> m_allLibraries;
  std::vector<ArduinoLibraryInfo> m_installedLibraries;
  std::set<std::string> m_supportedArchitectures;
  std::vector<int> m_filteredIndices; // mapping leaf index -> m_allLibraries

  void BuildUi();
  void InitData();
  void RebuildTopicChoices();
  void ApplyFilter();
  void UpdateColumnHeaders();
  void UpdateStateColors(const ArduinoLibraryInfo &lib, long item, const EditorColorScheme &colors);

  void StartInstallSpecs(const std::vector<ArduinoLibraryInstallSpec> &specs);

  bool MatchesArchitecture(const ArduinoLibraryInfo &lib) const;
  bool MatchesTopic(const ArduinoLibraryInfo &lib, const wxString &topic) const;
  bool MatchesType(const ArduinoLibraryInfo &lib, const wxString &type) const;
  bool MatchesSearch(const ArduinoLibraryInfo &lib, const wxString &text) const;
  bool MatchesExplicitLib(const ArduinoLibraryInfo &lib) const;

  void OnClose(wxCloseEvent &evt);
  void OnShow(wxShowEvent &evt);
  void OnTopicChanged(wxCommandEvent &evt);
  void OnTypeChanged(wxCommandEvent &evt);
  void OnSearchText(wxCommandEvent &evt);
  void OnSearchTimer(wxTimerEvent &evt);
  void OnItemActivated(wxListEvent &evt);
  void OnColClick(wxListEvent &evt);
  void OnPollTimer(wxTimerEvent &evt);
  void OnListContextMenu(wxContextMenuEvent &evt);
  void OnListInstallLatest(wxCommandEvent &evt);
  void OnListInstallVersion(wxCommandEvent &evt);
  void OnListUninstall(wxCommandEvent &evt);
  void OnBottomInstallButton(wxCommandEvent &evt);
  void OnBottomInstallSourceGit(wxCommandEvent &evt);
  void OnBottomInstallSourceZip(wxCommandEvent &evt);
  void OnUpdateLibraryIndex(wxCommandEvent &evt);
};
