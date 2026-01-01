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

#include "ard_cli.hpp"
#include <vector>
#include <wx/bmpbuttn.h>
#include <wx/clrpicker.h>
#include <wx/config.h>
#include <wx/fontpicker.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/wx.h>

struct UiLanguageEntry {
  wxString code;
  wxString label;
};

enum class ThemeMode {
  FollowSystem = 0,
  AlwaysLight = 1,
  AlwaysDark = 2,
};

enum ClangDiagnosticMode {
  noDiagnostic = 0,
  translationUnit,
  completeProject
};

enum ClangCompletionMode {
  noCompletion = 0,
  onlyOnRequest,
  always
};

enum ClangResolveMode {
  internalResolver = 0,
  compileCommandsResolver
};

enum ClangWarningMode {
  warningOff = 0,
  warningDefault,
  warningArduinoLike,
  warningStrict,
  warningCustom
};

struct EditorColorScheme {
  wxColour text;
  wxColour background;
  wxColour installed;
  wxColour deprecated;
  wxColour updatable;
  wxColour lineNumberText;
  wxColour lineNumberBackground;
  wxColour selection;
  wxColour edge;
  wxColour whitespace;
  wxColour braceMatch;
  wxColour braceBad;
  wxColour comment;
  wxColour string;
  wxColour keyword1;
  wxColour keyword2;
  wxColour number;
  wxColour preprocessor;
  wxColour caretLine;
  wxColour calltipText;
  wxColour calltipBackground;
  wxColour error;
  wxColour warning;
  wxColour note;
  wxColour symbolHighlight;
  wxColour aiUserBg;
  wxColour aiAssistantBg;
  wxColour aiSystemBg;
  wxColour aiInfoBg;
  wxColour aiErrorBg;

  void Load(wxConfigBase *cfg, ThemeMode themeMode);
  void Save(wxConfigBase *cfg, ThemeMode themeMode);
};

struct EditorSettings {
  EditorColorScheme colors[2];

  int tabWidth = 2;
  bool useTabs = false;
  bool showWhitespace = false;
  bool autoIndent = true;
  wxString clangFormatOverridesJson;

  bool showLineNumbers = true;
  bool wordWrap = false;
  bool showRightEdge = false;
  int edgeColumn = 80;
  bool highlightCurrentLine = true;
  bool highlightSymbols = true;
  bool highlightMatchingBraces = true;
  bool displayHoverInfo = true;

  wxString fontDesc; // e.g. "Monaco 11" etc.

  EditorColorScheme GetColors() const;
  wxFont GetFont() const;
  void Load(wxConfigBase *cfg);
  void Save(wxConfigBase *cfg);
};

struct ClangSettings {
  ClangDiagnosticMode diagnosticMode = translationUnit;
  ClangCompletionMode completionMode = always;
  ClangResolveMode resolveMode = internalResolver;
  ClangWarningMode warningMode = warningDefault;
  int autocompletionDelay = 1500;     // minimum 250ms, maximum unlimited
  int resolveDiagnosticsDelay = 5000; // minimum 1000ms, maximum unlimited
  bool resolveDiagOnlyAfterSave = true;

  bool openSourceFilesInside = true;
  wxString extSourceOpenCommand; // external editor for opening cpp/hpp/c/h source files

  std::vector<std::string> customWarningFlags;

  void Load(wxConfigBase *cfg);
  void Save(wxConfigBase *cfg) const;

  void OpenExternalSourceFile(const wxString &file, int line);

  void AppendWarningFlags(std::vector<const char *> &out) const;
};

enum AiSummarizationChatMode {
  noSumarization = 0,
  heuristicsSumarization,
  aiModelSumarization
};

struct AiSettings {
  bool enabled = false; // Master switch - enables AI
  // sessions persistence
  bool storeChatHistory = false;
  AiSummarizationChatMode summarizeChatSessionMode = noSumarization;

  wxString id;                       // Model id.
  wxString name;                     // User name
  wxString endpointUrl;              // e.q. https://api.openai.com/v1/responses
  wxString model;                    // e.q. gpt-4.1, gpt-5.1-codex, deepseek-coder, ...
  int maxIterations;                 // Maximum number of iterations
  int requestTimeout;                // Request timeout for single call
  wxString extraRequestJson;         // JSON request extra content
  bool forceModelQueryRange = false; // force file_range policy
  // context management
  bool fullInfoRequest = true;
  bool floatingWindow = true;
  bool hasAuthentization = false;

  void Load(wxConfigBase *cfg);
  void Save(wxConfigBase *cfg) const;
};

struct AiModelSettings {
  wxString id;          // stable identifier (for selection + keychain)
  wxString name;        // profile username (displayed in choice)
  wxString endpointUrl; // https://.../v1/responses
  wxString model;       // gpt-4.1, ...
  int maxIterations = 5;
  int requestTimeout = 60;           // in seconds (0 = no limit)
  wxString extraRequestJson;         // raw JSON string
  bool forceModelQueryRange = false; // force file_range policy

  // context management
  bool fullInfoRequest = true;
  bool floatingWindow = true;

  // auth
  bool hasAuthentization = false;
};

class ArduinoEditorSettingsDialog : public wxDialog {
public:
  ArduinoEditorSettingsDialog(wxWindow *parent,
                              const EditorSettings &settings,
                              const ArduinoCliConfig &cliConfig,
                              const ClangSettings &clangSettings,
                              const AiSettings &aiSettings,
                              wxConfigBase *config, ArduinoCli *cli);

  EditorSettings GetSettings() const;
  ArduinoCliConfig GetCliConfig() const;
  ClangSettings GetClangSettings() const;
  wxString GetSketchesDir() const;
  wxString GetCliPath() const;
  wxString GetSelectedLanguage() const;
  long GetUpdateCheckIntervalHours() const;
  AiSettings GetAiSettings() const;

private:
  EditorSettings m_settings;
  ArduinoCliConfig m_cliCfg;
  ClangSettings m_clangSettings;
  AiSettings m_aiSettings;
  wxConfigBase *m_config;
  ArduinoCli *m_cli = nullptr;
  std::vector<UiLanguageEntry> m_langEntries;

  // AI models list (stored in wxConfigBase)
  std::vector<AiModelSettings> m_aiModels;

  wxNotebook *m_notebook = nullptr;

  // --- Editor page controls (what you already have there) ---
  wxFontPickerCtrl *m_fontPicker = nullptr;

  wxScrolledWindow *m_colorsPanel = nullptr;

  // Light theme colors
  wxColourPickerCtrl *m_text{nullptr};
  wxColourPickerCtrl *m_background{nullptr};
  wxColourPickerCtrl *m_installed{nullptr};
  wxColourPickerCtrl *m_deprecated{nullptr};
  wxColourPickerCtrl *m_updatable{nullptr};
  wxColourPickerCtrl *m_lineNumText{nullptr};
  wxColourPickerCtrl *m_lineNumBg{nullptr};
  wxColourPickerCtrl *m_selection{nullptr};
  wxColourPickerCtrl *m_edge{nullptr};
  wxColourPickerCtrl *m_whitespace{nullptr};
  wxColourPickerCtrl *m_braceMatch{nullptr};
  wxColourPickerCtrl *m_braceBad{nullptr};
  wxColourPickerCtrl *m_comment = nullptr;
  wxColourPickerCtrl *m_string = nullptr;
  wxColourPickerCtrl *m_number = nullptr;
  wxColourPickerCtrl *m_kw1 = nullptr;
  wxColourPickerCtrl *m_kw2 = nullptr;
  wxColourPickerCtrl *m_preprocessor = nullptr;
  wxColourPickerCtrl *m_caretLine = nullptr;
  wxColourPickerCtrl *m_calltipText = nullptr;
  wxColourPickerCtrl *m_calltipBackground = nullptr;
  wxColourPickerCtrl *m_error = nullptr;
  wxColourPickerCtrl *m_warning = nullptr;
  wxColourPickerCtrl *m_note = nullptr;
  wxColourPickerCtrl *m_symbolHighlight = nullptr;

  // Dark theme colors
  wxColourPickerCtrl *m_installedDark{nullptr};
  wxColourPickerCtrl *m_updatableDark{nullptr};
  wxColourPickerCtrl *m_deprecatedDark{nullptr};
  wxColourPickerCtrl *m_commentDark{nullptr};
  wxColourPickerCtrl *m_stringDark{nullptr};
  wxColourPickerCtrl *m_numberDark{nullptr};
  wxColourPickerCtrl *m_kw1Dark{nullptr};
  wxColourPickerCtrl *m_kw2Dark{nullptr};
  wxColourPickerCtrl *m_preprocessorDark{nullptr};
  wxColourPickerCtrl *m_caretLineDark{nullptr};
  wxColourPickerCtrl *m_calltipTextDark{nullptr};
  wxColourPickerCtrl *m_calltipBackgroundDark{nullptr};
  wxColourPickerCtrl *m_errorDark{nullptr};
  wxColourPickerCtrl *m_warningDark{nullptr};
  wxColourPickerCtrl *m_noteDark{nullptr};
  wxColourPickerCtrl *m_symbolHighlightDark{nullptr};
  wxColourPickerCtrl *m_textDark{nullptr};
  wxColourPickerCtrl *m_backgroundDark{nullptr};
  wxColourPickerCtrl *m_lineNumTextDark{nullptr};
  wxColourPickerCtrl *m_lineNumBgDark{nullptr};
  wxColourPickerCtrl *m_selectionDark{nullptr};
  wxColourPickerCtrl *m_edgeDark{nullptr};
  wxColourPickerCtrl *m_whitespaceDark{nullptr};
  wxColourPickerCtrl *m_braceMatchDark{nullptr};
  wxColourPickerCtrl *m_braceBadDark{nullptr};

  // AI chat
  wxColourPickerCtrl *m_aiUserBg{nullptr};
  wxColourPickerCtrl *m_aiAssistantBg{nullptr};
  wxColourPickerCtrl *m_aiSystemBg{nullptr};
  wxColourPickerCtrl *m_aiInfoBg{nullptr};
  wxColourPickerCtrl *m_aiErrorBg{nullptr};
  wxColourPickerCtrl *m_aiUserBgDark{nullptr};
  wxColourPickerCtrl *m_aiAssistantBgDark{nullptr};
  wxColourPickerCtrl *m_aiSystemBgDark{nullptr};
  wxColourPickerCtrl *m_aiInfoBgDark{nullptr};
  wxColourPickerCtrl *m_aiErrorBgDark{nullptr};

  wxSpinCtrl *m_tabWidth = nullptr;
  wxCheckBox *m_useTabs = nullptr;
  wxCheckBox *m_showWhitespace = nullptr;
  wxCheckBox *m_autoIndent = nullptr;

  wxCheckBox *m_showLineNumbers = nullptr;
  wxCheckBox *m_wordWrap = nullptr;
  wxCheckBox *m_showRightEdge = nullptr;
  wxSpinCtrl *m_edgeColumn = nullptr;
  wxCheckBox *m_highlightCurrentLine = nullptr;
  wxCheckBox *m_highlightSymbols = nullptr;
  wxCheckBox *m_highlightMatchingBraces = nullptr;
  wxCheckBox *m_displayHoverInfo = nullptr;

  // --- CLI page controls ---
  wxTextCtrl *m_sketchesDirCtrl = nullptr;
  wxTextCtrl *m_cliUrls = nullptr;
  wxCheckBox *m_cliUnsafe = nullptr;
  wxTextCtrl *m_cliProxy = nullptr;
  wxSpinCtrl *m_cliTimeout = nullptr;
  wxTextCtrl *m_cliPathCtrl = nullptr;
  wxButton *m_cliPathBrowse = nullptr;
  wxSpinCtrl *m_clangAutoDelay = nullptr;
  wxSpinCtrl *m_clangDiagDelay = nullptr;

  // --- Clang widgets ---
  wxChoice *m_languageChoice = nullptr;
  wxCheckBox *m_updatesEnable = nullptr;
  wxSpinCtrl *m_updatesDays = nullptr;
  wxChoice *m_clangDiagChoice = nullptr;
  wxChoice *m_clangCompChoice = nullptr;
  wxChoice *m_clangWarnChoice = nullptr;
  wxBitmapButton *m_btnEditCustomWarnings = nullptr;
  wxCheckBox *m_openSourceInside = nullptr;
  wxTextCtrl *m_clangExtSourceCmd = nullptr;
  wxButton *m_clangExtSourceBrowse = nullptr;
  wxChoice *m_clangResolveChoice = nullptr;
  wxCheckBox *m_resolveAfterSave = nullptr;

  // AI / LLM settings
  wxCheckBox *m_aiEnable = nullptr;

  // Model selection
  wxChoice *m_aiModelChoice = nullptr;
  wxBitmapButton *m_aiModelEditBtn = nullptr;
  wxBitmapButton *m_aiModelAddBtn = nullptr;

  // Chat history
  wxCheckBox *m_aiStoreChatHistory = nullptr;
  wxChoice *m_summarizeChatSessionsModeChoice = nullptr;

  void UpdateAiControlsEnabled();

  void OnSysColourChanged(wxSysColourChangedEvent &event);

  void EndModal(int retCode) override;

  void OnClose(wxCloseEvent &evt);
  void OnBrowseSketchesDir(wxCommandEvent &evt);
  void OnBrowseExtSourceCommand(wxCommandEvent &evt);
  void OnSetupClangFormatting(wxCommandEvent &evt);
  void OnEditCustomWarnings(wxCommandEvent &evt);
};
