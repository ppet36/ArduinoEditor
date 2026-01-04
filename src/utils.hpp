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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <wx/artprov.h>
#include <wx/config.h>
#include <wx/dialog.h>
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/log.h>
#include <wx/menu.h>

#include <wx/string.h>

#define IMLI_TREECTRL_ARROW_NONE -1
#define IMLI_TREECTRL_ARROW_EMPTY 0
#define IMLI_TREECTRL_ARROW_UP 1
#define IMLI_TREECTRL_ARROW_DOWN 2

#define IMLI_NOTEBOOK_NONE -1
#define IMLI_NOTEBOOK_EMPTY 0
#define IMLI_NOTEBOOK_BULLET 1
#define IMLI_NOTEBOOK_LOCK 2
#define IMLI_NOTEBOOK_PLUS 3

using Clock = std::chrono::steady_clock;

struct EditorSettings;
class wxHtmlWindow;
class wxStyledTextCtrl;

void AppDebugLog(const char *fmt, ...);
void AppTraceLog(const char *fmt, ...);

#define APP_DEBUG_LOG(...)    \
  do {                        \
    AppDebugLog(__VA_ARGS__); \
  } while (0)

#define APP_TRACE_LOG(...)    \
  do {                        \
    AppTraceLog(__VA_ARGS__); \
  } while (0)

#define AE_TRAP_MSG(msg)                                                                    \
  do {                                                                                      \
    std::fprintf(stderr, "TRAP: %s\n  at %s:%d (%s)\n", msg, __FILE__, __LINE__, __func__); \
    __builtin_trap();                                                                       \
  } while (0)

/**
 * Helper for profiling.
 */
struct ScopeTimer {
  std::string name;
  Clock::time_point start;

  ScopeTimer(const char *fmt, ...) {
    char buf[512];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    name = buf;
    start = Clock::now();
  }

  ~ScopeTimer() {
    auto end = Clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    APP_DEBUG_LOG("TIMER [%s]: %lld us", name.c_str(), static_cast<long long>(us));
  }
};

struct SketchFileBuffer {
  std::string filename; // relative/absolute path within the sketch (.ino, .cpp, .hpp...)
  std::string code;     // current content of the editor
};

// Returns <0 if a < b, 0 if a == b, >0 if a > b
int CompareVersions(const std::string &a, const std::string &b);

bool CopyDirRecursive(const wxString &src, const wxString &dst);

std::string Trim(const std::string &s);

void TrimInPlace(std::string &s);

void LeftTrimInPlace(std::string &s);

void RightTrimInPlace(std::string &s);

std::string TrimCopy(std::string s);

wxString TrimCopy(wxString s);

bool LoadWindowSize(const wxString &prefix, wxWindow *win, wxConfigBase *config);

void SaveWindowSize(const wxString &prefix, wxWindow *win, wxConfigBase *config);

wxMenuItem *AddMenuItemWithArt(wxMenu *menu, int id, const wxString &text, const wxString &help, const wxArtID &artId);
void SetupStyledTextCtrl(wxStyledTextCtrl *stc, wxConfigBase *config);
void ApplyStyledTextCtrlSettings(wxStyledTextCtrl *stc, const EditorSettings &s);

void SetupHtmlWindow(wxHtmlWindow *w);

bool LoadFileToString(const std::string &path, std::string &out);
bool SaveFileFromString(const std::string &pathUtf8, const std::string &data);
bool WriteTextFile(const wxString &path, const wxString &text);

std::string Unquote(const std::string &s);
void SplitWhitespace(const std::string &content, std::vector<std::string> &out);
std::string ShellQuote(const std::string &s);

bool startsWithCaseSensitive(const std::string &s, const std::string &prefix);

bool startsWithCaseInsensitive(const std::string &s, const std::string &prefix);

bool containsCaseInsensitive(const std::string &haystack, const std::string &needle);

bool isAllUpperAlpha(const std::string &s);

int countUnderscores(const std::string &s);

bool hasSuffix(const std::string &s, const char *suf);

bool hasPrefix(const std::string &s, const char *pre);

std::string ToLower(const std::string &s);

bool isIno(const std::string &name);

bool isSourceFile(const std::string &name);

bool isSourceExt(const std::string &ext);

bool isHeaderFile(const std::string &name);

bool isHeaderExt(const std::string &ext);

uint64_t Fnv1a64(const uint8_t *data, size_t len);

std::string wxToStd(const wxString &str);

wxImageList *CreateListCtrlSortIndicatorImageList(const wxColour &color);
wxImageList *CreateNotebookPageImageList(const wxColour &color);

std::unordered_set<std::string> SearchCodeIncludes(const std::vector<SketchFileBuffer> &files, const std::string &sketchPath);

wxString GetLocalizationBaseDir();

wxSizer *CreateLocalizedSeparatedOkCancelSizer(wxDialog *dlg);

void ThreadNice();

bool LooksLikeIdentifier(const std::string &str);

std::string StripInoGeneratedSuffix(const std::string &filename);
std::string NormalizeFilename(const std::string &sketchPath, const std::string &filename);
std::string StripFilename(const std::string &sketchPath, const std::string &filename);
std::string DiagnosticsFilename(const std::string &sketchPath, const std::string &input, std::size_t keepParts = 3);

bool IsInSketchDir(const std::string &sketchPath, const std::string &file);

wxString ColorToHex(const wxColour &color);

// Fast methods for sums
uint64_t CcSumCode(const std::vector<SketchFileBuffer> &files);
uint64_t CcSumIncludes(const std::vector<SketchFileBuffer> &files);
uint64_t CcSumDecls(std::string_view filename, std::string_view code);

std::string NormalizeIndent(std::string_view code, size_t indent);

std::string ExtractCommentBlockAboveLine(const std::string &fileText, int declLine);
std::string ExtractBodySnippetFromText(const std::string &fileText, unsigned fromLine, unsigned toLine);

void DedupArgs(std::vector<std::string> &argv);

void SetListCtrlStale(wxListCtrl *lc, bool stale);

std::string StripQuotes(const std::string &s);

// arduino:avr:nano:cpu=atmega328old -> arduino:avr:nano
std::string BaseFqbn3(std::string fqbn);

bool ParseDefaultFqbnFromSketchYaml(const std::filesystem::path &yamlPath, std::string &outBaseFqbn3);
