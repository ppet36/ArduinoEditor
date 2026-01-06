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

#include "ard_ed_frm.hpp"
#include "ai_client.hpp"
#include "ard_aboutdlg.hpp"
#include "ard_ap.hpp"
#include "ard_borsel.hpp"
#include "ard_clidiag.hpp"
#include "ard_cliinst.hpp"
#include "ard_cliparse.hpp"
#include "ard_ev.hpp"
#include "ard_filestree.hpp"
#include "ard_finsymdlg.hpp"
#include "ard_initbrdsel.hpp"
#include "ard_renamedlg.hpp"
#include "ard_setdlg.hpp"
#include "ard_update.hpp"
#include "main.hpp"
#include "nsketch.hpp"
#include "utils.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unordered_set>
#include <wx/artprov.h>
#include <wx/clipbrd.h>
#include <wx/dir.h>
#include <wx/dirdlg.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/html/htmlwin.h>
#include <wx/richmsgdlg.h>
#include <wx/textdlg.h>
#include <wx/tokenzr.h>
#include <wx/tooltip.h>

#ifndef __WXMSW__
#include <sys/stat.h> // chmod
#endif

extern ArduinoArtProvider *g_artProvider;

namespace fs = std::filesystem;

enum {
  ID_MENU_OPEN_SKETCH = wxID_HIGHEST + 1,
  ID_MENU_NEW_SKETCH,
  ID_MENU_SAVE,
  ID_MENU_SAVE_ALL,
  ID_MENU_NAV_BACK,
  ID_MENU_NAV_FORWARD,
  ID_MENU_NAV_FIND_SYMBOL,
  ID_MENU_PROJECT_CLEAN,
  ID_MENU_PROJECT_BUILD,
  ID_MENU_PROJECT_UPLOAD,
  ID_MENU_VIEW_OUTPUT,
  ID_MENU_VIEW_SKETCHES,
  ID_MENU_VIEW_SYMBOLS,
  ID_MENU_VIEW_AI,
  ID_MENU_LIBRARY_MANAGER,
  ID_MENU_CORE_MANAGER,
  ID_MENU_SERIAL_MONITOR,
  ID_MENU_SKETCH_EXAMPLES,
  ID_MENU_CHECK_FOR_UPDATES,
  ID_MENU_LIBRARY_MANAGER_UPDATES,
  ID_MENU_CORE_MANAGER_UPDATES,
  ID_TIMER_DIAGNOSTIC,
  ID_TIMER_BOTTOM_PAGE_RETURN,
  ID_TIMER_UPDATES_AVAILABLE,
  ID_TABMENU_CLOSE,
  ID_TABMENU_CLOSE_OTHERS,
  ID_TABMENU_CLOSE_ALL,

  ID_PROCESS_LOAD_LIBRARIES,
  ID_PROCESS_SEARCH_LIBRARIES,
  ID_PROCESS_LOAD_INSTALLED_LIBRARIES,
  ID_PROCESS_LOAD_CORES,
  ID_PROCESS_LOAD_INSTALLED_CORES,
  ID_PROCESS_LOAD_BOARDS,
  ID_PROCESS_LOAD_BOARD_PARAMS,
  ID_PROCESS_LOAD_COMPILE_PARAMS,
  ID_PROCESS_RESOLVE_LIBRARIES,
  ID_PROCESS_CLI,
  ID_PROCESS_APP_INIT,
  ID_PROCESS_ACTION,
  ID_PROCESS_DIAG_EVAL,

  ID_MENU_OPEN_RECENT_CLEAR,
  ID_MENU_OPEN_RECENT_FIRST,
  ID_MENU_OPEN_RECENT_LAST = ID_MENU_OPEN_RECENT_FIRST + 9,

  ID_MENU_SKDIR_FIRST,
  ID_MENU_SKDIR_LAST = ID_MENU_SKDIR_FIRST + 99
};

std::vector<ArduinoEditor *> ArduinoEditorFrame::GetAllEditors(bool onlyEditable) {
  std::vector<ArduinoEditor *> result;
  if (!m_notebook) {
    return result;
  }

  for (int i = 0, n = (int)m_notebook->GetPageCount(); i < n; i++) {
    auto *ed = dynamic_cast<ArduinoEditor *>(m_notebook->GetPage(i));
    if (ed) {
      if (onlyEditable && ed->IsReadOnly()) {
        continue;
      }
      result.push_back(ed);
    }
  }

  return result;
}

void ArduinoEditorFrame::ApplySettings(const EditorSettings &settings) {
  for (auto *ed : GetAllEditors()) {
    ed->ApplySettings(settings);
  }

  m_buildOutputCtrl->SetFont(settings.GetFont());

  if (m_libManager) {
    m_libManager->ApplySettings(settings);
  }

  if (m_coreManager) {
    m_coreManager->ApplySettings(settings);
  }

  if (m_indic) {
    m_indic->ApplySettings(settings);
  }

  if (m_diagView) {
    m_diagView->ApplySettings(settings);
  }
}

void ArduinoEditorFrame::ApplySettings(const ClangSettings &settings) {
  for (auto *ed : GetAllEditors()) {
    ed->ApplySettings(settings);
  }

  if (completion) {
    completion->ApplySettings(settings);
  }
}

void ArduinoEditorFrame::ApplySettings(const AiSettings &settings) {
  m_aiSettings = settings;

  for (auto *ed : GetAllEditors()) {
    ed->ApplySettings(settings);
  }

  if (m_diagView) {
    m_diagView->ApplySettings(settings);
  }

  if (m_aiPanel) {
    m_aiPanel->ApplySettings(settings);
  }

  APP_DEBUG_LOG("FRM: AiSettings, fullInfoRequest=%d, floatingWindow=%d", settings.fullInfoRequest, settings.floatingWindow);

  m_aiGlobalActions.SetFullInfoRequest(settings.fullInfoRequest);
  m_aiGlobalActions.SetFloatingWindow(settings.floatingWindow);

  wxAuiPaneInfo &pane = m_auiManager.GetPane(wxT("ai"));
  if (pane.IsOk()) {
    if (!settings.enabled) {
      // AI disabled -> hide the panel
      pane.Show(false);
      if (m_menuBar) {
        m_menuBar->Check(ID_MENU_VIEW_AI, false);
      }
    }
    // if enabled, we leave it up to the user whether the panel is visible (View menu / saved layout)
  }

  if (m_menuBar) {
    m_menuBar->Enable(ID_MENU_VIEW_AI, settings.enabled);
    if (!settings.enabled) {
      m_menuBar->Check(ID_MENU_VIEW_AI, false);
    }
  }

  m_auiManager.Update();
}

void ArduinoEditorFrame::OnEditorSettings(wxCommandEvent &) {
  EditorSettings editorSettings;
  editorSettings.Load(config);

  ArduinoCliConfig cliCfg = arduinoCli->GetConfig();

  ArduinoEditorSettingsDialog dlg(this, editorSettings, cliCfg, m_clangSettings, m_aiSettings, config, arduinoCli);
  if (dlg.ShowModal() == wxID_OK) {
    EditorSettings newEditor = dlg.GetSettings();
    newEditor.Save(config);
    ApplySettings(newEditor);

    ArduinoCliConfig newCli = dlg.GetCliConfig();
    bool brdChanged = cliCfg.boardManagerAdditionalUrls != newCli.boardManagerAdditionalUrls;
    arduinoCli->ApplyConfig(newCli);

    if (brdChanged && CanPerformAction(coreupdateindex, true)) {
      arduinoCli->UpdateCoreIndexAsync(this);
    }

    auto newClangSettings = dlg.GetClangSettings();
    bool diagModeChanged = (newClangSettings.diagnosticMode != m_clangSettings.diagnosticMode);
    bool resolveModeChanged = (newClangSettings.resolveMode != m_clangSettings.resolveMode) || (newClangSettings.displayDiagnosticsOnlyFromSketch != m_clangSettings.displayDiagnosticsOnlyFromSketch);
    bool warningModeChanged = (newClangSettings.warningMode != m_clangSettings.warningMode) || (newClangSettings.customWarningFlags != m_clangSettings.customWarningFlags);
    m_clangSettings = newClangSettings;
    m_clangSettings.Save(config);
    ApplySettings(m_clangSettings);

    AiSettings newAiSettings = dlg.GetAiSettings();
    newAiSettings.Save(config);
    m_aiSettings = newAiSettings;
    ApplySettings(m_aiSettings);

    config->Write(wxT("Language"), dlg.GetSelectedLanguage());
    config->Write(wxT("Updates/check_interval_hours"), dlg.GetUpdateCheckIntervalHours());
    config->Write(wxT("Updates/libraries_check_interval_hours"), dlg.GetLibrariesUpdateCheckIntervalHours());
    config->Write(wxT("Updates/boards_check_interval_hours"), dlg.GetBoardsUpdateCheckIntervalHours());

    wxString oldSketchesDir;
    config->Read(wxT("SketchesDir"), &oldSketchesDir);

    wxString newSketchesDir = dlg.GetSketchesDir();
    if (!newSketchesDir.IsEmpty() && (oldSketchesDir != newSketchesDir)) {
      config->Write(wxT("SketchesDir"), newSketchesDir);
      config->Flush();

      RebuildSketchesDirMenu();
    }

    wxString oldCliPath;
    config->Read(wxT("ArduinoCliPath"), &oldCliPath);

    wxString newCliPath = dlg.GetCliPath();
    APP_DEBUG_LOG("FRM: oldCliPath=%s, newCliPath=%s", oldCliPath.ToUTF8().data(), newCliPath.ToUTF8().data());
    if (oldCliPath != newCliPath) {
      if (ArduinoCliInstaller::CheckArduinoCli(wxToStd(newCliPath))) {
        config->Write(wxT("ArduinoCliPath"), newCliPath);
        config->Flush();
      } else {
        ModalMsgDialog(
            _("The specified arduino-cli is invalid. It may be missing or outdated."),
            _("Warning"),
            wxOK | wxICON_WARNING);
      }
    }

    if (warningModeChanged || resolveModeChanged) {
      CleanProject();
    } else if (diagModeChanged && !resolveModeChanged) {
      ScheduleDiagRefresh();
    }
  }
}

void ArduinoEditorFrame::UpdateAiGlobalEditor() {
  auto *edit = GetCurrentEditor();
  if (edit) {
    APP_DEBUG_LOG("FRM: set AI editor to %s", edit->GetFilePath().c_str());
    m_aiGlobalActions.SetCurrentEditor(edit);
  }
}

void ArduinoEditorFrame::UpdateClassBrowserEditor() {
  // Update class browser symbols
  if (m_classBrowser && completion) {
    ArduinoEditor *ed = GetCurrentEditor();
    if (!ed) {
      m_classBrowser->Clear();
    } else {
      m_classBrowser->SetCompletion(completion);
      m_classBrowser->SetCurrentEditor(ed);
    }
  }
}

void ArduinoEditorFrame::UpdateClassBrowserEditorLine(int line) {
  if (m_classBrowser) {
    m_classBrowser->SetCurrentLine(line);
  }
}

void ArduinoEditorFrame::OpenSketch(const std::string &skp) {
  ArduinoEditApp &app = wxGetApp();
  app.SetSplashMessage(_("Initializing..."));

  // Convert std::string to wxString (assuming UTF-8)
  wxString rawPath = wxString::FromUTF8(skp.c_str());

  // Normalization:
  // - accept both the directory and any .ino file
  // - remove trailing slashes
  // - make the path absolute
  wxFileName nfn(rawPath);

  if (nfn.DirExists()) {
    // path is a directory -> we will take it as dir
    nfn.AssignDir(rawPath);
  } else if (nfn.FileExists()) {
    // path is a file (typically .ino from cmdline) -> we take its directory
    nfn.Normalize(wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS | wxPATH_NORM_TILDE); // make it an absolute path etc.
  } else {
    // nothing exists - but we still want to think of it as a directory
    nfn.AssignDir(rawPath);
  }

  // Lexical normalization and absolute path provisioning
  nfn.Normalize(wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS | wxPATH_NORM_TILDE);

  // *** This is our only "canonical" shape of the path to the sketch ***
  wxString dirPath = nfn.GetPath();
  std::string normPath = wxToStd(dirPath);

  APP_DEBUG_LOG("FRM: dirPath=\"%s\"", normPath.c_str());

  if (!wxDirExists(dirPath)) {
    ModalMsgDialog(wxString::Format(_("Directory '%s' does not exist."), dirPath));
    return;
  }

  UpdateSketchesDir(dirPath);

  // ---------------------------------------------------------------------------
  // Find (or create) the primary .ino file in the sketch root
  // ---------------------------------------------------------------------------
  wxString inoFileName; // e.g. mysketch.ino
  wxString inoFullPath; // full path to the .ino

  wxDir dir(dirPath);
  if (!dir.IsOpened()) {
    wxLogError(_("Directory '%s' could not be opened."), dirPath);
    return;
  }

  wxString filename;
  bool cont = dir.GetFirst(&filename, wxEmptyString, wxDIR_FILES);

  // If there are multiple .ino files, we pick the lexicographically first one
  // (case-insensitive) to keep behavior deterministic.
  while (cont) {
    wxFileName fn(dirPath, filename);
    if (fn.GetExt().Lower() == wxT("ino")) {
      if (inoFileName.empty() || fn.GetFullName().CmpNoCase(inoFileName) < 0) {
        inoFileName = fn.GetFullName();
        inoFullPath = fn.GetFullPath();
      }
    }
    cont = dir.GetNext(&filename);
  }

  app.SetSplashMessage(_("Opening sketch..."));

  std::string forcedFqbn;
  bool newContentCreated = false;

  // If no .ino file exists, create <folder_name>.ino
  if (inoFileName.empty()) {
    wxFileName dirName;
    dirName.AssignDir(dirPath); // set as directory

    wxString baseName;
    const wxArrayString &dirs = dirName.GetDirs();
    if (!dirs.IsEmpty()) {
      // last element of the path
      baseName = dirs.Last();
    }

    if (baseName.empty()) {
      // last resort - in case it fails for some reason
      baseName = wxT("sketch");
    }

    inoFileName = baseName + wxT(".ino");
    wxFileName inoFn(dirPath, inoFileName);
    inoFullPath = inoFn.GetFullPath();

    // Create a file with the Arduino skeleton
    wxFFile f(inoFullPath, wxT("w"));
    if (f.IsOpened()) {
      wxString tpl = wxT(
          "\n"
          "void setup() {\n\n"
          "}\n"
          "\n"
          "void loop() {\n\n"
          "}\n");

      f.Write(tpl);
      f.Close();

      forcedFqbn = "arduino:avr:uno";

      AddBoardToHistory(forcedFqbn);

      newContentCreated = true;
    } else {
      wxLogError(_("Can't create file '%s'."), inoFullPath);
      return;
    }
  }

  // At this point we are guaranteed to have one .ino file in the root:
  // inoFileName / inoFullPath

  // Initialize arduino-cli interface
  ArduinoCliInstaller installer(this, config);

  arduinoCli = installer.GetCli(normPath);
  if (!arduinoCli) {
    exit(0);
  }

  // if no INO file in sketch (new sketch) then set forced fqbn arduino:avr:uno.
  if (!forcedFqbn.empty()) {
    arduinoCli->SetFQBN(forcedFqbn);
  }

  std::string fqbn = arduinoCli->GetFQBN();

  app.SetSplashMessage(_("Updating board..."));
  if (!fqbn.empty()) {
    // we will ensure that the board is in choice, so that it can be selected
    AddBoardToHistory(fqbn);

    UpdateBoard(fqbn);
  } else {
    bool boardSelected = false;

    // if the fqbn for the sketch is not known, we will prompt the user to specify it
    ArduinoInitialBoardSelectDialog initDlg(this, inoFileName);
    initDlg.SetBoardHistory(m_boardHistory, "");

    int res = initDlg.ShowModal();

    if (res == wxID_OK) {
      // selected from history
      std::string newFqbn = initDlg.GetSelectedFqbn();
      if (!newFqbn.empty()) {
        AddBoardToHistory(newFqbn);

        UpdateBoard(newFqbn); // inside calls arduinoCli->SetFQBN(...)

        boardSelected = true;
      }
    } else if (res == wxID_YES) {
      // manual selection
      ArduinoBoardSelectDialog brdDlg(this, arduinoCli, config, "");
      if (brdDlg.ShowModal() == wxID_OK) {
        std::string newFqbn = brdDlg.GetSelectedFqbn();
        if (!newFqbn.empty()) {
          AddBoardToHistory(newFqbn);

          UpdateBoard(newFqbn);

          boardSelected = true;
        }
      }
    }

    if (!boardSelected) {
      std::string fallbackFqbn = "arduino:avr:uno";
      AddBoardToHistory(fallbackFqbn);
      UpdateBoard(fallbackFqbn);
    }
  }

  StartProcess(_("Loading available boards..."), ID_PROCESS_LOAD_BOARDS, ArduinoActivityState::Background);
  arduinoCli->GetAvailableBoardsAsync(this);

  StartProcess(_("Loading available cores..."), ID_PROCESS_LOAD_CORES, ArduinoActivityState::Background);
  arduinoCli->LoadCoresAsync(this);

  app.SetSplashMessage(_("Creating completion engine..."));
  completion = new ArduinoCodeCompletion(arduinoCli, m_clangSettings, [this](std::vector<SketchFileBuffer> &out) { this->CollectEditorSources(out); }, /*eventHandler=*/this);

  // ---------------------------------------------------------------------------
  // Create editor ONLY for the primary .ino file.
  // Other project files are opened lazily (files tree / workspace restore).
  // ---------------------------------------------------------------------------
  {
    std::string filenameStd = wxToStd(inoFileName);

    ArduinoEditor *editor =
        new ArduinoEditor(m_notebook, arduinoCli, completion, config, filenameStd);
    editor->ApplySettings(m_clangSettings);
    editor->ApplySettings(m_aiSettings);

    m_notebook->AddPage(editor, inoFileName, /*select=*/true, IMLI_NOTEBOOK_EMPTY);
    m_notebook->SetPageToolTip(m_notebook->GetPageCount() - 1, wxString::FromUTF8(editor->GetFilePath()));
  }

  UpdateAiGlobalEditor();

  wxString lastPort = wxString::FromUTF8(arduinoCli->GetSerialPort());
  if (!lastPort.empty()) {
    int idx = m_portChoice->FindString(lastPort);
    if (idx != wxNOT_FOUND) {
      m_portChoice->SetSelection(idx);
      std::string lp = wxToStd(lastPort);
      arduinoCli->SetSerialPort(lp);
    }
  }

  app.SetSplashMessage(_("Initiating background processes..."));
  StartProcess(_("Loading installed libraries..."), ID_PROCESS_LOAD_INSTALLED_LIBRARIES, ArduinoActivityState::Background);
  arduinoCli->LoadInstalledLibrariesAsync(this);

  wxFileName fn;
  fn.AssignDir(dirPath);
  const wxArrayString &dirs = fn.GetDirs();
  if (!dirs.IsEmpty()) {
    SetTitle(wxT("Arduino Editor [") + dirs.Last() + wxT("]"));
  } else {
    SetTitle(wxT("Arduino Editor [") + dirPath + wxT("]"));
  }

  // Configuration update
  config->Write(wxT("LastSketchPath"), dirPath);
  AddRecentSketch(dirPath);

  // If the sketch disappeared and was created with new empty content,
  // we will not restore the original workspace.
  if (!newContentCreated) {
    std::unique_ptr<wxFileConfig> wcfg(OpenWorkspaceConfig());

    // Open all saved editors
    wxString tabList;
    if (wcfg->Read(wxT("Tabs"), &tabList) && !tabList.empty()) {
      wxArrayString files = wxSplit(tabList, ';');
      for (auto &f : files) {
        std::string full = NormalizeFilename(wxToStd(f));
        if (!FindEditorWithFile(full, /*allowCreate=*/true)) {
          APP_DEBUG_LOG("FRM: Can't open editor for file %s", full.c_str());
        }
      }
    }

    // Activate last editor + put cursor to last position
    wxString active;
    if (wcfg->Read(wxT("Active"), &active) && !active.empty()) {
      std::string full = NormalizeFilename(wxToStd(active));
      ArduinoEditor *ed = FindEditorWithFile(full);
      if (ed) {
        int row = 1, column = 1;
        wcfg->Read(wxT("Active/Row"), &row, 1);
        wcfg->Read(wxT("Active/Column"), &column, 1);
        ActivateEditor(ed, row, column);
      }
    }
  }

  // Update files tree
  if (m_filesPanel) {
    m_filesPanel->SetRootPath(dirPath);
    UpdateFilesTreeSelectedFromNotebook();
  }
}

void ArduinoEditorFrame::SaveWorkspaceState() {
  auto editors = GetAllEditors(/*onlyEditable=*/true);

  std::unique_ptr<wxFileConfig> wcfg(OpenWorkspaceConfig());

  wxArrayString files;
  for (auto *ed : editors) {
    files.Add(wxString::FromUTF8(ed->GetFileName())); // relative
  }

  auto *currentEditor = GetCurrentEditor();
  if (!currentEditor) {
    return;
  }

  int line = 1, column = 1;
  currentEditor->GetCurrentCursor(line, column); // 1-based

  wcfg->Write(wxT("Tabs"), wxJoin(files, ';'));
  wcfg->Write(wxT("Active"), wxString::FromUTF8(currentEditor->GetFileName()));
  wcfg->Write(wxT("Active/Row"), line);
  wcfg->Write(wxT("Active/Column"), column);
  wcfg->Flush();
}

wxFileConfig *ArduinoEditorFrame::OpenWorkspaceConfig() {
  if (!arduinoCli) {
    return nullptr;
  }

  // ensure <sketchdir>/.ardedit exists
  wxString sketchDir = wxString::FromUTF8(arduinoCli->GetSketchPath());
  wxFileName dirFn(sketchDir, wxEmptyString);
  dirFn.AppendDir(wxT(".ardedit"));
  wxString dirPath = dirFn.GetPath();

  if (!wxDirExists(dirPath)) {
    wxFileName::Mkdir(dirPath, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
  }

  dirFn.SetFullName(wxT("workspace.ini"));

  // local file only
  return new wxFileConfig(
      wxEmptyString,
      wxEmptyString,
      dirFn.GetFullPath(),
      wxEmptyString,
      wxCONFIG_USE_LOCAL_FILE);
}

void ArduinoEditorFrame::OnClangArgsReady(wxThreadEvent &event) {
  StopProcess(ID_PROCESS_LOAD_BOARD_PARAMS);
  StopProcess(ID_PROCESS_APP_INIT);

  bool ok = (event.GetInt() != 0);

  APP_DEBUG_LOG("FRM: OnClangArgsReady() ok=%d", ok);

  if (ok) {
    for (auto *ed : GetAllEditors(/*onlyEditable=*/true)) {
      ed->ApplyBoardChange();
    }

    completion->SetReady();

    EnableUIActions(true);

    UpdateStatus(_("Arduino Clang completion initialized for ") + wxString::FromUTF8(arduinoCli->GetBoardName()));

    switch (m_clangSettings.resolveMode) {
      case internalResolver:
        if (m_firstInitCompleted) {
          // We only call it if the first initialization has already taken place.
          // Otherwise it is called from OnInstalledLibrariesUpdated so it would
          // be called twice unnecessarily.
          ResolveLibrariesOrDiagnostics();
        }
        break;
      case compileCommandsResolver:
        if (m_filesPanel) {
          auto libs = arduinoCli->GetResolvedLibrariesFromCompileCommands();

          APP_DEBUG_LOG("FRM: update %zu resolved libraries", libs.size());

          m_filesPanel->UpdateResolvedLibraries(libs);
        }

        ResolveLibrariesOrDiagnostics();
        break;
      default:
        break;
    }
  } else {
    if (!m_cleanTried) {
      m_cleanTried = true;
      CleanProject();
    } else {
      UpdateStatus(_("Failed create Arduino Clang completion!"));
    }
  }
}

void ArduinoEditorFrame::RebuildProject(bool withClean) {
  if (!completion->IsReady()) {
    return;
  }

  if (withClean) {
    CleanProject();
  }

  completion->InvalidateTranslationUnit();
  ScheduleDiagRefresh();
}

void ArduinoEditorFrame::UpdateStatus(const wxString &msg) {
  if (m_statusBar) {
    m_statusBar->SetStatusText(msg);
  }
}

void ArduinoEditorFrame::UpdateBoard(std::string &fqbn) {
  arduinoCli->SetFQBN(fqbn);

  EnableUIActions(false);

  if (completion) {
    completion->InvalidateTranslationUnit();
  }

  RebuildBoardChoice();

  UpdateStatus(_("Initializing Arduino Clang completion..."));

  switch (m_clangSettings.resolveMode) {
    case internalResolver:
      StartProcess(_("Loading board parameters..."), ID_PROCESS_LOAD_BOARD_PARAMS, ArduinoActivityState::Background);
      arduinoCli->LoadBoardParametersAsync(this);
      break;
    case compileCommandsResolver: {
      std::string cachedFqbn = arduinoCli->GetCachedEnviromentFqbn();
      if (arduinoCli->GetBoardName() != cachedFqbn) {
        CleanProject();
      } else {
        StartProcess(_("Loading compile parameters..."), ID_PROCESS_LOAD_COMPILE_PARAMS, ArduinoActivityState::Background);
        arduinoCli->LoadPropertiesAsync(this);
      }
      break;
    }
    default:
      break;
  }
}

void ArduinoEditorFrame::SelectBoard() {
  if (!arduinoCli) {
    return;
  }

  std::string currentFqbn = arduinoCli->GetBoardName();

  ArduinoBoardSelectDialog dlg(this, arduinoCli, config, currentFqbn);

  if (dlg.ShowModal() != wxID_OK) {
    return;
  }

  std::string newFqbn = dlg.GetSelectedFqbn();
  if (newFqbn.empty() || newFqbn == currentFqbn) {
    return;
  }

  AddBoardToHistory(newFqbn);

  UpdateBoard(newFqbn);
}

void ArduinoEditorFrame::OnChangeBoardOptions(wxCommandEvent &) {
  wxBeginBusyCursor();

  std::vector<ArduinoBoardOption> defOpts;

  if (!arduinoCli->GetDefaultBoardOptions(defOpts)) {
    wxEndBusyCursor();
    ModalMsgDialog(_("Error loading default board options."));
    return;
  }

  if (defOpts.empty()) {
    wxEndBusyCursor();
    ModalMsgDialog(_("Board has no options."), _("Information"), wxOK | wxICON_INFORMATION);
    return;
  }

  std::vector<ArduinoBoardOption> opts;

  if (!arduinoCli->GetBoardOptions(opts)) {
    wxEndBusyCursor();
    ModalMsgDialog(_("Error loading board options."));
    return;
  }

  if (opts.empty()) {
    opts = defOpts;
  }

  wxEndBusyCursor();
  ArduinoBoardOptionsDialog dlg(this, opts, _("Board settings"));

  if (dlg.ShowModal() == wxID_OK) {
    auto newOptions = dlg.GetOptions();
    std::string newFqbn = arduinoCli->BuildFqbnFromOptions(newOptions);

    UpdateBoard(newFqbn);
  }
}

void ArduinoEditorFrame::OnNotebookPageChanged(wxBookCtrlEvent &event) {
  if (completion) {
    if (m_clangSettings.diagnosticMode == translationUnit) {
      completion->InvalidateTranslationUnit();

      RefreshDiagnostics();
    }
  }

  UpdateFilesTreeSelectedFromNotebook();

  UpdateAiGlobalEditor();
  UpdateClassBrowserEditor();

  event.Skip();
}

void ArduinoEditorFrame::OnNotebookPageChanging(wxBookCtrlEvent &event) {
  if (!m_notebook) {
    event.Skip();
    return;
  }

  for (auto *editor : GetAllEditors()) {
    editor->CancelCallTip();
  }

  event.Skip();
}

void ArduinoEditorFrame::OnNotebookPageClose(wxAuiNotebookEvent &e) {
  if (!m_notebook) {
    e.Skip();
    return;
  }

  int idx = e.GetSelection();
  if (idx == wxNOT_FOUND)
    idx = m_notebook->GetSelection();

  if (idx < 0 || idx >= (int)m_notebook->GetPageCount()) {
    e.Skip();
    return;
  }

  wxWindow *page = m_notebook->GetPage(idx);
  auto *ed = dynamic_cast<ArduinoEditor *>(page);
  if (!ed) {
    e.Skip();
    return;
  }

  if (hasSuffix(ed->GetFileName(), ".ino")) {
    e.Veto();
    return;
  }

  if (!ed->IsReadOnly() && ed->IsModified()) {
    int res = ModalMsgDialog(
        _("The file in the editor is modified. Do you want to save the file?"),
        _("Close tab"),
        wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);

    if (res == wxID_YES && !ed->Save()) {
      ModalMsgDialog(_("Save error!"));
      e.Veto();
      return;
    }
  }

  e.Veto();

  m_aiGlobalActions.SetCurrentEditor(nullptr);

  wxWindow *pageToClose = page;
  CallAfter([this, pageToClose]() {
    if (!m_notebook)
      return;
    int p = m_notebook->FindPage(pageToClose);
    if (p != wxNOT_FOUND) {
      m_notebook->DeletePage(p);
    }
    UpdateAiGlobalEditor();
    UpdateClassBrowserEditor();
    UpdateFilesTreeSelectedFromNotebook();
  });
}

static bool CanCloseEditorTab(ArduinoEditor *ed) {
  if (!ed)
    return false;
  if (hasSuffix(ed->GetFileName(), ".ino")) {
    return false;
  }
  return true;
}

bool ArduinoEditorFrame::ConfirmAndCloseTab(int idx) {
  if (!m_notebook)
    return false;
  if (idx < 0 || idx >= (int)m_notebook->GetPageCount())
    return false;

  auto *ed = dynamic_cast<ArduinoEditor *>(m_notebook->GetPage(idx));
  if (!ed)
    return false;

  if (!CanCloseEditorTab(ed)) {
    return false;
  }

  if (!ed->IsReadOnly() && ed->IsModified()) {
    int res = ModalMsgDialog(
        _("The file in the editor is modified. Do you want to save the file?"),
        _("Close tab"),
        wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);

    if (res == wxID_YES && !ed->Save()) {
      ModalMsgDialog(_("Save error!"));
      return false;
    }
  }

  wxWindow *pageToClose = m_notebook->GetPage(idx);

  m_aiGlobalActions.SetCurrentEditor(nullptr);

  CallAfter([this, pageToClose]() {
    if (!m_notebook)
      return;
    int p = m_notebook->FindPage(pageToClose);
    if (p != wxNOT_FOUND) {
      m_notebook->DeletePage(p);
    }
    UpdateAiGlobalEditor();
    UpdateClassBrowserEditor();
    UpdateFilesTreeSelectedFromNotebook();
  });

  return true;
}

void ArduinoEditorFrame::OnNotebookTabRightUp(wxAuiNotebookEvent &e) {
  if (!m_notebook) {
    e.Skip();
    return;
  }

  m_tabPopupIndex = e.GetSelection();
  if (m_tabPopupIndex == wxNOT_FOUND) {
    m_tabPopupIndex = m_notebook->GetSelection();
  }

  wxMenu menu;
  menu.Append(ID_TABMENU_CLOSE, _("Close"));
  menu.Append(ID_TABMENU_CLOSE_OTHERS, _("Close Others"));
  menu.Append(ID_TABMENU_CLOSE_ALL, _("Close All"));

  auto *ed = (m_tabPopupIndex != wxNOT_FOUND)
                 ? dynamic_cast<ArduinoEditor *>(m_notebook->GetPage(m_tabPopupIndex))
                 : nullptr;

  if (!CanCloseEditorTab(ed)) {
    menu.Enable(ID_TABMENU_CLOSE, false);
    menu.Enable(ID_TABMENU_CLOSE_OTHERS, false);
  }

  wxPoint pt = wxGetMousePosition();
  pt = m_notebook->ScreenToClient(pt);
  m_notebook->PopupMenu(&menu, pt);

  e.Skip();
}

void ArduinoEditorFrame::OnTabMenuClose(wxCommandEvent &) {
  const int idx = m_tabPopupIndex;
  m_tabPopupIndex = wxNOT_FOUND;

  CallAfter([this, idx]() { ConfirmAndCloseTab(idx); });
}

void ArduinoEditorFrame::OnTabMenuCloseOthers(wxCommandEvent &) {
  const int keep = m_tabPopupIndex;
  m_tabPopupIndex = wxNOT_FOUND;

  CallAfter([this, keep]() {
    if (!m_notebook)
      return;
    for (int i = (int)m_notebook->GetPageCount() - 1; i >= 0; --i) {
      if (i == keep)
        continue;
      auto *ed = dynamic_cast<ArduinoEditor *>(m_notebook->GetPage(i));
      if (CanCloseEditorTab(ed)) {
        ConfirmAndCloseTab(i);
      }
    }
  });
}

void ArduinoEditorFrame::OnTabMenuCloseAll(wxCommandEvent &) {
  m_tabPopupIndex = wxNOT_FOUND;

  CallAfter([this]() {
    if (!m_notebook)
      return;
    for (int i = (int)m_notebook->GetPageCount() - 1; i >= 0; --i) {
      auto *ed = dynamic_cast<ArduinoEditor *>(m_notebook->GetPage(i));
      if (CanCloseEditorTab(ed)) {
        ConfirmAndCloseTab(i);
      }
    }
  });
}

void ArduinoEditorFrame::DeleteSketchFile(const wxString &filename) {
  wxFileName fn(filename);

  if (!fn.FileExists()) {
    ModalMsgDialog(wxString::Format(_("File %s not found."), fn.GetFullName()));
    return;
  }

  if (fn.GetExt().Lower() == wxT("ino")) {
    ModalMsgDialog(_("Cannot delete .ino file of the sketch."));
    return;
  }

  wxString fullPathWx = fn.GetFullPath();
  std::string filenameStd = wxToStd(fullPathWx);

  // if the file is open in the editor and is modified, ask
  ArduinoEditor *openedEd = FindEditorWithFile(filenameStd);
  if (openedEd) {
    if (openedEd->IsModified()) {
      int res = ModalMsgDialog(
          _("The file has unsaved changes.\n"
            "Do you really want to delete it?"),
          _("Delete file"), wxYES_NO | wxICON_WARNING);
      if (res != wxID_YES) {
        return;
      }
    }
  }

  // deletion from FS
  std::error_code ec;
  std::filesystem::remove(filenameStd, ec);

  if (ec) {
    ModalMsgDialog(wxString::Format(_("Failed to delete %s."), fullPathWx));
    return;
  }

  // if it is not open in any editor, we are done
  if (!openedEd) {
    m_filesPanel->RefreshTree();
    return;
  }

  std::string target = NormalizeFilename(filenameStd);

  for (int i = 0, n = m_notebook->GetPageCount(); i < n; i++) {
    auto *ed = dynamic_cast<ArduinoEditor *>(m_notebook->GetPage(i));

    if (ed) {
      if (NormalizeFilename(ed->GetFilePath()) == target) {
        m_notebook->DeletePage(i);
        break;
      }
    }
  }

  m_filesPanel->RefreshTree();

  RebuildProject(/*withClean=*/true);
}

/**
 * Creates an empty file based on the given full path. If the file is in
 * the sketchDir directory and has the extensions c, cpp, h, hpp, a tab
 * wxNotebook is also created.
 */
void ArduinoEditorFrame::CreateNewSketchFile(const wxString &filename) {
  wxString sketchDir = wxString::FromUTF8(arduinoCli->GetSketchPath().c_str());

  wxFileName fn(filename);

  if (fn.FileExists()) {
    ModalMsgDialog(_("File with this name already exists in the filesystem."));
    return;
  }

  if (!fn.GetFullPath().StartsWith(sketchDir)) {
    ModalMsgDialog(_("Cannot create file outside of project."));
    return;
  }

  // Create an empty file
  wxFFile f(filename, wxT("w"));
  if (!f.IsOpened()) {
    ModalMsgDialog(wxString::Format(_("Cannot create file '%s'."), filename));
    return;
  }
  f.Close();

  wxString ext = fn.GetExt().Lower();

  if (IsSupportedExtension(ext)) {
    std::string filepath = NormalizeFilename(wxToStd(fn.GetFullPath()));
    std::string filename = RelativizeFilename(filepath);

    ArduinoEditor *editor = new ArduinoEditor(m_notebook, arduinoCli, completion, config, filename, filepath);
    editor->ApplySettings(m_clangSettings);
    editor->ApplySettings(m_aiSettings);

    m_notebook->AddPage(editor, wxString::FromUTF8(filename), /*select=*/true, IMLI_NOTEBOOK_EMPTY);
    m_notebook->SetPageToolTip(m_notebook->GetPageCount() - 1, wxString::FromUTF8(filepath));
    UpdateAiGlobalEditor();
    UpdateClassBrowserEditor();
  } else {
    // not supported file is opened in external program
    m_clangSettings.OpenExternalSourceFile(filename, 1);
  }

  m_filesPanel->RefreshTree();
}

void ArduinoEditorFrame::CloseIfNeeded() {
  EditorSettings settings;
  settings.Load(config);

  if (settings.keepOneWindow) {
    CallAfter([this]() {
      Close();
    });
  }
}

bool ArduinoEditorFrame::NewSketch(bool inNewWindow) {
  wxString sketchesDir;

  if (config) {
    if (!config->Read(wxT("SketchesDir"), &sketchesDir) || sketchesDir.empty()) {
      return false;
    }
  } else {
    return false;
  }

  NewSketchDialog dlg(this, config, sketchesDir);
  if (dlg.ShowModal() != wxID_OK) {
    return false;
  }

  wxString sketchName = dlg.GetSketchName();
  sketchName.Trim(true).Trim(false);

  if (sketchName.empty()) {
    ModalMsgDialog(_("Sketch name cannot be empty."));
    return false;
  }

  wxString parentDir = dlg.GetDirectory();

  // parentDir / sketchName  -> this is the *target directory of the sketch*
  wxFileName sketchDir(parentDir, sketchName);
  wxString fullSketchPath = sketchDir.GetFullPath(); // parentDir/sketchName

  // Here we MUST work with fullSketchPath, not just with m_dir
  if (!wxDirExists(fullSketchPath)) {
    if (!wxFileName::Mkdir(fullSketchPath, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
      ModalMsgDialog(wxString::Format(_("Error creating directory '%s'."), fullSketchPath));
      return false;
    }
  }

  if (config) {
    config->Write(wxT("SketchesDir"), parentDir);
    config->Flush();
  }

  RebuildSketchesDirMenu();

  if (inNewWindow) {
    ArduinoEditApp &app = wxGetApp();
    app.OpenSketch(fullSketchPath);

    CloseIfNeeded();
  } else {
    OpenSketch(wxToStd(fullSketchPath));
  }

  return true;
}

void ArduinoEditorFrame::OnNewSketch(wxCommandEvent &WXUNUSED(evt)) {
  NewSketch(/*inNewWindow=*/true);
}

bool ArduinoEditorFrame::OpenSketchDialog(bool inNewWindow) {
  wxString sketchesDir;

  if (config) {
    // If the key does not exist, use the home dir as a fallback
    if (!config->Read(wxT("SketchesDir"), &sketchesDir) || sketchesDir.empty()) {
      sketchesDir = wxGetHomeDir();
    }
  } else {
    sketchesDir = wxGetHomeDir();
  }

  wxDirDialog dlg(
      this,
      _("Select sketch directory"),
      sketchesDir,
      wxDD_DIR_MUST_EXIST | wxDD_NEW_DIR_BUTTON);

  if (dlg.ShowModal() == wxID_OK) {
    wxString path = dlg.GetPath();

    if (inNewWindow) {
      ArduinoEditApp &app = wxGetApp();
      app.OpenSketch(path);

      CloseIfNeeded();
    } else {
      OpenSketch(wxToStd(path));
    }
    return true;
  } else {
    return false;
  }
}

void ArduinoEditorFrame::OnOpenSketch(wxCommandEvent &WXUNUSED(event)) {
  OpenSketchDialog(/*inNewWindow=*/true);
}

ArduinoEditor *ArduinoEditorFrame::GetCurrentEditor() {
  return dynamic_cast<ArduinoEditor *>(m_notebook->GetCurrentPage());
}

void ArduinoEditorFrame::OnSave(wxCommandEvent &WXUNUSED(event)) {
  ArduinoEditor *editor = GetCurrentEditor();
  if (editor) {
    if (editor->Save()) {
      ResolveLibrariesOrDiagnostics();
      UpdateStatus(_("Saved..."));
    } else {
      ModalMsgDialog(_("Save error!"));
    }
  }
}

bool ArduinoEditorFrame::SaveAll() {
  bool anyError = false;
  for (int i = 0, n = m_notebook->GetPageCount(); i < n; i++) {
    auto *ed = dynamic_cast<ArduinoEditor *>(m_notebook->GetPage(i));
    if (ed) {
      if (!ed->IsReadOnly() && !ed->Save()) {
        ResolveLibrariesOrDiagnostics();
        anyError = true;
      }
    }
  }

  return !anyError;
}

void ArduinoEditorFrame::OnSaveAll(wxCommandEvent &WXUNUSED(event)) {
  if (SaveAll()) {
    ScheduleDiagRefresh();
    UpdateStatus(_("All saved..."));
  } else {
    ModalMsgDialog(_("Save error!"));
  }
}

void ArduinoEditorFrame::OnExit(wxCommandEvent &WXUNUSED(event)) {
  Close(false);
}

void ArduinoEditorFrame::OnSerialPortChanged(wxCommandEvent &WXUNUSED(event)) {
  int sel = m_portChoice->GetSelection();
  if (sel == wxNOT_FOUND)
    return;

  wxString value = m_portChoice->GetString(sel);

  if (arduinoCli) {
    arduinoCli->SetSerialPort(wxToStd(value));
  }

  UpdateStatus(_("Serial port changed to ") + value);

  // after changing the port, toggle enable/disable of the serial monitor
  UpdateSerialMonitorAvailability();
}

void ArduinoEditorFrame::OnRefreshSerialPorts(wxCommandEvent &WXUNUSED(event)) {
  RefreshSerialPorts();
  UpdateStatus(_("Serial ports refreshed."));
}

void ArduinoEditorFrame::OnFindSymbol(wxCommandEvent &WXUNUSED(event)) {
  auto symbols = completion->GetAllSymbols();
  if (symbols.empty()) {
    ModalMsgDialog(_("No symbols found."), _("Find symbol"), wxOK | wxICON_INFORMATION);
    return;
  }

  if (!m_findSymbolDlg) {
    m_findSymbolDlg = new FindSymbolDialog(this, config, symbols);
  } else {
    m_findSymbolDlg->SetSymbols(symbols);
  }

  m_findSymbolDlg->Show();
  m_findSymbolDlg->Raise();
}

void ArduinoEditorFrame::OnSymbolActivated(ArduinoSymbolActivatedEvent &evt) {
  const SymbolInfo &s = evt.GetSymbol();

  if (auto *editor = GetCurrentEditor()) {
    int line, column;
    editor->GetCurrentCursor(line, column);
    PushNavLocation(editor->GetFilePath(), line, column);
  }

  JumpTarget tgt;
  tgt.file = s.file;
  tgt.line = s.line;
  tgt.column = s.column;

  this->HandleGoToLocation(tgt);

  Raise();
}

void ArduinoEditorFrame::CleanProject() {
  if (!arduinoCli) {
    return;
  }

  arduinoCli->CleanCachedEnvironment();
  arduinoCli->InvalidateLibraryCache();

  EnableUIActions(false);

  if (completion) {
    completion->SetReady(false);
    completion->InvalidateTranslationUnit();
  }

  m_lastSuccessfulCompileCodeSum = 0;

  ShowSingleDiagMessage(_("Rebuilding project..."));

  UpdateStatus(_("Initializing Arduino Clang completion..."));

  switch (m_clangSettings.resolveMode) {
    case internalResolver:
      StartProcess(_("Loading board parameters..."), ID_PROCESS_LOAD_BOARD_PARAMS, ArduinoActivityState::Background);
      arduinoCli->LoadBoardParametersAsync(this);
      break;
    case compileCommandsResolver:
      StartProcess(_("Loading compile parameters..."), ID_PROCESS_LOAD_COMPILE_PARAMS, ArduinoActivityState::Background);
      arduinoCli->LoadPropertiesAsync(this);
      break;
    default:
      break;
  }
}

void ArduinoEditorFrame::OnProjectClean(wxCommandEvent &WXUNUSED(event)) {
  CleanProject();
}

void ArduinoEditorFrame::OnCliProcessKill(wxCommandEvent &) {
  APP_DEBUG_LOG("FRM: OnCliProcessKill()");

  if (arduinoCli) {
    if (arduinoCli->CancelRunning()) {

      if (m_action == build) {
        // An aborted build usually corrupts the translation
        // directory, so we'd rather delete it.
        arduinoCli->CleanBuildDirectory();
      }
    }
  }
}

wxString ArduinoEditorFrame::GetCurrentActionName() const {
  wxString text;
  switch (m_action) {
    case build:
      text = _("Compilation");
      break;
    case upload:
      text = _("Upload");
      break;
    case libinstall:
      text = _("Library installation");
      break;
    case libremove:
      text = _("Library uninstalation");
      break;
    case libupdateindex:
      text = _("Update library index");
      break;
    case none:
    default:
      text = _("Action");
      break;
  }
  return text;
}

bool ArduinoEditorFrame::CanPerformAction(CurrentAction newAction, bool activate) {
  if (m_action != none) {
    ModalMsgDialog(wxString::Format(_("Other action \"%s\" running. Please try again later."), GetCurrentActionName()));
    return false;
  }
  if (activate) {
    SetCurrentAction(newAction);
  }
  return true;
}

void ArduinoEditorFrame::SetCurrentAction(CurrentAction action) {
  m_action = action;

  EnableUIActions(false);

  UpdateStatus(wxString::Format(_("%s started..."), GetCurrentActionName()));

  if (m_buildOutputCtrl) {
    m_buildOutputCtrl->Clear();
  }
}

void ArduinoEditorFrame::FinalizeCurrentAction(bool successful) {
  if (m_action == none) {
    return;
  }

  wxString text = GetCurrentActionName();
  UpdateStatus(text + wxT(" ") + (successful ? _("finished.") : _("failed.")));

  auto endingAction = m_action;
  m_action = none;

  EnableUIActions();

  bool customFailureHandling = false;

  switch (endingAction) {
    case libupdateindex:
      StartProcess(_("Loading libraries..."), ID_PROCESS_LOAD_LIBRARIES, ArduinoActivityState::Background);
      arduinoCli->InvalidateLibraryCache();
      arduinoCli->LoadLibrariesAsync(this);
      break;
    case build:
      m_lastSuccessfulCompileCodeSum = 0;
      if (successful) {
        std::vector<SketchFileBuffer> files;
        CollectEditorSources(files);
        m_lastSuccessfulCompileCodeSum = CcSumCode(files);

        if (m_runUploadAfterCompile) {
          m_runUploadAfterCompile = false;
          UploadProject();
          return;
        }
      } else {
        if (m_buildOutputCtrl) {
          wxString text = m_buildOutputCtrl->GetValue();
          if (!text.IsEmpty()) {
            std::vector<ArduinoParseError> cliErrors = ArduinoCliOutputParser::ParseCliOutput(wxToStd(text));

            APP_DEBUG_LOG("FRM: CliParse: %zu errors", cliErrors.size());
            for (auto &err : cliErrors) {
              APP_DEBUG_LOG("FRM: CliParse: %s", err.ToString().c_str());
            }

            if (!cliErrors.empty()) {
              if (!m_cliDiagDialog) {
                m_cliDiagDialog = new ArduinoCliDiagnosticsDialog(this, config, wxID_ANY, _("arduino-cli build diagnostics"));
                m_cliDiagDialog->SetSketchRoot(arduinoCli->GetSketchPath());
              }

              m_cliDiagDialog->SetDiagnostics(cliErrors);
              m_cliDiagDialog->Show();

              customFailureHandling = true;
            }
          }
        }
      }
      m_runUploadAfterCompile = false;
      break;
    case upload:
      if (m_serialMonitor) {
        m_serialMonitor->Unblock();
      }
      UpdateSerialMonitorAvailability();
      break;
    case coreupdateindex:
      StartProcess(_("Enumerating available boards..."), ID_PROCESS_LOAD_BOARDS, ArduinoActivityState::Background);
      arduinoCli->GetAvailableBoardsAsync(this);
      StartProcess(_("Loading core list..."), ID_PROCESS_LOAD_CORES, ArduinoActivityState::Background);
      arduinoCli->LoadCoresAsync(this);
      break;

    default:
      break;
  }

  if (!successful && !customFailureHandling) {
    ModalMsgDialog(wxString::Format(_("%s failed. See the CLI output for more information."), text));
    return;
  }

  if (successful) {
    if (m_lastBottomNotebookSelectedPage != wxNOT_FOUND) {
      if (m_returnBottomPageTimer.IsRunning()) {
        m_returnBottomPageTimer.Stop();
      }

      m_returnBottomPageTimer.Start(1000, wxTIMER_ONE_SHOT);
    }
  }
}

void ArduinoEditorFrame::EnableUIActions(bool enable) {
  m_boardChoice->Enable(enable);
  m_optionsButton->Enable(enable);
  m_buildButton->Enable(enable);
  m_uploadButton->Enable(enable);
  m_menuBar->Enable(ID_MENU_PROJECT_BUILD, enable);
  m_menuBar->Enable(ID_MENU_PROJECT_UPLOAD, enable);
  m_menuBar->Enable(ID_MENU_PROJECT_CLEAN, enable);
}

void ArduinoEditorFrame::OnProjectBuild(wxCommandEvent &WXUNUSED(event)) {
  m_runUploadAfterCompile = false;
  BuildProject();
}

// returns true if project build started
bool ArduinoEditorFrame::BuildProject() {
  if (arduinoCli) {
    if (CanPerformAction(build)) {
      if (SaveAll()) {
        if (m_buildOutputCtrl) {
          m_buildOutputCtrl->Clear();
        }

        SetCurrentAction(build);
        StartProcess(_("Building project..."), ID_PROCESS_CLI, ArduinoActivityState::Busy, /*canBeTerminated=*/true);
        arduinoCli->CompileAsync(this);
        return true;
      }
    }
  }
  return false;
}

void ArduinoEditorFrame::OnProjectUpload(wxCommandEvent &WXUNUSED(event)) {
  std::vector<SketchFileBuffer> files;
  CollectEditorSources(files);
  uint64_t currentSum = CcSumCode(files);
  if (currentSum != m_lastSuccessfulCompileCodeSum) {
    if (BuildProject()) {
      m_runUploadAfterCompile = true;
    }
  } else {
    UploadProject();
  }
}

bool ArduinoEditorFrame::UploadProject() {
  if (arduinoCli) {
    if (CanPerformAction(upload)) {
      if (SaveAll()) {
        if (m_buildOutputCtrl) {
          m_buildOutputCtrl->Clear();
        }

        SetCurrentAction(upload);
        if (m_serialMonitor) {
          m_serialMonitor->Block();
        }

        StartProcess(_("Uploading project..."), ID_PROCESS_CLI, ArduinoActivityState::Background, /*canBeTerminated=*/true);
        arduinoCli->UploadAsync(this);
        return true;
      }
    }
  }
  return false;
}

void ArduinoEditorFrame::OnCmdLineOutput(wxCommandEvent &event) {
  if (!m_buildOutputCtrl) {
    return;
  }

  wxAuiPaneInfo &pane = m_auiManager.GetPane(wxT("output"));
  if (!pane.IsOk())
    return;

  if (!pane.IsShown()) {
    pane.Show(TRUE);
    m_auiManager.Update();
  }

  int nbkSel = m_bottomNotebook->GetSelection();
  int pageIndex = m_bottomNotebook->FindPage(m_buildOutputCtrl);
  if ((pageIndex != wxNOT_FOUND) && (pageIndex != nbkSel)) {
    m_bottomNotebook->SetSelection(pageIndex);
    m_lastBottomNotebookSelectedPage = nbkSel;
  }

  wxString txt = event.GetString();

  m_buildOutputCtrl->AppendText(txt + wxT("\n"));

  if (event.GetInt() != 0) {
    StopProcess(ID_PROCESS_CLI);
    FinalizeCurrentAction(false);
    return;
  }

  if (txt.StartsWith(wxT("[arduino-cli "))) {
    StopProcess(ID_PROCESS_CLI);
    FinalizeCurrentAction(true);
  }
}

void ArduinoEditorFrame::OnReturnBottomPageTimer(wxTimerEvent &) {
  if (!m_bottomNotebook)
    return;
  if (m_lastBottomNotebookSelectedPage == wxNOT_FOUND)
    return;

  APP_DEBUG_LOG("FRM: returning old selected page %d", m_lastBottomNotebookSelectedPage);
  m_bottomNotebook->SetSelection(m_lastBottomNotebookSelectedPage);
  m_lastBottomNotebookSelectedPage = wxNOT_FOUND;
}

std::string ArduinoEditorFrame::ExtractMissingHeaderFromMessage(const std::string &msg) {
  // We are only interested in the typical message from clang/clangd:
  //   "'DS18B20h' file not found"
  //   "fatal error: 'OneWire.h' file not found"
  // or variant with "No such file or directory"
  if (msg.find("file not found") == std::string::npos &&
      msg.find("No such file or directory") == std::string::npos) {
    return {};
  }

  // Find the first apostrophe/quotation mark
  size_t first = msg.find('\'');
  char quoteChar = '\'';
  if (first == std::string::npos) {
    first = msg.find('"');
    quoteChar = '"';
  }
  if (first == std::string::npos) {
    return {};
  }

  size_t second = msg.find(quoteChar, first + 1);
  if (second == std::string::npos || second <= first + 1) {
    return {};
  }

  std::string header = msg.substr(first + 1, second - first - 1);

  // Trim any whitespace
  while (!header.empty() && std::isspace(static_cast<unsigned char>(header.front()))) {
    header.erase(header.begin());
  }
  while (!header.empty() && std::isspace(static_cast<unsigned char>(header.back()))) {
    header.pop_back();
  }

  return header;
}

void ArduinoEditorFrame::MaybeOfferToInstallLibrariesForHeaders(const std::vector<std::string> &headers) {
  if (!arduinoCli || headers.empty()) {
    return;
  }

  m_wantedHeaders.clear();
  m_wantedHeaders.reserve(headers.size());
  m_foundLibraryGroups.clear();

  for (const auto &header : headers) {
    bool alreadyAsked = std::find(m_queriedMissingHeaders.begin(), m_queriedMissingHeaders.end(), header) != m_queriedMissingHeaders.end();
    bool notFound = std::find(m_notFoundHeaders.begin(), m_notFoundHeaders.end(), header) != m_notFoundHeaders.end();

    if (alreadyAsked || notFound) {
      continue;
    }

    m_wantedHeaders.insert(header);
  }

  if (m_wantedHeaders.empty()) {
    return;
  }

  StartProcess(_("Resolving missing libraries..."), ID_PROCESS_SEARCH_LIBRARIES, ArduinoActivityState::Background);

  // all parallel
  for (const auto &wh : m_wantedHeaders) {
    arduinoCli->SearchLibraryProvidingHeaderAsync(wh, this);
  }
}

void ArduinoEditorFrame::RequestShowLibraries(const std::vector<ArduinoLibraryInfo> &libs) {
  if (!m_libManager) {
    StartProcess(_("Loading libraries..."), ID_PROCESS_LOAD_LIBRARIES, ArduinoActivityState::Background);

    if (!m_librariesUpdated) {
      m_librariesUpdated = true;
      arduinoCli->LoadLibrariesAsync(this);
    }

    m_libManager = new ArduinoLibraryManagerFrame(this, arduinoCli, m_availableBoards, config, _("All"));
  }

  m_libManager->Show(TRUE);

  m_libManager->ShowLibraries(libs);
}

void ArduinoEditorFrame::ShowSingleDiagMessage(const wxString &message) {
  if (m_diagView) {
    m_diagView->ShowMessage(message);
  }

  for (auto *ed : GetAllEditors()) {
    ed->ClearDiagnosticsIndicators();
  }
}

void ArduinoEditorFrame::OnDiagnosticsUpdated(wxThreadEvent &evt) {
  StopProcess(ID_PROCESS_DIAG_EVAL);

  if (!completion || !m_diagView || !m_bottomNotebook) {
    return;
  }

  m_diagView->SetStale(false);

  if (evt.GetInt() == 0) {
    // diagnosis has not changed.
    return;
  }

  UpdateClassBrowserEditor();

  int problemsPageIndex = m_bottomNotebook->FindPage(m_diagView);
  if (problemsPageIndex == wxNOT_FOUND) {
    return;
  }

  // --- tab title updater (runs on every return) ---
  int errorCount = 0; // number of errors/warnings
  auto updateProblemsTabTitle = [&]() {
    wxString text = _("Problems");
    if (errorCount > 0) {
      text += wxString::Format(wxT(" (%d)"), errorCount);
    }

    m_bottomNotebook->SetPageText(problemsPageIndex, text);
  };

  // scope-guard (RAII); calls update nbk text on every return
  struct TabTitleGuard {
    decltype(updateProblemsTabTitle) &fn;
    ~TabTitleGuard() { fn(); }
  } tabTitleGuard{updateProblemsTabTitle};

  // If autocompleting is active in the editor, we ignore errors
  ArduinoEditor *ed = GetCurrentEditor();
  if (ed && ed->IsAutoCompActive()) {
    APP_DEBUG_LOG("FRM: Completion active; skipping diagnostics.");
    return;
  }

  if (!completion->IsReady()) {
    ShowSingleDiagMessage(_("Preparing..."));
    return;
  }

  std::vector<ArduinoParseError> errors;

  switch (m_clangSettings.diagnosticMode) {
    case translationUnit: {
      if (!completion->IsTranslationUnitValid()) {
        ShowSingleDiagMessage(_("Translation unit not prepared."));
        return;
      }
      ArduinoEditor *ed = GetCurrentEditor();
      if (ed) {
        errors = completion->GetErrorsFor(ed->GetFilePath());
      }

      break;
    }
    case completeProject:
      errors = completion->GetLastProjectErrors();
      break;
    default:
      m_firstInitCompleted = true;
      ShowSingleDiagMessage(_("Diagnostics turned off."));
      return;
  }

  m_firstInitCompleted = true;

  errorCount = errors.size();
  APP_DEBUG_LOG("FRM: OnDiagnosticsUpdated: %d diagnostics", errorCount);

  if (errors.empty()) {
    ShowSingleDiagMessage(_("No problems found."));
    return;
  }

  std::vector<ArduinoParseError> dispDiagnostic;
  for (auto &diag : errors) {
    dispDiagnostic.push_back(diag);
    for (auto &child : diag.childs) {
      dispDiagnostic.push_back(child);
    }
  }

  m_diagView->SetSketchRoot(arduinoCli->GetSketchPath());
  m_diagView->SetDiagnostics(dispDiagnostic);

  for (auto *ed : GetAllEditors()) {
    ed->ClearDiagnosticsIndicators();
  }

  for (const auto &e : errors) {
    bool isError = (e.severity == CXDiagnostic_Error ||
                    e.severity == CXDiagnostic_Fatal);

    ArduinoEditor *ed = FindEditorWithFile(e.file);
    if (!ed)
      continue;

    ed->AddDiagnosticUnderline(e.line, e.column, isError);
  }

  CheckMissingHeadersInDiagnostics(errors);
}

bool ArduinoEditorFrame::GetDiagnosticsAt(const std::string &filename, unsigned row, unsigned column, std::vector<ArduinoParseError> &outDiagnostics) {
  if (m_diagView) {
    return m_diagView->GetDiagnosticsAt(filename, row, column, outDiagnostics);
  }
  return false;
}

void ArduinoEditorFrame::CheckMissingHeadersInDiagnostics(const std::vector<ArduinoParseError> &errors) {
  std::vector<std::string> missingHeaders;

  for (auto e : errors) {
    std::string missing = ExtractMissingHeaderFromMessage(e.message);
    if (!missing.empty()) {
      bool alreadyInBatch =
          std::find(missingHeaders.begin(), missingHeaders.end(), missing) != missingHeaders.end();
      if (!alreadyInBatch) {
        missingHeaders.push_back(missing);
      }
    }
  }

  MaybeOfferToInstallLibrariesForHeaders(missingHeaders);
}

// Collects the project's source files, either from open editors or from the filesystem.
void ArduinoEditorFrame::CollectEditorSources(std::vector<SketchFileBuffer> &files) {
  ScopeTimer t("FRM: CollectEditorSources()");
  APP_DEBUG_LOG("FRM: CollectEditorSources()");

  // 1) Collect buffers from all open editors (these override disk content)
  for (auto *e : GetAllEditors(/*onlyEditable=*/true)) {
    std::string filename = e->GetFilePath();
    std::string code = e->GetText();

    APP_DEBUG_LOG("FRM: - added %s with code size %d", filename.c_str(), code.size());

    files.push_back(SketchFileBuffer{filename, std::move(code)});
  }

  // Track which files are already included (from editors)
  std::unordered_set<std::string> seenFiles;
  seenFiles.reserve(files.size() * 2);
  for (const auto &buf : files) {
    seenFiles.insert(buf.filename);
  }

  std::error_code ec;
  fs::path rootPath = fs::u8path(arduinoCli->GetSketchPath());

  if (!fs::exists(rootPath, ec) || !fs::is_directory(rootPath, ec)) {
    return; // Nothing to traverse
  }

  // Helper: attempt to add a file if it matches Arduino sketch rules
  auto tryAddFile = [&](const fs::path &p) {
    std::string pathUtf8 = p.u8string();

    bool isHeader = isHeaderFile(pathUtf8);
    bool isSource = isSourceFile(pathUtf8);

    if (!isHeader && !isSource) {
      return; // Skip irrelevant files
    }

    // Arduino build rules apply ONLY to source files, not headers
    if (isSource) {
      fs::path parent = p.parent_path();

      bool isInRoot = (parent == rootPath);
      fs::path rel = p.lexically_relative(rootPath);
      bool isUnderSrc = !rel.empty() && *rel.begin() == fs::path("src");

      std::string ext = p.extension().string();
      bool isInoLike = (ext == ".ino" || ext == ".pde");

      if (isInoLike) {
        // .ino/.pde MUST be in sketch root
        if (!isInRoot)
          return;
      } else {
        // .c/.cpp/.S must be:
        //   - in sketch root, OR
        //   - anywhere under src/
        if (!isInRoot && !isUnderSrc)
          return;
      }
    }
    // Headers are allowed from anywhere except hidden dirs (filtered earlier)

    // Skip if already added (open editors override disk version)
    if (!seenFiles.insert(pathUtf8).second) {
      return;
    }

    std::string code;
    if (!LoadFileToString(pathUtf8, code)) {
      return; // Could not load file
    }

    APP_DEBUG_LOG("FRM: - added %s with code size %d", pathUtf8.c_str(), code.size());

    files.push_back(SketchFileBuffer{std::move(pathUtf8), std::move(code)});
  };

  // 2) Traverse the entire sketch directory tree
  fs::recursive_directory_iterator it(rootPath, ec), end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      continue; // Skip problematic entries
    }

    const fs::directory_entry &entry = *it;

    if (entry.is_directory(ec)) {
      // Skip hidden or system directories starting with '.' or '_'
      std::string dirname = entry.path().filename().u8string();
      if (!dirname.empty() && (dirname[0] == '.' || dirname[0] == '_')) {
        it.disable_recursion_pending();
      }
      continue;
    }

    if (!entry.is_regular_file(ec)) {
      continue;
    }

    tryAddFile(entry.path());
  }
}

void ArduinoEditorFrame::RefreshDiagnostics() {
  APP_DEBUG_LOG("FRM: RefreshDiagnostics(mode=%d)", m_clangSettings.diagnosticMode);

  switch (m_clangSettings.diagnosticMode) {
    case completeProject: {
      StartProcess(_("Project diagnostics evaluation..."), ID_PROCESS_DIAG_EVAL, ArduinoActivityState::Background);
      std::vector<SketchFileBuffer> files;
      CollectEditorSources(files);
      completion->RefreshProjectDiagnosticsAsync(files);
      break;
    }
    case translationUnit: {
      ArduinoEditor *ed = GetCurrentEditor();
      if (!ed) {
        return;
      }

      std::string filename = ed->GetFilePath();
      std::string code = ed->GetText();

      StartProcess(_("Diagnostic evaluation..."), ID_PROCESS_DIAG_EVAL, ArduinoActivityState::Background);
      completion->RefreshDiagnosticsAsync(filename, code);
      break;
    }
    default:
      APP_DEBUG_LOG("FRM: Diagnostic turned off; skiping RefreshDiagnostics()");
      ShowSingleDiagMessage(_("Diagnostics turned off."));
      break;
  }
}

void ArduinoEditorFrame::OnLibrariesUpdated(wxThreadEvent &evt) {
  StopProcess(ID_PROCESS_LOAD_LIBRARIES);

  bool ok = (evt.GetInt() == 1);
  if (!ok) {
    wxLogWarning(_("Failed to load Arduino libraries from arduino-cli."));
    return;
  }

  const auto &libs = arduinoCli->GetLibraries();

  APP_DEBUG_LOG("FRM: Available libraries: %d", libs.size());

  if (m_libManager) {
    m_libManager->RefreshLibraries();
  }

  // Invalidate not found libs
  m_notFoundHeaders.clear();
}

void ArduinoEditorFrame::OnLibrariesFound(wxThreadEvent &evt) {
  std::string header = wxToStd(evt.GetString());

  // remove from queue
  m_wantedHeaders.erase(header);

  bool ok = (evt.GetInt() == 1);

  if (ok) {
    auto libs = evt.GetPayload<std::vector<ArduinoLibraryInfo>>();

    if (libs.empty()) {
      m_notFoundHeaders.push_back(header);
    } else {
      libs.erase(
          std::remove_if(libs.begin(), libs.end(),
                         [&](const ArduinoLibraryInfo &lib) {
                           return !arduinoCli->IsLibraryArchitectureCompatible(lib.latest.architectures);
                         }),
          libs.end());

      if (!libs.empty()) {
        m_foundLibraryGroups.push_back({header, std::move(libs)});
      }
    }
  }

  if (m_wantedHeaders.empty()) {
    // all candidates collected
    StopProcess(ID_PROCESS_SEARCH_LIBRARIES);

    if (m_foundLibraryGroups.empty()) {
      return;
    }

    // If resolving is performed via compile commands
    // and we found at least one library that is installed but
    // still missing its header, we will start a project rebuild.
    if (m_clangSettings.resolveMode == compileCommandsResolver) {
      const auto it = std::find_if(
          m_foundLibraryGroups.begin(), m_foundLibraryGroups.end(),
          [&](const auto &group) {
            return std::any_of(group.libs.begin(), group.libs.end(),
                               [&](const auto &lib) {
                                 return arduinoCli->IsArduinoLibraryInstalled(lib);
                               });
          });

      if (it != m_foundLibraryGroups.end()) {
        auto header = it->header;
        m_queriedMissingHeaders.push_back(header);

        const wxString msg = wxString::Format(
            _("Header '%s' was not found, but it looks like it belongs to an installed Arduino library.\n\n"
              "In this mode, libraries are detected from the last project build, so newly added #include directives "
              "won't be picked up until you rebuild.\n\n"
              "Rebuild will regenerate the build configuration (including compile_commands.json) and attach libraries "
              "to this sketch.\n\n"
              "Rebuild now?"),
            wxString::FromUTF8(it->header));

        const int res = ModalMsgDialog(
            msg,
            _("Rebuild required"),
            wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);

        if (res == wxID_YES) {
          if (SaveAll()) {
            CleanProject();
          }
        }
        return;
      }
    }

    wxString msg;
    msg << _("The following missing headers are claimed to be provided by Arduino libraries:\n\n");

    constexpr size_t kMaxLibsPerHeader = 5;

    for (const auto &g : m_foundLibraryGroups) {
      wxString headerWx = wxString::FromUTF8(g.header.c_str());
      msg << wxT("- <") << headerWx << wxT(">\n");

      const size_t showCount = std::min(g.libs.size(), kMaxLibsPerHeader);

      for (size_t i = 0; i < showCount; ++i) {
        const auto &lib = g.libs[i];

        wxString nameWx = wxString::FromUTF8(lib.name.c_str());
        wxString sentWx = wxString::FromUTF8(lib.latest.sentence.c_str());

        msg << wxT("    \u2022 \"") << nameWx << wxT("\"");
        if (!sentWx.IsEmpty()) {
          msg << wxT(" \u2014 ") << sentWx;
        }
        msg << wxT("\n");
      }

      if (g.libs.size() > showCount) {
        const unsigned long more = (unsigned long)(g.libs.size() - showCount);
        msg << wxT("    \u2022 ") << wxString::Format(_("... and %lu more"), more) << wxT("\n");
      }

      msg << wxT("\n");
    }

    msg << _("\nNote: Arduino library metadata can be wrong. The Library Manager will be opened so you can choose what to install.\n\n");

    wxRichMessageDialog dlg(
        this,
        msg,
        _("Missing headers"),
        wxYES_NO | wxICON_INFORMATION);

    dlg.SetYesNoLabels(_("Open Library Manager"), _("Ignore"));
    dlg.SetSize(wxSize(820, 520));
    dlg.CentreOnParent();

    int res = dlg.ShowModal();

    for (const auto &g : m_foundLibraryGroups) {
      m_queriedMissingHeaders.push_back(g.header);
    }

    if (res != wxID_YES) {
      return;
    }

    std::vector<ArduinoLibraryInfo> uniqueToShow;
    std::unordered_set<std::string> seen;

    for (const auto &g : m_foundLibraryGroups) {
      for (const auto &lib : g.libs) {
        if (seen.insert(lib.name).second) {
          uniqueToShow.push_back(lib);
        }
      }
    }

    RequestShowLibraries(uniqueToShow);
  }
  // wait for next event
}

void ArduinoEditorFrame::OnInstalledLibrariesUpdated(wxThreadEvent &evt) {
  StopProcess(ID_PROCESS_LOAD_INSTALLED_LIBRARIES);

  bool ok = (evt.GetInt() == 1);
  if (!ok) {
    wxLogWarning(_("Failed to load installed Arduino libraries from arduino-cli."));
    return;
  }

  const auto &iLibs = arduinoCli->GetInstalledLibraries();

  APP_DEBUG_LOG("FRM: OnInstalledLibrariesUpdated (%zu libraries)", iLibs.size());

  if (m_libManager) {
    m_libManager->RefreshInstalledLibraries();
  }

  if (m_examplesFrame) {
    m_examplesFrame->RefreshLibraries();
  }

  // If the user installed/uninstalled/updated a library, we may have satisfied
  // a previously detected update. Drop such entries from m_librariesForUpdate.
  bool updatesChanged = false;

  if (!m_librariesForUpdate.empty()) {
    std::unordered_map<std::string, std::string> installedVer;
    installedVer.reserve(iLibs.size());

    for (const auto &li : iLibs) {
      if (!li.name.empty()) {
        installedVer[li.name] = li.latest.version; // installed version is stored here
      }
    }

    const size_t before = m_librariesForUpdate.size();

    m_librariesForUpdate.erase(
        std::remove_if(m_librariesForUpdate.begin(), m_librariesForUpdate.end(),
                       [&](const ArduinoLibraryInfo &u) {
                         // If the library is no longer installed, it can't be updated.
                         auto it = installedVer.find(u.name);
                         if (it == installedVer.end()) {
                           return true;
                         }

                         const std::string &have = it->second;
                         const std::string &need = u.latest.version;
                         if (have.empty() || need.empty()) {
                           return false; // keep it (unknown), conservative
                         }

                         // Remove if installed version >= version that was requested/expected.
                         return CompareVersions(have, need) >= 0;
                       }),
        m_librariesForUpdate.end());

    updatesChanged = (m_librariesForUpdate.size() != before);
  }

  if (updatesChanged) {
    UpdateStatusBarUpdates();
  }

  // Reset successful compile sum
  m_lastSuccessfulCompileCodeSum = 0;

  if (m_clangSettings.resolveMode == compileCommandsResolver) {
    if (m_firstInitCompleted) {
      // Here we will heuristically determine if the user has the library manager running/displayed and
      // from that we can conclude whether they could potentially have installed a library and thus trigger
      // this event. If so, we need to rebuild the project because the libraries used may have been changed by the sketch.

      if (m_libManager && m_libManager->IsShownOnScreen()) {
        CleanProject();
      }
    }
  } else {
    if (m_firstInitCompleted) {
      APP_DEBUG_LOG("FRM: cleaning project after lib install...");
      CleanProject();
    } else {
      ResolveLibrariesOrDiagnostics();
    }
  }
}

void ArduinoEditorFrame::OnResolvedLibrariesReady(wxThreadEvent &evt) {
  StopProcess(ID_PROCESS_RESOLVE_LIBRARIES);

  int ok = evt.GetInt();
  if (!ok) {
    return;
  }

  auto libs = evt.GetPayload<std::vector<ResolvedLibraryInfo>>();

  APP_DEBUG_LOG("FRM: OnResolvedLibrariesReady (%zu libraries)", libs.size());

  if (m_filesPanel) {
    m_filesPanel->UpdateResolvedLibraries(libs);
  }

  RefreshDiagnostics();
}

void ArduinoEditorFrame::OnShowLibraryManager(wxCommandEvent &WXUNUSED(evt)) {
  if (!m_libManager) {
    StartProcess(_("Loading libraries..."), ID_PROCESS_LOAD_LIBRARIES, ArduinoActivityState::Background);

    if (!m_librariesUpdated) {
      m_librariesUpdated = true;
      arduinoCli->LoadLibrariesAsync(this);
    }

    m_libManager = new ArduinoLibraryManagerFrame(this, arduinoCli, m_availableBoards, config, _("All"));
  }
  m_libManager->Show();
  m_libManager->Raise();
}

void ArduinoEditorFrame::OnInstalledCoresUpdated(wxThreadEvent &evt) {
  StopProcess(ID_PROCESS_LOAD_INSTALLED_CORES);
  StopProcess(ID_PROCESS_INSTALL_CORE);

  bool ok = (evt.GetInt() == 1);
  if (!ok) {
    wxLogWarning(_("Failed to load installed Arduino cores from arduino-cli."));
    return;
  }

  const auto &iCores = arduinoCli->GetCores();

  APP_DEBUG_LOG("FRM: cores count %d", iCores.size());

  if (m_coreManager) {
    m_coreManager->RefreshCores();
  }

  // If the user installed/uninstalled/updated a core, we may have satisfied
  // a previously detected update. Drop such entries from m_coresForUpdate.
  bool updatesChanged = false;

  if (!m_coresForUpdate.empty()) {
    std::unordered_map<std::string, std::string> installedVer;
    installedVer.reserve(iCores.size());

    for (const auto &c : iCores) {
      if (!c.id.empty() && !c.installedVersion.empty()) {
        installedVer[c.id] = c.installedVersion;
      }
    }

    const size_t before = m_coresForUpdate.size();

    m_coresForUpdate.erase(
        std::remove_if(m_coresForUpdate.begin(), m_coresForUpdate.end(),
                       [&](const ArduinoCoreInfo &u) {
                         // If the core is no longer installed, it can't be updated.
                         auto it = installedVer.find(u.id);
                         if (it == installedVer.end()) {
                           return true;
                         }

                         const std::string &have = it->second;
                         const std::string &need = u.latestVersion;
                         if (have.empty() || need.empty()) {
                           return false; // keep it (unknown), conservative
                         }

                         // Remove if installed version >= version that was requested/expected.
                         return CompareVersions(have, need) >= 0;
                       }),
        m_coresForUpdate.end());

    updatesChanged = (m_coresForUpdate.size() != before);
  }

  if (updatesChanged) {
    UpdateStatusBarUpdates();
  }

  StartProcess(_("Enumerating available boards..."), ID_PROCESS_LOAD_BOARDS, ArduinoActivityState::Background);
  arduinoCli->GetAvailableBoardsAsync(this);
}

void ArduinoEditorFrame::OnCoresLoaded(wxThreadEvent &evt) {
  StopProcess(ID_PROCESS_LOAD_CORES);

  bool ok = (evt.GetInt() == 1);
  if (!ok) {
    wxLogWarning(_("Failed to load Arduino cores from arduino-cli."));
    return;
  }

  APP_DEBUG_LOG("FRM: OnCoresLoaded() count=%d", arduinoCli->GetCores().size());

  if (m_coreManager) {
    m_coreManager->RefreshCores();
  }
}

void ArduinoEditorFrame::OnShowCoreManager(wxCommandEvent &WXUNUSED(evt)) {
  if (!m_coreManager) {
    m_coreManager = new ArduinoCoreManagerFrame(this, arduinoCli, config, _("All"));
  }

  m_coreManager->Show();
  m_coreManager->Raise();
}

void ArduinoEditorFrame::OnAvailableBoardsUpdated(wxThreadEvent &evt) {
  StopProcess(ID_PROCESS_LOAD_BOARDS);

  bool ok = (evt.GetInt() == 1);
  if (!ok) {
    wxLogWarning(_("Failed to load available boards from arduino-cli."));
    return;
  }

  m_availableBoards = evt.GetPayload<std::vector<ArduinoCoreBoard>>();

  if (m_libManager) {
    m_libManager->RefreshAvailableBoards(m_availableBoards);
  }

  RebuildBoardChoice();
  RefreshSerialPorts();
}

void ArduinoEditorFrame::ResolveLibrariesOrDiagnostics() {
  APP_DEBUG_LOG("FRM: ResolveLibrariesOrDiagnostics()");

  if (m_clangSettings.resolveMode == compileCommandsResolver) {
    // If the compile commands resolver is used, the libraries are
    // already evaluated in it, so we can start diagnostics straight away.
    RefreshDiagnostics();
    return;
  }

  StartProcess(_("Resolving libraries..."), ID_PROCESS_RESOLVE_LIBRARIES, ArduinoActivityState::Background);

  std::vector<SketchFileBuffer> files;
  CollectEditorSources(files);
  arduinoCli->GetResolvedLibrariesAsync(files, this);
}

void ArduinoEditorFrame::ScheduleDiagRefresh() {
  if (m_diagTimer.IsRunning()) {
    m_diagTimer.Stop();
  }

  if (m_diagView) {
    m_diagView->SetStale();
  }

  APP_DEBUG_LOG("FRM: ScheduleDiagRefresh()");
  m_diagTimer.Start(m_clangSettings.resolveDiagnosticsDelay, wxTIMER_ONE_SHOT);
}

void ArduinoEditorFrame::OnDiagTimer(wxTimerEvent &WXUNUSED(event)) {
  APP_DEBUG_LOG("FRM: OnDiagTimer()");

  ResolveLibrariesOrDiagnostics();
}

void ArduinoEditorFrame::RefreshSerialPorts() {

  m_portChoice->Clear();
  m_serialPorts.clear();

  if (!arduinoCli) {
    UpdateSerialMonitorAvailability();
    return;
  }

  m_serialPorts = arduinoCli->GetSerialPorts();
  for (const auto &p : m_serialPorts) {
    // we already have label as a nice description
    m_portChoice->Append(wxString::FromUTF8(p.label.c_str()));
  }

  wxString port = wxString::FromUTF8(arduinoCli->GetSerialPort());

  m_portChoice->SetStringSelection(port);

  // according to the current selection (or its absence) enable/disable the serial monitor
  UpdateSerialMonitorAvailability();
}

void ArduinoEditorFrame::UpdateSerialMonitorAvailability() {
  bool enable = false;

  if (m_portChoice) {
    int sel = m_portChoice->GetSelection();
    if (sel != wxNOT_FOUND &&
        sel >= 0 &&
        sel < static_cast<int>(m_serialPorts.size())) {

      const auto &info = m_serialPorts[sel];
      // Serial monitor only for classic "serial" ports
      enable = (info.protocol == "serial");
    }
  }

  if (m_menuBar) {
    m_menuBar->Enable(ID_MENU_SERIAL_MONITOR, enable);
  }

  if (m_serialMonitorButton) {
    m_serialMonitorButton->Enable(enable);
  }

  if (!enable && m_serialMonitor) {
    m_serialMonitor->Close();
    // m_serialMonitor resets the callback
  }
}

void ArduinoEditorFrame::OnAbout(wxCommandEvent &) {
  ArduinoAboutDialog dlg(this);
  dlg.ShowModal();
}

void ArduinoEditorFrame::OnClose(wxCloseEvent &event) {
  // We check if there is anything modified
  bool hasModified = false;
  for (auto *ed : GetAllEditors(/*onlyEditable=*/true)) {
    if (ed->IsModified()) {
      hasModified = true;
      break;
    }
  }

  if (hasModified) {
    wxMessageDialog dlg(
        this,
        _("Some files have unsaved changes.\nDo you want to save them before exiting?"),
        _("Confirm exit"),
        wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxICON_WARNING);

    dlg.SetYesNoCancelLabels(_("Save"), _("Discard"), _("Cancel"));

    int res = dlg.ShowModal();

    if (res == wxID_CANCEL) {
      event.Veto(); // we stay in the application
      return;
    }

    if (res == wxID_YES) {
      if (!SaveAll()) {
        ModalMsgDialog(_("Some files couldn't be saved."), _("Save error"));
        event.Veto(); // something went wrong -> do not close
        return;
      }
    }
  }

  if (arduinoCli) {
    arduinoCli->CancelAsyncOperations();
  }

  if (completion) {
    completion->CancelAsyncOperations();
  }

  // Stop the timer so that events don't fly around here
  if (m_diagTimer.IsRunning()) {
    m_diagTimer.Stop();
  }

  SaveWorkspaceState();

  // Save the perspective
  if (config) {
    wxString perspective = m_auiManager.SavePerspective();
    config->Write(wxT("Layout.Perspective"), perspective);

    // Window geometry
    bool isMax = IsMaximized();
    config->Write(wxT("Layout.Maximized"), (long)(isMax ? 1 : 0));

    if (!isMax) {
      SaveWindowSize(wxT("Layout"), this, config);
    }

    config->Flush();
  }

  event.Skip();
}

void ArduinoEditorFrame::BindEvents() {
  Bind(EVT_CLANG_ARGS_READY, &ArduinoEditorFrame::OnClangArgsReady, this);

  m_statusBar->Bind(wxEVT_SIZE, &ArduinoEditorFrame::OnStatusBarSize, this);
  m_statusBar->Bind(wxEVT_LEFT_UP, &ArduinoEditorFrame::OnStatusBarLeftUp, this);
  m_statusBar->Bind(wxEVT_MOTION, &ArduinoEditorFrame::OnStatusBarMotion, this);
  m_statusBar->Bind(wxEVT_LEAVE_WINDOW, &ArduinoEditorFrame::OnStatusBarLeave, this);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnLibraryUpdatesFromStatusBar, this, ID_MENU_LIBRARY_MANAGER_UPDATES);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnCoreUpdatesFromStatusBar, this, ID_MENU_CORE_MANAGER_UPDATES);

  m_statusBar->Bind(EVT_PROCESS_TERMINATE_REQUEST, &ArduinoEditorFrame::OnCliProcessKill, this);
  m_boardChoice->Bind(wxEVT_CHOICE, &ArduinoEditorFrame::OnBoardChoiceChanged, this);
  m_optionsButton->Bind(wxEVT_BUTTON, &ArduinoEditorFrame::OnChangeBoardOptions, this);

  m_notebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &ArduinoEditorFrame::OnNotebookPageChanged, this);
  m_notebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGING, &ArduinoEditorFrame::OnNotebookPageChanging, this);
  m_notebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &ArduinoEditorFrame::OnNotebookPageClose, this);

  m_portChoice->Bind(wxEVT_CHOICE, &ArduinoEditorFrame::OnSerialPortChanged, this);

  Bind(wxEVT_CLOSE_WINDOW, &ArduinoEditorFrame::OnClose, this);
  Bind(EVT_DIAGNOSTICS_UPDATED, &ArduinoEditorFrame::OnDiagnosticsUpdated, this);
  Bind(wxEVT_TIMER, &ArduinoEditorFrame::OnDiagTimer, this, m_diagTimer.GetId());
  Bind(wxEVT_TIMER, &ArduinoEditorFrame::OnReturnBottomPageTimer, this, m_returnBottomPageTimer.GetId());

  m_auiManager.Bind(wxEVT_AUI_PANE_CLOSE, &ArduinoEditorFrame::OnAuiPaneClose, this);

  Bind(EVT_ARD_DIAG_JUMP, &ArduinoEditorFrame::OnDiagJumpFromView, this);
  Bind(EVT_ARD_DIAG_SOLVE_AI, &ArduinoEditorFrame::OnDiagSolveAiFromView, this);

  Bind(EVT_COMMANDLINE_OUTPUT_MSG, &ArduinoEditorFrame::OnCmdLineOutput, this);

  m_refreshPortsButton->Bind(wxEVT_BUTTON, &ArduinoEditorFrame::OnRefreshSerialPorts, this);
  m_serialMonitorButton->Bind(wxEVT_BUTTON, &ArduinoEditorFrame::OnOpenSerialMonitor, this);

  m_buildButton->Bind(wxEVT_BUTTON, &ArduinoEditorFrame::OnProjectBuild, this);
  m_uploadButton->Bind(wxEVT_BUTTON, &ArduinoEditorFrame::OnProjectUpload, this);

  Bind(EVT_LIBRARIES_UPDATED, &ArduinoEditorFrame::OnLibrariesUpdated, this);
  Bind(EVT_INSTALLED_LIBRARIES_UPDATED, &ArduinoEditorFrame::OnInstalledLibrariesUpdated, this);
  Bind(EVT_LIBRARIES_FOUND, &ArduinoEditorFrame::OnLibrariesFound, this);

  Bind(EVT_RESOLVED_LIBRARIES_READY, &ArduinoEditorFrame::OnResolvedLibrariesReady, this);

  Bind(EVT_CORES_UPDATED, &ArduinoEditorFrame::OnInstalledCoresUpdated, this);
  Bind(EVT_CORES_LOADED, &ArduinoEditorFrame::OnCoresLoaded, this);

  Bind(EVT_AVAILABLE_BOARDS_UPDATED, &ArduinoEditorFrame::OnAvailableBoardsUpdated, this);

  Bind(wxEVT_SYS_COLOUR_CHANGED, &ArduinoEditorFrame::OnSysColoursChanged, this);

  Bind(EVT_STOP_PROCESS, [this](wxThreadEvent &ev) {
    StopProcess(ev.GetInt());
  });

  if (m_filesPanel) {
    int srcId = m_filesPanel->GetId();

    Bind(EVT_SKETCH_TREE_OPEN_ITEM,
         &ArduinoEditorFrame::OnSketchTreeOpenItem,
         this,
         srcId);

    Bind(EVT_SKETCH_TREE_NEW_FILE,
         &ArduinoEditorFrame::OnSketchTreeNewFile,
         this,
         srcId);

    Bind(EVT_SKETCH_TREE_NEW_FOLDER,
         &ArduinoEditorFrame::OnSketchTreeNewFolder,
         this,
         srcId);

    Bind(EVT_SKETCH_TREE_DELETE,
         &ArduinoEditorFrame::OnSketchTreeDelete,
         this,
         srcId);

    Bind(EVT_SKETCH_TREE_RENAME,
         &ArduinoEditorFrame::OnSketchTreeRename,
         this,
         srcId);

    Bind(EVT_SKETCH_TREE_OPEN_EXTERNALLY,
         &ArduinoEditorFrame::OnSketchTreeOpenExternally,
         this,
         srcId);
  }

  Bind(wxEVT_AUINOTEBOOK_TAB_RIGHT_UP, &ArduinoEditorFrame::OnNotebookTabRightUp, this, m_notebook->GetId());
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnTabMenuClose, this, ID_TABMENU_CLOSE);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnTabMenuCloseOthers, this, ID_TABMENU_CLOSE_OTHERS);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnTabMenuCloseAll, this, ID_TABMENU_CLOSE_ALL);
  Bind(EVT_ARD_SYMBOL_ACTIVATED, &ArduinoEditorFrame::OnSymbolActivated, this);

  Bind(wxEVT_TIMER, &ArduinoEditorFrame::OnUpdatesLibsCoresAvailable, this, m_updatesTimer.GetId());
  Bind(EVT_LIBS_UPDATES_AVAILABLE, &ArduinoEditorFrame::OnLibrariesUpdatesAvailable, this);
  Bind(EVT_CORES_UPDATES_AVAILABLE, &ArduinoEditorFrame::OnCoresUpdatesAvailable, this);
}

void ArduinoEditorFrame::LoadConfig() {
  m_clangSettings.Load(config);
  m_aiSettings.Load(config);

  ApplySettings(m_clangSettings);
  ApplySettings(m_aiSettings);
}

wxMenuBar *ArduinoEditorFrame::CreateMenuBar() {
  wxMenu *fileMenu = new wxMenu();

  // File
  AddMenuItemWithArt(fileMenu,
                     ID_MENU_NEW_SKETCH,
                     _("New Sketch...\tCtrl+N"),
                     _("Create new sketch"),
                     wxAEArt::New);

  AddMenuItemWithArt(fileMenu,
                     ID_MENU_OPEN_SKETCH,
                     _("Open Sketch...\tCtrl+O"),
                     _("Open an existing Arduino sketch directory"),
                     wxAEArt::FileOpen);

  // Open recent (wxMenu does not have Bitmap for submenu, we leave it without an icon)
  m_recentMenu = new wxMenu();
  fileMenu->AppendSubMenu(m_recentMenu, _("Open recent"));
  RebuildRecentMenu();

  // Sketches submenu - also without an icon
  m_sketchesDirMenu = new wxMenu();
  fileMenu->AppendSubMenu(m_sketchesDirMenu, _("Sketches"));
  RebuildSketchesDirMenu();

  AddMenuItemWithArt(fileMenu,
                     ID_MENU_SKETCH_EXAMPLES,
                     _("Examples...\tCtrl+E"),
                     _("Browse and preview Arduino library examples"),
                     wxAEArt::ListView);

  AddMenuItemWithArt(fileMenu,
                     ID_MENU_SAVE,
                     _("Save\tCtrl+S"),
                     _("Save the current file"),
                     wxAEArt::FileSave);

  AddMenuItemWithArt(fileMenu,
                     ID_MENU_SAVE_ALL,
                     _("Save All\tCtrl+Shift+S"),
                     _("Save all opened files"),
                     wxAEArt::FileSaveAs);

  fileMenu->AppendSeparator();

  AddMenuItemWithArt(fileMenu,
                     wxID_PREFERENCES,
                     _("Settings..."),
                     _("Open configuration (cli, fonts, colors and behavior)"),
#ifdef __WXMAC__
                     // In macOS, the item appears in the system menu, where icons don't like it.
                     wxEmptyString
#else
                     wxAEArt::Settings
#endif
  );

  fileMenu->AppendSeparator();

  AddMenuItemWithArt(fileMenu,
                     wxID_EXIT,
                     _("Exit"),
                     _("Exit the application"),
#ifdef __WXMAC__
                     wxEmptyString
#else
                     wxAEArt::Quit
#endif
  );

  // Tools / Navigation menu
  wxMenu *navMenu = new wxMenu();
  AddMenuItemWithArt(navMenu,
                     ID_MENU_NAV_BACK,
                     _("Back\tAlt-Left"),
                     _("Jump back to the previous cursor navigation location"),
                     wxAEArt::GoBack);

  AddMenuItemWithArt(navMenu,
                     ID_MENU_NAV_FORWARD,
                     _("Forward\tAlt-Right"),
                     _("Go forward in navigation history"),
                     wxAEArt::GoForward);

  AddMenuItemWithArt(navMenu,
                     ID_MENU_NAV_FIND_SYMBOL,
                     _("Find symbol...\tCtrl-Shift-O"),
                     _("Find and navigate to any symbol in the current sketch"),
                     wxAEArt::Find);

  navMenu->AppendSeparator();

  AddMenuItemWithArt(navMenu,
                     ID_MENU_LIBRARY_MANAGER,
                     _("Library manager...\tCtrl+Shift+L"),
                     _("Manage Arduino libraries"),
                     wxAEArt::ListView);

  AddMenuItemWithArt(navMenu,
                     ID_MENU_CORE_MANAGER,
                     _("Boards manager...\tCtrl+Shift+B"),
                     _("Manage Arduino boards (cores)"),
                     wxAEArt::DevBoard);

  navMenu->AppendSeparator();
  AddMenuItemWithArt(navMenu,
                     ID_MENU_SERIAL_MONITOR,
                     _("Serial monitor"),
                     _("Shows serial monitor of connected board"),
                     wxAEArt::SerMon);

  // Project menu
  wxMenu *projectMenu = new wxMenu();
  AddMenuItemWithArt(projectMenu,
                     ID_MENU_PROJECT_BUILD,
                     _("Build\tCtrl+R"),
                     _("Build sketch using arduino-cli"),
                     wxAEArt::Check);

  AddMenuItemWithArt(projectMenu,
                     ID_MENU_PROJECT_UPLOAD,
                     _("Upload\tCtrl+U"),
                     _("Upload sketch using arduino-cli"),
                     wxAEArt::Play);

  projectMenu->AppendSeparator();

  AddMenuItemWithArt(projectMenu,
                     ID_MENU_PROJECT_CLEAN,
                     _("Clean\tShift+Ctrl+K"),
                     _("Remove build cache and regenerate clang properties"),
                     wxAEArt::Delete);

  projectMenu->AppendSeparator();

  // View menu - check items with icons
  wxMenu *viewMenu = new wxMenu();
  wxMenuItem *outputItem =
      viewMenu->AppendCheckItem(ID_MENU_VIEW_OUTPUT,
                                _("Output"),
                                _("Output window problems / build"));
  {
    wxBitmapBundle bmp = AEGetArtBundle(wxAEArt::ReportView);
    if (bmp.IsOk()) {
      outputItem->SetBitmap(bmp);
    }
  }

  wxMenuItem *sketchBrowserItem =
      viewMenu->AppendCheckItem(ID_MENU_VIEW_SKETCHES,
                                _("Sketch browser"),
                                _("Show browser for Sketch directory"));
  {
    wxBitmapBundle bmp = AEGetArtBundle(wxAEArt::FolderOpen);
    if (bmp.IsOk()) {
      sketchBrowserItem->SetBitmap(bmp);
    }
  }

  wxMenuItem *symbolsItem =
      viewMenu->AppendCheckItem(ID_MENU_VIEW_SYMBOLS,
                                _("Class browser"),
                                _("Show symbols tree for current editor"));
  {
    wxBitmapBundle bmp = AEGetArtBundle(wxAEArt::ListView);
    if (bmp.IsOk()) {
      symbolsItem->SetBitmap(bmp);
    }
  }

  wxMenuItem *aiItem =
      viewMenu->AppendCheckItem(ID_MENU_VIEW_AI,
                                _("AI assistant"),
                                _("Show AI chat panel"));
  {
    wxBitmapBundle bmp = AEGetArtBundle(wxAEArt::Tip);
    if (bmp.IsOk()) {
      aiItem->SetBitmap(bmp);
    }
  }

  // Help menu
  wxMenu *helpMenu = new wxMenu();

  AddMenuItemWithArt(helpMenu,
                     ID_MENU_CHECK_FOR_UPDATES,
                     _("Check for Updates..."),
                     _("Check GitHub for a newer version of Arduino Editor"),
                     wxAEArt::CheckForUpdates);

  helpMenu->AppendSeparator();

  AddMenuItemWithArt(helpMenu,
                     wxID_ABOUT,
                     _("&About Arduino Editor...\tShift-F1"),
                     _("About Arduino Editor"),
#ifdef __WXMAC__
                     // In macOS, the item appears in the system menu, where icons don't like it.
                     wxEmptyString
#else
                     wxAEArt::Information
#endif
  );

  wxMenuBar *m_menuBar = new wxMenuBar();
  m_menuBar->Append(fileMenu, _("&File"));
  m_menuBar->Append(projectMenu, _("&Project"));
  m_menuBar->Append(navMenu, _("&Tools"));
  m_menuBar->Append(viewMenu, _("&View"));
  m_menuBar->Append(helpMenu, _("&Help"));

  // Bind events
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnNewSketch, this, ID_MENU_NEW_SKETCH);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnOpenSketch, this, ID_MENU_OPEN_SKETCH);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnSave, this, ID_MENU_SAVE);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnSaveAll, this, ID_MENU_SAVE_ALL);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnExit, this, wxID_EXIT);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnEditorSettings, this, wxID_PREFERENCES);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnNavBack, this, ID_MENU_NAV_BACK);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnNavForward, this, ID_MENU_NAV_FORWARD);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnFindSymbol, this, ID_MENU_NAV_FIND_SYMBOL);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnShowLibraryManager, this, ID_MENU_LIBRARY_MANAGER);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnShowCoreManager, this, ID_MENU_CORE_MANAGER);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnOpenSerialMonitor, this, ID_MENU_SERIAL_MONITOR);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnProjectClean, this, ID_MENU_PROJECT_CLEAN);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnProjectBuild, this, ID_MENU_PROJECT_BUILD);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnProjectUpload, this, ID_MENU_PROJECT_UPLOAD);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnAbout, this, wxID_ABOUT);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnViewOutput, this, ID_MENU_VIEW_OUTPUT);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnViewFiles, this, ID_MENU_VIEW_SKETCHES);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnViewSymbols, this, ID_MENU_VIEW_SYMBOLS);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnViewAi, this, ID_MENU_VIEW_AI);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnOpenRecent, this, ID_MENU_OPEN_RECENT_FIRST, ID_MENU_OPEN_RECENT_LAST);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnClearRecent, this, ID_MENU_OPEN_RECENT_CLEAR);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnOpenSketchFromDir, this, ID_MENU_SKDIR_FIRST, ID_MENU_SKDIR_LAST);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnShowExamples, this, ID_MENU_SKETCH_EXAMPLES);
  Bind(wxEVT_MENU, &ArduinoEditorFrame::OnCheckForUpdates, this, ID_MENU_CHECK_FOR_UPDATES);

  return m_menuBar;
}

void ArduinoEditorFrame::OnNavBack(wxCommandEvent &WXUNUSED(event)) {
  GoBackInNavigation();
}

void ArduinoEditorFrame::OnNavForward(wxCommandEvent &WXUNUSED(event)) {
  GoForwardInNavigation();
}

int ArduinoEditorFrame::ModalMsgDialog(const wxString &message, const wxString &caption, int styles) {
  wxRichMessageDialog dlg(this, message, caption, styles);
  return dlg.ShowModal();
}

bool ArduinoEditorFrame::IsSupportedExtension(const wxString &ext) {
  return (ext == wxT("c") || ext == wxT("h") ||
          ext == wxT("cpp") || ext == wxT("hpp") || ext == wxT("cxx") ||
          ext == wxT("ino"));
}

void ArduinoEditorFrame::InitComponents() {
  EditorSettings settings;
  settings.Load(config);

  Freeze();

  // Status bar + menu
  m_statusBar = CreateStatusBar(3);

  int widths[3] = {
      -1,
      250,
      32};
  m_statusBar->SetStatusWidths(3, widths);

  m_statusBar->SetStatusText(wxEmptyString, 1);

  if (m_statusBar) {
    m_indic = new ArduinoActivityDotCtrl(m_statusBar, wxID_ANY);
    m_indic->EnablePulse(true);
    m_indic->ApplySettings(settings);
  }

  m_menuBar = CreateMenuBar();
  SetMenuBar(m_menuBar);

  // ------------------------------------------------------
  // CENTER PANEL: toolbar + main notebook
  // ------------------------------------------------------
  wxPanel *centerPanel = new wxPanel(this);
  wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

  // ---- TOP TOOLBAR PANEL (inside centerPanel) ---------
  wxPanel *toolbarPanel = new wxPanel(centerPanel);
  wxBoxSizer *toolbarSizer = new wxBoxSizer(wxHORIZONTAL);

  // --- Build / Upload toolbar buttons (wxBitmapButton) ---
  auto addToolBmpBtn = [&](wxBitmapButton *&btn, const wxArtID &art, const wxString &tip) {
    wxBitmapBundle bb = AEGetArtBundle(art);

    btn = new wxBitmapButton(toolbarPanel, wxID_ANY, bb, wxDefaultPosition, wxDefaultSize);

    btn->SetToolTip(tip);

    toolbarSizer->Add(btn, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
  };

  // choice with history of used boards + select board option
  m_boardChoice = new wxChoice(toolbarPanel, wxID_ANY, wxDefaultPosition, wxSize(300, -1));
  toolbarSizer->Add(m_boardChoice, 1, wxALIGN_CENTER_VERTICAL, 0);

  // board options
  addToolBmpBtn(m_optionsButton, wxAEArt::Settings, _("Development board options"));
  toolbarSizer->AddSpacer(5);
  addToolBmpBtn(m_buildButton, wxAEArt::Check, _("Build sketch (Ctrl+R)"));
  toolbarSizer->AddSpacer(5);
  addToolBmpBtn(m_uploadButton, wxAEArt::Play, _("Upload sketch (Ctrl+U)"));

  m_portChoice = new wxChoice(toolbarPanel, wxID_ANY);
  m_portChoice->SetMinSize(wxSize(300, -1));
  toolbarSizer->Add(m_portChoice, 1, wxALIGN_CENTER_VERTICAL, 0);

  addToolBmpBtn(m_refreshPortsButton, wxAEArt::Refresh, _("Refresh port list"));
  addToolBmpBtn(m_serialMonitorButton, wxAEArt::SerMon, _("Open Serial Monitor"));

  // fill from history (LoadBoardHistory already ran in the constructor)
  RebuildBoardChoice();

  toolbarPanel->SetSizer(toolbarSizer);

  // ---- MAIN EDITOR NOTEBOOK (CENTER) -------------------
  long nbStyle =
      wxAUI_NB_TOP |
      wxAUI_NB_TAB_MOVE |
      wxAUI_NB_WINDOWLIST_BUTTON
#ifndef __WXMAC__
      // This doesn't work on MAC, so we won't put it there. The options are still available in the popup menu.
      | wxNB_MULTILINE | wxAUI_NB_CLOSE_ON_ALL_TABS
#endif
      ;

  m_notebook = new wxAuiNotebook(centerPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, nbStyle);

  mainSizer->Add(toolbarPanel, 0, wxEXPAND);
  mainSizer->Add(m_notebook, 1, wxEXPAND);

  centerPanel->SetSizer(mainSizer);

  m_tabImageList = CreateNotebookPageImageList(m_notebook->GetForegroundColour());
  m_notebook->AssignImageList(m_tabImageList);

  // ------------------------------------------------------
  // BOTTOM NOTEBOOK (Diagnostics / Console) - still this child
  // ------------------------------------------------------
  m_bottomNotebook = new wxNotebook(this, wxID_ANY);

  // Diagnostics view (new)
  m_diagView = new ArduinoDiagnosticsView(m_bottomNotebook, config);

  // Build / upload console
  m_buildOutputCtrl = new wxTextCtrl(
      m_bottomNotebook, wxID_ANY, wxEmptyString,
      wxDefaultPosition, wxDefaultSize,
      wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);

  m_buildOutputCtrl->SetFont(settings.GetFont());

  m_bottomNotebook->AddPage(m_diagView, _("Problems"));
  m_bottomNotebook->AddPage(m_buildOutputCtrl, _("CLI"));

  m_lastBottomNotebookSelectedPage = wxNOT_FOUND;

  // ------------------------------------------------------
  // SKETCH FILES PANE (AUI on the left) - tree of the current sketch
  // ------------------------------------------------------
  m_filesPanel = new SketchFilesPanel(this);

  // ------------------------------------------------------
  // AI PANEL (AUI on the right) - initially hidden
  // ------------------------------------------------------
  m_aiPanel = new ArduinoAiChatPanel(this, &m_aiGlobalActions, config);

  // ------------------------------------------------------
  // CLASS BROWSER PANE (AUI on the left, under Sketch files)
  // ------------------------------------------------------
  m_classBrowser = new ArduinoClassBrowserPanel(this);

  // ------------------------------------------------------
  // AUI MANAGER: centerPanel + bottomNotebook
  // ------------------------------------------------------
  m_auiManager.SetManagedWindow(this);

  // entire center (toolbar + editors) as a single pane
  m_auiManager.AddPane(
      centerPanel,
      wxAuiPaneInfo()
          .Name(wxT("center"))
          .CenterPane()
          .PaneBorder(false));

  // left "Sketches" pane - floatable + closable, hidden by default
  m_auiManager.AddPane(
      m_filesPanel,
      wxAuiPaneInfo()
          .Name(wxT("files"))
          .Left()
          .Caption(_("Sketch files"))
          .BestSize(260, -1)
          .CloseButton(true)
          .MaximizeButton(false)
          .Floatable(true)
          .Show());

  // left "Symbols" pane - below Sketch files
  if (m_classBrowser) {
    m_auiManager.AddPane(
        m_classBrowser,
        wxAuiPaneInfo()
            .Name(wxT("symbols"))
            .Left()
            .Caption(_("Symbols"))
            .BestSize(260, 240)
            .MinSize(200, 120)
            .CloseButton(true)
            .MaximizeButton(false)
            .Floatable(true)
            .Dockable(true)
            .Show(true)
            .Row(1));
  }

  // bottom "Output" pane - floatable + closable
  m_auiManager.AddPane(
      m_bottomNotebook,
      wxAuiPaneInfo()
          .Name(wxT("output"))
          .Bottom()
          .Caption(_("Output"))
          .BestSize(-1, 200)
          .CloseButton(true)
          .MaximizeButton(false)
          .Floatable(true)
          .Show());

  // AI chat panel
  if (m_aiPanel) {
    m_auiManager.AddPane(m_aiPanel,
                         wxAuiPaneInfo()
                             .Name(wxT("ai"))
                             .Caption(_("AI assistant"))
                             .Right()
                             .Dockable(true)
                             .Floatable(true)
                             .BestSize(320, 640)
                             .MinSize(200, 150)
                             .CloseButton(true)
                             .MaximizeButton(false)
                             .Show(false));
  }

  // Let's try to load the saved perspective
  if (config) {
    wxString perspective;
    if (config->Read(wxT("Layout.Perspective"), &perspective) && !perspective.empty()) {
      m_auiManager.LoadPerspective(perspective);

      for (auto &p : m_auiManager.GetAllPanes()) {
        if (p.name == wxT("files")) {
          p.Caption(_("Sketch files"));
        } else if (p.name == wxT("symbols")) {
          p.Caption(_("Class browser"));
        } else if (p.name == wxT("output")) {
          p.Caption(_("Output"));
        } else if (p.name == wxT("ai")) {
          p.Caption(_("AI assistant"));
        }
      }
    }
  }

  m_auiManager.Update();

  wxAuiPaneInfo &pane = m_auiManager.GetPane(wxT("output"));
  bool visible = pane.IsOk() && pane.IsShown();
  m_menuBar->Check(ID_MENU_VIEW_OUTPUT, visible);

  wxAuiPaneInfo &skPane = m_auiManager.GetPane(wxT("files"));
  bool sketchesVisible = skPane.IsOk() && skPane.IsShown();
  m_menuBar->Check(ID_MENU_VIEW_SKETCHES, sketchesVisible);

  wxAuiPaneInfo &symPane = m_auiManager.GetPane(wxT("symbols"));
  bool symbolsVisible = symPane.IsOk() && symPane.IsShown();
  m_menuBar->Check(ID_MENU_VIEW_SYMBOLS, symbolsVisible);

  wxAuiPaneInfo &aiPane = m_auiManager.GetPane(wxT("ai"));
  bool aiVisible = aiPane.IsOk() && aiPane.IsShown();
  if (!m_aiSettings.enabled && aiPane.IsOk()) {
    aiPane.Show(false);
    aiVisible = false;
    m_auiManager.Update();
  }

  if (m_menuBar) {
    m_menuBar->Check(ID_MENU_VIEW_AI, aiVisible);
    m_menuBar->Enable(ID_MENU_VIEW_AI, m_aiSettings.enabled);
  }

  toolbarPanel->Layout();
  LayoutStatusBarIndicator();

  Thaw();
}

void ArduinoEditorFrame::PushNavLocation(const std::string &file, int line, int column) {
  std::string normFile = file;

  // at any new jump we clear the forward stack
  m_navForwardStack.clear();

  APP_DEBUG_LOG("FRM: PushNavLocation: file=%s, line=%d, column=%d", normFile.c_str(), line, column);
  if (m_navBackStack.size() >= 100) {
    m_navBackStack.erase(m_navBackStack.begin());
  }
  m_navBackStack.push_back(JumpTarget{normFile, line, column});
}

std::string ArduinoEditorFrame::NormalizeFilename(const std::string &filename) const {
  return ::NormalizeFilename(arduinoCli->GetSketchPath(), filename);
}

std::string ArduinoEditorFrame::RelativizeFilename(const std::string &filename) const {
  return ::StripFilename(arduinoCli->GetSketchPath(), filename);
}

ArduinoEditor *ArduinoEditorFrame::FindEditorWithFile(const std::string &filename, bool allowCreate) {
  if (!m_notebook) {
    return nullptr;
  }

  // search opened files in notebook
  std::string targetFile = NormalizeFilename(filename);
  APP_DEBUG_LOG("FRM: FindEditorWithFile() filename=%s", targetFile.c_str());

  int pageCount = m_notebook->GetPageCount();
  for (int i = 0; i < pageCount; ++i) {
    auto *editor = dynamic_cast<ArduinoEditor *>(m_notebook->GetPage(i));
    if (!editor) {
      continue;
    }

    std::string edfn = editor->GetFilePath();
    if (edfn == targetFile) {
      return editor;
    }
  }

  if (!allowCreate) {
    APP_DEBUG_LOG("FRM: FindEditorWithFile() editor not opened and allowCreate=false");
    return nullptr;
  }

  // If the files are not opened in another way and are not part of the project,
  // we will not open them with this method.
  if (!IsProjectFile(targetFile)) {
    APP_DEBUG_LOG("FRM: file %s is not project file; deny page open", targetFile.c_str());
    return nullptr;
  }

  std::error_code ec;
  const fs::path p = fs::u8path(targetFile);

  if (fs::exists(p, ec) && !ec && fs::is_regular_file(p)) {
    std::string basename = RelativizeFilename(targetFile);

    auto *newEdit = new ArduinoEditor(m_notebook,
                                      arduinoCli,
                                      completion,
                                      config,
                                      /*filename=*/basename,
                                      /*filepath=*/targetFile);

    newEdit->ApplySettings(m_clangSettings);
    newEdit->ApplySettings(m_aiSettings);

    m_notebook->AddPage(newEdit, wxString::FromUTF8(basename), /*select=*/false, IMLI_NOTEBOOK_EMPTY);
    m_notebook->SetPageToolTip(m_notebook->GetPageCount() - 1, wxString::FromUTF8(targetFile));
    return newEdit;
  }

  return nullptr;
}

void ArduinoEditorFrame::HandleGoToLocation(const JumpTarget &tgt) {
  unsigned line = tgt.line;
  unsigned column = tgt.column;

  std::error_code ec;
  const fs::path p = fs::u8path(NormalizeFilename(tgt.file));

  if (!fs::exists(p, ec) || ec || !fs::is_regular_file(p)) {
    APP_DEBUG_LOG("FRM: HandleGoToLocation: file does not exist (or not accessible): %s", tgt.file.c_str());
    return;
  }

  APP_DEBUG_LOG("FRM: HandleGoToLocation(file=%s, line=%u, column=%u)",
                tgt.file.c_str(), line, column);

  CreateEditorForFile(tgt.file, line, column);
}

ArduinoEditor *ArduinoEditorFrame::CreateEditorForFile(const std::string &filePath, int lineToGo, int columnToGo) {
  std::string filePathNorm = NormalizeFilename(filePath);

  APP_DEBUG_LOG("FRM: CreateEditorForFile(%s, lineToGo=%d, columnToGo=%d)", filePathNorm.c_str(), lineToGo, columnToGo);

  // first we try to find an existing editor by "logical" name
  ArduinoEditor *edit = FindEditorWithFile(filePathNorm);
  if (edit) {
    ActivateEditor(edit, lineToGo, columnToGo);
    return edit;
  }

  bool projectFile = IsProjectFile(filePathNorm);
  if (!projectFile && !m_clangSettings.openSourceFilesInside) {
    m_clangSettings.OpenExternalSourceFile(wxString::FromUTF8(filePathNorm), lineToGo);
    return nullptr;
  }

  std::string basename = RelativizeFilename(filePathNorm);
  std::string tabTitle = basename;

  // is project file or external file
  if (!projectFile) {
    tabTitle = fs::path(filePathNorm).filename().string();
  }

  auto *newEdit = new ArduinoEditor(m_notebook,
                                    arduinoCli,
                                    completion,
                                    config,
                                    /*filename=*/basename,
                                    /*filepath=*/filePathNorm);

  newEdit->ApplySettings(m_clangSettings);
  newEdit->ApplySettings(m_aiSettings);

  m_notebook->AddPage(newEdit, wxString::FromUTF8(tabTitle), true, IMLI_NOTEBOOK_EMPTY);
  m_notebook->SetPageToolTip(m_notebook->GetPageCount() - 1, wxString::FromUTF8(filePathNorm));
  newEdit->SetReadOnly(!projectFile);
  newEdit->Goto(lineToGo, columnToGo);
  return newEdit;
}

void ArduinoEditorFrame::GoBackInNavigation() {
  while (!m_navBackStack.empty()) {
    JumpTarget loc = m_navBackStack.back();
    m_navBackStack.pop_back();

    ArduinoEditor *editor = FindEditorWithFile(loc.file, /*allowCreate=*/true);
    if (editor) {

      // save the current position (sketch and external) to the forward stack
      ArduinoEditor *curEd =
          m_notebook
              ? dynamic_cast<ArduinoEditor *>(m_notebook->GetCurrentPage())
              : nullptr;

      if (curEd) {
        JumpTarget cur;
        cur.file = curEd->GetFilePath();
        curEd->GetCurrentCursor(cur.line, cur.column);
        m_navForwardStack.push_back(cur);
      }

      ActivateEditor(editor, loc.line, loc.column);
      break;
    }
  }
}

void ArduinoEditorFrame::GoForwardInNavigation() {
  while (!m_navForwardStack.empty()) {
    JumpTarget loc = m_navForwardStack.back();
    m_navForwardStack.pop_back();

    ArduinoEditor *editor = FindEditorWithFile(loc.file, /*allowCreate=*/true);
    if (editor) {

      // symmetry to GoBackInNavigation - save the current position
      ArduinoEditor *curEd =
          m_notebook
              ? dynamic_cast<ArduinoEditor *>(m_notebook->GetCurrentPage())
              : nullptr;

      if (curEd) {
        JumpTarget cur;
        cur.file = curEd->GetFilePath();
        curEd->GetCurrentCursor(cur.line, cur.column);

        if (m_navBackStack.size() >= 100) {
          m_navBackStack.erase(m_navBackStack.begin());
        }
        m_navBackStack.push_back(cur);
      }

      ActivateEditor(editor, loc.line, loc.column);
      break;
    }
    // If external file, ignore and continue
  }
}

void ArduinoEditorFrame::ActivateEditor(ArduinoEditor *editor, int line, int column) {
  for (size_t i = 0; i < m_notebook->GetPageCount(); ++i) {
    auto *ed = dynamic_cast<ArduinoEditor *>(m_notebook->GetPage(i));
    if (!ed)
      continue;

    if (ed == editor) {
      m_notebook->SetSelection(i);
      UpdateAiGlobalEditor();
      UpdateClassBrowserEditor();
      ed->Goto(line, column);
      ed->SetFocus();
      ed->FlashLine(line);
      break;
    }
  }
}

void ArduinoEditorFrame::ShowOutputPane(bool show) {
  wxAuiPaneInfo &pane = m_auiManager.GetPane(wxT("output"));
  if (!pane.IsOk())
    return;

  pane.Show(show);
  m_auiManager.Update();
}

void ArduinoEditorFrame::ShowSymbolsPane(bool show) {
  wxAuiPaneInfo &pane = m_auiManager.GetPane(wxT("symbols"));
  if (!pane.IsOk())
    return;

  pane.Show(show);
  m_auiManager.Update();

  UpdateClassBrowserEditor();
}

void ArduinoEditorFrame::OnViewSymbols(wxCommandEvent &evt) {
  ShowSymbolsPane(evt.IsChecked());
}

void ArduinoEditorFrame::OnToggleOutput(wxCommandEvent &event) {
  ShowOutputPane(event.IsChecked());
}

ArduinoEditorFrame::ArduinoEditorFrame(wxConfigBase *cfg) : wxFrame(nullptr, wxID_ANY, _("Arduino Editor"), wxDefaultPosition, wxSize(1024, 768)), m_auiManager(this), m_diagTimer(this, ID_TIMER_DIAGNOSTIC), m_returnBottomPageTimer(this, ID_TIMER_BOTTOM_PAGE_RETURN), m_updatesTimer(this, ID_TIMER_UPDATES_AVAILABLE) {
  config = cfg;

#ifdef __WXMSW__
  SetIcon(wxICON(IDI_APP_ICON));
#endif

  StartProcess(_("Application initializing..."), ID_PROCESS_APP_INIT, ArduinoActivityState::Busy);

  LoadConfig();
  LoadRecentSketches();
  LoadBoardHistory();

  InitComponents();
  BindEvents();

  RestoreWindowPlacement();

  ArduinoEditApp &app = wxGetApp();
  app.HideSplash();
}

void ArduinoEditorFrame::OnViewOutput(wxCommandEvent &WXUNUSED(evt)) {
  UpdateOutputTabsFromMenu();
}

void ArduinoEditorFrame::UpdateOutputTabsFromMenu() {
  wxMenuBar *mb = GetMenuBar();
  if (!mb || !m_bottomNotebook)
    return;

  bool showOutput = mb->IsChecked(ID_MENU_VIEW_OUTPUT);

  wxAuiPaneInfo &pane = m_auiManager.GetPane(wxT("output"));
  if (!pane.IsOk())
    return;

  pane.Show(showOutput);
  m_auiManager.Update();
}

void ArduinoEditorFrame::OnAuiPaneClose(wxAuiManagerEvent &evt) {
  if (evt.GetPane() && evt.GetPane()->name == wxT("output")) {
    if (m_menuBar) {
      m_menuBar->Check(ID_MENU_VIEW_OUTPUT, false);
    }
  } else if (evt.GetPane()->name == wxT("files")) {
    if (m_menuBar) {
      m_menuBar->Check(ID_MENU_VIEW_SKETCHES, false);
    }
  } else if (evt.GetPane()->name == wxT("symbols")) {
    if (m_menuBar) {
      m_menuBar->Check(ID_MENU_VIEW_SYMBOLS, false);
    }
  } else if (evt.GetPane()->name == wxT("ai")) {
    if (m_menuBar) {
      m_menuBar->Check(ID_MENU_VIEW_AI, false);
      m_aiGlobalActions.ResetInteractiveChat();
      m_aiPanel->Clear();
    }
  }
  evt.Skip();
}

void ArduinoEditorFrame::RestoreWindowPlacement() {
  if (!config)
    return;

  LoadWindowSize(wxT("Layout"), this, config);

  int max;
  if (config->Read(wxT("Layout.Maximized"), &max) && max != 0) {
    Maximize();
  }
}

void ArduinoEditorFrame::OnDiagJumpFromView(ArduinoDiagnosticsActionEvent &ev) {
  if (!ev.HasJumpTarget())
    return;
  HandleGoToLocation(ev.GetJumpTarget());
}

void ArduinoEditorFrame::OnDiagSolveAiFromView(ArduinoDiagnosticsActionEvent &ev) {
  if (!m_aiSettings.enabled)
    return;
  if (!ev.HasJumpTarget() || !ev.HasDiagnostic())
    return;

  HandleGoToLocation(ev.GetJumpTarget());

  ArduinoEditor *ed = GetCurrentEditor();
  if (ed) {
    ed->AiSolveError(ev.GetDiagnostic());
  }
}

void ArduinoEditorFrame::OnOpenSerialMonitor(wxCommandEvent &) {
  // security check - serial monitor only for the "serial" port
  int sel = m_portChoice ? m_portChoice->GetSelection() : wxNOT_FOUND;
  if (sel == wxNOT_FOUND ||
      sel < 0 ||
      sel >= static_cast<int>(m_serialPorts.size()) ||
      m_serialPorts[sel].protocol != "serial") {

    ModalMsgDialog(_("Serial monitor is available only for local serial ports.\n\n"
                     "Select a serial port in the toolbar."),
                   _("Serial monitor"));
    return;
  }

  wxString portName = wxString::FromUTF8(arduinoCli->GetSerialPort());
  long baud = 115200;

  if (!m_serialMonitor) {
    if (CanPerformAction(openserialmon, true)) {
      m_serialMonitor = new ArduinoSerialMonitorFrame(this, config, portName, baud);
      m_serialMonitor->Show();
      FinalizeCurrentAction(true);
    }
  } else {
    m_serialMonitor->Show();
    m_serialMonitor->Raise();
  }
}

void ArduinoEditorFrame::OnCloseSerialMonitor() {
  m_serialMonitor = nullptr;
}

void ArduinoEditorFrame::LoadRecentSketches() {
  m_recentSketches.clear();

  if (!config)
    return;

  wxString all;
  if (!config->Read(wxT("RecentSketches"), &all) || all.empty())
    return;

  wxStringTokenizer tk(all, wxT("\n"), wxTOKEN_STRTOK);
  while (tk.HasMoreTokens()) {
    wxString token = tk.GetNextToken();
    token.Trim(true).Trim(false);
    if (!token.empty()) {
      m_recentSketches.push_back(token);
    }
  }
}

void ArduinoEditorFrame::SaveRecentSketches() {
  if (!config)
    return;

  wxString all;
  for (size_t i = 0; i < m_recentSketches.size(); ++i) {
    all += m_recentSketches[i];
    if (i + 1 < m_recentSketches.size()) {
      all += wxT("\n");
    }
  }

  config->Write(wxT("RecentSketches"), all);
  config->Flush();
}

void ArduinoEditorFrame::AddRecentSketch(const wxString &path) {
  if (path.empty())
    return;

  // normalize to an absolute path
  wxFileName fn;
  fn.AssignDir(path);
  wxString norm = fn.GetFullPath();

  // remove any duplicate
  auto it = std::remove(m_recentSketches.begin(), m_recentSketches.end(), norm);
  if (it != m_recentSketches.end()) {
    m_recentSketches.erase(it, m_recentSketches.end());
  }

  // insert at the beginning
  m_recentSketches.insert(m_recentSketches.begin(), norm);

  // trim to max
  if ((int)m_recentSketches.size() > MAX_RECENT_SKETCHES) {
    m_recentSketches.resize(MAX_RECENT_SKETCHES);
  }

  SaveRecentSketches();
  RebuildRecentMenu();
}

void ArduinoEditorFrame::RebuildRecentMenu() {
  if (!m_recentMenu)
    return;

  while (m_recentMenu->GetMenuItemCount() > 0) {
    wxMenuItem *item = m_recentMenu->FindItemByPosition(0);
    if (!item)
      break;
    m_recentMenu->Delete(item);
  }

  if (m_recentSketches.empty()) {
    // empty history - only a disabled "(Empty)" item
    wxMenuItem *emptyItem = m_recentMenu->Append(wxID_ANY, _("(Empty)"));
    emptyItem->Enable(false);
    m_recentMenu->AppendSeparator();
    wxMenuItem *clearItem = m_recentMenu->Append(ID_MENU_OPEN_RECENT_CLEAR, _("Clear history"));
    clearItem->Enable(false); // nothing to delete
    return;
  }

  int maxCount = std::min((int)m_recentSketches.size(), MAX_RECENT_SKETCHES);
  for (int i = 0; i < maxCount; ++i) {
    int id = ID_MENU_OPEN_RECENT_FIRST + i;
    wxString path = m_recentSketches[i];

    // Label: directory name + full path in parentheses
    wxFileName fn;
    fn.AssignDir(path);
    wxString name;
    const wxArrayString &dirs = fn.GetDirs();
    if (!dirs.IsEmpty()) {
      name = dirs.Last();
    } else {
      name = path;
    }

    wxString label = wxString::Format(wxT("%s\t%s"), name, path);
    m_recentMenu->Append(id, label);
  }

  m_recentMenu->AppendSeparator();
  m_recentMenu->Append(ID_MENU_OPEN_RECENT_CLEAR, _("Clear history"));
}

void ArduinoEditorFrame::OnOpenRecent(wxCommandEvent &event) {
  int id = event.GetId();
  int index = id - ID_MENU_OPEN_RECENT_FIRST;

  if (index < 0 || index >= (int)m_recentSketches.size())
    return;

  wxString path = m_recentSketches[index];

  if (!wxDirExists(path)) {
    ModalMsgDialog(wxString::Format(_("Directory '%s' does not exist."), path));

    // Delete non-existent record and update the menu
    m_recentSketches.erase(m_recentSketches.begin() + index);
    SaveRecentSketches();
    RebuildRecentMenu();
    return;
  }

  ArduinoEditApp &app = wxGetApp();
  app.OpenSketch(path);

  CloseIfNeeded();
}

void ArduinoEditorFrame::OnClearRecent(wxCommandEvent &WXUNUSED(event)) {
  if (m_recentSketches.empty())
    return;

  int res = ModalMsgDialog(_("Do you really want to clear the recent sketches history?"), _("Clear history"), wxYES_NO | wxICON_QUESTION);

  if (res != wxYES)
    return;

  m_recentSketches.clear();
  SaveRecentSketches();
  RebuildRecentMenu();
}

void ArduinoEditorFrame::UpdateFilesTreeSelectedFromNotebook() {
  if (!m_filesPanel)
    return;

  auto *ed = GetCurrentEditor();
  if (ed) {
    m_filesPanel->SelectPath(wxString::FromUTF8(ed->GetFilePath()));
  }
}

void ArduinoEditorFrame::ShowFilesPane(bool show) {
  wxAuiPaneInfo &pane = m_auiManager.GetPane(wxT("files"));
  if (!pane.IsOk())
    return;

  pane.Show(show);
  m_auiManager.Update();

  if (show && m_filesPanel) {
    m_filesPanel->RefreshTree();
  }
}

void ArduinoEditorFrame::OnViewFiles(wxCommandEvent &evt) {
  ShowFilesPane(evt.IsChecked());
}

void ArduinoEditorFrame::RebuildSketchesDirMenu() {
  if (!m_sketchesDirMenu)
    return;

  // Deletion of the menu
  while (m_sketchesDirMenu->GetMenuItemCount() > 0) {
    wxMenuItem *item = m_sketchesDirMenu->FindItemByPosition(0);
    if (!item) {
      break;
    }
    m_sketchesDirMenu->Delete(item);
  }

  m_sketchesPaths.clear();

  wxString sketchesDir;

  if (config) {
    if (!config->Read(wxT("SketchesDir"), &sketchesDir) || sketchesDir.empty()) {
      sketchesDir = wxGetHomeDir();
    }
  } else {
    sketchesDir = wxGetHomeDir();
  }

  APP_DEBUG_LOG("FRM: SketchesDir=%s", sketchesDir.ToUTF8().data());

  if (!wxDirExists(sketchesDir)) {
    wxMenuItem *item = m_sketchesDirMenu->Append(wxID_ANY, _("(SketchesDir does not exist)"));
    item->Enable(false);
    return;
  }

  wxDir dir(sketchesDir);
  if (!dir.IsOpened()) {
    wxMenuItem *item = m_sketchesDirMenu->Append(wxID_ANY, _("(Cannot open SketchesDir)"));
    item->Enable(false);
    return;
  }

  // --- Prepare the structure for sorting ---
  struct SketchEntry {
    wxString name;
    wxString fullPath;
  };
  std::vector<SketchEntry> entries;

  wxString subdirName;
  bool cont = dir.GetFirst(&subdirName, wxEmptyString, wxDIR_DIRS);

  while (cont) {
    wxFileName subdir(sketchesDir, subdirName);
    wxString fullPath = subdir.GetFullPath();

    // Does the directory have at least one *.ino?
    bool hasIno = false;
    wxDir d2(fullPath);
    if (d2.IsOpened()) {
      wxString file;
      bool cont2 = d2.GetFirst(&file, wxEmptyString, wxDIR_FILES);
      while (cont2) {
        wxFileName fn(fullPath, file);
        if (fn.GetExt().Lower() == wxT("ino")) {
          hasIno = true;
          break;
        }
        cont2 = d2.GetNext(&file);
      }
    }

    if (hasIno) {
      entries.push_back({subdirName, fullPath});
    }

    cont = dir.GetNext(&subdirName);
  }

  // --- Sort alphabetically by name ---
  std::sort(entries.begin(), entries.end(),
            [](const SketchEntry &a, const SketchEntry &b) {
              return a.name.CmpNoCase(b.name) < 0;
            });

  // --- Fill the menu ---
  int idx = 0;
  for (const auto &e : entries) {
    if (idx > (ID_MENU_SKDIR_LAST - ID_MENU_SKDIR_FIRST))
      break;

    int id = ID_MENU_SKDIR_FIRST + idx;

    wxString label = e.name + wxT("\t") + e.fullPath;
    m_sketchesDirMenu->Append(id, label);

    m_sketchesPaths.push_back(e.fullPath);
    ++idx;
  }

  if (m_sketchesPaths.empty()) {
    wxMenuItem *item = m_sketchesDirMenu->Append(wxID_ANY, _("(No sketches found)"));
    item->Enable(false);
  }
}

void ArduinoEditorFrame::ShowAiPane(bool show) {
  wxAuiPaneInfo &pane = m_auiManager.GetPane(wxT("ai"));
  if (!pane.IsOk())
    return;

  pane.Show(show);
  m_auiManager.Update();

  if (m_menuBar) {
    m_menuBar->Check(ID_MENU_VIEW_AI, show);
  }
}

void ArduinoEditorFrame::OnViewAi(wxCommandEvent &evt) {
  bool want = evt.IsChecked();

  if (!m_aiSettings.enabled) {
    if (m_menuBar) {
      m_menuBar->Check(ID_MENU_VIEW_AI, false);
    }
    ShowAiPane(false);
    return;
  }

  ShowAiPane(want);
}

void ArduinoEditorFrame::OnOpenSketchFromDir(wxCommandEvent &event) {
  int id = event.GetId();
  int index = id - ID_MENU_SKDIR_FIRST;

  if (index < 0 || index >= (int)m_sketchesPaths.size())
    return;

  wxString path = m_sketchesPaths[index];

  if (!wxDirExists(path)) {
    ModalMsgDialog(wxString::Format(_("Directory '%s' does not exist."), path));

    // refresh the menu in case something was deleted in the meantime
    RebuildSketchesDirMenu();
    return;
  }

  ArduinoEditApp &app = wxGetApp();
  app.OpenSketch(path);

  CloseIfNeeded();
}

void ArduinoEditorFrame::OnShowExamples(wxCommandEvent &WXUNUSED(evt)) {
  if (!arduinoCli) {
    return;
  }

  if (!m_examplesFrame) {
    m_examplesFrame = new ArduinoExamplesFrame(this, arduinoCli, config);

    m_examplesFrame->Bind(wxEVT_CLOSE_WINDOW,
                          [this](wxCloseEvent &e) {
                            e.Skip(); // enable standard behavior (Destroy)
                            m_examplesFrame = nullptr;
                          });
  }

  m_examplesFrame->Show();
  m_examplesFrame->Raise();
}

void ArduinoEditorFrame::OnSysColoursChanged(wxSysColourChangedEvent &evt) {
  EditorSettings settings;
  settings.Load(config);

  wxSystemAppearance app = wxSystemSettings::GetAppearance();
  if (app.IsDark()) {
    APP_DEBUG_LOG("FRM: GetAppearance() -> DARK");
  } else {
    APP_DEBUG_LOG("FRM: GetAppearance() -> LIGHT");
  }

  ApplySettings(settings);

  m_tabImageList = CreateNotebookPageImageList(settings.GetColors().text);
  m_notebook->AssignImageList(m_tabImageList);

  m_optionsButton->SetBitmap(AEGetArtBundle(wxAEArt::Settings));
  m_buildButton->SetBitmap(AEGetArtBundle(wxAEArt::Check));
  m_uploadButton->SetBitmap(AEGetArtBundle(wxAEArt::Play));
  m_refreshPortsButton->SetBitmap(AEGetArtBundle(wxAEArt::Refresh));
  m_serialMonitorButton->SetBitmap(AEGetArtBundle(wxAEArt::SerMon));

  auto ReplaceMenuItemBitmap = [this](int id, const wxArtID &bitmapId) {
    auto bmp = AEGetArtBundle(bitmapId);
    wxMenu *menu = nullptr;
    wxMenuItem *item = m_menuBar->FindItem(id, &menu);
    if (!item || !menu)
      return;

    // find index (pos) of the item inside this menu
    auto FindPos = [](wxMenu *m, int itemId) -> int {
      if (!m)
        return wxNOT_FOUND;
      const wxMenuItemList &items = m->GetMenuItems();
      int pos = 0;
      for (auto node = items.GetFirst(); node; node = node->GetNext(), ++pos) {
        wxMenuItem *it = node->GetData();
        if (it && it->GetId() == itemId)
          return pos;
      }
      return wxNOT_FOUND;
    };

    int pos = FindPos(menu, id);
    if (pos == wxNOT_FOUND)
      return;

    // Remove returns the item pointer; it does NOT delete it
    wxMenuItem *removed = menu->Remove(id);
    if (!removed)
      return;

    removed->SetBitmap(bmp);
    menu->Insert(pos, removed);
  };

  ReplaceMenuItemBitmap(ID_MENU_NEW_SKETCH, wxAEArt::New);
  ReplaceMenuItemBitmap(ID_MENU_OPEN_SKETCH, wxAEArt::FileOpen);
  ReplaceMenuItemBitmap(ID_MENU_SKETCH_EXAMPLES, wxAEArt::ListView);
  ReplaceMenuItemBitmap(ID_MENU_SAVE, wxAEArt::FileSave);
  ReplaceMenuItemBitmap(ID_MENU_SAVE_ALL, wxAEArt::FileSaveAs);
#ifndef __WXMAC__
  ReplaceMenuItemBitmap(wxID_PREFERENCES, wxAEArt::Settings);
  ReplaceMenuItemBitmap(wxID_EXIT, wxAEArt::Quit);
#endif
  ReplaceMenuItemBitmap(ID_MENU_NAV_BACK, wxAEArt::GoBack);
  ReplaceMenuItemBitmap(ID_MENU_NAV_FORWARD, wxAEArt::GoForward);
  ReplaceMenuItemBitmap(ID_MENU_NAV_FIND_SYMBOL, wxAEArt::Find);
  ReplaceMenuItemBitmap(ID_MENU_LIBRARY_MANAGER, wxAEArt::ListView);
  ReplaceMenuItemBitmap(ID_MENU_CORE_MANAGER, wxAEArt::DevBoard);
  ReplaceMenuItemBitmap(ID_MENU_SERIAL_MONITOR, wxAEArt::SerMon);
  ReplaceMenuItemBitmap(ID_MENU_PROJECT_BUILD, wxAEArt::Check);
  ReplaceMenuItemBitmap(ID_MENU_PROJECT_UPLOAD, wxAEArt::Play);
  ReplaceMenuItemBitmap(ID_MENU_PROJECT_CLEAN, wxAEArt::Delete);
  ReplaceMenuItemBitmap(ID_MENU_VIEW_OUTPUT, wxAEArt::ReportView);
  ReplaceMenuItemBitmap(ID_MENU_VIEW_SKETCHES, wxAEArt::FolderOpen);
  ReplaceMenuItemBitmap(ID_MENU_VIEW_AI, wxAEArt::Tip);
  ReplaceMenuItemBitmap(ID_MENU_CHECK_FOR_UPDATES, wxAEArt::CheckForUpdates);
  ReplaceMenuItemBitmap(ID_MENU_VIEW_SYMBOLS, wxAEArt::ListView);
#ifndef __WXMAC__
  ReplaceMenuItemBitmap(wxID_ABOUT, wxAEArt::Information);
#endif

  m_menuBar->Refresh();
  m_menuBar->Update();

  Layout();

  evt.Skip();
}

bool ArduinoEditorFrame::IsProjectFile(const std::string &filePath) {
  std::string sp = arduinoCli->GetSketchPath();
  return ::IsInSketchDir(sp, filePath);
}

void ArduinoEditorFrame::OnSketchTreeOpenItem(wxCommandEvent &evt) {
  wxString fullPath = evt.GetString();
  bool isDir = (evt.GetInt() != 0);

  std::string fullPathStd = wxToStd(fullPath);

  APP_DEBUG_LOG("OnSketchTreeOpenItem(filename=%s, isDir=%d)", fullPathStd.c_str(), isDir);

  if (isDir) {
    return;
  }

  ArduinoEditor *ed = FindEditorWithFile(fullPathStd, /*allowCreate=*/false);
  if (ed) {
    ActivateEditor(ed, 1, 1);
    return;
  }

  if (isSourceFile(fullPathStd) || isHeaderFile(fullPathStd)) {
    if (IsProjectFile(fullPathStd)) {
      CreateEditorForFile(fullPathStd, 1, 1);
    } else {
      m_clangSettings.OpenExternalSourceFile(fullPath, 1);
    }
  } else {
    if (!wxLaunchDefaultApplication(fullPath)) {
      ModalMsgDialog(wxString::Format(_("Cannot open '%s' in associated application."), fullPath), _("Open file"));
    }
  }
}

void ArduinoEditorFrame::OnSketchTreeNewFile(wxCommandEvent &evt) {
  wxString basePath = evt.GetString();
  bool isDir = (evt.GetInt() != 0);

  wxString parentDir = isDir ? basePath : wxFileName(basePath).GetPath();

  wxTextEntryDialog dlg(this, _("Enter new file name:"), _("New file"), _("new_file.cpp"));

  if (dlg.ShowModal() != wxID_OK)
    return;

  wxString name = dlg.GetValue();
  name.Trim(true).Trim(false);

  if (name.Find('/') != wxNOT_FOUND || name.Find('\\') != wxNOT_FOUND) {
    ModalMsgDialog(_("File name must not contain path separators."));
    return;
  }

  wxFileName fn(parentDir, name);
  CreateNewSketchFile(fn.GetFullPath());
}

void ArduinoEditorFrame::OnSketchTreeNewFolder(wxCommandEvent &evt) {
  wxString basePath = evt.GetString();
  bool isDir = (evt.GetInt() != 0);

  if (!isDir)
    return;

  wxTextEntryDialog dlg(this,
                        _("Folder name:"),
                        _("New folder"),
                        _("new_folder"));

  if (dlg.ShowModal() != wxID_OK)
    return;

  wxString name = dlg.GetValue();
  name.Trim(true).Trim(false);

  if (name.empty() || name.Find('/') != wxNOT_FOUND || name.Find('\\') != wxNOT_FOUND) {
    ModalMsgDialog(_("Invalid folder name."));
    return;
  }

  wxFileName fn(basePath, name);
  if (!wxFileName::Mkdir(fn.GetFullPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
    ModalMsgDialog(_("Cannot create folder."));
    return;
  }

  if (m_filesPanel) {
    m_filesPanel->RefreshTree();
  }
}

void ArduinoEditorFrame::OnSketchTreeDelete(wxCommandEvent &evt) {
  wxString path = evt.GetString();
  bool isDir = (evt.GetInt() != 0);

  wxFileName fn(path);

  if (!isDir && fn.GetExt().Lower() == wxT("ino")) {
    ModalMsgDialog(_("Cannot delete .ino file of sketch."));
    return;
  }

  wxString msg = wxString::Format(isDir
                                      ? _("Do you want to delete folder %s and all its contents?")
                                      : _("Do you want to delete file %s?"),
                                  fn.GetFullName());

  if (ModalMsgDialog(msg, _("Delete"), wxYES_NO | wxICON_WARNING) != wxID_YES)
    return;

  if (isDir) {
    std::error_code ec;
    std::filesystem::remove_all(wxToStd(path), ec);

    if (ec) {
      ModalMsgDialog(_("Failed to delete: ") + path);
      return;
    }

    if (m_filesPanel) {
      m_filesPanel->RefreshTree();
    }
    return;
  }

  DeleteSketchFile(fn.GetFullPath());
}

void ArduinoEditorFrame::OnSketchTreeRename(wxCommandEvent &evt) {
  wxString path = evt.GetString();
  wxFileName fn(path);

  wxTextEntryDialog dlg(this, _("New name:"), _("Rename"), fn.GetFullName());

  if (dlg.ShowModal() != wxID_OK)
    return;

  wxString newName = dlg.GetValue();
  newName.Trim(true).Trim(false);

  if (newName.empty())
    return;

  wxFileName target(fn.GetPath(), newName);

  if (wxRenameFile(fn.GetFullPath(), target.GetFullPath(), false)) {
    if (m_filesPanel) {
      m_filesPanel->RefreshTree();
    }

    // Update editor
    int pageCount = m_notebook->GetPageCount();
    for (int i = 0; i < pageCount; ++i) {
      auto *editor = dynamic_cast<ArduinoEditor *>(m_notebook->GetPage(i));
      if (!editor) {
        continue;
      }

      if (fn.GetFullPath() == wxString::FromUTF8(editor->GetFilePath())) {
        editor->SetFilePath(wxToStd(target.GetFullPath()));
        // editor generates relative file from sketchpath
        m_notebook->SetPageText(i, wxString::FromUTF8(editor->GetFileName()));
        break;
      }
    }
  } else {
    ModalMsgDialog(_("Rename failed."));
  }
}

void ArduinoEditorFrame::OnSketchTreeOpenExternally(wxCommandEvent &evt) {
  wxString path = evt.GetString();
  bool isDir = (evt.GetInt() != 0);

  std::string stdPath = wxToStd(path);

  if (!isDir && (isSourceFile(stdPath) || isHeaderFile(stdPath))) {
    m_clangSettings.OpenExternalSourceFile(path, 1);
  } else {
    if (!wxLaunchDefaultApplication(path)) {
      ModalMsgDialog(wxString::Format(_("Cannot open '%s' in associated application."), wxString::FromUTF8(stdPath)), _("Open file"));
    }
  }
}

wxString ArduinoEditorFrame::MakeBoardLabel(const std::string &fqbn) const {
  if (fqbn.empty()) {
    return _("No board selected");
  }

  std::string name;
  for (const auto &b : m_availableBoards) {
    if (b.fqbn == fqbn) {
      name = b.name;
      break;
    }
  }

  if (!name.empty() && !fqbn.empty() && name != fqbn) {
    return wxString::FromUTF8(name.c_str()) +
           wxT(" (") + wxString::FromUTF8(fqbn) + wxT(")");
  } else if (!name.empty()) {
    return wxString::FromUTF8(name);
  } else {
    return wxString::FromUTF8(fqbn);
  }
}

void ArduinoEditorFrame::RebuildBoardChoice() {
  if (!m_boardChoice) {
    return;
  }

  m_boardChoice->Clear();

  std::string currentFqbn;
  if (arduinoCli) {
    currentFqbn = arduinoCli->GetBoardName();
  }

  int selIndex = wxNOT_FOUND;

  for (size_t i = 0; i < m_boardHistory.size(); ++i) {
    const std::string &fqbn = m_boardHistory[i];
    wxString label = MakeBoardLabel(fqbn);
    m_boardChoice->Append(label);

    if (!currentFqbn.empty() && fqbn == currentFqbn) {
      selIndex = static_cast<int>(i);
    }
  }

  m_lastBoardSelection = (selIndex == wxNOT_FOUND) ? 0 : selIndex;
  m_boardChoice->SetSelection(m_lastBoardSelection);

  m_boardChoice->Append(_("--- Select board ---"));
}

void ArduinoEditorFrame::LoadBoardHistory() {
  m_boardHistory.clear();

  wxString sketchesDir;
  if (!config->Read(wxT("SketchesDir"), &sketchesDir)) {
    return;
  }

  std::unordered_set<std::string> uniq;
  uniq.reserve(128);

  std::error_code ec;
  fs::path root(wxToStd(sketchesDir));
  if (root.empty()) {
    return;
  }

  fs::path absRoot = fs::absolute(root, ec);
  if (ec) {
    absRoot = root;
  }

  if (!fs::exists(absRoot, ec) || !fs::is_directory(absRoot, ec)) {
    return;
  }

  fs::recursive_directory_iterator it(
      absRoot,
      fs::directory_options::skip_permission_denied,
      ec);
  fs::recursive_directory_iterator end;

  for (; it != end && !ec; it.increment(ec)) {
    const fs::directory_entry &e = *it;

    if (!e.is_directory(ec))
      continue;

    fs::path yamlPath = e.path() / "sketch.yaml";
    if (!fs::exists(yamlPath, ec) || !fs::is_regular_file(yamlPath, ec)) {
      continue;
    }

    std::string baseFqbn;
    if (ParseDefaultFqbnFromSketchYaml(yamlPath, baseFqbn) && !baseFqbn.empty()) {
      uniq.insert(std::move(baseFqbn));
    }

    it.disable_recursion_pending();
  }

  m_boardHistory.assign(uniq.begin(), uniq.end());
  SortBoardHistory();
}

void ArduinoEditorFrame::AddBoardToHistory(const std::string &fqbn) {
  std::string newFqbn = BaseFqbn3(fqbn);
  if (std::find(m_boardHistory.begin(), m_boardHistory.end(), newFqbn) == m_boardHistory.end()) {
    APP_DEBUG_LOG("FRM: adding new board %s to history", newFqbn.c_str());
    m_boardHistory.push_back(newFqbn);

    SortBoardHistory();
  }
}

void ArduinoEditorFrame::SortBoardHistory() {
  std::sort(m_boardHistory.begin(), m_boardHistory.end(),
            [](const std::string &a, const std::string &b) {
              auto lower = [](unsigned char c) -> unsigned char {
                return static_cast<unsigned char>(std::tolower(c));
              };

              const std::size_t n = std::min(a.size(), b.size());
              for (std::size_t i = 0; i < n; ++i) {
                unsigned char ca = lower(static_cast<unsigned char>(a[i]));
                unsigned char cb = lower(static_cast<unsigned char>(b[i]));
                if (ca < cb)
                  return true;
                if (ca > cb)
                  return false;
              }
              if (a.size() != b.size())
                return a.size() < b.size();
              return a < b;
            });
}

void ArduinoEditorFrame::OnBoardChoiceChanged(wxCommandEvent &WXUNUSED(event)) {
  int sel = m_boardChoice ? m_boardChoice->GetSelection() : wxNOT_FOUND;
  if (sel == wxNOT_FOUND) {
    return;
  }

  if (!arduinoCli) {
    return;
  }

  if (m_boardChoice->GetStringSelection() == _("--- Select board ---")) {
    m_boardChoice->SetSelection(m_lastBoardSelection);
    CallAfter([this]() {
      SelectBoard();
    });
    return;
  }

  if (sel < 0 || sel >= static_cast<int>(m_boardHistory.size())) {
    return;
  }

  std::string newFqbn = m_boardHistory[static_cast<size_t>(sel)];

  std::string current = arduinoCli->GetBoardName();
  if (newFqbn == current) {
    return; // nothing changes
  }

  m_cleanTried = false;

  UpdateBoard(newFqbn);

  RebuildProject(/*withClean=*/true);
}

void ArduinoEditorFrame::UpdateSketchesDir(const wxString &sketchDir) {
  if (!config || sketchDir.empty()) {
    return;
  }

  wxFileName fn;
  fn.AssignDir(sketchDir); // normalization, handles trailing slash etc.
  fn.RemoveLastDir();      // we go up one directory = root of sketches

  wxString root = fn.GetPath();
  if (root.IsEmpty()) {
    // fallback - in case for some reason it was not possible to calculate the parent
    root = sketchDir;
  }

  config->Write(wxT("SketchesDir"), root);
  config->Flush();

  // This is environment for arduino-cli.
  wxSetEnv(wxT("ARDUINO_DIRECTORIES_USER"), root);

  RebuildSketchesDirMenu();
}

void ArduinoEditorFrame::StartProcess(const wxString &name, int id, ArduinoActivityState state, bool canBeTerminated) {
  if (m_indic) {
    m_indic->StartProcess(name, id, state, canBeTerminated);
  }
}

void ArduinoEditorFrame::StopProcess(int id) {
  if (m_indic)
    m_indic->StopProcess(id);
}

void ArduinoEditorFrame::OnStatusBarSize(wxSizeEvent &evt) {
  LayoutStatusBarIndicator();
  evt.Skip();
}

void ArduinoEditorFrame::LayoutStatusBarIndicator() {
  if (!m_statusBar || !m_indic)
    return;

  wxRect rect;
  if (!m_statusBar->GetFieldRect(2, rect))
    return;

  int size = std::min(rect.GetWidth(), rect.GetHeight()) - 4;
  if (size < 8) {
    size = std::min(rect.GetWidth(), rect.GetHeight());
  }

  int x = rect.x + (rect.width - size) / 2;
  int y = rect.y + (rect.height - size) / 2;

  m_indic->SetSize(x, y, size, size);
}

void ArduinoEditorFrame::UpdateEditorTabIcon(ArduinoEditor *ed, bool modified, bool readOnly) {
  if (!m_notebook || !ed || !m_tabImageList) {
    return;
  }

  int pageIndex = m_notebook->FindPage(ed);
  if (pageIndex == wxNOT_FOUND) {
    return;
  }

  m_notebook->SetPageImage(pageIndex, IMLI_NOTEBOOK_NONE);

  int imgIndex = IMLI_NOTEBOOK_NONE;

  if (readOnly) {
    imgIndex = IMLI_NOTEBOOK_LOCK;
  } else if (modified) {
    imgIndex = IMLI_NOTEBOOK_BULLET;
  } else {
    imgIndex = IMLI_NOTEBOOK_EMPTY;
  }

  APP_DEBUG_LOG("FRM: UpdateEditorTabIcon() pageIndex=%d, modified=%d, readOnly=%d, imgIndex=%d",
                pageIndex, (int)modified, (int)readOnly, imgIndex);

  m_notebook->SetPageImage(pageIndex, imgIndex);
}

void ArduinoEditorFrame::RefactorRenameSymbol(ArduinoEditor *originEditor, int line, int column, const wxString &oldName) {
  if (!originEditor || !completion || !completion->IsReady()) {
    ModalMsgDialog(_("Code completion engine is not available."));
    return;
  }

  // Check source code validity
  std::vector<ArduinoParseError> errors;

  switch (m_clangSettings.diagnosticMode) {
    case translationUnit: {
      if (!completion->IsTranslationUnitValid()) {
        ModalMsgDialog(_("Translation unit is not valid."), _("Rename symbol"));
        return;
      }
      ArduinoEditor *ed = GetCurrentEditor();
      if (ed) {
        errors = completion->GetErrorsFor(ed->GetFilePath());
      }

      break;
    }
    case completeProject:
      errors = completion->GetLastProjectErrors();
      break;
    default: // noCompletion
      return;
  }

  for (auto &e : errors) {
    auto sev = static_cast<int>(e.severity);
    if (sev == static_cast<int>(CXDiagnostic_Error) ||
        sev == static_cast<int>(CXDiagnostic_Fatal)) {
      ModalMsgDialog(_("Sources contains compilation errors."), _("Rename symbol"));
      return;
    }
  }

  std::string filename = originEditor->GetFilePath();
  std::string code = originEditor->GetText();

  std::vector<SketchFileBuffer> files;
  CollectEditorSources(files);

  std::vector<JumpTarget> occs;
  if (!completion->FindSymbolOccurrencesProjectWide(files, filename, code, line, column, /*onlyFromSketch=*/false, occs) || occs.empty()) {
    ModalMsgDialog(_("No usages of this symbol were found."), _("Rename symbol"), wxOK | wxICON_INFORMATION);
    return;
  }

  const std::string sketchRoot = arduinoCli->GetSketchPath();

  for (auto &jt : occs) {
    APP_DEBUG_LOG("FRM: Occurence at line=%d, column=%d in file %s", jt.line, jt.column, jt.file.c_str());
    if (!IsInSketchDir(sketchRoot, jt.file)) {
      ModalMsgDialog(_("Symbol is declared outside of sketch directory."), _("Rename symbol"), wxOK | wxICON_INFORMATION);
      return;
    }
  }

  std::string oldNameStd = wxToStd(oldName);

  ArduinoRenameSymbolDialog dlg(this,
                                config,
                                oldName,
                                occs,
                                sketchRoot);

  if (dlg.ShowModal() != wxID_OK) {
    return;
  }

  wxString newNameWx = dlg.GetNewName();
  newNameWx.Trim(true).Trim(false);

  if (newNameWx.IsEmpty()) {
    ModalMsgDialog(_("New name cannot be empty."), _("Rename symbol"), wxOK | wxICON_WARNING);
    return;
  }

  std::string newName = wxToStd(newNameWx);
  if (newName == oldNameStd) {
    return;
  }

  if (!LooksLikeIdentifier(newName)) {
    ModalMsgDialog(_("New name is not a valid identifier."), _("Rename symbol"), wxOK | wxICON_WARNING);
    return;
  }

  std::vector<JumpTarget> selectedOccs;
  dlg.GetSelectedOccurrences(selectedOccs);
  if (selectedOccs.empty()) {
    ModalMsgDialog(_("No occurrences selected."), _("Rename symbol"), wxOK | wxICON_INFORMATION);
    return;
  }

  std::map<std::string, std::vector<JumpTarget>> occByFile;
  for (const auto &o : selectedOccs) {
    if (!IsInSketchDir(sketchRoot, o.file)) {
      continue;
    }
    std::string norm = NormalizeFilename(o.file);
    occByFile[norm].push_back(o);
  }

  if (occByFile.empty()) {
    ModalMsgDialog(_("No usages of this symbol were found inside the current sketch."), _("Rename symbol"), wxOK | wxICON_INFORMATION);
    return;
  }

  // rename in editors
  int totalReplacements = 0;

  for (auto &pair : occByFile) {
    const std::string &filePathNorm = pair.first;
    auto &fileOccs = pair.second;

    ArduinoEditor *ed = FindEditorWithFile(filePathNorm); // this creates editor if file not opened
    if (!ed) {
      continue;
    }

    // sort by line/col desc
    std::sort(fileOccs.begin(), fileOccs.end(),
              [](const JumpTarget &a, const JumpTarget &b) {
                if (a.line != b.line)
                  return a.line > b.line;
                return a.column > b.column;
              });

    totalReplacements += ed->RefactorRenameSymbol(fileOccs, oldNameStd, newName);
  }

  if (totalReplacements == 0) {
    ModalMsgDialog(_("No matching occurrences to rename were found."), _("Rename symbol"), wxOK | wxICON_INFORMATION);
    return;
  }

  wxString msg = wxString::Format(_("Renamed %d occurrences."), totalReplacements);
  UpdateStatus(msg);

  ScheduleDiagRefresh();
}

void ArduinoEditorFrame::OnCheckForUpdates(wxCommandEvent &) {
  ArduinoEditorUpdateDialog::CheckAndShowIfNeeded(this, *config, /*force=*/true);
}

void ArduinoEditorFrame::CheckForUpdatesIfNeeded() {
  if (!arduinoCli) {
    return;
  }

  ArdUpdateScheduler scheduler(config);

  if (scheduler.IsDueLibraries()) {
    arduinoCli->CheckForLibrariesUpdateAsync(this);
  }

  if (scheduler.IsDueBoards()) {
    arduinoCli->CheckForCoresUpdateAsync(this);
  }
}

void ArduinoEditorFrame::OnLibrariesUpdatesAvailable(wxThreadEvent &event) {
  if (event.GetInt() == 0) {
    APP_DEBUG_LOG("FRM: Library update check failed!");
    return;
  }

  ArdUpdateScheduler scheduler(config);
  scheduler.MarkCheckedNow(ArdUpdateScheduler::Kind::Libraries);

  auto libs = event.GetPayload<std::vector<ArduinoLibraryInfo>>();

  m_librariesForUpdate = std::move(libs);

  ScheduleLibsCoresUpdateInfo();
}

void ArduinoEditorFrame::OnCoresUpdatesAvailable(wxThreadEvent &event) {
  if (event.GetInt() == 0) {
    m_coresForUpdate.clear();
    APP_DEBUG_LOG("FRM: Core update check failed!");
    return;
  }

  ArdUpdateScheduler scheduler(config);
  scheduler.MarkCheckedNow(ArdUpdateScheduler::Kind::Boards);

  auto cores = event.GetPayload<std::vector<ArduinoCoreInfo>>();

  m_coresForUpdate = std::move(cores);

  ScheduleLibsCoresUpdateInfo();
}

void ArduinoEditorFrame::ScheduleLibsCoresUpdateInfo() {
  if (m_updatesTimer.IsRunning()) {
    m_updatesTimer.Stop();
  }

  APP_DEBUG_LOG("FRM: ScheduleLibsCoresUpdateInfo()");
  m_updatesTimer.Start(1000, wxTIMER_ONE_SHOT);
}

void ArduinoEditorFrame::OnUpdatesLibsCoresAvailable(wxTimerEvent &) {
  UpdateStatusBarUpdates();
}

void ArduinoEditorFrame::UpdateStatusBarUpdates() {
  if (!m_statusBar) {
    return;
  }

  if (!m_librariesForUpdate.empty() && !m_coresForUpdate.empty()) {
    m_statusBar->SetStatusText(wxString::Format(_("Updates: %zu libs and %zu boards"), m_librariesForUpdate.size(), m_coresForUpdate.size()), 1);
  } else if (!m_librariesForUpdate.empty()) {
    m_statusBar->SetStatusText(wxString::Format(_("Updates: %zu libraries"), m_librariesForUpdate.size()), 1);
  } else if (!m_coresForUpdate.empty()) {
    m_statusBar->SetStatusText(wxString::Format(_("Updates: %zu boards"), m_coresForUpdate.size()), 1);
  } else {
    m_statusBar->SetStatusText(wxEmptyString, 1);
  }

  APP_DEBUG_LOG("FRM: Updates: %zu libraries, %zu boards...", m_librariesForUpdate.size(), m_coresForUpdate.size());
}

void ArduinoEditorFrame::OnStatusBarLeftUp(wxMouseEvent &e) {
  if (!m_statusBar) {
    e.Skip();
    return;
  }

  wxPoint pt = e.GetPosition();
  wxPoint screenPt = m_statusBar->ClientToScreen(pt);
  wxPoint framePt = this->ScreenToClient(screenPt);

  wxRect updRect;
  if (m_statusBar->GetFieldRect(1, updRect) && updRect.Contains(pt)) {
    wxMenu menu;

    if (!m_librariesForUpdate.empty()) {
      menu.Append(ID_MENU_LIBRARY_MANAGER_UPDATES, wxString::Format(_("Library updates (%zu)..."), m_librariesForUpdate.size()));
    } else {
      auto *it = menu.Append(ID_MENU_LIBRARY_MANAGER, _("Library updates..."));
      it->Enable(false);
    }

    if (!m_coresForUpdate.empty()) {
      menu.Append(ID_MENU_CORE_MANAGER_UPDATES, wxString::Format(_("Board updates (%zu)..."), m_coresForUpdate.size()));
    } else {
      auto *it = menu.Append(ID_MENU_CORE_MANAGER, _("Board updates..."));
      it->Enable(false);
    }

    PopupMenu(&menu, framePt);
    return;
  }

  e.Skip();
}

void ArduinoEditorFrame::OnLibraryUpdatesFromStatusBar(wxCommandEvent &) {
  if (m_librariesForUpdate.empty())
    return;

  RequestShowLibraries(m_librariesForUpdate);
}

void ArduinoEditorFrame::OnCoreUpdatesFromStatusBar(wxCommandEvent &) {
  if (m_coresForUpdate.empty())
    return;

  if (!m_coreManager) {
    m_coreManager = new ArduinoCoreManagerFrame(this, arduinoCli, config, _("All"));
  }

  m_coreManager->Show();
  m_coreManager->Raise();

  m_coreManager->SetExplicitCores(m_coresForUpdate);
}

void ArduinoEditorFrame::OnStatusBarMotion(wxMouseEvent &e) {
  if (!m_statusBar) {
    e.Skip();
    return;
  }

  wxPoint pt = e.GetPosition();

  wxRect updRect;
  bool overUpd = m_statusBar->GetFieldRect(1, updRect) && updRect.Contains(pt) &&
                 (!m_librariesForUpdate.empty() || !m_coresForUpdate.empty());
  m_statusBar->SetCursor(overUpd ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
  e.Skip();
}

void ArduinoEditorFrame::OnStatusBarLeave(wxMouseEvent &e) {
  if (m_statusBar) {
    m_statusBar->SetCursor(wxNullCursor);
  }
  e.Skip();
}

ArduinoEditorFrame::~ArduinoEditorFrame() {
  m_auiManager.UnInit();
}

#ifdef __WXMSW__
#include <windows.h>

// Here, the only solution is to ensure that on Windows,
// any association to INO opens the sketch in the same instance of ArduinoEditor.
WXLRESULT ArduinoEditorFrame::MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) {
  if (message == WM_COPYDATA) {
    auto *cds = reinterpret_cast<COPYDATASTRUCT *>(lParam);
    if (cds && cds->dwData == 1 && cds->lpData && cds->cbData > 0) {
      const char *utf8 = static_cast<const char *>(cds->lpData);
      wxString path = wxString::FromUTF8(utf8);

      if (!path.IsEmpty()) {
        wxFileName fn(path);
        wxString openPath;

        if (fn.FileExists() && fn.GetExt().CmpNoCase(wxT("ino")) == 0) {
          openPath = fn.GetPath();
        } else if (fn.DirExists()) {
          openPath = path;
        }

        if (!openPath.IsEmpty()) {
          APP_DEBUG_LOG("WM_COPYDATA: OpenSketch(%s)", wxToStd(openPath).c_str());
          ArduinoEditApp &app = wxGetApp();
          app.OpenSketch(openPath);

          CloseIfNeeded();
        }
      }
    }
    return 0;
  }

  return wxFrame::MSWWindowProc(message, wParam, lParam);
}
#endif
