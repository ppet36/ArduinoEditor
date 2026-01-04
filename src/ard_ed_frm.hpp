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

#include "ard_ai.hpp"
#include "ard_aipnl.hpp"
#include "ard_boptdlg.hpp"
#include "ard_cli.hpp"
#include "ard_coreman.hpp"
#include "ard_diagview.hpp"
#include "ard_edit.hpp"
#include "ard_examples.hpp"
#include "ard_finsymdlg.hpp"
#include "ard_indic.hpp"
#include "ard_libman.hpp"
#include "ard_sermon.hpp"
#include "ard_setdlg.hpp"
#include <string>
#include <vector>
#include <wx/aui/auibook.h>
#include <wx/aui/framemanager.h>
#include <wx/fileconf.h>
#include <wx/notebook.h>
#include <wx/stc/stc.h>
#include <wx/treectrl.h>
#include <wx/wx.h>

enum CurrentAction {
  none,
  build,
  upload,
  libinstall,
  libremove,
  libupdateindex,
  coreinstall,
  coreremove,
  coreupdateindex,
  openserialmon
};

class SketchFilesPanel;
class ArduinoCliDiagnosticsDialog;

class ArduinoEditorFrame : public wxFrame {
private:
  static constexpr int MAX_RECENT_SKETCHES = 10;

  struct CandidateGroup {
    std::string header;
    std::vector<ArduinoLibraryInfo> libs;
  };

  wxConfigBase *config;
  ArduinoCli *arduinoCli = nullptr;
  ArduinoCodeCompletion *completion = nullptr;
  bool m_firstInitCompleted = false;

  ArduinoAiActions m_aiGlobalActions{/*editor=*/nullptr};
  void UpdateAiGlobalEditor();

  ClangSettings m_clangSettings;

  AiSettings m_aiSettings;

  bool m_cleanTried = false;

  std::vector<ArduinoCoreBoard> m_availableBoards;
  std::vector<SerialPortInfo> m_serialPorts;

  wxAuiManager m_auiManager;

  wxChoice *m_boardChoice = nullptr;
  int m_lastBoardSelection = -1;
  wxBitmapButton *m_optionsButton = nullptr;
  wxChoice *m_portChoice = nullptr;
  wxStatusBar *m_statusBar = nullptr;
  ArduinoActivityDotCtrl *m_indic = nullptr;
  wxAuiNotebook *m_notebook = nullptr;
  wxImageList *m_tabImageList = nullptr;
  wxMenuBar *m_menuBar = nullptr;

  wxNotebook *m_bottomNotebook = nullptr;
  int m_lastBottomNotebookSelectedPage = wxNOT_FOUND;

  ArduinoDiagnosticsView *m_diagView = nullptr;
  wxTextCtrl *m_buildOutputCtrl = nullptr;
  wxBitmapButton *m_refreshPortsButton = nullptr;
  wxMenu *m_sketchesDirMenu = nullptr;
  wxBitmapButton *m_buildButton = nullptr;
  wxBitmapButton *m_uploadButton = nullptr;
  wxBitmapButton *m_serialMonitorButton = nullptr;
  SketchFilesPanel *m_filesPanel = nullptr;
  ArduinoAiChatPanel *m_aiPanel = nullptr;

  // Kill async cli stream
  void OnCliProcessKill(wxCommandEvent &);

  std::vector<wxString> m_sketchesPaths;
  std::vector<wxString> m_recentSketches;
  wxMenu *m_recentMenu = nullptr;

  wxTimer m_diagTimer;

  wxTimer m_returnBottomPageTimer;
  void OnReturnBottomPageTimer(wxTimerEvent &);

  uint64_t m_lastSuccessfulCompileCodeSum = 0;
  bool m_runUploadAfterCompile = false;

  CurrentAction m_action = none;

  std::vector<ArduinoEditor *> GetAllEditors(bool onlyEditable = false);

  // Navigations stacks
  std::vector<JumpTarget> m_navBackStack;
  std::vector<JumpTarget> m_navForwardStack;

  // history of FQBN
  std::vector<std::string> m_boardHistory;

  // symbol search
  FindSymbolDialog *m_findSymbolDlg = nullptr;
  void OnSymbolActivated(ArduinoSymbolActivatedEvent &evt);

  ArduinoLibraryManagerFrame *m_libManager = nullptr;
  ArduinoExamplesFrame *m_examplesFrame = nullptr;
  ArduinoCoreManagerFrame *m_coreManager = nullptr;
  ArduinoSerialMonitorFrame *m_serialMonitor = nullptr;
  ArduinoCliDiagnosticsDialog *m_cliDiagDialog = nullptr;

  int ModalMsgDialog(const wxString &message, const wxString &caption = _("Error"), int styles = wxOK | wxICON_ERROR);
  static bool IsSupportedExtension(const wxString &ext);

  void InitCli(const std::string &path);

  void UpdateStatus(const wxString &msg);

  void OnStatusBarSize(wxSizeEvent &evt);
  void LayoutStatusBarIndicator();

  void OnDiagJumpFromView(ArduinoDiagnosticsActionEvent &ev);
  void OnDiagSolveAiFromView(ArduinoDiagnosticsActionEvent &ev);

  void ApplySettings(const EditorSettings &settings);
  void ApplySettings(const ClangSettings &settings);

public:
  // This is called also from ArduinoAiChatPanel for model switch
  void ApplySettings(const AiSettings &settings);

private:
  void UpdateBoard(std::string &fqbn);
  void SelectBoard();

  void OnEditorSettings(wxCommandEvent &);
  void OnChangeBoardOptions(wxCommandEvent &);
  void OnClangArgsReady(wxThreadEvent &event);

  // wxAuiNotebook
  void OnNotebookPageChanged(wxBookCtrlEvent &event);
  void OnNotebookPageChanging(wxBookCtrlEvent &event);
  void OnNotebookPageClose(wxAuiNotebookEvent &e);
  bool ConfirmAndCloseTab(int idx);

  int m_tabPopupIndex = wxNOT_FOUND;
  void OnNotebookTabRightUp(wxAuiNotebookEvent &e);

  void OnTabMenuClose(wxCommandEvent &);
  void OnTabMenuCloseOthers(wxCommandEvent &);
  void OnTabMenuCloseAll(wxCommandEvent &);

  // Sketch management
  void DeleteSketchFile(const wxString &filename);

  void OnNewSketch(wxCommandEvent &evt);
  void OnOpenSketch(wxCommandEvent &evt);
  void OnSave(wxCommandEvent &evt);
  bool SaveAll();
  void OnSaveAll(wxCommandEvent &evt);
  void OnExit(wxCommandEvent &evt);
  void OnColors(wxCommandEvent &evt);

  // Navigation
  void OnNavBack(wxCommandEvent &event);
  void OnNavForward(wxCommandEvent &event);
  void OnFindSymbol(wxCommandEvent &event);

  // Project menu
  void OnProjectClean(wxCommandEvent &event);
  void CleanProjectIfFqbnChanged(const std::string &newFqbn);
  bool BuildProject();
  void OnProjectBuild(wxCommandEvent &event);
  bool UploadProject();
  void OnProjectUpload(wxCommandEvent &event);
  void OnCmdLineOutput(wxCommandEvent &event);

  void OnAbout(wxCommandEvent &event);
  void OnClose(wxCloseEvent &event);

  void ShowSingleDiagMessage(const wxString &message);
  void OnDiagnosticsUpdated(wxThreadEvent &evt);
  void OnDiagTimer(wxTimerEvent &event);
  void RefreshDiagnostics();
  void OnDiagItemActivated(wxListEvent &event);
  void OnDiagContextMenu(wxContextMenuEvent &evt);
  void OnDiagCopySelected(wxCommandEvent &evt);
  void OnDiagCopyAll(wxCommandEvent &evt);
  void OnDiagSolveAi(wxCommandEvent &evt);
  wxString GetDiagRowText(long row) const;

  void OnResolvedLibrariesReady(wxThreadEvent &event);
  bool m_librariesUpdated = false;
  void OnLibrariesUpdated(wxThreadEvent &evt);
  void OnInstalledLibrariesUpdated(wxThreadEvent &evt);
  void OnShowLibraryManager(wxCommandEvent &evt);

  void OnCoresLoaded(wxThreadEvent &evt);
  void OnInstalledCoresUpdated(wxThreadEvent &evt);
  void OnShowCoreManager(wxCommandEvent &evt);
  void OnAvailableBoardsUpdated(wxThreadEvent &event);

  void OnOpenRecent(wxCommandEvent &event);
  void OnClearRecent(wxCommandEvent &event);

  wxMenuBar *CreateMenuBar();
  void InitComponents();
  void BindEvents();
  void LoadConfig();

  void OnSerialPortChanged(wxCommandEvent &event);
  void OnRefreshSerialPorts(wxCommandEvent &event);
  void RefreshSerialPorts();
  void UpdateSerialMonitorAvailability();

  // Output
  void ShowOutputPane(bool show);
  void OnToggleOutput(wxCommandEvent &event);
  void OnViewOutput(wxCommandEvent &evt);
  void OnAuiPaneClose(wxAuiManagerEvent &evt);
  void UpdateOutputTabsFromMenu();
  void RestoreWindowPlacement();
  void OnOpenSerialMonitor(wxCommandEvent &);

  // Ai
  void ShowAiPane(bool show);
  void OnViewAi(wxCommandEvent &evt);

  // Files tree
  void UpdateFilesTreeSelectedFromNotebook();
  void ShowFilesPane(bool show);
  void OnViewFiles(wxCommandEvent &);
  void OnSketchTreeOpenItem(wxCommandEvent &evt);
  void OnSketchTreeNewFile(wxCommandEvent &evt);
  void OnSketchTreeNewFolder(wxCommandEvent &evt);
  void OnSketchTreeDelete(wxCommandEvent &evt);
  void OnSketchTreeRename(wxCommandEvent &evt);
  void OnSketchTreeOpenExternally(wxCommandEvent &evt);

  // Search of missing libraries
  static std::string ExtractMissingHeaderFromMessage(const std::string &msg);
  void MaybeOfferToInstallLibrariesForHeaders(const std::vector<std::string> &headers);
  void RequestShowLibraries(const std::vector<ArduinoLibraryInfo> &libs);
  void CheckMissingHeadersInDiagnostics(const std::vector<ArduinoParseError> &errors);
  std::vector<std::string> m_queriedMissingHeaders;
  std::vector<std::string> m_notFoundHeaders;
  std::unordered_set<std::string> m_wantedHeaders;
  std::vector<CandidateGroup> m_foundLibraryGroups;
  void OnLibrariesFound(wxThreadEvent &evt);

  // Recent sketches management
  void LoadRecentSketches();
  void SaveRecentSketches();
  void AddRecentSketch(const wxString &path);
  void RebuildRecentMenu();

  void OnOpenSketchFromDir(wxCommandEvent &event);
  void RebuildSketchesDirMenu();

  void OnShowExamples(wxCommandEvent &evt);

  void OnSysColoursChanged(wxSysColourChangedEvent &evt);

  void LoadBoardHistory();
  void RebuildBoardChoice();
  wxString MakeBoardLabel(const std::string &fqbn) const;

  void OnBoardChoiceChanged(wxCommandEvent &event);

  void ResolveLibrariesOrDiagnostics();

  void OnCheckForUpdates(wxCommandEvent &);

#ifdef __WXMSW__
  WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam);
#endif

public:
  ArduinoEditorFrame(wxConfigBase *cfg);
  ~ArduinoEditorFrame();

  void UpdateSketchesDir(const wxString &sketchDir);

  bool IsProjectFile(const std::string &filePath);

  // Progress inication
  void StartProcess(const wxString &name, int id, ArduinoActivityState state, bool canBeTerminated = false);
  void StopProcess(int id);

  // Rebuild project
  void CleanProject();
  void RebuildProject(bool withClean = false);
  void ScheduleDiagRefresh();

  // Navigation
  void PushNavLocation(const std::string &file, int line, int column);
  void GoBackInNavigation();
  void GoForwardInNavigation();
  bool GetDiagnosticsAt(const std::string &filename, unsigned row, unsigned column, std::vector<ArduinoParseError> &outDiagnostics);

  // Refactoring
  void CollectEditorSources(std::vector<SketchFileBuffer> &files);
  void RefactorRenameSymbol(ArduinoEditor *originEditor, int line, int column, const wxString &oldName);
  void CreateNewSketchFile(const wxString &filename);

  // Editors manipulation
  void ActivateEditor(ArduinoEditor *editor, int line, int column);
  ArduinoEditor *FindEditorWithFile(const std::string &filename, bool allowCreate = true);
  ArduinoEditor *CreateEditorForFile(const std::string &filename, int lineToGo, int columnToGo);
  void HandleGoToLocation(const JumpTarget &tgt);
  ArduinoEditor *GetCurrentEditor();

  // Shorts to path manipulation
  std::string NormalizeFilename(const std::string &filename) const;
  std::string RelativizeFilename(const std::string &filename) const;

  void OnCloseSerialMonitor();

  // Action management
  wxString GetCurrentActionName() const;
  bool CanPerformAction(CurrentAction newAction, bool activate = false);
  void SetCurrentAction(CurrentAction action);
  void FinalizeCurrentAction(bool successful);
  void EnableUIActions(bool enable = true);

  // Sketch operations
  bool NewSketch(bool inNewWindow);
  bool OpenSketchDialog(bool inNewWindow);
  void OpenSketch(const std::string &path);
  void SaveWorkspaceState();
  wxFileConfig *OpenWorkspaceConfig();

  // "Modified" indicator
  void UpdateEditorTabIcon(ArduinoEditor *ed, bool modified, bool readOnly);
};
