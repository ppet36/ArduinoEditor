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

#include "ard_setdlg.hpp"
#include "ai_client.hpp"
#include "ard_aimdldlg.hpp"
#include "ard_ap.hpp"
#include "ard_fmtdlg.hpp"
#include "utils.hpp"
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <vector>
#include <wx/bmpbuttn.h>
#include <wx/combobox.h>
#include <wx/dir.h>
#include <wx/filedlg.h>
#include <wx/intl.h>
#include <wx/menu.h>
#include <wx/richmsgdlg.h>
#include <wx/secretstore.h>
#include <wx/sstream.h>
#include <wx/stc/stc.h>
#include <wx/tokenzr.h>
#include <wx/wfstream.h>
#include <wx/wx.h>

const wxString defaultClangFormat = wxT(
    "{\n"
    "  \"BasedOnStyle\": \"LLVM\",\n"
    "  \"SortIncludes\": false,\n"
    "  \"ReflowComments\": false,\n"
    "  \"AllowShortBlocksOnASingleLine\": \"Empty\",\n"
    "  \"BreakBeforeBraces\": \"Attach\",\n"
    "  \"SpaceBeforeParens\": \"ControlStatements\"\n"
    "}\n");

struct CStrListView {
  const char *const *data = nullptr;
  std::size_t size = 0;
};

inline CStrListView GetWarningFlagsView(ClangWarningMode mode) {
  static constexpr const char *kOff[] = {"-w"};
  static constexpr const char *kDefault[] = {};

  static constexpr const char *kArduinoLike[] = {
      "-Wformat=2",
      "-Wshadow",
      "-Wunused-variable",
      "-Wunused-parameter",
      "-Wsign-compare",
      "-Wreorder",
      "-Wimplicit-fallthrough"};

  static constexpr const char *kStrict[] = {
      "-Wall",
      "-Wextra",
      "-Wpedantic",
      "-Wconversion",
      "-Wformat=2",
      "-Wshadow",
      "-Wunused-variable",
      "-Wunused-parameter",
      "-Wsign-compare",
      "-Wreorder",
      "-Wimplicit-fallthrough"};

  switch (mode) {
    case ClangWarningMode::warningOff:
      return {kOff, sizeof(kOff) / sizeof(kOff[0])};
    case ClangWarningMode::warningDefault:
      return {kDefault, 0};
    case ClangWarningMode::warningArduinoLike:
      return {kArduinoLike, sizeof(kArduinoLike) / sizeof(kArduinoLike[0])};
    case ClangWarningMode::warningStrict:
      return {kStrict, sizeof(kStrict) / sizeof(kStrict[0])};
    case ClangWarningMode::warningCustom:
      return {kDefault, 0};
  }
  return {kDefault, 0};
}

static wxString WarningFlagsToMultilineText(ClangWarningMode mode) {
  wxString out;
  auto v = GetWarningFlagsView(mode);
  for (std::size_t i = 0; i < v.size; ++i) {
    if (!out.empty()) {
      out += wxT("\n");
    }
    out += wxString::FromUTF8(v.data[i]);
  }
  return out;
}

static wxString JoinFlagsMultiline(const std::vector<std::string> &flags) {
  wxString out;
  for (const auto &s : flags) {
    if (s.empty())
      continue;
    if (!out.empty())
      out += wxT("\n");
    out += wxString::FromUTF8(s.c_str());
  }
  return out;
}

static void SplitFlagsWhitespace(const wxString &text, std::vector<std::string> &out) {
  out.clear();

  wxStringTokenizer tok(text, wxT(" \t\r\n"), wxTOKEN_STRTOK);
  while (tok.HasMoreTokens()) {
    wxString t = TrimCopy(tok.GetNextToken());
    if (!t.empty()) {
      out.emplace_back(wxToStd(t));
    }
  }
}

static wxColour ReadEditorColour(wxConfigBase *cfg, const wxString &prefix, ThemeMode themeMode, const wxColour &defColor) {
  long r, g, b;
  wxString p = wxString::Format(wxT("Editor/%d/"), (int)themeMode);

  if (cfg->Read(p + prefix + wxT("R"), &r) &&
      cfg->Read(p + prefix + wxT("G"), &g) &&
      cfg->Read(p + prefix + wxT("B"), &b)) {
    return wxColour(r, g, b);
  } else {
    return defColor;
  }
}

static void ConfigReadString(wxConfigBase *cfg, const wxString &key, wxString &value, const wxString &defValue) {
  wxString v;
  if (cfg->Read(key, &v)) {
    if (v.IsEmpty()) {
      value = defValue;
    } else {
      value = v;
    }
  } else {
    value = defValue;
  }
}

static void ConfigReadInt(wxConfigBase *cfg, const wxString &key, int &value, int defValue) {
  long lw;
  if (cfg->Read(key, &lw)) {
    value = (int)lw;
  } else {
    value = defValue;
  }
}

static void ConfigReadBool(wxConfigBase *cfg, const wxString &key, bool &value, bool defValue) {
  bool b;
  if (cfg->Read(key, &b)) {
    value = b;
  } else {
    value = defValue;
  }
}

static void WriteEditorColour(wxConfigBase *cfg, const wxString &prefix, ThemeMode themeMode, const wxColour &color) {
  wxString p = wxString::Format(wxT("Editor/%d/"), (int)themeMode);
  cfg->Write(p + prefix + wxT("R"), (long)color.Red());
  cfg->Write(p + prefix + wxT("G"), (long)color.Green());
  cfg->Write(p + prefix + wxT("B"), (long)color.Blue());
}

static wxString GenAiModelId() {
  static long counter = 0;
  ++counter;
  auto ms = wxGetUTCTimeMillis();
  return wxString::Format(wxT("mdl_%lld_%ld"), (long long)ms.GetValue(), counter);
}

static wxString AiModelsBaseKey() {
  return wxT("AI/Models");
}

static void SaveAiModels(wxConfigBase *cfg, const std::vector<AiModelSettings> &models) {
  if (!cfg)
    return;

  cfg->DeleteGroup(AiModelsBaseKey());
  cfg->Write(AiModelsBaseKey() + wxT("/Count"), (long)models.size());

  for (size_t i = 0; i < models.size(); ++i) {
    const auto &m = models[i];
    wxString base = AiModelsBaseKey() + wxString::Format(wxT("/Item%zu"), i);

    cfg->Write(base + wxT("/Id"), m.id);
    cfg->Write(base + wxT("/Name"), m.name);
    cfg->Write(base + wxT("/EndpointUrl"), m.endpointUrl);
    cfg->Write(base + wxT("/Model"), m.model);

    cfg->Write(base + wxT("/MaxIterations"), (long)m.maxIterations);
    cfg->Write(base + wxT("/RequestTimeout"), (long)m.requestTimeout);

    cfg->Write(base + wxT("/ExtraRequestJson"), m.extraRequestJson);

    cfg->Write(base + wxT("/ForceModelQueryRange"), m.forceModelQueryRange);
    cfg->Write(base + wxT("/FullInfoRequest"), m.fullInfoRequest);
    cfg->Write(base + wxT("/FloatingWindow"), m.floatingWindow);
    cfg->Write(base + wxT("/HasAuthentization"), m.hasAuthentization);
  }
}

// -----------------------------------------------------------------------------------------------------

void EditorColorScheme::Load(wxConfigBase *cfg, ThemeMode themeMode) {
  // Base fg/bg
  wxColour defBg = (themeMode == ThemeMode::AlwaysDark) ? wxColour(30, 30, 30) : wxColour(255, 255, 255);
  wxColour defText = (themeMode == ThemeMode::AlwaysDark) ? wxColour(212, 212, 212) : wxColour(0, 0, 0);

  background = ReadEditorColour(cfg, wxT("Background"), themeMode, defBg);
  text = ReadEditorColour(cfg, wxT("Text"), themeMode, defText);

  // Installed / deprecated / updatable
  wxColour defInstalled = (themeMode == ThemeMode::AlwaysDark) ? wxColour(30, 120, 30) : wxColour(230, 255, 230);
  wxColour defUpdatable = (themeMode == ThemeMode::AlwaysDark) ? wxColour(60, 60, 60) : wxColour(235, 235, 235);
  wxColour defDeprecated = (themeMode == ThemeMode::AlwaysDark) ? wxColour(212, 212, 212) : wxColour(0, 0, 0);

  installed = ReadEditorColour(cfg, wxT("Installed"), themeMode, defInstalled);
  updatable = ReadEditorColour(cfg, wxT("Updatable"), themeMode, defUpdatable);
  deprecated = ReadEditorColour(cfg, wxT("Deprecated"), themeMode, defDeprecated);

  // Line numbers
  wxColour defLnBg = defBg;
  wxColour defLnFg = (themeMode == ThemeMode::AlwaysDark) ? wxColour(133, 133, 133) : wxColour(128, 128, 128);

  lineNumberBackground = ReadEditorColour(cfg, wxT("LineNumberBg"), themeMode, defLnBg);
  lineNumberText = ReadEditorColour(cfg, wxT("LineNumberFg"), themeMode, defLnFg);

  // Selection, edge, whitespace
  wxColour defSel = (themeMode == ThemeMode::AlwaysDark) ? wxColour(38, 79, 120) : wxColour(200, 220, 255);
  wxColour defEdge = (themeMode == ThemeMode::AlwaysDark) ? wxColour(70, 70, 70) : wxColour(200, 200, 200);
  wxColour defWs = (themeMode == ThemeMode::AlwaysDark) ? wxColour(80, 80, 80) : wxColour(220, 220, 220);

  selection = ReadEditorColour(cfg, wxT("Selection"), themeMode, defSel);
  edge = ReadEditorColour(cfg, wxT("Edge"), themeMode, defEdge);
  whitespace = ReadEditorColour(cfg, wxT("Whitespace"), themeMode, defWs);

  // Braces
  wxColour defBraceMatch = (themeMode == ThemeMode::AlwaysDark) ? wxColour(80, 150, 80) : wxColour(200, 255, 200);
  wxColour defBraceBad = wxColour(244, 71, 71);

  braceMatch = ReadEditorColour(cfg, wxT("BraceMatch"), themeMode, defBraceMatch);
  braceBad = ReadEditorColour(cfg, wxT("BraceBad"), themeMode, defBraceBad);

  comment = ReadEditorColour(cfg, wxT("Comment"), themeMode,
                             (themeMode == ThemeMode::AlwaysDark)
                                 ? wxColour(106, 153, 85) // Dark: VSCode green
                                 : wxColour(0, 128, 0)    // Light
  );

  string = ReadEditorColour(
      cfg, wxT("String"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(206, 145, 120) // Dark: VSCode string
          : wxColour(163, 21, 21)   // Light: classic C++ red
  );

  keyword1 = ReadEditorColour(
      cfg, wxT("Keyword1"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(86, 156, 214) // Dark: blue (#569CD6)
          : wxColour(0, 0, 255));

  keyword2 = ReadEditorColour(
      cfg, wxT("Keyword2"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(79, 193, 255) // slightly lighter blue
          : wxColour(0, 128, 192));

  number = ReadEditorColour(
      cfg, wxT("Number"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(181, 206, 168) // Dark: pastel green
          : wxColour(163, 21, 21)   // Light: similar to string
  );

  preprocessor = ReadEditorColour(
      cfg, wxT("Preprocessor"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(197, 134, 192) // Dark: VSCode purple
          : wxColour(128, 0, 128)   // Light: purple
  );

  caretLine = ReadEditorColour(
      cfg, wxT("CaretLine"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(42, 42, 42)
          : wxColour(240, 240, 240));

  calltipText = ReadEditorColour(
      cfg, wxT("CalltipText"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(212, 212, 212) // Dark: #D4D4D4 (VS/VSCode editor foreground-ish)
          : wxColour(30, 30, 30));  // Light: #1E1E1E (near-black)

  calltipBackground = ReadEditorColour(
      cfg, wxT("CalltipBackground"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(65, 65, 78)      // Dark: (VS/VSCode tooltip-ish dark)
          : wxColour(255, 255, 225)); // Light: #FFFFE1 (classic Scintilla/tooltip „pale yellow“)

  error = ReadEditorColour(
      cfg, wxT("Error"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(255, 85, 85)
          : wxColour(220, 30, 30));

  warning = ReadEditorColour(
      cfg, wxT("Warning"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(255, 205, 112)
          : wxColour(255, 160, 0));

  note = ReadEditorColour(
      cfg, wxT("Note"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(86, 156, 214)  // Dark: info blue (#569CD6)
          : wxColour(0, 102, 204)); // Light: calmer blue (#0066CC)

  symbolHighlight = ReadEditorColour(
      cfg, wxT("SymbolHighlight"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(90, 140, 240)    // blue highlight
          : wxColour(255, 255, 128)); // yellow highlight

  // Markdown / chat background colors
  aiUserBg = ReadEditorColour(
      cfg, wxT("AiUserBackground"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(48, 64, 96)      // #304060  dark
          : wxColour(232, 242, 255)); // #e8f2ff light

  aiAssistantBg = ReadEditorColour(
      cfg, wxT("AiAssistantBackground"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(58, 58, 58)      // #3a3a3a dark
          : wxColour(242, 242, 242)); // #f2f2f2 light

  aiSystemBg = ReadEditorColour(
      cfg, wxT("AiSystemBackground"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(128, 106, 58)    // #806a3a dark
          : wxColour(255, 244, 214)); // #fff4d6 light

  aiInfoBg = ReadEditorColour(
      cfg, wxT("AiInfoBackground"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(35, 75, 95)      // #234b5f dark
          : wxColour(230, 247, 255)); // #e6f7ff light

  aiErrorBg = ReadEditorColour(
      cfg, wxT("AiErrorBackground"), themeMode,
      (themeMode == ThemeMode::AlwaysDark)
          ? wxColour(96, 48, 48)      // #603030 dark
          : wxColour(255, 230, 230)); // #ffe6e6 light
}

void EditorColorScheme::Save(wxConfigBase *cfg, ThemeMode themeMode) {
  WriteEditorColour(cfg, wxT("Background"), themeMode, background);
  WriteEditorColour(cfg, wxT("Text"), themeMode, text);
  WriteEditorColour(cfg, wxT("Installed"), themeMode, installed);
  WriteEditorColour(cfg, wxT("Updatable"), themeMode, updatable);
  WriteEditorColour(cfg, wxT("Deprecated"), themeMode, deprecated);
  WriteEditorColour(cfg, wxT("LineNumberBg"), themeMode, lineNumberBackground);
  WriteEditorColour(cfg, wxT("LineNumberFg"), themeMode, lineNumberText);
  WriteEditorColour(cfg, wxT("Selection"), themeMode, selection);
  WriteEditorColour(cfg, wxT("Edge"), themeMode, edge);
  WriteEditorColour(cfg, wxT("Whitespace"), themeMode, whitespace);
  WriteEditorColour(cfg, wxT("BraceMatch"), themeMode, braceMatch);
  WriteEditorColour(cfg, wxT("BraceBad"), themeMode, braceBad);
  WriteEditorColour(cfg, wxT("Comment"), themeMode, comment);
  WriteEditorColour(cfg, wxT("String"), themeMode, string);
  WriteEditorColour(cfg, wxT("Keyword1"), themeMode, keyword1);
  WriteEditorColour(cfg, wxT("Keyword2"), themeMode, keyword2);
  WriteEditorColour(cfg, wxT("Number"), themeMode, number);
  WriteEditorColour(cfg, wxT("Preprocessor"), themeMode, preprocessor);
  WriteEditorColour(cfg, wxT("CaretLine"), themeMode, caretLine);
  WriteEditorColour(cfg, wxT("CalltipText"), themeMode, calltipText);
  WriteEditorColour(cfg, wxT("CalltipBackground"), themeMode, calltipBackground);
  WriteEditorColour(cfg, wxT("Error"), themeMode, error);
  WriteEditorColour(cfg, wxT("Warning"), themeMode, warning);
  WriteEditorColour(cfg, wxT("Note"), themeMode, note);
  WriteEditorColour(cfg, wxT("SymbolHighlight"), themeMode, symbolHighlight);
  WriteEditorColour(cfg, wxT("AiUserBackground"), themeMode, aiUserBg);
  WriteEditorColour(cfg, wxT("AiAssistantBackground"), themeMode, aiAssistantBg);
  WriteEditorColour(cfg, wxT("AiSystemBackground"), themeMode, aiSystemBg);
  WriteEditorColour(cfg, wxT("AiInfoBackground"), themeMode, aiInfoBg);
  WriteEditorColour(cfg, wxT("AiErrorBackground"), themeMode, aiErrorBg);
}

void EditorSettings::Load(wxConfigBase *cfg) {
  colors[0].Load(cfg, ThemeMode::AlwaysLight);
  colors[1].Load(cfg, ThemeMode::AlwaysDark);

  ConfigReadInt(cfg, wxT("Editor/TabWidth"), tabWidth, 2);
  ConfigReadBool(cfg, wxT("Editor/UseTabs"), useTabs, false);
  ConfigReadBool(cfg, wxT("Editor/ShowWhitespace"), showWhitespace, false);
  ConfigReadBool(cfg, wxT("Editor/AutoIndent"), autoIndent, true);
  ConfigReadBool(cfg, wxT("Editor/ShowLineNumbers"), showLineNumbers, true);
  ConfigReadBool(cfg, wxT("Editor/WordWrap"), wordWrap, false);
  ConfigReadBool(cfg, wxT("Editor/ShowRightEdge"), showRightEdge, false);
  ConfigReadInt(cfg, wxT("Editor/EdgeColumn"), edgeColumn, 80);
  ConfigReadBool(cfg, wxT("Editor/HighlightCurrentLine"), highlightCurrentLine, true);
  ConfigReadBool(cfg, wxT("Editor/HighlightSymbols"), highlightSymbols, true);
  ConfigReadBool(cfg, wxT("Editor/HighlightMatchingBraces"), highlightMatchingBraces, true);
  ConfigReadBool(cfg, wxT("Editor/DisplayHoverInfo"), displayHoverInfo, true);
  ConfigReadString(cfg, wxT("Editor/Font"), fontDesc, wxEmptyString);
  ConfigReadString(cfg, wxT("Editor/ClangFormatOverridesJson"), clangFormatOverridesJson, defaultClangFormat);
}

EditorColorScheme EditorSettings::GetColors() const {
  int index = 0;

  wxSystemAppearance app = wxSystemSettings::GetAppearance();
  if (app.IsDark()) {
    index = 1; // dark
  } else {
    index = 0; // light
  }

  return colors[index];
}

wxFont EditorSettings::GetFont() const {
  if (!fontDesc.empty()) {
    wxFont font;
    font.SetNativeFontInfo(fontDesc);
    if (font.IsOk()) {
      return font;
    }
  }
  // fallback/default
  wxFont font(wxFontInfo(11).Family(wxFONTFAMILY_MODERN));
  return font;
}

void EditorSettings::Save(wxConfigBase *cfg) {
  colors[0].Save(cfg, ThemeMode::AlwaysLight);
  colors[1].Save(cfg, ThemeMode::AlwaysDark);

  cfg->Write(wxT("Editor/ThemeMode"), (long)ThemeMode::FollowSystem);
  cfg->Write(wxT("Editor/TabWidth"), (long)tabWidth);
  cfg->Write(wxT("Editor/UseTabs"), useTabs);
  cfg->Write(wxT("Editor/ShowWhitespace"), showWhitespace);
  cfg->Write(wxT("Editor/AutoIndent"), autoIndent);

  cfg->Write(wxT("Editor/ShowLineNumbers"), showLineNumbers);
  cfg->Write(wxT("Editor/WordWrap"), wordWrap);
  cfg->Write(wxT("Editor/ShowRightEdge"), showRightEdge);
  cfg->Write(wxT("Editor/EdgeColumn"), (long)edgeColumn);
  cfg->Write(wxT("Editor/HighlightCurrentLine"), highlightCurrentLine);
  cfg->Write(wxT("Editor/HighlightSymbols"), highlightSymbols);
  cfg->Write(wxT("Editor/HighlightMatchingBraces"), highlightMatchingBraces);
  cfg->Write(wxT("Editor/DisplayHoverInfo"), displayHoverInfo);
  cfg->Write(wxT("Editor/Font"), fontDesc);
  cfg->Write(wxT("Editor/ClangFormatOverridesJson"), clangFormatOverridesJson);
}

void ClangSettings::Load(wxConfigBase *cfg) {
  long lw;

  if (cfg->Read(wxT("Clang/DiagnosticMode"), &lw))
    diagnosticMode = static_cast<ClangDiagnosticMode>(lw);
  else
    diagnosticMode = translationUnit;

  if (cfg->Read(wxT("Clang/CompletionMode"), &lw))
    completionMode = static_cast<ClangCompletionMode>(lw);
  else
    completionMode = always;

  if (cfg->Read(wxT("Clang/ResolveMode"), &lw))
    resolveMode = static_cast<ClangResolveMode>(lw);
  else
    resolveMode = internalResolver;

  if (cfg->Read(wxT("Clang/WarningMode"), &lw))
    warningMode = static_cast<ClangWarningMode>(lw);
  else
    warningMode = warningDefault;

  ConfigReadInt(cfg, wxT("Clang/AutocompletionDelay"), autocompletionDelay, 1500);
  ConfigReadInt(cfg, wxT("Clang/ResolveDiagnosticsDelay"), resolveDiagnosticsDelay, 5000);
  ConfigReadBool(cfg, wxT("Clang/ResolveOnlyAfterSave"), resolveDiagOnlyAfterSave, true);
  ConfigReadBool(cfg, wxT("Clang/DisplayDiagnosticsOnlyFromSketch"), displayDiagnosticsOnlyFromSketch, true);
  ConfigReadString(cfg, wxT("Clang/ExtSourceOpenCommand"), extSourceOpenCommand, wxEmptyString);
  ConfigReadBool(cfg, wxT("Clang/OpenSourceFilesInside"), openSourceFilesInside, true);

  int i, wfc;
  customWarningFlags.clear();

  ConfigReadInt(cfg, wxT("Clang/CustomWarningFlagsCount"), wfc, 0);
  for (i = 0; i < wfc; i++) {
    wxString s;
    ConfigReadString(cfg, wxString::Format(wxT("Clang/CustomWarningFlag%d"), i), s, wxEmptyString);
    if (!s.IsEmpty()) {
      customWarningFlags.push_back(wxToStd(s));
    }
  }
}

void ClangSettings::Save(wxConfigBase *cfg) const {
  cfg->Write(wxT("Clang/DiagnosticMode"), (long)diagnosticMode);
  cfg->Write(wxT("Clang/CompletionMode"), (long)completionMode);
  cfg->Write(wxT("Clang/ResolveMode"), (long)resolveMode);
  cfg->Write(wxT("Clang/WarningMode"), (long)warningMode);
  cfg->Write(wxT("Clang/AutocompletionDelay"), (long)autocompletionDelay);
  cfg->Write(wxT("Clang/ResolveDiagnosticsDelay"), (long)resolveDiagnosticsDelay);
  cfg->Write(wxT("Clang/ResolveOnlyAfterSave"), resolveDiagOnlyAfterSave);
  cfg->Write(wxT("Clang/DisplayDiagnosticsOnlyFromSketch"), displayDiagnosticsOnlyFromSketch);
  cfg->Write(wxT("Clang/ExtSourceOpenCommand"), extSourceOpenCommand);
  cfg->Write(wxT("Clang/OpenSourceFilesInside"), openSourceFilesInside);

  cfg->Write(wxT("Clang/CustomWarningFlagsCount"), (long)customWarningFlags.size());
  int index = 0;
  for (const auto &f : customWarningFlags) {
    if (!f.empty()) {
      cfg->Write(wxString::Format(wxT("Clang/CustomWarningFlag%d"), index), wxString::FromUTF8(f));
      index++;
    }
  }
}

void ClangSettings::OpenExternalSourceFile(const wxString &filename, int line) {
  if (filename.empty())
    return;

  wxString cmd = TrimCopy(extSourceOpenCommand);

  // 1) If the command is not set -> use the default OS application
  if (cmd.empty()) {
    wxLaunchDefaultApplication(filename);
    return;
  }

  // 2) The command is filled-in, but does not contain %s -> we run the program
  // and pass it filename as the first argument.
  // (line is not resolved in this mode.)
  if (cmd.Find(wxT("%s")) == wxNOT_FOUND) {
    cmd += wxT(" \"") + filename + wxT("\"");
    // async, we don't want to block the GUI
    wxExecute(cmd, wxEXEC_ASYNC);
    return;
  }

  // 3) The command contains placeholders -> replace %s and possibly %l
  wxString formatted = cmd;

  // filename - I leave it without automatic quoting, the user can
  // write in the command e.g. `"code" -g "%s":%l`
  formatted.Replace(wxT("%s"), filename);

  if (formatted.Find(wxT("%l")) != wxNOT_FOUND) {
    if (line <= 0)
      line = 1;
    wxString lineStr;
    lineStr.Printf(wxT("%d"), line);
    formatted.Replace(wxT("%l)"), lineStr);
  }

  // Run via shell (wxExecute(string) -> /bin/sh -c / cmd.exe /c)
  wxExecute(formatted, wxEXEC_ASYNC);
}

void ClangSettings::AppendWarningFlags(std::vector<const char *> &out) const {
  if (warningMode == warningCustom) {
    out.reserve(out.size() + customWarningFlags.size());
    for (const auto &s : customWarningFlags) {
      out.push_back(s.c_str());
    }
    return;
  }

  auto v = GetWarningFlagsView(warningMode);
  out.insert(out.end(), v.data, v.data + v.size);
}

void AiSettings::Load(wxConfigBase *cfg) {
  ConfigReadBool(cfg, wxT("AI/Enabled"), enabled, false);
  ConfigReadString(cfg, wxT("AI/EndpointUrl"), endpointUrl, wxEmptyString);
  ConfigReadString(cfg, wxT("AI/Model"), model, wxEmptyString);
  ConfigReadString(cfg, wxT("AI/ID"), id, wxEmptyString);
  ConfigReadString(cfg, wxT("AI/Name"), name, wxEmptyString);
  ConfigReadInt(cfg, wxT("AI/MaxIterations"), maxIterations, 5);
  ConfigReadInt(cfg, wxT("AI/RequestTimeout"), requestTimeout, 60);
  ConfigReadString(cfg, wxT("AI/ExtraRequestJson"), extraRequestJson, wxEmptyString);
  ConfigReadBool(cfg, wxT("AI/StoreChatHistory"), storeChatHistory, false);

  long lw;
  if (cfg->Read(wxT("AI/SummarizationChatMode"), &lw))
    summarizeChatSessionMode = static_cast<AiSummarizationChatMode>(lw);
  else
    summarizeChatSessionMode = noSumarization;

  ConfigReadBool(cfg, wxT("AI/FullInfoRequest"), fullInfoRequest, true);
  ConfigReadBool(cfg, wxT("AI/FloatingWindow"), floatingWindow, true);
  ConfigReadBool(cfg, wxT("AI/HasAuthentization"), hasAuthentization, false);
}

void AiSettings::Save(wxConfigBase *cfg) const {
  cfg->Write(wxT("AI/Enabled"), enabled);
  cfg->Write(wxT("AI/EndpointUrl"), endpointUrl);
  cfg->Write(wxT("AI/Model"), model);
  cfg->Write(wxT("AI/ID"), id);
  cfg->Write(wxT("AI/Name"), name);
  cfg->Write(wxT("AI/MaxIterations"), maxIterations);
  cfg->Write(wxT("AI/RequestTimeout"), requestTimeout);
  cfg->Write(wxT("AI/ExtraRequestJson"), extraRequestJson);
  cfg->Write(wxT("AI/StoreChatHistory"), storeChatHistory);
  cfg->Write(wxT("AI/SummarizationChatMode"), (long)summarizeChatSessionMode);
  cfg->Write(wxT("AI/FullInfoRequest"), fullInfoRequest);
  cfg->Write(wxT("AI/FloatingWindow"), floatingWindow);
  cfg->Write(wxT("AI/HasAuthentization"), hasAuthentization);
}

static long ParseDurationToSeconds(const std::string &val) {
  // empty = use default 60s
  std::string s = val;
  // trim
  auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && is_space((unsigned char)s.front()))
    s.erase(s.begin());
  while (!s.empty() && is_space((unsigned char)s.back()))
    s.pop_back();

  if (s.empty())
    return 60;

  long total = 0;
  long current = 0;
  bool anyUnit = false;

  auto flushNumber = [&](char unit) {
    if (current < 0)
      return;
    anyUnit = true;
    switch (unit) {
      case 'h':
        total += current * 3600;
        break;
      case 'm':
        total += current * 60;
        break;
      case 's':
        total += current;
        break;
      default:
        break;
    }
    current = 0;
  };

  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c >= '0' && c <= '9') {
      current = current * 10 + (c - '0');
    } else if (c == 'h' || c == 'm' || c == 's') {
      flushNumber(c);
    } else {
      // unknown character -> interrupt
      break;
    }
  }

  if (!anyUnit) {
    // pure number = seconds
    return current;
  }

  return total;
}

static std::string SecondsToDuration(long sec) {
  if (sec <= 0) {
    // 0 = no limit according to docs
    return "0";
  }
  // simple: always as "<sec>s"
  return std::to_string(sec) + "s";
}

ArduinoEditorSettingsDialog::ArduinoEditorSettingsDialog(wxWindow *parent,
                                                         const EditorSettings &settings,
                                                         const ArduinoCliConfig &cliConfig,
                                                         const ClangSettings &clangSettings,
                                                         const AiSettings &aiSettings,
                                                         wxConfigBase *config, ArduinoCli *cli)
    : wxDialog(parent, wxID_ANY, _("Settings"),
               wxDefaultPosition, wxSize(760, 600), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_settings(settings), m_cliCfg(cliConfig), m_clangSettings(clangSettings), m_aiSettings(aiSettings), m_config(config), m_cli(cli) {

  Freeze();

  // AI models list
  LoadAiModels(m_config, m_aiModels);

  // fallback: if the list is empty, create a default profile from the current AiSettings
  if (m_aiModels.empty()) {
    AiModelSettings m;
    m.id = GenAiModelId();
    m.name = _("Default");
    m.endpointUrl = TrimCopy(m_aiSettings.endpointUrl);
    m.model = TrimCopy(m_aiSettings.model);
    m.maxIterations = m_aiSettings.maxIterations;
    m.requestTimeout = m_aiSettings.requestTimeout;
    m.extraRequestJson = TrimCopy(m_aiSettings.extraRequestJson);
    m.forceModelQueryRange = m_aiSettings.forceModelQueryRange;
    m.fullInfoRequest = m_aiSettings.fullInfoRequest;
    m.floatingWindow = m_aiSettings.floatingWindow;
    m.hasAuthentization = false;
    m_aiModels.push_back(m);
  }

  // UI
  auto *mainSizer = new wxBoxSizer(wxVERTICAL);

  // --- Notebook ---
  m_notebook = new wxNotebook(this, wxID_ANY);

  // ==== GENERAL PAGE ====
  wxPanel *generalPage = new wxPanel(m_notebook, wxID_ANY);
  auto *generalPageSizer = new wxBoxSizer(wxVERTICAL);

  // --- Language box ---
  auto *langBox = new wxStaticBoxSizer(wxVERTICAL, generalPage, _("Language"));

  auto *langRow = new wxBoxSizer(wxHORIZONTAL);
  auto *langLabel = new wxStaticText(generalPage, wxID_ANY, _("User interface language:"));
  langRow->Add(langLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

  m_languageChoice = new wxChoice(generalPage, wxID_ANY);
  const int langIndent = langLabel->GetBestSize().GetWidth() + 5;

  m_langEntries.clear();

  m_langEntries.push_back({wxT("system"), _("System language")});
  m_langEntries.push_back({wxT("en"), _("English")});

  wxString locBase = GetLocalizationBaseDir();
  wxDir dir(locBase);
  if (dir.IsOpened()) {
    wxString subdir;
    bool cont = dir.GetFirst(&subdir, wxEmptyString, wxDIR_DIRS);
    while (cont) {
      // expected structure <locBase>/<subdir>/LC_MESSAGES/ArduinoEditor.mo
      wxFileName mo(locBase, wxEmptyString);
      mo.AppendDir(subdir);
      mo.AppendDir(wxT("LC_MESSAGES"));
      mo.SetFullName(wxT("ArduinoEditor.mo"));

      if (mo.FileExists()) {
        wxString code = subdir;

        const wxLanguageInfo *info = wxLocale::FindLanguageInfo(code);
        wxString label;
        if (info) {
          label = info->Description;
        } else {
          label = code;
        }

        m_langEntries.push_back({code, label});
      }

      cont = dir.GetNext(&subdir);
    }
  }

  wxArrayString choices;
  for (const auto &e : m_langEntries) {
    choices.Add(e.label);
  }
  m_languageChoice->Append(choices);

  wxString langPref = wxT("system");
  if (m_config) {
    m_config->Read(wxT("Language"), &langPref, wxT("system"));
  }

  int sel = 0;
  for (size_t i = 0; i < m_langEntries.size(); ++i) {
    if (m_langEntries[i].code == langPref) {
      sel = static_cast<int>(i);
      break;
    }
  }
  m_languageChoice->SetSelection(sel);
  langRow->Add(m_languageChoice, 1, wxEXPAND);
  langBox->Add(langRow, 0, wxALL | wxEXPAND, 5);

  auto *langHint = new wxStaticText(
      generalPage, wxID_ANY,
      _("Language change will take effect after restarting the application."));
  langHint->Wrap(520);

  auto *langHintRow = new wxBoxSizer(wxHORIZONTAL);
  langHintRow->Add(langIndent, 0);
  langHintRow->Add(langHint, 1, wxEXPAND);
  langBox->Add(langHintRow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

  generalPageSizer->Add(langBox, 0, wxALL | wxEXPAND, 10);

  // --- Updates box ---
  auto *updatesBox = new wxStaticBoxSizer(wxVERTICAL, generalPage, _("Updates"));

  long updHours = 24;
  if (m_config) {
    m_config->Read(wxT("ArduinoEditor/Updates/check_interval_hours"), &updHours, 24L);
  }

  bool updEnabled = (updHours > 0);
  int updDays = 1;
  if (updHours > 0) {
    updDays = (int)((updHours + 23) / 24); // round up
    if (updDays < 1) {
      updDays = 1;
    }
  }

  auto *updRow = new wxBoxSizer(wxHORIZONTAL);

  // left label
  updRow->Add(new wxStaticText(generalPage, wxID_ANY, _("Automatically check for updates ")),
              0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

  // right side controls
  m_updatesEnable = new wxCheckBox(generalPage, wxID_ANY, _("every"));
  m_updatesEnable->SetValue(updEnabled);
  updRow->Add(m_updatesEnable, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

  m_updatesDays = new wxSpinCtrl(generalPage, wxID_ANY);
  m_updatesDays->SetRange(1, 30);
  m_updatesDays->SetValue(updDays);
  updRow->Add(m_updatesDays, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

  updRow->Add(new wxStaticText(generalPage, wxID_ANY, _("day(s)")),
              0, wxALIGN_CENTER_VERTICAL);

  updatesBox->Add(updRow, 0, wxALL | wxEXPAND, 5);

  // enable/disable spin based on checkbox
  m_updatesDays->Enable(m_updatesEnable->GetValue());
  m_updatesEnable->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
    if (m_updatesDays && m_updatesEnable) {
      m_updatesDays->Enable(m_updatesEnable->GetValue());
    }
  });

  generalPageSizer->Add(updatesBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  // === External files ===
  auto *extLinksBox = new wxStaticBoxSizer(wxVERTICAL, generalPage, _("External files"));

  m_openSourceInside = new wxCheckBox(
      generalPage,
      wxID_ANY,
      _("Open external C/C++ source files inside the IDE (when possible)"));
  m_openSourceInside->SetValue(m_clangSettings.openSourceFilesInside);
  extLinksBox->Add(m_openSourceInside, 0, wxALL | wxEXPAND, 5);

  auto *extInfo = new wxStaticText(
      generalPage, wxID_ANY,
      _("When this option is disabled, the following command is used to open C/C++ source files "
        "that are outside the current sketch/project.\n"
        "Use %s as a placeholder for the file path, and %l for the line number.\n"
        "All other file types will be opened with the system's default application."));

  extLinksBox->Add(extInfo, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

  auto *extRow = new wxBoxSizer(wxHORIZONTAL);

  m_clangExtSourceCmd = new wxTextCtrl(
      generalPage,
      wxID_ANY,
      m_clangSettings.extSourceOpenCommand);

  extRow->Add(m_clangExtSourceCmd, 1, wxRIGHT | wxEXPAND, 5);

  m_clangExtSourceBrowse = new wxButton(
      generalPage,
      wxID_ANY,
      _("Browse..."));

  extRow->Add(m_clangExtSourceBrowse, 0);

  // Browse handler
  m_clangExtSourceBrowse->Bind(
      wxEVT_BUTTON,
      &ArduinoEditorSettingsDialog::OnBrowseExtSourceCommand,
      this);

  // Init enabled state
  bool inside = m_openSourceInside->GetValue();
  m_clangExtSourceCmd->Enable(!inside);
  m_clangExtSourceBrowse->Enable(!inside);

  m_openSourceInside->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &evt) {
    bool useInside = m_openSourceInside->GetValue();
    if (m_clangExtSourceCmd)
      m_clangExtSourceCmd->Enable(!useInside);
    if (m_clangExtSourceBrowse)
      m_clangExtSourceBrowse->Enable(!useInside);
    evt.Skip();
  });

  extLinksBox->Add(extRow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

  generalPageSizer->Add(extLinksBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  generalPage->SetSizer(generalPageSizer);
  m_notebook->AddPage(generalPage, _("General"), true);

  // ==== CLI PAGE ====
  wxPanel *cliPage = new wxPanel(m_notebook, wxID_ANY);
  auto *cliSizer = new wxBoxSizer(wxVERTICAL);

  // ---- Sketches directory ----
  wxString sketchesDir;
  if (!m_config->Read(wxT("SketchesDir"), &sketchesDir)) {
    sketchesDir = wxGetHomeDir();
  }

  auto *sketchRow = new wxBoxSizer(wxHORIZONTAL);

  sketchRow->Add(new wxStaticText(cliPage, wxID_ANY, _("Sketches directory:")),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

  m_sketchesDirCtrl = new wxTextCtrl(cliPage, wxID_ANY, sketchesDir);
  sketchRow->Add(m_sketchesDirCtrl, 1, wxRIGHT | wxEXPAND, 5);

  auto *browseBtn = new wxButton(cliPage, wxID_ANY, _("Browse..."));
  sketchRow->Add(browseBtn, 0);

  browseBtn->Bind(wxEVT_BUTTON,
                  &ArduinoEditorSettingsDialog::OnBrowseSketchesDir,
                  this);

  cliSizer->Add(sketchRow, 0, wxALL | wxEXPAND, 8);

  // ---- Arduino CLI executable path ----
  wxString cliPath;
  if (!m_config->Read(wxT("ArduinoCliPath"), &cliPath)) {
#ifdef __WXMSW__
    cliPath = wxT("arduino-cli.exe");
#else
    cliPath = wxT("arduino-cli");
#endif
  }

  auto *cliPathRow = new wxBoxSizer(wxHORIZONTAL);

  cliPathRow->Add(new wxStaticText(cliPage, wxID_ANY, _("arduino-cli executable:")),
                  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

  m_cliPathCtrl = new wxTextCtrl(cliPage, wxID_ANY, cliPath);
  m_cliPathCtrl->SetHint(wxString::FromUTF8(m_cli->GetCliPath()));
  cliPathRow->Add(m_cliPathCtrl, 1, wxRIGHT | wxEXPAND, 5);

  m_cliPathBrowse = new wxButton(cliPage, wxID_ANY, _("Browse..."));
  cliPathRow->Add(m_cliPathBrowse, 0);

  m_cliPathBrowse->Bind(wxEVT_BUTTON,
                        [this](wxCommandEvent &) {
                          wxString start = TrimCopy(m_cliPathCtrl->GetValue());
                          if (start.empty())
                            start = wxEmptyString;

#ifdef __WXMSW__
                          wxString wildcard = _("arduino-cli.exe|arduino-cli.exe|Executable files (*.exe)|*.exe|All files (*.*)|*.*");
#else
                          wxString wildcard = _("arduino-cli|arduino-cli|All files (*)|*");
#endif

                          wxFileDialog dlg(
                              this,
                              _("Select arduino-cli executable"),
                              wxEmptyString,
                              start,
                              wildcard,
                              wxFD_OPEN | wxFD_FILE_MUST_EXIST);

                          if (dlg.ShowModal() == wxID_OK) {
                            m_cliPathCtrl->SetValue(dlg.GetPath());
                          }
                        });

  cliSizer->Add(cliPathRow, 0, wxALL | wxEXPAND, 8);

  // Additional URLs
  cliSizer->Add(new wxStaticText(cliPage, wxID_ANY,
                                 _("Additional Boards Manager URLs (one per line):")),
                0, wxALL, 8);

  m_cliUrls = new wxTextCtrl(cliPage, wxID_ANY, wxEmptyString,
                             wxDefaultPosition, wxDefaultSize,
                             wxTE_MULTILINE | wxBORDER_SIMPLE);

  wxString urlsText;
  for (const auto &u : m_cliCfg.boardManagerAdditionalUrls) {
    if (!urlsText.empty())
      urlsText += wxT("\n");
    urlsText += wxString::FromUTF8(u.c_str());
  }
  m_cliUrls->SetValue(urlsText);
  m_cliUrls->SetMinSize(wxSize(-1, 120));

  cliSizer->Add(m_cliUrls, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

  // Unsafe installation
  m_cliUnsafe = new wxCheckBox(cliPage, wxID_ANY, _("Allow unsafe install"));
  m_cliUnsafe->SetValue(m_cliCfg.boardManagerEnableUnsafeInstall);
  cliSizer->Add(m_cliUnsafe, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

  // Proxy
  cliSizer->Add(new wxStaticText(cliPage, wxID_ANY, _("Network proxy:")),
                0, wxLEFT | wxRIGHT | wxTOP, 8);

  m_cliProxy = new wxTextCtrl(cliPage, wxID_ANY,
                              wxString::FromUTF8(m_cliCfg.networkProxy.c_str()));
  m_cliProxy->SetHint(_("user:pass@host:port OR host:port"));
  cliSizer->Add(m_cliProxy, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

  // Timeout
  cliSizer->Add(new wxStaticText(cliPage, wxID_ANY,
                                 _("Network connection timeout (seconds, 0 = no limit):")),
                0, wxLEFT | wxRIGHT | wxTOP, 8);

  m_cliTimeout = new wxSpinCtrl(cliPage, wxID_ANY);
  m_cliTimeout->SetRange(0, 3600); // 0-3600s, adjust the range if needed
  long sec = ParseDurationToSeconds(m_cliCfg.networkConnectionTimeout);
  if (sec < 0)
    sec = 60;
  if (sec > 3600)
    sec = 3600;
  m_cliTimeout->SetValue((int)sec);
  cliSizer->Add(m_cliTimeout, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

  cliPage->SetSizer(cliSizer);
  m_notebook->AddPage(cliPage, _("CLI"), false);

  // ==== EDITOR PAGE ====
  wxPanel *editorPage = new wxPanel(m_notebook, wxID_ANY);
  auto *editorPageSizer = new wxBoxSizer(wxVERTICAL);

  // --- prepare the font for the picker ---
  wxFont initialFont;

  if (!m_settings.fontDesc.empty()) {
    initialFont.SetNativeFontInfo(m_settings.fontDesc);
  }
  if (!initialFont.IsOk()) {
    // fallback - we take the dialog font or the monospaced default
    initialFont = GetFont();
    if (!initialFont.IsOk()) {
      initialFont = wxFontInfo(11).Family(wxFONTFAMILY_MODERN);
    }
  }

  m_fontPicker = new wxFontPickerCtrl(editorPage, wxID_ANY, initialFont,
                                      wxDefaultPosition, wxDefaultSize,
                                      wxFNTP_FONTDESC_AS_LABEL);

  // --- Font box ---
  auto *fontBox = new wxStaticBoxSizer(wxVERTICAL, editorPage, _("Font"));
  auto *fontSizer = new wxBoxSizer(wxHORIZONTAL);
  fontSizer->Add(new wxStaticText(editorPage, wxID_ANY, _("Editor font:")),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
  fontSizer->Add(m_fontPicker, 1, wxEXPAND);
  fontBox->Add(fontSizer, 1, wxALL | wxEXPAND, 5);

  // --- Syntax colors ---
  auto *colorsBox = new wxStaticBoxSizer(wxVERTICAL, editorPage, _("Colors"));

  // Scrollovac panel pro barvy
  m_colorsPanel = new wxScrolledWindow(
      editorPage, wxID_ANY,
      wxDefaultPosition, wxDefaultSize,
      wxVSCROLL);
  m_colorsPanel->SetScrollRate(0, 10);

  // Sizer uvnit scrolled panelu
  auto *colorsPanelSizer = new wxBoxSizer(wxVERTICAL);

  // --- grid pro barvy: label / light / dark ---
  auto *grid = new wxFlexGridSizer(3, 5, 5);
  grid->AddGrowableCol(1, 1);
  grid->AddGrowableCol(2, 1);

  // Hlavika sloupc
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, wxEmptyString));
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Light theme")), 0, wxALIGN_CENTER);
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Dark theme")), 0, wxALIGN_CENTER);

  // Default text
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Text:")), 0, wxALIGN_CENTER_VERTICAL);
  m_text = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].text);
  grid->Add(m_text, 1, wxEXPAND);
  m_textDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].text);
  grid->Add(m_textDark, 1, wxEXPAND);

  // Background
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Background:")), 0, wxALIGN_CENTER_VERTICAL);
  m_background = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].background);
  grid->Add(m_background, 1, wxEXPAND);
  m_backgroundDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].background);
  grid->Add(m_backgroundDark, 1, wxEXPAND);

  // Installed
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Installed list color:")), 0, wxALIGN_CENTER_VERTICAL);
  m_installed = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].installed);
  grid->Add(m_installed, 1, wxEXPAND);
  m_installedDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].installed);
  grid->Add(m_installedDark, 1, wxEXPAND);

  // Updatable
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Updatable list color:")), 0, wxALIGN_CENTER_VERTICAL);
  m_updatable = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].updatable);
  grid->Add(m_updatable, 1, wxEXPAND);
  m_updatableDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].updatable);
  grid->Add(m_updatableDark, 1, wxEXPAND);

  // Deprecated
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Deprecated list color:")), 0, wxALIGN_CENTER_VERTICAL);
  m_deprecated = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].deprecated);
  grid->Add(m_deprecated, 1, wxEXPAND);
  m_deprecatedDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].deprecated);
  grid->Add(m_deprecatedDark, 1, wxEXPAND);

  // Line numbers (text)
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Line numbers (text):")), 0, wxALIGN_CENTER_VERTICAL);
  m_lineNumText = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].lineNumberText);
  grid->Add(m_lineNumText, 1, wxEXPAND);
  m_lineNumTextDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].lineNumberText);
  grid->Add(m_lineNumTextDark, 1, wxEXPAND);

  // Line numbers (background)
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Line numbers (background):")), 0, wxALIGN_CENTER_VERTICAL);
  m_lineNumBg = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].lineNumberBackground);
  grid->Add(m_lineNumBg, 1, wxEXPAND);
  m_lineNumBgDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].lineNumberBackground);
  grid->Add(m_lineNumBgDark, 1, wxEXPAND);

  // Selection
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Selection:")), 0, wxALIGN_CENTER_VERTICAL);
  m_selection = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].selection);
  grid->Add(m_selection, 1, wxEXPAND);
  m_selectionDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].selection);
  grid->Add(m_selectionDark, 1, wxEXPAND);

  // Edge
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Right edge:")), 0, wxALIGN_CENTER_VERTICAL);
  m_edge = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].edge);
  grid->Add(m_edge, 1, wxEXPAND);
  m_edgeDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].edge);
  grid->Add(m_edgeDark, 1, wxEXPAND);

  // Whitespace
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Whitespace:")), 0, wxALIGN_CENTER_VERTICAL);
  m_whitespace = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].whitespace);
  grid->Add(m_whitespace, 1, wxEXPAND);
  m_whitespaceDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].whitespace);
  grid->Add(m_whitespaceDark, 1, wxEXPAND);

  // Brace match
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Brace match:")), 0, wxALIGN_CENTER_VERTICAL);
  m_braceMatch = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].braceMatch);
  grid->Add(m_braceMatch, 1, wxEXPAND);
  m_braceMatchDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].braceMatch);
  grid->Add(m_braceMatchDark, 1, wxEXPAND);

  // Brace mismatch
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Brace mismatch:")), 0, wxALIGN_CENTER_VERTICAL);
  m_braceBad = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].braceBad);
  grid->Add(m_braceBad, 1, wxEXPAND);
  m_braceBadDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].braceBad);
  grid->Add(m_braceBadDark, 1, wxEXPAND);

  // Comments
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Comments:")), 0, wxALIGN_CENTER_VERTICAL);
  m_comment = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].comment);
  grid->Add(m_comment, 1, wxEXPAND);
  m_commentDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].comment);
  grid->Add(m_commentDark, 1, wxEXPAND);

  // Strings
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Strings:")), 0, wxALIGN_CENTER_VERTICAL);
  m_string = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].string);
  grid->Add(m_string, 1, wxEXPAND);
  m_stringDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].string);
  grid->Add(m_stringDark, 1, wxEXPAND);

  // Numbers
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Numbers:")), 0, wxALIGN_CENTER_VERTICAL);
  m_number = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].number);
  grid->Add(m_number, 1, wxEXPAND);
  m_numberDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].number);
  grid->Add(m_numberDark, 1, wxEXPAND);

  // Keywords #1
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Keywords #1:")), 0, wxALIGN_CENTER_VERTICAL);
  m_kw1 = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].keyword1);
  grid->Add(m_kw1, 1, wxEXPAND);
  m_kw1Dark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].keyword1);
  grid->Add(m_kw1Dark, 1, wxEXPAND);

  // Keywords #2
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Keywords #2:")), 0, wxALIGN_CENTER_VERTICAL);
  m_kw2 = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].keyword2);
  grid->Add(m_kw2, 1, wxEXPAND);
  m_kw2Dark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].keyword2);
  grid->Add(m_kw2Dark, 1, wxEXPAND);

  // Preprocessor
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Preprocessor:")), 0, wxALIGN_CENTER_VERTICAL);
  m_preprocessor = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].preprocessor);
  grid->Add(m_preprocessor, 1, wxEXPAND);
  m_preprocessorDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].preprocessor);
  grid->Add(m_preprocessorDark, 1, wxEXPAND);

  // Current line background
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Current line background:")), 0, wxALIGN_CENTER_VERTICAL);
  m_caretLine = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].caretLine);
  grid->Add(m_caretLine, 1, wxEXPAND);
  m_caretLineDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].caretLine);
  grid->Add(m_caretLineDark, 1, wxEXPAND);

  // Calltip text
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Calltip text:")), 0, wxALIGN_CENTER_VERTICAL);
  m_calltipText = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].calltipText);
  grid->Add(m_calltipText, 1, wxEXPAND);
  m_calltipTextDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].calltipText);
  grid->Add(m_calltipTextDark, 1, wxEXPAND);

  // Current line background
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Calltip background:")), 0, wxALIGN_CENTER_VERTICAL);
  m_calltipBackground = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].calltipBackground);
  grid->Add(m_calltipBackground, 1, wxEXPAND);
  m_calltipBackgroundDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].calltipBackground);
  grid->Add(m_calltipBackgroundDark, 1, wxEXPAND);

  // Error
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Error:")), 0, wxALIGN_CENTER_VERTICAL);
  m_error = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].error);
  grid->Add(m_error, 1, wxEXPAND);
  m_errorDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].error);
  grid->Add(m_errorDark, 1, wxEXPAND);

  // Warning
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Warning:")), 0, wxALIGN_CENTER_VERTICAL);
  m_warning = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].warning);
  grid->Add(m_warning, 1, wxEXPAND);
  m_warningDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].warning);
  grid->Add(m_warningDark, 1, wxEXPAND);

  // Note
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Note:")), 0, wxALIGN_CENTER_VERTICAL);
  m_note = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].note);
  grid->Add(m_note, 1, wxEXPAND);
  m_noteDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].note);
  grid->Add(m_noteDark, 1, wxEXPAND);

  // Symbol highlight
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("Symbol highlight:")), 0, wxALIGN_CENTER_VERTICAL);
  m_symbolHighlight = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].symbolHighlight);
  grid->Add(m_symbolHighlight, 1, wxEXPAND);
  m_symbolHighlightDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].symbolHighlight);
  grid->Add(m_symbolHighlightDark, 1, wxEXPAND);

  // AI chat - user messages background
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("AI chat - user messages:")), 0, wxALIGN_CENTER_VERTICAL);
  m_aiUserBg = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].aiUserBg);
  grid->Add(m_aiUserBg, 1, wxEXPAND);
  m_aiUserBgDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].aiUserBg);
  grid->Add(m_aiUserBgDark, 1, wxEXPAND);

  // AI chat - assistant messages background
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("AI chat - assistant messages:")), 0, wxALIGN_CENTER_VERTICAL);
  m_aiAssistantBg = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].aiAssistantBg);
  grid->Add(m_aiAssistantBg, 1, wxEXPAND);
  m_aiAssistantBgDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].aiAssistantBg);
  grid->Add(m_aiAssistantBgDark, 1, wxEXPAND);

  // AI chat - system messages background
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("AI chat - system messages:")), 0, wxALIGN_CENTER_VERTICAL);
  m_aiSystemBg = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].aiSystemBg);
  grid->Add(m_aiSystemBg, 1, wxEXPAND);
  m_aiSystemBgDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].aiSystemBg);
  grid->Add(m_aiSystemBgDark, 1, wxEXPAND);

  // AI chat - info messages background
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("AI chat - info messages:")), 0, wxALIGN_CENTER_VERTICAL);
  m_aiInfoBg = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].aiInfoBg);
  grid->Add(m_aiInfoBg, 1, wxEXPAND);
  m_aiInfoBgDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].aiInfoBg);
  grid->Add(m_aiInfoBgDark, 1, wxEXPAND);

  // AI chat - error messages background
  grid->Add(new wxStaticText(m_colorsPanel, wxID_ANY, _("AI chat - error messages:")), 0, wxALIGN_CENTER_VERTICAL);
  m_aiErrorBg = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[0].aiErrorBg);
  grid->Add(m_aiErrorBg, 1, wxEXPAND);
  m_aiErrorBgDark = new wxColourPickerCtrl(m_colorsPanel, wxID_ANY, m_settings.colors[1].aiErrorBg);
  grid->Add(m_aiErrorBgDark, 1, wxEXPAND);

  colorsPanelSizer->Add(grid, 0, wxALL | wxEXPAND, 5);

  m_colorsPanel->SetSizer(colorsPanelSizer);
  m_colorsPanel->SetMinSize(wxSize(-1, 260));

  Bind(wxEVT_INIT_DIALOG, [this](wxInitDialogEvent &e) {
    e.Skip();

    if (m_colorsPanel) {
      m_colorsPanel->FitInside();
    }

    Layout();
  });

  colorsBox->Add(m_colorsPanel, 1, wxALL | wxEXPAND, 5);

  // --- Editor behavior ---
  auto *editorBox = new wxStaticBoxSizer(wxVERTICAL, editorPage, _("Editor behavior"));

  auto *grid2 = new wxFlexGridSizer(2, 5, 5);
  grid2->AddGrowableCol(1, 1);

  // Tab width
  grid2->Add(new wxStaticText(editorPage, wxID_ANY, _("Tab width:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_tabWidth = new wxSpinCtrl(editorPage, wxID_ANY);
  m_tabWidth->SetRange(1, 16);
  m_tabWidth->SetValue(m_settings.tabWidth);
  grid2->Add(m_tabWidth, 1, wxEXPAND);

  // Use tabs
  grid2->Add(new wxStaticText(editorPage, wxID_ANY, _("Use tabs (instead of spaces):")),
             0, wxALIGN_CENTER_VERTICAL);
  m_useTabs = new wxCheckBox(editorPage, wxID_ANY, wxEmptyString);
  m_useTabs->SetValue(m_settings.useTabs);
  grid2->Add(m_useTabs, 1, wxEXPAND);

  // Show whitespace
  grid2->Add(new wxStaticText(editorPage, wxID_ANY, _("Show whitespace:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_showWhitespace = new wxCheckBox(editorPage, wxID_ANY, wxEmptyString);
  m_showWhitespace->SetValue(m_settings.showWhitespace);
  grid2->Add(m_showWhitespace, 1, wxEXPAND);

  // Auto indent
  grid2->Add(new wxStaticText(editorPage, wxID_ANY, _("Auto indent:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_autoIndent = new wxCheckBox(editorPage, wxID_ANY, wxEmptyString);
  m_autoIndent->SetValue(m_settings.autoIndent);
  grid2->Add(m_autoIndent, 1, wxEXPAND);

  // Clang format - advanced setup
  grid2->Add(new wxStaticText(editorPage, wxID_ANY, _("Clang format:")),
             0, wxALIGN_CENTER_VERTICAL);

  auto *clangFmtSetup = new wxButton(editorPage, wxID_ANY, _("Setup..."));
  clangFmtSetup->SetToolTip(_("Configure advanced clang-format overrides (used when there is no .clang-format in the sketch)."));
  grid2->Add(clangFmtSetup, 0, wxALIGN_CENTER_VERTICAL);

  clangFmtSetup->Bind(wxEVT_BUTTON, &ArduinoEditorSettingsDialog::OnSetupClangFormatting, this);

  editorBox->Add(grid2, 1, wxALL | wxEXPAND, 5);

  // --- View & layout ---
  auto *viewBox = new wxStaticBoxSizer(wxVERTICAL, editorPage, _("View && layout"));

  auto *grid3 = new wxFlexGridSizer(2, 5, 5);
  grid3->AddGrowableCol(1, 1);

  // Line numbers
  grid3->Add(new wxStaticText(editorPage, wxID_ANY, _("Show line numbers:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_showLineNumbers = new wxCheckBox(editorPage, wxID_ANY, wxEmptyString);
  m_showLineNumbers->SetValue(m_settings.showLineNumbers);
  grid3->Add(m_showLineNumbers, 1, wxEXPAND);

  // Word wrap
  grid3->Add(new wxStaticText(editorPage, wxID_ANY, _("Word wrap:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_wordWrap = new wxCheckBox(editorPage, wxID_ANY, wxEmptyString);
  m_wordWrap->SetValue(m_settings.wordWrap);
  grid3->Add(m_wordWrap, 1, wxEXPAND);

  // Right edge
  grid3->Add(new wxStaticText(editorPage, wxID_ANY, _("Show right edge:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_showRightEdge = new wxCheckBox(editorPage, wxID_ANY, wxEmptyString);
  m_showRightEdge->SetValue(m_settings.showRightEdge);
  grid3->Add(m_showRightEdge, 1, wxEXPAND);

  // Edge column
  grid3->Add(new wxStaticText(editorPage, wxID_ANY, _("Edge column:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_edgeColumn = new wxSpinCtrl(editorPage, wxID_ANY);
  m_edgeColumn->SetRange(40, 240);
  m_edgeColumn->SetValue(m_settings.edgeColumn);
  grid3->Add(m_edgeColumn, 1, wxEXPAND);

  // Highlight current line
  grid3->Add(new wxStaticText(editorPage, wxID_ANY, _("Highlight current line:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_highlightCurrentLine = new wxCheckBox(editorPage, wxID_ANY, wxEmptyString);
  m_highlightCurrentLine->SetValue(m_settings.highlightCurrentLine);
  grid3->Add(m_highlightCurrentLine, 1, wxEXPAND);

  // Highlight symbols
  grid3->Add(new wxStaticText(editorPage, wxID_ANY, _("Highlight symbols:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_highlightSymbols = new wxCheckBox(editorPage, wxID_ANY, wxEmptyString);
  m_highlightSymbols->SetValue(m_settings.highlightSymbols);
  grid3->Add(m_highlightSymbols, 1, wxEXPAND);

  // Highlight braces
  grid3->Add(new wxStaticText(editorPage, wxID_ANY, _("Highlight matching braces:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_highlightMatchingBraces = new wxCheckBox(editorPage, wxID_ANY, wxEmptyString);
  m_highlightMatchingBraces->SetValue(m_settings.highlightMatchingBraces);
  grid3->Add(m_highlightMatchingBraces, 1, wxEXPAND);

  // Display hover info
  grid3->Add(new wxStaticText(editorPage, wxID_ANY, _("Display hover info:")),
             0, wxALIGN_CENTER_VERTICAL);
  m_displayHoverInfo = new wxCheckBox(editorPage, wxID_ANY, wxEmptyString);
  m_displayHoverInfo->SetValue(m_settings.displayHoverInfo);
  grid3->Add(m_displayHoverInfo, 1, wxEXPAND);

  viewBox->Add(grid3, 1, wxALL | wxEXPAND, 5);

  // --- two-column layout (left / right) ---
  auto *columns = new wxBoxSizer(wxHORIZONTAL);
  auto *leftCol = new wxBoxSizer(wxVERTICAL);
  auto *rightCol = new wxBoxSizer(wxVERTICAL);

  leftCol->Add(fontBox, 0, wxALL | wxEXPAND, 0);
  leftCol->Add(colorsBox, 1, wxTOP | wxEXPAND, 10);

  rightCol->Add(editorBox, 0, wxALL | wxEXPAND, 0);
  rightCol->Add(viewBox, 0, wxTOP | wxEXPAND, 10);

  columns->Add(leftCol, 1, wxEXPAND | wxRIGHT, 10);
  columns->Add(rightCol, 1, wxEXPAND | wxLEFT, 10);

  editorPageSizer->Add(columns, 1, wxEXPAND | wxALL, 10);
  editorPage->SetSizer(editorPageSizer);

  m_notebook->AddPage(editorPage, _("Editor"), false);

  // ==== CLANG/LLVM PAGE ====
  wxPanel *clangPage = new wxPanel(m_notebook, wxID_ANY);
  auto *clangPageSizer = new wxBoxSizer(wxVERTICAL);

  // --- Clang/LLVM box ---
  auto *clangBox = new wxStaticBoxSizer(wxVERTICAL, clangPage, _("Clang/LLVM"));

  auto *clangGrid = new wxFlexGridSizer(2, 5, 5);
  clangGrid->AddGrowableCol(1, 1);

  // --- Diagnostic mode choice ---
  clangGrid->Add(new wxStaticText(
                     clangPage,
                     wxID_ANY,
                     _("Diagnostics mode:\n"
                       "- Off - diagnostics will not be generated.\n"
                       "- Translation unit - only the source files reachable from the .ino file.\n"
                       "- Whole project - all source files belonging to the sketch.")),
                 0, wxALIGN_CENTER_VERTICAL);

  wxArrayString dModeChoices;
  dModeChoices.Add(_("Off"));
  dModeChoices.Add(_("Translation unit"));
  dModeChoices.Add(_("Whole project"));

  m_clangDiagChoice = new wxChoice(clangPage, wxID_ANY,
                                   wxDefaultPosition, wxDefaultSize,
                                   dModeChoices);

  int modeIndex = static_cast<int>(m_clangSettings.diagnosticMode);
  if (modeIndex < 0 || modeIndex >= (int)dModeChoices.GetCount()) {
    modeIndex = 1; // translationUnit
  }
  m_clangDiagChoice->SetSelection(modeIndex);

  clangGrid->Add(m_clangDiagChoice, 1, wxEXPAND);

  clangGrid->AddSpacer(5);

  m_resolveAfterSave = new wxCheckBox(clangPage, wxID_ANY, _("Resolve after save"));
  m_resolveAfterSave->SetValue(m_clangSettings.resolveDiagOnlyAfterSave);
  m_resolveAfterSave->SetToolTip(_("Diagnostics are resolved only after saving the file."));

  m_displayDiagOnlyFromSketch = new wxCheckBox(clangPage, wxID_ANY, _("Show diagnostics only for sketch files"));
  m_displayDiagOnlyFromSketch->SetValue(m_clangSettings.displayDiagnosticsOnlyFromSketch);
  m_displayDiagOnlyFromSketch->SetToolTip(_(
      "When enabled, diagnostics are shown only for files in the current sketch. "
      "When disabled, all diagnostics reported by Clang are shown, including those from libraries and the toolchain."));

  auto *diagFlagsRow = new wxBoxSizer(wxHORIZONTAL);

  diagFlagsRow->Add(m_resolveAfterSave, 0, wxRIGHT, 10);
  diagFlagsRow->Add(m_displayDiagOnlyFromSketch, 1, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);

  clangGrid->Add(diagFlagsRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

  // --- Completion mode choice ---
  clangGrid->Add(new wxStaticText(
                     clangPage,
                     wxID_ANY,
                     _("Completion mode:\n"
                       "- Off - code completion is disabled.\n"
                       "- Only on request - completion is triggered only with Ctrl+Space.\n"
                       "- While writing - dynamic code completion while typing.")),
                 0, wxALIGN_CENTER_VERTICAL);

  wxArrayString cModeChoices;
  cModeChoices.Add(_("Off"));
  cModeChoices.Add(_("Only on request"));
  cModeChoices.Add(_("While writing"));

  m_clangCompChoice = new wxChoice(clangPage, wxID_ANY,
                                   wxDefaultPosition, wxDefaultSize,
                                   cModeChoices);

  modeIndex = static_cast<int>(m_clangSettings.completionMode);
  if (modeIndex < 0 || modeIndex >= (int)cModeChoices.GetCount()) {
    modeIndex = 2; // always
  }
  m_clangCompChoice->SetSelection(modeIndex);

  clangGrid->Add(m_clangCompChoice, 1, wxEXPAND);

  // --- Resolve mode info label ---
  clangGrid->AddSpacer(10);
  clangGrid->AddSpacer(10);

  auto *resolveInfo = new wxStaticText(
      clangPage,
      wxID_ANY,
      _("Library && include resolution mode:\n"
        "- Internal resolver - fast, but may miss some include paths.\n"
        "- Compile-commands resolver - slower, but the most accurate."));
  clangGrid->Add(resolveInfo, 0, wxALIGN_LEFT | wxTOP);

  // --- Resolve mode choice ---
  wxArrayString rModeChoices;
  rModeChoices.Add(_("Internal resolver"));
  rModeChoices.Add(_("Compile-commands resolver"));

  m_clangResolveChoice = new wxChoice(
      clangPage,
      wxID_ANY,
      wxDefaultPosition,
      wxDefaultSize,
      rModeChoices);

  int rIndex = static_cast<int>(m_clangSettings.resolveMode);
  if (rIndex < 0 || rIndex >= (int)rModeChoices.GetCount()) {
    rIndex = 1; // internalResolver default
  }
  m_clangResolveChoice->SetSelection(rIndex);

  clangGrid->Add(m_clangResolveChoice, 1, wxEXPAND, 10);

  // --- Warning mode choice ---
  clangGrid->AddSpacer(10);
  clangGrid->AddSpacer(10);

  clangGrid->Add(new wxStaticText(
                     clangPage,
                     wxID_ANY,
                     _("Warnings mode:\n"
                       "- Off - disables compiler warnings (equivalent to -w).\n"
                       "- Default - uses Clang's default warning set.\n"
                       "- Arduino-like - enables a practical set of warnings for typical Arduino sketches.\n"
                       "- Strict - enables a very strict warning set (-Wall, -Wextra, -Wpedantic, etc.).")),
                 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT);

  wxArrayString wModeChoices;
  wModeChoices.Add(_("Off"));
  wModeChoices.Add(_("Default"));
  wModeChoices.Add(_("Arduino-like"));
  wModeChoices.Add(_("Strict"));
  wModeChoices.Add(_("Custom"));

  m_clangWarnChoice = new wxChoice(
      clangPage,
      wxID_ANY,
      wxDefaultPosition,
      wxDefaultSize,
      wModeChoices);

  int wIndex = static_cast<int>(m_clangSettings.warningMode);
  if (wIndex < 0 || wIndex >= (int)wModeChoices.GetCount()) {
    wIndex = 1; // warningDefault
  }
  m_clangWarnChoice->SetSelection(wIndex);

  m_btnEditCustomWarnings = new wxBitmapButton(
      clangPage,
      wxID_ANY,
      AEGetArtBundle(wxAEArt::Edit).GetBitmapFor(clangPage));

  auto *warnRow = new wxBoxSizer(wxHORIZONTAL);

  warnRow->Add(m_clangWarnChoice, 1, wxEXPAND | wxRIGHT, 6);
  warnRow->Add(m_btnEditCustomWarnings, 0, wxALIGN_CENTER_VERTICAL);

  m_btnEditCustomWarnings->Bind(wxEVT_BUTTON, &ArduinoEditorSettingsDialog::OnEditCustomWarnings, this);

  clangGrid->Add(warnRow, 1, wxEXPAND);
  clangBox->Add(clangGrid, 1, wxALL | wxEXPAND, 5);
  clangPageSizer->Add(clangBox, 0, wxALL | wxEXPAND, 10);

  // --- Behavior box ---
  auto *behaviorBox = new wxStaticBoxSizer(wxVERTICAL, clangPage, _("Behavior"));

  auto *behaviorGrid = new wxFlexGridSizer(2, 5, 5);
  behaviorGrid->AddGrowableCol(1, 1);

  // --- Autocompletion delay ---
  behaviorGrid->Add(new wxStaticText(clangPage, wxID_ANY, _("Autocompletion delay (ms):")),
                    0, wxALIGN_CENTER_VERTICAL);

  m_clangAutoDelay = new wxSpinCtrl(clangPage, wxID_ANY);
  m_clangAutoDelay->SetRange(250, 999999);
  m_clangAutoDelay->SetValue((int)m_clangSettings.autocompletionDelay);
  behaviorGrid->Add(m_clangAutoDelay, 1, wxEXPAND);

  // --- Resolve diagnostics delay ---
  behaviorGrid->Add(new wxStaticText(clangPage, wxID_ANY, _("Resolve diagnostics delay (ms):")),
                    0, wxALIGN_CENTER_VERTICAL);

  m_clangDiagDelay = new wxSpinCtrl(clangPage, wxID_ANY);
  m_clangDiagDelay->SetRange(1000, 999999);
  m_clangDiagDelay->SetValue((int)m_clangSettings.resolveDiagnosticsDelay);
  behaviorGrid->Add(m_clangDiagDelay, 1, wxEXPAND);

  behaviorBox->Add(behaviorGrid, 1, wxALL | wxEXPAND, 5);
  clangPageSizer->Add(behaviorBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  clangPage->SetSizer(clangPageSizer);

  m_notebook->AddPage(clangPage, _("Clang/LLVM"), false);

  // ==== AI PAGE ====
  wxPanel *aiPage = new wxPanel(m_notebook, wxID_ANY);
  auto *aiSizer = new wxBoxSizer(wxVERTICAL);

  // --- AI master switch & info ---
  auto *aiMainBox = new wxStaticBoxSizer(wxVERTICAL, aiPage, _("AI assistant"));

  m_aiEnable = new wxCheckBox(
      aiPage,
      wxID_ANY,
      _("Enable AI-powered features (code fixes, explanations, ...)"));
  m_aiEnable->SetValue(m_aiSettings.enabled);
  aiMainBox->Add(m_aiEnable, 0, wxALL | wxEXPAND, 5);

  m_aiEnable->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &evt) {
    UpdateAiControlsEnabled();
    evt.Skip();
  });

  auto *aiInfo = new wxStaticText(
      aiPage,
      wxID_ANY,
      _("AI integration uses an HTTP(S) API endpoint.\n"
        "You can connect either to a cloud provider (e.g. OpenAI-compatible API)\n"
        "or to a local server exposing a compatible endpoint."));
  aiMainBox->Add(aiInfo, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

  aiSizer->Add(aiMainBox, 0, wxALL | wxEXPAND, 10);

  // --- Model selection ---
  auto *modelBox = new wxStaticBoxSizer(wxVERTICAL, aiPage, _("Model"));

  auto *modelRow = new wxBoxSizer(wxHORIZONTAL);

  modelRow->Add(new wxStaticText(aiPage, wxID_ANY, _("Active model:")),
                0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

  m_aiModelChoice = new wxChoice(aiPage, wxID_ANY);
  modelRow->Add(m_aiModelChoice, 1, wxRIGHT | wxEXPAND, 5);

  m_aiModelEditBtn = new wxBitmapButton(aiPage, wxID_ANY, AEGetArtBundle(wxAEArt::Edit));
  modelRow->Add(m_aiModelEditBtn, 0, wxRIGHT, 5);

  m_aiModelAddBtn = new wxBitmapButton(aiPage, wxID_ANY, AEGetArtBundle(wxAEArt::Plus));
  modelRow->Add(m_aiModelAddBtn, 0);

  modelBox->Add(modelRow, 0, wxALL | wxEXPAND, 5);

  auto MakeModelLabel = [](const AiModelSettings &m) -> wxString {
    wxString n = TrimCopy(m.name);
    if (n.empty())
      n = _("(unnamed)");
    wxString mm = TrimCopy(m.model);
    if (!mm.empty())
      return wxString::Format(wxT("%s  -  %s"), n, mm);
    return n;
  };

  auto RebuildModelChoice = [this, MakeModelLabel]() {
    if (!m_aiModelChoice)
      return;

    wxArrayString items;
    for (const auto &m : m_aiModels)
      items.Add(MakeModelLabel(m));

    m_aiModelChoice->Clear();
    if (!items.empty())
      m_aiModelChoice->Append(items);

    for (size_t i = 0; i < m_aiModels.size(); ++i) {
      if (m_aiModels[i].id == m_aiSettings.id) {
        m_aiModelChoice->SetSelection((int)i);
        break;
      }
    }
  };

  auto ApplySelectedModelToAiSettings = [this]() {
    if (!m_aiModelChoice)
      return;
    int sel = m_aiModelChoice->GetSelection();
    if (sel < 0 || sel >= (int)m_aiModels.size())
      return;

    const auto &m = m_aiModels[(size_t)sel];
    ApplyModelToAiSettings(m, m_aiSettings);
  };

  RebuildModelChoice();
  ApplySelectedModelToAiSettings();

  m_aiModelChoice->Bind(wxEVT_CHOICE, [ApplySelectedModelToAiSettings](wxCommandEvent &e) {
    ApplySelectedModelToAiSettings();
    e.Skip();
  });

  m_aiModelEditBtn->Bind(wxEVT_BUTTON, [this, RebuildModelChoice, ApplySelectedModelToAiSettings](wxCommandEvent &) {
    if (!m_aiModelChoice)
      return;
    int sel = m_aiModelChoice->GetSelection();
    if (sel < 0 || sel >= (int)m_aiModels.size())
      return;

    auto &m = m_aiModels[(size_t)sel];
    ArduinoAiModelDialog dlg(this, m, m_config);
    int rc = dlg.ShowModal();

    if (rc == wxID_OK) {
      RebuildModelChoice();
      m_aiModelChoice->SetSelection(sel);
      ApplySelectedModelToAiSettings();
    } else if (rc == wxID_DELETE) {
      m_aiModels.erase(m_aiModels.begin() + sel);
      RebuildModelChoice();
      ApplySelectedModelToAiSettings();
    }
  });

  m_aiModelAddBtn->Bind(wxEVT_BUTTON, [this, RebuildModelChoice, ApplySelectedModelToAiSettings](wxCommandEvent &) {
    wxMenu menu;
    menu.Append(1, _("Add..."));
    menu.Append(2, _("Import..."));

    int id = GetPopupMenuSelectionFromUser(menu);
    if (id == 1) {
      AiModelSettings m;
      m.id = GenAiModelId();
      m.name = _("New model");
      m.endpointUrl = TrimCopy(m_aiSettings.endpointUrl);
      m.model = TrimCopy(m_aiSettings.model);

      ArduinoAiModelDialog dlg(this, m, m_config);
      int rc = dlg.ShowModal();
      if (rc == wxID_OK) {
        m_aiModels.push_back(m);
        RebuildModelChoice();
        m_aiModelChoice->SetSelection((int)m_aiModels.size() - 1);
        ApplySelectedModelToAiSettings();
      }
    } else if (id == 2) {
      wxFileDialog fd(this, _("Import model from JSON"), wxEmptyString, wxEmptyString,
                      _("JSON files (*.json)|*.json|All files|*.*"),
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
      if (fd.ShowModal() != wxID_OK)
        return;

      wxFileInputStream is(fd.GetPath());
      if (!is.IsOk()) {
        wxMessageBox(_("Failed to read file."), _("Import"), wxOK | wxICON_ERROR, this);
        return;
      }
      wxStringOutputStream os;
      is.Read(os);

      try {
        auto j = nlohmann::json::parse(std::string(os.GetString().utf8_str()));

        AiModelSettings m;
        m.id = GenAiModelId();
        m.name = wxString::FromUTF8(j.value("name", std::string()));
        m.endpointUrl = wxString::FromUTF8(j.value("endpointUrl", std::string()));
        m.model = wxString::FromUTF8(j.value("model", std::string()));
        m.maxIterations = j.value("maxIterations", 5);
        m.requestTimeout = j.value("requestTimeout", 60);
        m.extraRequestJson = wxString::FromUTF8(j.value("extraRequestJson", std::string()));
        m.forceModelQueryRange = j.value("forceModelQueryRange", false);
        m.fullInfoRequest = j.value("fullInfoRequest", true);
        m.floatingWindow = j.value("floatingWindow", true);
        m.hasAuthentization = j.value("hasAuthentization", false);

        ArduinoAiModelDialog dlg(this, m, m_config);
        int rc = dlg.ShowModal();
        if (rc == wxID_OK) {
          m_aiModels.push_back(m);
          RebuildModelChoice();
          m_aiModelChoice->SetSelection((int)m_aiModels.size() - 1);
          ApplySelectedModelToAiSettings();
        }
      } catch (...) {
        wxMessageBox(_("Invalid JSON file."), _("Import"), wxOK | wxICON_ERROR, this);
        return;
      }
    }
  });

  aiSizer->Add(modelBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  // --- Chat history (opt-in) ---
  auto *histBox = new wxStaticBoxSizer(wxVERTICAL, aiPage, _("Chat history"));

  m_aiStoreChatHistory = new wxCheckBox(
      aiPage,
      wxID_ANY,
      _("Store AI chat sessions inside the sketch folder"));
  m_aiStoreChatHistory->SetValue(m_aiSettings.storeChatHistory);

  m_aiStoreChatHistory->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &evt) {
    bool on = m_aiStoreChatHistory->GetValue();
    if (m_summarizeChatSessionsModeChoice)
      m_summarizeChatSessionsModeChoice->Enable(on && (m_aiEnable && m_aiEnable->GetValue()));
    evt.Skip();
  });

  histBox->Add(m_aiStoreChatHistory, 0, wxALL | wxEXPAND, 5);

  auto *histInfo = new wxStaticText(
      aiPage,
      wxID_ANY,
      _("When enabled, the IDE may save the AI conversation transcript (including code snippets)\n"
        "into a hidden subfolder inside the current sketch. This helps you continue where you\n"
        "left off, but may store sensitive data (API keys, passwords, proprietary code, ...).\n"
        "Enable only if you understand the risks."));

  histBox->Add(histInfo, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

  // --- Session title / summarization mode ---
  auto *sumRow = new wxBoxSizer(wxHORIZONTAL);

  sumRow->Add(new wxStaticText(aiPage, wxID_ANY, _("Session title:")),
              0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

  wxArrayString sumChoices;
  sumChoices.Add(_("Use date only (no summarization)"));
  sumChoices.Add(_("Generate from first message (no extra tokens)"));
  sumChoices.Add(_("Generate with AI from first message (uses tokens)"));

  m_summarizeChatSessionsModeChoice = new wxChoice(
      aiPage, wxID_ANY, wxDefaultPosition, wxDefaultSize, sumChoices);

  int sumSel = static_cast<int>(m_aiSettings.summarizeChatSessionMode);
  if (sumSel < 0 || sumSel >= (int)sumChoices.GetCount())
    sumSel = 0;
  m_summarizeChatSessionsModeChoice->SetSelection(sumSel);

  m_summarizeChatSessionsModeChoice->SetToolTip(
      _("Controls how saved chat sessions are labeled in the session list.\n"
        "The title is generated only once, when the session starts."));

  m_summarizeChatSessionsModeChoice->Enable(m_aiStoreChatHistory->GetValue());

  sumRow->Add(m_summarizeChatSessionsModeChoice, 1, wxEXPAND);

  histBox->Add(sumRow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

  aiSizer->Add(histBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  aiPage->SetSizer(aiSizer);
  m_notebook->AddPage(aiPage, _("AI"), false);

  UpdateAiControlsEnabled();

  // --- Notebook into mainSizer ---
  mainSizer->Add(m_notebook, 1, wxEXPAND | wxALL, 10);

  // --- Buttons ---
  mainSizer->Add(CreateLocalizedSeparatedOkCancelSizer(this),
                 0, wxALL | wxEXPAND, 10);

  SetSizer(mainSizer);
  Thaw();
  Layout();

  Bind(wxEVT_CLOSE_WINDOW, &ArduinoEditorSettingsDialog::OnClose, this);
  Bind(wxEVT_SYS_COLOUR_CHANGED, &ArduinoEditorSettingsDialog::OnSysColourChanged, this);

  wxSize best = GetBestSize();
  SetMinSize(best);

  // --- Restore dialog size ---
  if (!LoadWindowSize(wxT("SettingsDlg"), this, m_config)) {
    SetSize(best);
    CentreOnParent();
  }
}

void ArduinoEditorSettingsDialog::UpdateAiControlsEnabled() {
  bool en = (m_aiEnable && m_aiEnable->GetValue());

  if (m_aiModelChoice)
    m_aiModelChoice->Enable(en);
  if (m_aiModelEditBtn)
    m_aiModelEditBtn->Enable(en);
  if (m_aiModelAddBtn)
    m_aiModelAddBtn->Enable(en);

  if (m_aiStoreChatHistory)
    m_aiStoreChatHistory->Enable(en);

  if (m_summarizeChatSessionsModeChoice) {
    m_summarizeChatSessionsModeChoice->Enable(en && m_aiStoreChatHistory && m_aiStoreChatHistory->GetValue());
  }
}

void ArduinoEditorSettingsDialog::OnSysColourChanged(wxSysColourChangedEvent &event) {
  if (m_aiModelAddBtn) {
    m_aiModelAddBtn->SetBitmap(AEGetArtBundle(wxAEArt::Plus));
  }

  if (m_aiModelEditBtn) {
    m_aiModelEditBtn->SetBitmap(AEGetArtBundle(wxAEArt::Edit));
  }

  Layout();

  event.Skip();
}

void ArduinoEditorSettingsDialog::EndModal(int retCode) {
  SaveWindowSize(wxT("SettingsDlg"), this, m_config);
  wxDialog::EndModal(retCode);
}

void ArduinoEditorSettingsDialog::OnClose(wxCloseEvent &WXUNUSED(evt)) {
  // "X" acts as Cancel
  if (IsModal()) {
    EndModal(wxID_CANCEL);
  } else {
    SaveWindowSize(wxT("SettingsDlg"), this, m_config);
    Destroy();
  }
}

void ArduinoEditorSettingsDialog::LoadAiModels(wxConfigBase *cfg, std::vector<AiModelSettings> &out) {
  out.clear();
  if (!cfg)
    return;

  long cnt = 0;
  cfg->Read(AiModelsBaseKey() + wxT("/Count"), &cnt, 0L);

  for (long i = 0; i < cnt; ++i) {
    wxString base = AiModelsBaseKey() + wxString::Format(wxT("/Item%ld"), i);

    AiModelSettings m;
    cfg->Read(base + wxT("/Id"), &m.id, wxEmptyString);
    cfg->Read(base + wxT("/Name"), &m.name, wxEmptyString);
    cfg->Read(base + wxT("/EndpointUrl"), &m.endpointUrl, wxEmptyString);
    cfg->Read(base + wxT("/Model"), &m.model, wxEmptyString);

    cfg->Read(base + wxT("/MaxIterations"), &m.maxIterations, 5L);
    cfg->Read(base + wxT("/RequestTimeout"), &m.requestTimeout, 60L);

    cfg->Read(base + wxT("/ExtraRequestJson"), &m.extraRequestJson, wxEmptyString);

    cfg->Read(base + wxT("/ForceModelQueryRange"), &m.forceModelQueryRange, false);
    cfg->Read(base + wxT("/FullInfoRequest"), &m.fullInfoRequest, true);
    cfg->Read(base + wxT("/FloatingWindow"), &m.floatingWindow, true);
    cfg->Read(base + wxT("/HasAuthentization"), &m.hasAuthentization, false);

    if (m.id.empty())
      m.id = GenAiModelId();

    out.push_back(m);
  }
}

void ArduinoEditorSettingsDialog::ApplyModelToAiSettings(const AiModelSettings &m, AiSettings &settings) {
  settings.id = m.id;
  settings.name = m.name;
  settings.endpointUrl = m.endpointUrl;
  settings.model = m.model;
  settings.maxIterations = m.maxIterations;
  settings.requestTimeout = m.requestTimeout;
  settings.extraRequestJson = m.extraRequestJson;
  settings.forceModelQueryRange = m.forceModelQueryRange;
  settings.fullInfoRequest = m.fullInfoRequest;
  settings.floatingWindow = m.floatingWindow;
  settings.hasAuthentization = m.hasAuthentization;
}

EditorSettings ArduinoEditorSettingsDialog::GetSettings() const {
  EditorSettings s = m_settings;

  // light colors (index 0)
  s.colors[0].text = m_text->GetColour();
  s.colors[0].background = m_background->GetColour();
  s.colors[0].installed = m_installed->GetColour();
  s.colors[0].updatable = m_updatable->GetColour();
  s.colors[0].deprecated = m_deprecated->GetColour();
  s.colors[0].lineNumberText = m_lineNumText->GetColour();
  s.colors[0].lineNumberBackground = m_lineNumBg->GetColour();
  s.colors[0].selection = m_selection->GetColour();
  s.colors[0].edge = m_edge->GetColour();
  s.colors[0].whitespace = m_whitespace->GetColour();
  s.colors[0].braceMatch = m_braceMatch->GetColour();
  s.colors[0].braceBad = m_braceBad->GetColour();
  s.colors[0].comment = m_comment->GetColour();
  s.colors[0].string = m_string->GetColour();
  s.colors[0].number = m_number->GetColour();
  s.colors[0].keyword1 = m_kw1->GetColour();
  s.colors[0].keyword2 = m_kw2->GetColour();
  s.colors[0].preprocessor = m_preprocessor->GetColour();
  s.colors[0].caretLine = m_caretLine->GetColour();
  s.colors[0].calltipText = m_calltipText->GetColour();
  s.colors[0].calltipBackground = m_calltipBackground->GetColour();
  s.colors[0].error = m_error->GetColour();
  s.colors[0].warning = m_warning->GetColour();
  s.colors[0].note = m_note->GetColour();
  s.colors[0].symbolHighlight = m_symbolHighlight->GetColour();
  s.colors[0].aiUserBg = m_aiUserBg->GetColour();
  s.colors[0].aiAssistantBg = m_aiAssistantBg->GetColour();
  s.colors[0].aiSystemBg = m_aiSystemBg->GetColour();
  s.colors[0].aiInfoBg = m_aiInfoBg->GetColour();
  s.colors[0].aiErrorBg = m_aiErrorBg->GetColour();

  // dark colors (index 1)
  s.colors[1].text = m_textDark->GetColour();
  s.colors[1].background = m_backgroundDark->GetColour();
  s.colors[1].installed = m_installedDark->GetColour();
  s.colors[1].updatable = m_updatableDark->GetColour();
  s.colors[1].deprecated = m_deprecatedDark->GetColour();
  s.colors[1].lineNumberText = m_lineNumTextDark->GetColour();
  s.colors[1].lineNumberBackground = m_lineNumBgDark->GetColour();
  s.colors[1].selection = m_selectionDark->GetColour();
  s.colors[1].edge = m_edgeDark->GetColour();
  s.colors[1].whitespace = m_whitespaceDark->GetColour();
  s.colors[1].braceMatch = m_braceMatchDark->GetColour();
  s.colors[1].braceBad = m_braceBadDark->GetColour();
  s.colors[1].comment = m_commentDark->GetColour();
  s.colors[1].string = m_stringDark->GetColour();
  s.colors[1].number = m_numberDark->GetColour();
  s.colors[1].keyword1 = m_kw1Dark->GetColour();
  s.colors[1].keyword2 = m_kw2Dark->GetColour();
  s.colors[1].preprocessor = m_preprocessorDark->GetColour();
  s.colors[1].caretLine = m_caretLineDark->GetColour();
  s.colors[1].calltipText = m_calltipTextDark->GetColour();
  s.colors[1].calltipBackground = m_calltipBackgroundDark->GetColour();
  s.colors[1].error = m_errorDark->GetColour();
  s.colors[1].warning = m_warningDark->GetColour();
  s.colors[1].note = m_noteDark->GetColour();
  s.colors[1].symbolHighlight = m_symbolHighlightDark->GetColour();
  s.colors[1].aiUserBg = m_aiUserBgDark->GetColour();
  s.colors[1].aiAssistantBg = m_aiAssistantBgDark->GetColour();
  s.colors[1].aiSystemBg = m_aiSystemBgDark->GetColour();
  s.colors[1].aiInfoBg = m_aiInfoBgDark->GetColour();
  s.colors[1].aiErrorBg = m_aiErrorBgDark->GetColour();

  // general settings
  s.tabWidth = m_tabWidth->GetValue();
  s.useTabs = m_useTabs->GetValue();
  s.showWhitespace = m_showWhitespace->GetValue();
  s.autoIndent = m_autoIndent->GetValue();

  // behavior
  s.showLineNumbers = m_showLineNumbers->GetValue();
  s.wordWrap = m_wordWrap->GetValue();
  s.showRightEdge = m_showRightEdge->GetValue();
  s.edgeColumn = m_edgeColumn->GetValue();
  s.highlightCurrentLine = m_highlightCurrentLine->GetValue();
  s.highlightSymbols = m_highlightSymbols->GetValue();
  s.highlightMatchingBraces = m_highlightMatchingBraces->GetValue();
  s.displayHoverInfo = m_displayHoverInfo->GetValue();

  if (m_fontPicker) {
    wxFont f = m_fontPicker->GetSelectedFont();
    if (f.IsOk())
      s.fontDesc = f.GetNativeFontInfoDesc();
    else
      s.fontDesc.clear();
  }

  return s;
}

ArduinoCliConfig ArduinoEditorSettingsDialog::GetCliConfig() const {
  ArduinoCliConfig cfg = m_cliCfg; // start with the original values

  // URLs line by line
  cfg.boardManagerAdditionalUrls.clear();
  wxArrayString lines = wxSplit(m_cliUrls->GetValue(), '\n', '\0');
  for (auto &line : lines) {
    line.Trim(true).Trim(false);
    if (!line.IsEmpty()) {
      cfg.boardManagerAdditionalUrls.push_back(std::string(line.utf8_str()));
    }
  }

  // Unsafe install
  cfg.boardManagerEnableUnsafeInstall = m_cliUnsafe->GetValue();

  // Proxy
  wxString proxy = TrimCopy(m_cliProxy->GetValue());
  cfg.networkProxy = std::string(proxy.utf8_str());

  // Timeout
  long sec = (long)m_cliTimeout->GetValue();
  cfg.networkConnectionTimeout = SecondsToDuration(sec);

  return cfg;
}

ClangSettings ArduinoEditorSettingsDialog::GetClangSettings() const {
  ClangSettings s = m_clangSettings;

  if (m_clangDiagChoice) {
    int sel = m_clangDiagChoice->GetSelection();
    if (sel < 0)
      sel = 0;
    s.diagnosticMode = static_cast<ClangDiagnosticMode>(sel);
  }

  if (m_clangCompChoice) {
    int sel = m_clangCompChoice->GetSelection();
    if (sel < 0)
      sel = 0;
    s.completionMode = static_cast<ClangCompletionMode>(sel);
  }

  if (m_clangResolveChoice) {
    int sel = m_clangResolveChoice->GetSelection();
    if (sel < 0)
      sel = 0; // default = internalResolver
    s.resolveMode = static_cast<ClangResolveMode>(sel);
  }

  if (m_clangWarnChoice) {
    int sel = m_clangWarnChoice->GetSelection();
    if (sel < 0)
      sel = 1; // warningDefault
    s.warningMode = static_cast<ClangWarningMode>(sel);
  }

  if (m_clangAutoDelay) {
    int v = m_clangAutoDelay->GetValue();
    if (v < 250)
      v = 250;
    s.autocompletionDelay = (unsigned)v;
  }

  if (m_clangDiagDelay) {
    int v = m_clangDiagDelay->GetValue();
    if (v < 1000)
      v = 1000;
    s.resolveDiagnosticsDelay = (unsigned)v;
  }

  if (m_resolveAfterSave) {
    s.resolveDiagOnlyAfterSave = m_resolveAfterSave->GetValue();
  }

  if (m_displayDiagOnlyFromSketch) {
    s.displayDiagnosticsOnlyFromSketch = m_displayDiagOnlyFromSketch->GetValue();
  }

  if (m_clangExtSourceCmd) {
    wxString cmd = TrimCopy(m_clangExtSourceCmd->GetValue());
    s.extSourceOpenCommand = cmd;
  }

  if (m_openSourceInside) {
    s.openSourceFilesInside = m_openSourceInside->GetValue();
  }

  return s;
}

AiSettings ArduinoEditorSettingsDialog::GetAiSettings() const {
  AiSettings s = m_aiSettings;

  if (m_aiEnable) {
    s.enabled = m_aiEnable->GetValue();
  }

  if (m_aiStoreChatHistory) {
    s.storeChatHistory = m_aiStoreChatHistory->GetValue();
  }

  if (m_summarizeChatSessionsModeChoice) {
    int sel = m_summarizeChatSessionsModeChoice->GetSelection();
    if (sel < 0)
      sel = 0; // default = noSumarization
    s.summarizeChatSessionMode = static_cast<AiSummarizationChatMode>(sel);
  }

  if (m_config) {
    SaveAiModels(m_config, m_aiModels);
  }

  return s;
}

void ArduinoEditorSettingsDialog::OnBrowseSketchesDir(wxCommandEvent &WXUNUSED(evt)) {
  wxString startDir = TrimCopy(m_sketchesDirCtrl->GetValue());

  if (startDir.empty() || !wxDirExists(startDir)) {
    startDir = wxGetHomeDir();
  }

  wxDirDialog dlg(this,
                  _("Select sketches directory"),
                  startDir,
                  wxDD_DIR_MUST_EXIST | wxDD_DEFAULT_STYLE);

  if (dlg.ShowModal() == wxID_OK) {
    if (m_sketchesDirCtrl) {
      m_sketchesDirCtrl->SetValue(dlg.GetPath());
    }
  }
}

void ArduinoEditorSettingsDialog::OnSetupClangFormatting(wxCommandEvent &WXUNUSED(evt)) {
  wxString in = TrimCopy(m_settings.clangFormatOverridesJson);

  if (in.IsEmpty()) {
    in = defaultClangFormat; // JSON starter pack defined at the top of the file
  }

  ArduinoClangFormatSettingsDialog dlg(this, in);
  if (dlg.ShowModal() == wxID_OK) {
    wxString out = TrimCopy(dlg.GetOverridesJson());

    // Dialog returns "{}" when everything is default -> we can save empty (smaller config)
    if (out == wxT("{}")) {
      out.clear();
    }

    m_settings.clangFormatOverridesJson = out;
  }
}

void ArduinoEditorSettingsDialog::OnEditCustomWarnings(wxCommandEvent &WXUNUSED(evt)) {
  if (!m_clangWarnChoice)
    return;

  int sel = m_clangWarnChoice->GetSelection();
  if (sel < 0) {
    sel = (int)ClangWarningMode::warningDefault;
  }

  ClangWarningMode mode = static_cast<ClangWarningMode>(sel);

  wxString initial;
  if (mode == ClangWarningMode::warningCustom) {
    initial = JoinFlagsMultiline(m_clangSettings.customWarningFlags); // vector<string> -> wxString
  } else {
    if (!m_clangSettings.customWarningFlags.empty()) {
      wxRichMessageDialog dlg(
          this,
          _("This will replace your current Custom warning flags with the selected preset.\n"
            "Do you want to continue?"),
          _("Confirmation"),
          wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT);

      if (dlg.ShowModal() != wxID_YES) {
        return;
      }
    }

    initial = WarningFlagsToMultilineText(mode); // preset
  }

  wxTextEntryDialog dlg(
      this,
      _("Enter custom warning flags.\n"
        "Tip: one flag per line is easiest to read (e.g. -Wall, -Wextra, -Wno-shadow, -Wformat=2)."),
      _("Custom warning flags"),
      initial,
      wxOK | wxCANCEL | wxTE_MULTILINE);

  dlg.SetSize(wxSize(400, 640));
  dlg.CentreOnParent();

  if (dlg.ShowModal() != wxID_OK) {
    return;
  }

  m_clangWarnChoice->SetSelection(4); // Custom

  // wxString -> vector<string>
  SplitFlagsWhitespace(dlg.GetValue(), m_clangSettings.customWarningFlags);
}

void ArduinoEditorSettingsDialog::OnBrowseExtSourceCommand(wxCommandEvent &WXUNUSED(evt)) {
  wxString start;
  if (m_clangExtSourceCmd) {
    start = TrimCopy(m_clangExtSourceCmd->GetValue());
  }

#ifdef __WXMSW__
  wxString wildcard = _("Executable files (*.exe)|*.exe|All files (*.*)|*.*");
#else
  wxString wildcard = _("All files (*)|*");
#endif

  wxFileDialog dlg(
      this,
      _("Select external program"),
      wxEmptyString,
      start,
      wildcard,
      wxFD_OPEN | wxFD_FILE_MUST_EXIST);

  if (dlg.ShowModal() == wxID_OK && m_clangExtSourceCmd) {
    m_clangExtSourceCmd->SetValue(dlg.GetPath());
  }
}

wxString ArduinoEditorSettingsDialog::GetSketchesDir() const {
  if (!m_sketchesDirCtrl)
    return wxString();
  wxString s = TrimCopy(m_sketchesDirCtrl->GetValue());
  return s;
}

wxString ArduinoEditorSettingsDialog::GetCliPath() const {
  if (!m_cliPathCtrl)
    return wxString();
  wxString s = TrimCopy(m_cliPathCtrl->GetValue());
  return s;
}

wxString ArduinoEditorSettingsDialog::GetSelectedLanguage() const {
  if (!m_languageChoice || m_langEntries.empty()) {
    return wxT("system");
  }

  int sel = m_languageChoice->GetSelection();
  if (sel < 0 || sel >= static_cast<int>(m_langEntries.size())) {
    return wxT("system");
  }

  return m_langEntries[sel].code;
}

long ArduinoEditorSettingsDialog::GetUpdateCheckIntervalHours() const {
  if (!m_updatesEnable || !m_updatesDays) {
    return 24;
  }
  if (!m_updatesEnable->GetValue()) {
    return 0; // disabled
  }
  int days = m_updatesDays->GetValue();
  if (days < 1)
    days = 1;
  return (long)days * 24L;
}
