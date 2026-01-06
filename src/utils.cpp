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

#include "utils.hpp"
#include "ard_ap.hpp"
#include "ard_setdlg.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <system_error>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/html/htmlwin.h>
#include <wx/sstream.h>
#include <wx/statline.h>
#include <wx/stc/stc.h>
#include <wx/stdpaths.h>
#include <wx/wfstream.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

extern bool g_debugLogging;
extern bool g_verboseLogging;

/* XPM for listctrl sort indicators */
static const char *arrow_down_xpm[] = {
    "8 8 2 1",
    ". c None",
    "# c Black",
    "........",
    "........",
    ".#######",
    "..#####.",
    "...###..",
    "....#...",
    "........",
    "........"};

/* XPM for listctrl sort indicators */
static const char *arrow_up_xpm[] = {
    "8 8 2 1",
    ". c None",
    "# c Black",
    "........",
    "........",
    "....#...",
    "...###..",
    "..#####.",
    ".#######",
    "........",
    "........"};

/* XPM for listctrl sort indicators */
static const char *arrow_empty_xpm[] = {
    "8 8 2 1",
    ". c None",
    "# c Black",
    "........",
    "........",
    "........",
    "........",
    "........",
    "........",
    "........",
    "........"};

static const char *x16_empty_xpm[] = {
    "16 16 2 1",
    ". c None",
    "# c Black",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................"};

static const char *x16_plus_xpm[] = {
    "16 16 2 1",
    ". c None",
    "# c Black",
    "................",
    "................",
    "................",
    "......##........",
    "......##........",
    "......##........",
    "......##........",
    "..##########....",
    "..##########....",
    "......##........",
    "......##........",
    "......##........",
    "......##........",
    "................",
    "................",
    "................"};

static const char *x16_bullet_xpm[] = {
    "16 16 2 1",
    ". c None",
    "# c Black",
    "................",
    "................",
    "................",
    "................",
    "......####......",
    ".....######.....",
    "....########....",
    "...##########...",
    "...##########...",
    "...##########...",
    "....########....",
    ".....######.....",
    "......####......",
    "................",
    "................",
    "................"};

static const char *x16_lock_xpm[] = {
    "16 16 2 1",
    ". c None",
    "# c Black",
    "................",
    "................",
    "......####......",
    ".....######.....",
    "....##....##....",
    "....##....##....",
    "....##....##....",
    "...##########...",
    "...##########...",
    "...##########...",
    "...##########...",
    "...##########...",
    "...##########...",
    "....########....",
    "................",
    "................"};

void AppDebugLog(const char *fmt, ...) {
  if (!g_debugLogging) {
    return;
  }

  char buf[65535];

  va_list args;
  va_start(args, fmt);
#if defined(_MSC_VER)
  vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
#else
  vsnprintf(buf, sizeof(buf), fmt, args);
#endif
  va_end(args);

  buf[sizeof(buf) - 1] = '\0';

// On Windows it's a mix of literals and CP1250 output from the toolchain.
// Local conversion (CP1250 -> wchar) is safest for debugging.
#if defined(__WXMSW__)
  wxString msg(buf, wxConvLocal);
#else
  wxString msg = wxString::FromUTF8(buf);
#endif

  wxLogMessage(wxT("[DBG] %s"), msg);
}

void AppTraceLog(const char *fmt, ...) {
  if (!g_verboseLogging) {
    return;
  }

  char buf[65535];

  va_list args;
  va_start(args, fmt);
#if defined(_MSC_VER)
  vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
#else
  vsnprintf(buf, sizeof(buf), fmt, args);
#endif
  va_end(args);

  buf[sizeof(buf) - 1] = '\0';

#if defined(__WXMSW__)
  wxString msg(buf, wxConvLocal);
#else
  wxString msg = wxString::FromUTF8(buf);
#endif

  wxLogMessage(wxT("[TRC] %s"), msg);
}

// Returns <0 if a < b, 0 if a == b, >0 if a > b
int CompareVersions(const std::string &a, const std::string &b) {
  auto split = [](const std::string &s) {
    std::vector<int> parts;
    size_t start = 0;

    while (start < s.size()) {
      size_t dot = s.find('.', start);
      size_t len = (dot == std::string::npos) ? s.size() - start : dot - start;

      std::string token = s.substr(start, len);

      // we take only the initial digits (in case of "1.3.1-beta")
      size_t i = 0;
      while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i]))) {
        ++i;
      }

      int value = 0;
      if (i > 0) {
        try {
          value = std::stoi(token.substr(0, i));
        } catch (...) {
          value = 0;
        }
      }

      parts.push_back(value);

      if (dot == std::string::npos)
        break;
      start = dot + 1;
    }

    return parts;
  };

  auto va = split(a);
  auto vb = split(b);

  size_t n = std::max(va.size(), vb.size());
  for (size_t i = 0; i < n; ++i) {
    int ai = (i < va.size()) ? va[i] : 0;
    int bi = (i < vb.size()) ? vb[i] : 0;
    if (ai < bi)
      return -1;
    if (ai > bi)
      return 1;
  }
  return 0;
}

bool CopyDirRecursive(const wxString &src, const wxString &dst) {
  if (!wxDirExists(src)) {
    return false;
  }

  if (!wxDirExists(dst)) {
    if (!wxFileName::Mkdir(dst, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
      return false;
    }
  }

  wxDir dir(src);
  if (!dir.IsOpened()) {
    return false;
  }

  // First the files
  wxString name;
  bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES);
  while (cont) {
    wxFileName srcFn(src, name);
    wxFileName dstFn(dst, name);

    if (!wxCopyFile(srcFn.GetFullPath(), dstFn.GetFullPath(), true)) {
      return false;
    }

    cont = dir.GetNext(&name);
  }

  // Then subdirectories (recursively)
  cont = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
  while (cont) {
    wxFileName srcSub(src, name);
    wxFileName dstSub(dst, name);

    if (!CopyDirRecursive(srcSub.GetFullPath(), dstSub.GetFullPath())) {
      return false;
    }

    cont = dir.GetNext(&name);
  }

  return true;
}

std::string Trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return std::string();
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

void TrimInPlace(std::string &s) {
  auto notSpace = [](int ch) { return !std::isspace(ch); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
}

void LeftTrimInPlace(std::string &s) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r'))
    ++i;
  s.erase(0, i);
}

void RightTrimInPlace(std::string &s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.pop_back();
}

std::string TrimCopy(std::string s) {
  auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

wxString TrimCopy(wxString s) {
  s.Trim(true).Trim(false);
  return s;
}

bool LoadWindowSize(const wxString &prefix, wxWindow *win, wxConfigBase *config) {
  long x, y, w, h;
  if (config->Read(prefix + wxT("/PosX"), &x) &&
      config->Read(prefix + wxT("/PosY"), &y) &&
      config->Read(prefix + wxT("/Width"), &w) &&
      config->Read(prefix + wxT("/Height"), &h)) {
    win->SetSize(x, y, w, h);
    return true;
  }
  return false;
}

void SaveWindowSize(const wxString &prefix, wxWindow *win, wxConfigBase *config) {
  wxPoint pos = win->GetPosition();
  wxSize size = win->GetSize();

  config->Write(prefix + wxT("/PosX"), pos.x);
  config->Write(prefix + wxT("/PosY"), pos.y);
  config->Write(prefix + wxT("/Width"), size.GetWidth());
  config->Write(prefix + wxT("/Height"), size.GetHeight());
  config->Flush();
}

wxMenuItem *AddMenuItemWithArt(wxMenu *menu, int id, const wxString &text, const wxString &help, const wxArtID &artId) {
  wxMenuItem *item = new wxMenuItem(menu, id, text, help);
  if (!artId.IsEmpty()) {
    wxBitmapBundle bmp = AEGetArtBundle(artId);
    if (bmp.IsOk()) {
      item->SetBitmap(bmp);
    }
  }
  menu->Append(item);
  return item;
}

bool LoadFileToString(const std::string &pathUtf8, std::string &out) {
  out.clear();

#ifdef _WIN32
  // We interpret the input as UTF-8 and convert to wide path.
  std::filesystem::path fsPath = std::filesystem::u8path(pathUtf8);
  std::ifstream ifs(fsPath, std::ios::binary);
#else
  std::ifstream ifs(pathUtf8, std::ios::binary);
#endif

  if (!ifs.is_open()) {
    return false;
  }

  std::ostringstream oss;
  oss << ifs.rdbuf();
  out = oss.str();
  return true;
}

bool SaveFileFromString(const std::string &pathUtf8, const std::string &data) {
#ifdef _WIN32
  std::filesystem::path fsPath = std::filesystem::u8path(pathUtf8);
  std::ofstream ofs(fsPath, std::ios::binary);
#else
  std::ofstream ofs(pathUtf8, std::ios::binary);
#endif

  if (!ofs.is_open())
    return false;

  if (!data.empty()) {
    ofs.write(data.data(), (std::streamsize)data.size());
  }

  return ofs.good();
}

bool WriteTextFile(const wxString &path, const wxString &text) {
  wxFileOutputStream os(path);
  if (!os.IsOk())
    return false;
  wxStringInputStream is(text);
  os.Write(is);
  return os.IsOk();
}

std::string Unquote(const std::string &s) {
  if (s.size() >= 2 &&
      ((s.front() == '"' && s.back() == '"') ||
       (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

void SplitWhitespace(const std::string &content, std::vector<std::string> &out) {
  std::istringstream iss(content);
  std::string token;
  while (iss >> token) {
    out.push_back(token);
  }
}

std::string ShellQuote(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

bool startsWithCaseSensitive(const std::string &s, const std::string &prefix) {
  if (prefix.size() > s.size())
    return false;
  return std::equal(prefix.begin(), prefix.end(), s.begin());
}

bool startsWithCaseInsensitive(const std::string &s, const std::string &prefix) {
  if (prefix.size() > s.size())
    return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    unsigned char c1 = (unsigned char)s[i];
    unsigned char c2 = (unsigned char)prefix[i];
    if (std::tolower(c1) != std::tolower(c2))
      return false;
  }
  return true;
}

bool containsCaseInsensitive(const std::string &haystack, const std::string &needle) {
  if (needle.empty())
    return true;
  if (haystack.size() < needle.size())
    return false;

  auto toLower = [](unsigned char ch) { return (char)std::tolower(ch); };

  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (toLower((unsigned char)haystack[i + j]) !=
          toLower((unsigned char)needle[j])) {
        match = false;
        break;
      }
    }
    if (match)
      return true;
  }
  return false;
}

bool isAllUpperAlpha(const std::string &s) {
  bool hasAlpha = false;
  for (unsigned char ch : s) {
    if (std::isalpha(ch)) {
      hasAlpha = true;
      if (std::islower(ch))
        return false;
    }
  }
  return hasAlpha;
}

int countUnderscores(const std::string &s) {
  return (int)std::count(s.begin(), s.end(), '_');
}

bool hasSuffix(const std::string &s, const char *suf) {
  size_t len = std::strlen(suf);
  return s.size() >= len && s.compare(s.size() - len, len, suf) == 0;
}

bool hasPrefix(const std::string &s, const char *pre) {
  size_t len = std::strlen(pre);
  return s.size() >= len && s.compare(0, len, pre) == 0;
}

std::string ToLower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return out;
}

void SetupStyledTextCtrl(wxStyledTextCtrl *stc, wxConfigBase *config) {
  stc->SetLexer(wxSTC_LEX_CPP);
  stc->SetProperty(wxT("lexer.cpp.track.preprocessor"), wxT("0"));
  stc->SetProperty(wxT("lexer.cpp.update.preprocessor"), wxT("0"));

  // 0 = language keywords + Arduino functions/constants
  stc->SetKeyWords(0, wxT(
                          // C++ keywords
                          "alignas alignof asm auto break case catch  class compl concept const const_cast consteval constexpr constinit "
                          "continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern false  for friend goto if "
                          "inline mutable namespace new noexcept nullptr operator private protected public register reinterpret_cast requires return "
                          "sizeof static static_assert static_cast struct switch template this thread_local throw try typedef typeid typename union using "
                          "virtual volatile while "

                          // Arduino "language"
                          "setup loop "

                          // GPIO / timing
                          "pinMode digitalWrite digitalRead analogRead analogWrite analogReference "
                          "tone noTone shiftOut shiftIn pulseIn "
                          "delay delayMicroseconds millis micros "

                          // Interrupts
                          "attachInterrupt detachInterrupt interrupts noInterrupts "

                          // Constants for pins / interrupts
                          "LOW HIGH INPUT OUTPUT INPUT_PULLUP "
                          "CHANGE RISING FALLING "

                          // A0..A5 and LED (frequently used)
                          "A0 A1 A2 A3 A4 A5 LED_BUILTIN "

                          // Bits + characters
                          "bitRead bitWrite bitSet bitClear bit "
                          "isAlpha isAlphaNumeric isAscii isWhitespace isControl isDigit isGraph isLowerCase isPrintable isPunct isSpace isUpperCase isHexadecimalDigit "

                          // Math / random
                          "min max abs constrain map pow sq sqrt sin cos tan "
                          "random randomSeed "

                          // Bytes/words
                          "highByte lowByte word "

                          // Serial / stream
                          "Serial Serial1 Serial2 Serial3 "
                          "Stream Print "

                          // Other useful things
                          "F PROGMEM PSTR LSBFIRST MSBFIRST"));

  // 1 = types and literals
  stc->SetKeyWords(1, wxT(
                          // booleans / NULL
                          "true false NULL nullptr "

                          // C / C++ types
                          "bool boolean byte word String StringSumHelper "
                          "size_t ptrdiff_t off_t signed unsigned "
                          "int8_t uint8_t int16_t uint16_t int32_t uint32_t int64_t uint64_t "
                          "int short long float double char wchar_t char char16_t char32_t char8_t void "

                          // Arduino classes / types (it's okay if some specific core doesn't have them)
                          "HardwareSerial IPAddress File SPIClass TwoWire "
                          "__FlashStringHelper"));

  if (config) {
    EditorSettings s;
    s.Load(config);

    ApplyStyledTextCtrlSettings(stc, s);
  }
}

void ApplyStyledTextCtrlSettings(wxStyledTextCtrl *stc, const EditorSettings &s) {
  EditorColorScheme c = s.GetColors();

  APP_DEBUG_LOG("UTIL: ApplyStyledTextCtrlSettings()");

  // ---- Default style: fonts + colors ----
  stc->StyleSetFont(wxSTC_STYLE_DEFAULT, s.GetFont());
  stc->StyleSetForeground(wxSTC_STYLE_DEFAULT, c.text);
  stc->StyleSetBackground(wxSTC_STYLE_DEFAULT, c.background);

  stc->StyleClearAll();

  stc->SetBackgroundColour(c.background);
  stc->SetCaretForeground(c.text);

  // ---- Line numbers ----
  const int MARGIN_LINENUMBERS = 0;

  stc->StyleSetForeground(wxSTC_STYLE_LINENUMBER, c.lineNumberText);
  stc->StyleSetBackground(wxSTC_STYLE_LINENUMBER, c.lineNumberBackground);

  if (s.showLineNumbers) {
    stc->SetMarginType(MARGIN_LINENUMBERS, wxSTC_MARGIN_NUMBER);
    int width = stc->TextWidth(wxSTC_STYLE_LINENUMBER, wxT("_9999"));
    stc->SetMarginWidth(MARGIN_LINENUMBERS, width);
  } else {
    stc->SetMarginWidth(MARGIN_LINENUMBERS, 0);
  }

  // ---- Syntax barvy ----
  stc->StyleSetForeground(wxSTC_C_COMMENT, c.comment);
  stc->StyleSetForeground(wxSTC_C_COMMENTLINE, c.comment);
  stc->StyleSetForeground(wxSTC_C_COMMENTDOC, c.comment);
  stc->StyleSetForeground(wxSTC_C_COMMENTDOCKEYWORD, c.comment);
  stc->StyleSetForeground(wxSTC_C_COMMENTDOCKEYWORDERROR, c.comment);

  stc->StyleSetForeground(wxSTC_C_STRING, c.string);
  stc->StyleSetForeground(wxSTC_C_CHARACTER, c.string);

  stc->StyleSetForeground(wxSTC_C_WORD, c.keyword1);
  stc->StyleSetForeground(wxSTC_C_WORD2, c.keyword2);

  stc->StyleSetForeground(wxSTC_C_NUMBER, c.number);
  stc->StyleSetForeground(wxSTC_C_PREPROCESSOR, c.preprocessor);

  // ---- Selection ----
  stc->SetSelBackground(true, c.selection);

  // ---- Whitespace ----
  stc->SetViewWhiteSpace(
      s.showWhitespace ? wxSTC_WS_VISIBLEALWAYS : wxSTC_WS_INVISIBLE);
  stc->SetWhitespaceForeground(true, c.whitespace);

  // ---- Indent ----
  stc->SetTabWidth(s.tabWidth);
  stc->SetUseTabs(s.useTabs);
  stc->SetTabIndents(true);
  stc->SetBackSpaceUnIndents(true);
  stc->SetIndent(s.tabWidth);
  stc->SetIndentationGuides(wxSTC_IV_REAL);

  // ---- Word wrap ----
  stc->SetWrapMode(s.wordWrap ? wxSTC_WRAP_WORD : wxSTC_WRAP_NONE);

  // ---- Right edge / long line marker ----
  if (s.showRightEdge) {
    stc->SetEdgeMode(wxSTC_EDGE_LINE);
    stc->SetEdgeColumn(s.edgeColumn);
    stc->SetEdgeColour(c.edge);
  } else {
    stc->SetEdgeMode(wxSTC_EDGE_NONE);
  }

  // ---- Highlight current line ----
  stc->SetCaretLineVisible(s.highlightCurrentLine);
  if (s.highlightCurrentLine) {
    stc->SetCaretLineBackground(c.caretLine);
  }

  // ---- Braces (match / bad) ----
  // BraceHighlight / BraceBad
  stc->StyleSetForeground(wxSTC_STYLE_BRACELIGHT, c.text);
  stc->StyleSetBold(wxSTC_STYLE_BRACELIGHT, true);
  stc->StyleSetBackground(wxSTC_STYLE_BRACELIGHT, c.braceMatch);

  stc->StyleSetForeground(wxSTC_STYLE_BRACEBAD, c.text);
  stc->StyleSetBold(wxSTC_STYLE_BRACEBAD, true);
  stc->StyleSetBackground(wxSTC_STYLE_BRACEBAD, c.braceBad);

  // ---- Calltip ----
  stc->CallTipSetForeground(c.calltipText);
  stc->CallTipSetBackground(c.calltipBackground);
}

void SetupHtmlWindow(wxHtmlWindow *w) {
  wxConfigBase *config = wxConfigBase::Get();
  EditorSettings settings;
  settings.Load(config);

  int pointSize = settings.GetFont().GetPointSize();

#ifdef __WXMAC__
  w->SetFonts(wxT("Helvetica Neue"), wxT("Menlo"), nullptr);
#elif defined(__WXMSW__)
  w->SetFonts(wxT("Segoe UI"), wxT("Consolas"), nullptr);
#else
  w->SetFonts(wxT("DejaVu Sans"), wxT("DejaVu Sans Mono"), nullptr);
#endif

  static const int sizes[] = {pointSize, pointSize + 1, pointSize + 2, pointSize + 3, pointSize + 4, pointSize + 5, pointSize + 6};
  w->SetFonts(wxEmptyString, wxEmptyString, sizes);
}

bool isIno(const std::string &name) {
  return hasSuffix(name, ".ino");
}

bool isSourceFile(const std::string &name) {
  return hasSuffix(name, ".cpp") ||
         hasSuffix(name, ".cc") ||
         hasSuffix(name, ".cxx") ||
         hasSuffix(name, ".c") ||
         isIno(name);
}

bool isSourceExt(const std::string &ext) {
  return (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c");
}

bool isHeaderFile(const std::string &name) {
  return hasSuffix(name, ".h") ||
         hasSuffix(name, ".hpp") ||
         hasSuffix(name, ".hh");
}

bool isHeaderExt(const std::string &ext) {
  return (ext == "h" || ext == "hpp" || ext == "hh");
}

uint64_t Fnv1a64(const uint8_t *data, size_t len) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= (uint64_t)data[i];
    h *= 1099511628211ULL;
  }
  return h;
}

std::string wxToStd(const wxString &str) {
  wxCharBuffer buf = str.utf8_str();
  return std::string(buf.data(), buf.length());
}

static wxBitmap RecolorXpm(const char **xpm, const wxColour &to) {
  wxImage img = wxBitmap(xpm).ConvertToImage();
  img.Replace(0, 0, 0, to.Red(), to.Green(), to.Blue());
  return wxBitmap(img);
}

wxImageList *CreateListCtrlSortIndicatorImageList(const wxColour &color) {
  wxImageList *imgList = new wxImageList(8, 8, true);
  imgList->Add(RecolorXpm(arrow_empty_xpm, color));
  imgList->Add(RecolorXpm(arrow_up_xpm, color));
  imgList->Add(RecolorXpm(arrow_down_xpm, color));
  return imgList;
}

wxImageList *CreateNotebookPageImageList(const wxColour &color) {
  wxImageList *imgList = new wxImageList(16, 16, true);
  imgList->Add(RecolorXpm(x16_empty_xpm, color));
  imgList->Add(RecolorXpm(x16_bullet_xpm, color));
  imgList->Add(RecolorXpm(x16_lock_xpm, color));
  imgList->Add(RecolorXpm(x16_plus_xpm, color));
  return imgList;
}

std::unordered_set<std::string> SearchCodeIncludes(const std::vector<SketchFileBuffer> &files, const std::string &sketchPath) {
  ScopeTimer t("UTIL: SearchCodeIncludes()");

  // Why this exists:
  // We want to collect header names from `#include ...` directives in the sketch sources.
  // This is used to resolve Arduino libraries that provide those headers.
  //
  // Performance goals:
  // - Avoid std::regex (often surprisingly slow in C++).
  // - Avoid allocating a new std::string for every line (substr()).
  // - Avoid filesystem access for quoted includes ("Foo.h") by checking against the
  //   already-known in-memory file list (SketchFileBuffer vector).
  //
  // Trade-offs:
  // - This is a pragmatic preprocessor-line scanner, not a full C/C++ preprocessor.
  // - It intentionally focuses on common Arduino patterns: `#include <X.h>` and `#include "X.hpp"`.

  const fs::path sketchDir(sketchPath);

  // Build a set of "local" headers that exist inside the sketch itself.
  // This lets us skip resolving `"local.h"` as a library candidate without hitting the filesystem.
  //
  // Note: We store normalized relative paths using generic_string() (forward slashes),
  // so it matches typical include syntax like "subdir/Foo.h".
  std::unordered_set<std::string> localRel;
  localRel.reserve(files.size() * 2);

  for (const auto &sf : files) {
    fs::path p(sf.filename);
    std::error_code ec;

    fs::path rel = p.is_absolute() ? p.lexically_relative(sketchDir) : p;
    rel = rel.lexically_normal();

    // Store "subdir/Foo.h"
    localRel.insert(rel.generic_string());

    // Also store "Foo.h" for root-level files (common include form)
    if (!rel.has_parent_path() || rel.parent_path() == ".") {
      localRel.insert(rel.filename().generic_string());
    }
  }

  // Output: unique list of header strings to resolve via libraries
  std::unordered_set<std::string> headerNames;
  headerNames.reserve(64);

  auto isSpace = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
  };

  auto skipWs = [&](const char *p, const char *end) {
    while (p < end && isSpace((unsigned char)*p))
      ++p;
    return p;
  };

  auto endsWith = [](std::string_view s, std::string_view suf) {
    return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
  };

  // Scan each file line-by-line using pointers and memchr (no per-line allocations).
  for (const auto &sf : files) {
    const std::string &code = sf.code;

    // Be careful with logging in hot paths. Even in debug builds this can cost real time.
    // APP_DEBUG_LOG("UTIL: Resolving includes in %s", sf.filename.c_str());

    const char *p = code.data();
    const char *end = p + code.size();

    while (p < end) {
      const char *line = p;
      const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
      if (!eol)
        eol = end;
      p = (eol < end) ? eol + 1 : end;

      // Minimal parsing:
      //   optional whitespace
      //   '#'
      //   optional whitespace
      //   "include"
      //   optional whitespace
      //   '<' ... '>'  OR  '"' ... '"'
      const char *s = skipWs(line, eol);
      if (s >= eol || *s != '#')
        continue;

      ++s; // skip '#'
      s = skipWs(s, eol);

      // Match the keyword "include" exactly (case-sensitive, like the preprocessor).
      static constexpr char kw[] = "include";
      constexpr size_t kwLen = sizeof(kw) - 1;

      if ((size_t)(eol - s) < kwLen)
        continue;
      if (std::memcmp(s, kw, kwLen) != 0)
        continue;

      s += kwLen;
      s = skipWs(s, eol);
      if (s >= eol)
        continue;

      const char delim = *s;
      char closing = 0;

      if (delim == '<')
        closing = '>';
      else if (delim == '"')
        closing = '"';
      else
        continue;

      ++s; // skip opening delimiter

      // Trim leading whitespace inside delimiters
      while (s < eol && isSpace((unsigned char)*s))
        ++s;
      if (s >= eol)
        continue;

      const char *start = s;
      while (s < eol && *s != closing)
        ++s;
      if (s >= eol)
        continue;

      const char *stop = s;

      // Trim trailing whitespace before the closing delimiter
      while (stop > start && isSpace((unsigned char)stop[-1]))
        --stop;

      std::string_view inner(start, (size_t)(stop - start));
      if (inner.empty())
        continue;

      // Only interested in typical Arduino header extensions
      // (Keep this cheap: avoid fs::path just for extension checks.)
      const bool isHeader = endsWith(inner, ".h") || endsWith(inner, ".hpp");
      if (!isHeader)
        continue;

      // If it's a quoted include, it might be a local sketch header.
      // Check against our precomputed set instead of hitting the filesystem.
      bool isLocalHeader = false;
      if (delim == '"') {
        // Normalize the include path (handles "subdir/../Foo.h" etc.).
        // This allocation happens only for includes found (usually few), not per line.
        fs::path relPath{std::string(inner)};
        std::string relKey = relPath.lexically_normal().generic_string();

        if (localRel.find(relKey) != localRel.end()) {
          isLocalHeader = true;
        }
      }

      if (!isLocalHeader) {
        // This is a library candidate.
        headerNames.emplace(inner); // constructs std::string from string_view
        // APP_DEBUG_LOG(" - resolved %.*s", (int)inner.size(), inner.data());
      }
    }
  }

  return headerNames;
}

static wxString g_locBaseDir;

wxString GetLocalizationBaseDir() {
  if (!g_locBaseDir.empty())
    return g_locBaseDir;

  const wxString exe = wxStandardPaths::Get().GetExecutablePath();
  const wxString exeDir = wxFileName(exe).GetPath();
  const wxString portable = exeDir + wxT("/localization");
  if (wxDirExists(portable)) {
    g_locBaseDir = portable;
    return g_locBaseDir;
  }

  const wxString systemLocale = wxT("/usr/share/locale");
  if (wxDirExists(systemLocale)) {
    g_locBaseDir = systemLocale;
    return g_locBaseDir;
  }

  const wxString dataDir = wxStandardPaths::Get().GetDataDir();
  const wxString dataLocale = dataDir + wxT("/locale");
  if (wxDirExists(dataLocale)) {
    g_locBaseDir = dataLocale;
    return g_locBaseDir;
  }

  g_locBaseDir = portable;
  return g_locBaseDir;
}

wxSizer *CreateLocalizedSeparatedOkCancelSizer(wxDialog *dlg) {
  const int border = dlg->FromDIP(10);

  auto *btnSizer = new wxStdDialogButtonSizer();

  auto *ok = new wxButton(dlg, wxID_OK, _("OK"));
  auto *cancel = new wxButton(dlg, wxID_CANCEL, _("Cancel"));

  btnSizer->AddButton(ok);
  btnSizer->AddButton(cancel);
  btnSizer->Realize();

  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  topSizer->Add(
      new wxStaticLine(dlg),
      0,
      wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
      border);

  topSizer->Add(
      btnSizer,
      0,
      wxALIGN_RIGHT | wxALL,
      border);

  return topSizer;
}

void ThreadNice() {
#if defined(__WXMSW__)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
#if defined(__APPLE__) || defined(__linux__)
  nice(5);
#endif
}

bool LooksLikeIdentifier(const std::string &str) {
  // very simple check - if it looks like a C/C++ identifier
  auto isIdentChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };

  if (str.empty() || !(std::isalpha(static_cast<unsigned char>(str[0])) || str[0] == '_')) {
    return false;
  }
  for (char c : str) {
    if (!isIdentChar(c)) {
      return false;
    }
  }

  return true;
}

std::string StripInoGeneratedSuffix(const std::string &filename) {
  if (hasSuffix(filename, ".ino.cpp") || hasSuffix(filename, ".ino.hpp")) {
    return filename.substr(0, filename.size() - 4);
  }
  return filename;
}

std::string NormalizeFilename(const std::string &sketchPath, const std::string &filename) {
  // 1) Replace ".ino.cpp" suffix with ".ino"
  std::string cleaned = StripInoGeneratedSuffix(filename);

  // 2) Convert to filesystem path
  fs::path p(cleaned);
  fs::path sp(sketchPath);

  // 3) If the path is relative, prepend the sketch directory
  if (p.string().rfind(sp.string(), 0) != 0) {
    p = sp / p;
  }

  // 4) Perform lexical normalization (clean up ".", "..", duplicate separators, etc.)
  p = p.lexically_normal();

  // 5) Return the path using the native separator style for the platform
  return p.string();
}

std::string StripFilename(const std::string &sketchPath, const std::string &filename) {
  std::string f = filename;

  if (f.rfind(sketchPath, 0) == 0) {
    f = f.substr(sketchPath.size());
    if (!f.empty() && (f[0] == '/' || f[0] == '\\')) {
      f = f.substr(1);
    }
  }

  return StripInoGeneratedSuffix(f);
}

// Normalize + keep last N path components, BUT:
// - If input is inside sketchPath => return path relative to sketchPath (no "...").
// - If filename ends with .ino.cpp or .ino.hpp => replace with .ino.
std::string DiagnosticsFilename(const std::string &sketchPath, const std::string &input, std::size_t keepParts) {

  auto Normalize = [](fs::path p) -> fs::path {
    fs::path norm = p.lexically_normal();
    std::error_code ec;
    fs::path can = fs::weakly_canonical(norm, ec);
    if (!ec)
      norm = std::move(can);
    return norm;
  };

  fs::path in = Normalize(fs::u8path(input));

  // If we have a sketchPath, try to return relative path when input is inside it.
  if (!sketchPath.empty()) {
    fs::path base = Normalize(fs::u8path(sketchPath));

    // Purely lexical check: if relative path doesn't start with "..", it's inside.
    fs::path rel = in.lexically_relative(base);

    bool inside = false;
    if (!rel.empty() && rel.is_relative()) {
      auto it = rel.begin();
      if (it != rel.end() && *it != "..") {
        inside = true;
      } else if (rel == ".") {
        // input == sketchPath (unlikely for a file); treat as inside anyway
        inside = true;
        rel.clear();
      }
    }

    if (inside) {
      std::string out = rel.generic_string();
      if (out.empty())
        out = fs::path(in.filename()).generic_string();
      return StripInoGeneratedSuffix(out);
    }
  }

  // --- Outside sketch: keep last N components with optional ".../" prefix ---

  // Drop root ("/" or "C:\") so we only count real components.
  fs::path relNoRoot = in.relative_path();

  std::vector<fs::path> parts;
  parts.reserve(16);
  for (const auto &part : relNoRoot) {
    if (part.empty() || part == ".")
      continue;
    parts.push_back(part);
  }

  if (parts.empty()) {
    return std::string("...");
  }

  const std::size_t n = (keepParts == 0) ? 0 : keepParts;
  const std::size_t start = (n == 0 || parts.size() <= n) ? 0 : (parts.size() - n);

  fs::path tail;
  for (std::size_t i = start; i < parts.size(); ++i) {
    tail /= parts[i];
  }

  const bool trimmed = (start != 0);

  std::string out = trimmed
                        ? (std::string(".../") + tail.generic_string())
                        : tail.generic_string();

  return StripInoGeneratedSuffix(out);
}

static std::string NormalizeForCompare(const std::filesystem::path &p) {
  // generic_string() uses '/' even on Windows
  std::string s = p.lexically_normal().generic_string();

#ifdef _WIN32
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (unsigned char)std::tolower(c); });
#endif
  return s;
}

bool IsInSketchDir(const std::string &sketchPath, const std::string &file) {
  namespace fs = std::filesystem;

  fs::path sketchAbs = fs::absolute(fs::path(sketchPath));
  fs::path fileAbs = fs::absolute(fs::path(NormalizeFilename(sketchPath, file)));

  std::string sketchNorm = NormalizeForCompare(sketchAbs);
  std::string fileNorm = NormalizeForCompare(fileAbs);

  // Important: trailing slash prevents false match "sketch" vs "sketchbook"
  if (!sketchNorm.empty() && sketchNorm.back() != '/')
    sketchNorm.push_back('/');

  return fileNorm.rfind(sketchNorm, 0) == 0; // prefix
}

wxString ColorToHex(const wxColour &color) {
  return wxString::Format(wxT("#%02X%02X%02X"),
                          color.Red(),
                          color.Green(),
                          color.Blue());
}

// ---------- 64-bit FNV-1a ----------
static inline uint64_t fnv1a64_init() {
  return 14695981039346656037ull;
}
static inline uint64_t fnv1a64_update(uint64_t h, const void *data, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ull;
  }
  return h;
}
static inline uint64_t fnv1a64_update_sv(uint64_t h, std::string_view sv) {
  return fnv1a64_update(h, sv.data(), sv.size());
}

static inline bool is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static inline const char *skip_ws(const char *p, const char *end) {
  while (p < end && is_space((unsigned char)*p))
    ++p;
  return p;
}

// Case-sensitive match "include" after '#'
static inline bool match_word(const char *p, const char *end, const char *word) {
  const size_t n = std::strlen(word);
  if ((size_t)(end - p) < n)
    return false;
  return std::memcmp(p, word, n) == 0;
}

// ---------- SUM #1: full code ----------
uint64_t CcSumCode(const std::vector<SketchFileBuffer> &files) {
  ScopeTimer t("UTIL: CcSumCode(%zu files)", files.size());

  uint64_t h = fnv1a64_init();
  for (const auto &f : files) {
    std::string_view name = f.filename;
    std::string_view code = f.code;

    h = fnv1a64_update_sv(h, name);
    h = fnv1a64_update(h, "\0", 1); // separator
    h = fnv1a64_update_sv(h, code);
    h = fnv1a64_update(h, "\0", 1); // separator
  }
  return h;
}

// ---------- SUM #2: includes only ----------
uint64_t CcSumIncludes(const std::vector<SketchFileBuffer> &files) {
  ScopeTimer t("UTIL: CcSumIncludes(%zu files)", files.size());

  uint64_t h = fnv1a64_init();

  for (const auto &f : files) {
    std::string_view name = f.filename;
    std::string_view code = f.code;

    h = fnv1a64_update_sv(h, name);
    h = fnv1a64_update(h, "\n", 1);

    const char *p = code.data();
    const char *end = p + code.size();

    bool inBlockComment = false;

    while (p < end) {
      const char *line = p;
      const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
      if (!eol)
        eol = end;
      p = (eol < end) ? eol + 1 : end;

      const char *s = line;
      const char *le = eol;

      // fast removal of block comments /* ... */ across lines
      // (line-based; enough for includes)
      // also ignore // comments at the end
      // This is not a C preprocessor parser; just “good enough”.
      // Find the first relevant character outside the block comment.
      for (;;) {
        if (inBlockComment) {
          const char *c = (const char *)memchr(s, '*', (size_t)(le - s));
          if (!c) {
            s = le;
            break;
          }
          if (c + 1 < le && c[1] == '/') {
            inBlockComment = false;
            s = c + 2;
            continue;
          }
          s = c + 1;
          continue;
        }

        // skip ws
        s = skip_ws(s, le);
        if (s >= le)
          break;

        // line comment?
        if (s + 1 < le && s[0] == '/' && s[1] == '/') {
          s = le;
          break;
        }

        // block comment start?
        if (s + 1 < le && s[0] == '/' && s[1] == '*') {
          inBlockComment = true;
          s += 2;
          continue;
        }

        break;
      }

      if (s >= le)
        continue;

      // Preprocessor?
      if (*s != '#')
        continue;
      s++;
      s = skip_ws(s, le);

      if (!match_word(s, le, "include"))
        continue;
      s += 7;
      s = skip_ws(s, le);

      if (s >= le)
        continue;

      char kind = 0;
      char closing = 0;
      if (*s == '<') {
        kind = '<';
        closing = '>';
        s++;
      } else if (*s == '"') {
        kind = '"';
        closing = '"';
        s++;
      } else
        continue;

      const char *start = s;
      while (s < le && *s != closing)
        ++s;
      if (s >= le)
        continue;

      std::string_view header(start, (size_t)(s - start));

      while (!header.empty() && is_space((unsigned char)header.front()))
        header.remove_prefix(1);
      while (!header.empty() && is_space((unsigned char)header.back()))
        header.remove_suffix(1);

      h = fnv1a64_update(h, &kind, 1);
      h = fnv1a64_update_sv(h, header);
      h = fnv1a64_update(h, "\n", 1);
    }
  }

  return h;
}

// ---------- SUM: declarations/signatures (skip function bodies) ----------
// Computes a fast fingerprint of "declaration-ish" text by skipping function bodies.
// Intended use: cache key for ino.hpp generation / prototype extraction.
// Notes:
// - This is a heuristic scanner, not a full C++ parser.
// - It ignores string/char literals and comments.
// - It normalizes whitespace.
// - It skips everything inside function bodies detected as: ')' ... '{' ... matching '}'.
//
// The key property: edits inside function bodies usually won't change the hash.
uint64_t CcSumDecls(std::string_view filename, std::string_view code) {
  ScopeTimer t("UTIL: CcSumDecls(%zu bytes)", (size_t)code.size());

  uint64_t h = fnv1a64_init();

  // Mix filename into the hash so same code in another file doesn't collide.
  h = fnv1a64_update_sv(h, filename);
  h = fnv1a64_update(h, "\n", 1);

  const char *p = code.data();
  const char *end = p + code.size();

  bool inLineComment = false;
  bool inBlockComment = false;
  bool inString = false;
  bool inChar = false;
  bool escape = false;

  int skipBodyDepth = 0;        // >0 => inside skipped function body
  bool pendingFuncBody = false; // saw ')' and waiting to see if '{' follows

  bool lastWasSpace = false;

  auto hash_space = [&]() {
    if (!lastWasSpace) {
      const char sp = ' ';
      h = fnv1a64_update(h, &sp, 1);
      lastWasSpace = true;
    }
  };

  auto hash_char = [&](char c) {
    h = fnv1a64_update(h, &c, 1);
    lastWasSpace = false;
  };

  while (p < end) {
    const char c = *p;
    const char n = (p + 1 < end) ? p[1] : '\0';

    // Treat newlines as whitespace and terminate line comments
    if (c == '\n') {
      inLineComment = false;
      hash_space();
      ++p;
      continue;
    }

    // ---------- Inside skipped body: only track braces, but still respect comments/strings ----------
    if (skipBodyDepth > 0) {
      if (inLineComment) {
        ++p;
        continue;
      }

      if (inBlockComment) {
        if (c == '*' && n == '/') {
          inBlockComment = false;
          p += 2;
          continue;
        }
        ++p;
        continue;
      }

      if (inString) {
        if (!escape && c == '"')
          inString = false;
        escape = (!escape && c == '\\');
        ++p;
        continue;
      }

      if (inChar) {
        if (!escape && c == '\'')
          inChar = false;
        escape = (!escape && c == '\\');
        ++p;
        continue;
      }

      // Comment / string starts
      if (c == '/' && n == '/') {
        inLineComment = true;
        p += 2;
        continue;
      }
      if (c == '/' && n == '*') {
        inBlockComment = true;
        p += 2;
        continue;
      }
      if (c == '"') {
        inString = true;
        escape = false;
        ++p;
        continue;
      }
      if (c == '\'') {
        inChar = true;
        escape = false;
        ++p;
        continue;
      }

      // Track nested braces to find end of body
      if (c == '{') {
        ++skipBodyDepth;
        ++p;
        continue;
      }
      if (c == '}') {
        --skipBodyDepth;
        if (skipBodyDepth == 0) {
          // Include a marker that a body ended (keeps "body presence" visible in the hash)
          hash_char('}');
        }
        ++p;
        continue;
      }

      ++p;
      continue;
    }

    // ---------- Outside body: handle comments/strings first ----------
    if (inLineComment) {
      ++p;
      continue;
    }

    if (inBlockComment) {
      if (c == '*' && n == '/') {
        inBlockComment = false;
        p += 2;
        continue;
      }
      ++p;
      continue;
    }

    if (inString) {
      if (!escape && c == '"')
        inString = false;
      escape = (!escape && c == '\\');
      ++p;
      continue; // ignore string contents
    }

    if (inChar) {
      if (!escape && c == '\'')
        inChar = false;
      escape = (!escape && c == '\\');
      ++p;
      continue; // ignore char contents
    }

    // Start comment?
    if (c == '/' && n == '/') {
      inLineComment = true;
      p += 2;
      continue;
    }
    if (c == '/' && n == '*') {
      inBlockComment = true;
      p += 2;
      continue;
    }

    // Start string/char?
    if (c == '"') {
      inString = true;
      escape = false;
      ++p;
      continue;
    }
    if (c == '\'') {
      inChar = true;
      escape = false;
      ++p;
      continue;
    }

    // Normalize whitespace
    if (is_space((unsigned char)c)) {
      hash_space();
      ++p;
      continue;
    }

    // Heuristic: after ')' we may be entering a function body if '{' follows soon.
    if (c == ')') {
      pendingFuncBody = true;
      hash_char(c);
      ++p;
      continue;
    }

    // ';' cancels pending function body
    if (c == ';') {
      pendingFuncBody = false;
      hash_char(c);
      ++p;
      continue;
    }

    // '{' after ')' => treat as function body start and skip its contents
    if (c == '{') {
      if (pendingFuncBody) {
        pendingFuncBody = false;
        hash_char('{');    // include body-start marker
        skipBodyDepth = 1; // skip until matching '}'
        ++p;
        continue;
      }

      // Non-function block (namespace/class/if/while etc.) is not skipped
      hash_char('{');
      ++p;
      continue;
    }

    // Default: hash the character
    hash_char(c);
    ++p;
  }

  return h;
}

std::string NormalizeIndent(std::string_view code, size_t indent) {
  // Split into lines, keeping '\n' (except possibly last line).
  auto splitLinesKeepNewline = [](std::string_view s) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
      size_t j = s.find('\n', i);
      if (j == std::string_view::npos) {
        out.push_back(s.substr(i));
        break;
      }
      out.push_back(s.substr(i, (j - i) + 1)); // include '\n'
      i = j + 1;
    }
    if (s.empty())
      out.push_back(std::string_view{});
    return out;
  };

  auto isBlankLine = [](std::string_view line) -> bool {
    for (char c : line) {
      if (c == '\n')
        break;
      if (c != ' ' && c != '\t' && c != '\r')
        return false;
    }
    return true;
  };

  auto leadingWsCount = [](std::string_view line) -> size_t {
    size_t n = 0;
    for (char c : line) {
      if (c == ' ' || c == '\t' || c == '\r') {
        ++n;
        continue;
      }
      if (c == '\n')
        break;
      break;
    }
    return n;
  };

  auto lines = splitLinesKeepNewline(code);

  // Find minimal common indentation among non-blank lines.
  size_t minIndent = std::numeric_limits<size_t>::max();
  bool anyNonBlank = false;
  for (auto line : lines) {
    if (isBlankLine(line))
      continue;
    anyNonBlank = true;
    minIndent = std::min(minIndent, leadingWsCount(line));
  }
  if (!anyNonBlank) {
    return std::string(code); // preserve all-blank blocks as-is
  }
  if (minIndent == std::numeric_limits<size_t>::max())
    minIndent = 0;

  const std::string targetPrefix(indent, ' ');

  // Rebuild.
  std::string out;
  out.reserve(code.size() + lines.size() * indent);

  for (auto line : lines) {
    if (isBlankLine(line)) {
      // Keep blank lines exactly as-is (incl. newline).
      out.append(line.data(), line.size());
      continue;
    }

    // Strip exactly minIndent leading whitespace *characters*.
    size_t toStrip = minIndent;
    size_t pos = 0;
    while (pos < line.size() && toStrip > 0) {
      char c = line[pos];
      if (c == ' ' || c == '\t' || c == '\r') {
        ++pos;
        --toStrip;
        continue;
      }
      break;
    }

    out += targetPrefix;
    out.append(line.data() + pos, line.size() - pos);
  }

  return out;
}

// declLine is 1-based (clang line).
std::string ExtractCommentBlockAboveLine(const std::string &fileText, int declLine) {
  if (declLine <= 1)
    return {};

  // index of lines beginning
  std::vector<size_t> starts;
  starts.reserve(4096);
  starts.push_back(0);
  for (size_t i = 0; i < fileText.size(); ++i) {
    if (fileText[i] == '\n')
      starts.push_back(i + 1);
  }
  int lineCount = (int)starts.size();
  if (declLine > lineCount)
    declLine = lineCount;

  auto getLine = [&](int line1) -> std::string {
    if (line1 < 1 || line1 > lineCount)
      return {};
    size_t b = starts[(size_t)(line1 - 1)];
    size_t e = (line1 < lineCount) ? starts[(size_t)line1] : fileText.size();
    // strip trailing '\n'
    if (e > b && fileText[e - 1] == '\n')
      --e;
    return fileText.substr(b, e - b);
  };

  std::vector<std::string> collected;
  collected.reserve(32);

  bool inBlock = false;
  int safety = 0;

  // start: line above declaration
  for (int ln = declLine - 1; ln >= 1 && safety++ < 1000; --ln) {
    std::string raw = getLine(ln);
    std::string t = raw;
    TrimInPlace(t);

    if (!inBlock) {
      if (t.empty()) {
        break; // empty line = end of doc block
      }

      // line comments //
      if (startsWithCaseSensitive(t, "//")) {
        collected.push_back(raw);
        continue;
      }

      // block comments /* ... */
      if (t.find("*/") != std::string::npos) {
        inBlock = true;
        collected.push_back(raw);
        if (t.find("/*") != std::string::npos) {
          inBlock = false;
        }
        continue;
      }

      // anything else => end
      break;
    } else {
      // we are inside /* ... */ and we are collecting up to "/*"
      collected.push_back(raw);

      if (t.find("/*") != std::string::npos) {
        inBlock = false;
      }
    }
  }

  if (collected.empty())
    return {};

  std::reverse(collected.begin(), collected.end());

  // normalize: strip //, /*, */, and leading '*'
  std::string out;
  out.reserve(1024);

  for (std::string line : collected) {
    std::string s = line;
    LeftTrimInPlace(s);

    if (startsWithCaseSensitive(s, "//")) {
      s.erase(0, 2);
      if (!s.empty() && s[0] == ' ')
        s.erase(0, 1);
    } else {
      // /* ... */ block
      auto pos = s.find("/*");
      if (pos != std::string::npos) {
        s.erase(pos, 2);
      }
      pos = s.find("*/");
      if (pos != std::string::npos) {
        s.erase(pos, 2);
      }
      LeftTrimInPlace(s);
      if (!s.empty() && s[0] == '*') {
        s.erase(0, 1);
        if (!s.empty() && s[0] == ' ')
          s.erase(0, 1);
      }
    }

    RightTrimInPlace(s);
    out += s;
    out += '\n';
  }

  while (!out.empty() && (out.front() == '\n' || out.front() == '\r'))
    out.erase(out.begin());
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    out.pop_back();

  // sanity limit (to avoid taking giant heads/licenses)
  if (out.size() > 8192) {
    out.resize(8192);
    out += "...";
  }

  return out;
}

std::string ExtractBodySnippetFromText(const std::string &fileText, unsigned fromLine, unsigned toLine) {
  if (fromLine == 0 || toLine == 0 || toLine < fromLine) {
    return {};
  }

  // index of line beginnings
  std::vector<size_t> starts;
  starts.reserve(4096);
  starts.push_back(0);
  for (size_t i = 0; i < fileText.size(); ++i) {
    if (fileText[i] == '\n')
      starts.push_back(i + 1);
  }
  const unsigned lineCount = (unsigned)starts.size();
  if (lineCount == 0) {
    return {};
  }

  if (fromLine > lineCount) {
    return {};
  }
  if (toLine > lineCount) {
    toLine = lineCount;
  }

  auto getLine = [&](unsigned line1) -> std::string {
    if (line1 < 1 || line1 > lineCount)
      return {};
    size_t b = starts[(size_t)(line1 - 1)];
    size_t e = (line1 < lineCount) ? starts[(size_t)line1] : fileText.size();
    if (e > b && fileText[e - 1] == '\n')
      --e;
    return fileText.substr(b, e - b);
  };

  const unsigned totalLines = toLine - fromLine + 1;

  std::vector<std::string> lines;
  lines.reserve((totalLines <= 6) ? (size_t)totalLines : 7);

  if (totalLines <= 6) {
    for (unsigned ln = fromLine; ln <= toLine; ++ln) {
      lines.push_back(getLine(ln));
    }
  } else {
    for (unsigned i = 0; i < 3; ++i) {
      lines.push_back(getLine(fromLine + i));
    }
    lines.push_back("...");
    for (unsigned i = 0; i < 3; ++i) {
      lines.push_back(getLine(toLine - 2 + i));
    }
  }

  // Join
  std::string out;
  out.reserve(2048);
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size())
      out += '\n';
  }

  // Keep it sane
  constexpr size_t MAX_SNIPPET = 4096;
  if (out.size() > MAX_SNIPPET) {
    out.resize(MAX_SNIPPET);
    out += "...";
  }

  // Trim leading / trailing blank lines
  while (!out.empty() && (out.front() == '\n' || out.front() == '\r'))
    out.erase(out.begin());
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    out.pop_back();

  return out;
}

static std::string NormalizeIncPath(const std::string &raw) {
  std::filesystem::path p = std::filesystem::u8path(raw).lexically_normal();
  return p.u8string();
}

/// Compiler args deduplication.
void DedupArgs(std::vector<std::string> &argv) {
  ScopeTimer t("UTIL: DedupArgs(%zu args)", argv.size());

  // Pass 1: figure out "last index" for single-choice flags.
  // We'll remove earlier ones in Pass 2.
  auto keySingle = [](const std::string &a) -> std::string {
    // Return empty => not a single-choice flag
    if (a.rfind("-std=", 0) == 0)
      return "std";
    if (a == "-std")
      return "std"; // (rare)
    if (a == "-target")
      return "target";
    if (a.rfind("-target=", 0) == 0)
      return "target";
    if (a == "-x")
      return "x";
    if (a.rfind("-x", 0) == 0 && a.size() > 2)
      return "x"; // -xfoo
    if (a == "-isysroot")
      return "isysroot";
    return {};
  };

  std::unordered_map<std::string, std::size_t> lastSingle;
  lastSingle.reserve(32);

  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string &a = argv[i];
    std::string k = keySingle(a);
    if (!k.empty()) {
      // For two-token form (-std gnu++17), treat "-std" token as the flag position.
      lastSingle[k] = i;
      continue;
    }
  }

  // Sets for exact dedup (stable, keep first)
  std::unordered_set<std::string> seenInc; // "I:<path>" / "S:<path>"
  std::unordered_set<std::string> seenDef; // exact "-D..." or "-U..."
  seenInc.reserve(argv.size() / 4);
  seenDef.reserve(argv.size() / 4);

  std::vector<std::string> out;
  out.reserve(argv.size());

  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string &a = argv[i];

    // --- single-choice: drop earlier occurrences, keep only last
    {
      std::string k = keySingle(a);
      if (!k.empty()) {
        auto it = lastSingle.find(k);
        if (it != lastSingle.end() && it->second != i) {
          // skip this token (and if it is two-token form, also skip its value)
          if ((a == "-std" || a == "-target" || a == "-x" || a == "-isysroot") && (i + 1 < argv.size())) {
            ++i;
          }
          continue;
        }
        // keep it (and its value if two-token)
        out.push_back(a);
        if ((a == "-std" || a == "-target" || a == "-x" || a == "-isysroot") && (i + 1 < argv.size())) {
          out.push_back(argv[++i]);
        }
        continue;
      }
    }

    // --- includes
    if (a == "-I" && i + 1 < argv.size()) {
      std::string p = NormalizeIncPath(argv[i + 1]);
      std::string key = "U:" + p;
      if (seenInc.insert(key).second) {
        out.push_back("-I");
        out.push_back(p);
      }
      ++i;
      continue;
    }
    if (a.rfind("-I", 0) == 0 && a.size() > 2) {
      std::string p = NormalizeIncPath(a.substr(2));
      std::string key = "U:" + p;
      if (seenInc.insert(key).second) {
        out.push_back("-I" + p); // keep attached form, ok
      }
      continue;
    }
    if (a == "-isystem" && i + 1 < argv.size()) {
      std::string p = NormalizeIncPath(argv[i + 1]);
      std::string key = "S:" + p;
      if (seenInc.insert(key).second) {
        out.push_back("-isystem");
        out.push_back(p);
      }
      ++i;
      continue;
    }
    if (a.rfind("-isystem", 0) == 0 && a.size() > 8) {
      std::string p = NormalizeIncPath(a.substr(8));
      std::string key = "S:" + p;
      if (seenInc.insert(key).second) {
        out.push_back("-isystem");
        out.push_back(p); // normalize to two-token, simpler
      }
      continue;
    }

    // --- defines / undefines (exact)
    if (a.rfind("-D", 0) == 0 || a.rfind("-U", 0) == 0) {
      if (seenDef.insert(a).second) {
        out.push_back(a);
      }
      continue;
    }
    if ((a == "-D" || a == "-U") && i + 1 < argv.size()) {
      std::string merged = a + argv[i + 1]; // exact identity key
      if (seenDef.insert(merged).second) {
        out.push_back(a);
        out.push_back(argv[i + 1]);
      }
      ++i;
      continue;
    }

    // --- everything else unchanged
    out.push_back(a);
  }

  APP_DEBUG_LOG("UTIL: DedupArgs() %zu -> %zu", argv.size(), out.size());
  argv.swap(out);
}

void SetListCtrlStale(wxListCtrl *lc, bool stale) {
  if (!lc)
    return;

  auto blend = [](const wxColour &a, const wxColour &b, double t) -> wxColour {
    t = std::clamp(t, 0.0, 1.0);
    auto lerp = [t](unsigned char x, unsigned char y) -> unsigned char {
      return (unsigned char)std::lround((1.0 - t) * (double)x + t * (double)y);
    };
    return wxColour(lerp(a.Red(), b.Red()),
                    lerp(a.Green(), b.Green()),
                    lerp(a.Blue(), b.Blue()));
  };

  const wxColour normalFg = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT);
  const wxColour normalBg = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);

  const double textT = IsDarkMode() ? 0.60 : 0.55;

  const wxColour staleFg = blend(normalFg, normalBg, textT);

  lc->Freeze();

  lc->SetTextColour(stale ? staleFg : normalFg);

  const long count = lc->GetItemCount();
  for (long i = 0; i < count; ++i) {
    lc->SetItemTextColour(i, stale ? staleFg : normalFg);
  }

  lc->Refresh();
  lc->Thaw();
}

std::string StripQuotes(const std::string &s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// arduino:avr:nano:cpu=atmega328old -> arduino:avr:nano
std::string BaseFqbn3(std::string fqbn) {
  TrimInPlace(fqbn);
  fqbn = StripQuotes(fqbn);
  if (fqbn.empty())
    return {};

  std::string out;
  out.reserve(fqbn.size());

  int parts = 0;
  std::size_t start = 0;
  while (true) {
    std::size_t pos = fqbn.find(':', start);
    std::size_t len = (pos == std::string::npos) ? (fqbn.size() - start) : (pos - start);
    std::string part = fqbn.substr(start, len);
    TrimInPlace(part);
    if (part.empty())
      break;

    if (!out.empty())
      out.push_back(':');
    out += part;
    parts++;

    if (parts >= 3)
      break;
    if (pos == std::string::npos)
      break;
    start = pos + 1;
  }

  return out;
}

bool ParseDefaultFqbnFromSketchYaml(const fs::path &yamlPath, std::string &outBaseFqbn3) {
  std::ifstream in(yamlPath);
  if (!in)
    return false;

  std::string line;
  bool waitingIndentedValue = false;

  while (std::getline(in, line)) {
    // Cut off comment (YAML comment from #; we don't deal with quoting-escape, intentionally simple and fast)
    if (auto hash = line.find('#'); hash != std::string::npos) {
      line.erase(hash);
    }

    // If we are waiting for the value on the next line (default_fqbn: <empty>)
    if (waitingIndentedValue) {
      std::string tmp = line;
      // The value should be on an indented line
      if (!tmp.empty() && std::isspace(static_cast<unsigned char>(tmp[0]))) {
        TrimInPlace(tmp);
        if (!tmp.empty()) {
          outBaseFqbn3 = BaseFqbn3(tmp);
          return !outBaseFqbn3.empty();
        }
        continue;
      } else {
        // it's no longer indented -> end of block
        waitingIndentedValue = false;
      }
    }

    std::string s = line;
    TrimInPlace(s);
    if (s.empty())
      continue;

    // We want a line starting with "default_fqbn:"
    static constexpr const char *key = "default_fqbn";
    if (s.rfind(key, 0) == 0) {
      // must be followed by ':'
      std::size_t colon = s.find(':', std::strlen(key));
      if (colon == std::string::npos)
        continue;

      std::string val = s.substr(colon + 1);
      TrimInPlace(val);

      if (val.empty()) {
        waitingIndentedValue = true; // value may be on the next line
        continue;
      }

      outBaseFqbn3 = BaseFqbn3(val);
      return !outBaseFqbn3.empty();
    }
  }

  return false;
}

void KillUnfocusedColor(wxGenericTreeCtrl *tree) {
  // Keep selection readable even when the control loses focus (dark mode friendly).
  // We intentionally swallow KILL_FOCUS so wxGenericTreeCtrl doesn't switch to "inactive selection" colors.
  tree->Bind(wxEVT_KILL_FOCUS, [tree](wxFocusEvent &) {
    // No e.Skip() on purpose.
    tree->Refresh(false);
  });

  tree->Bind(wxEVT_SET_FOCUS, [tree](wxFocusEvent &e) {
    e.Skip(); // let the control handle focus normally
    tree->Refresh(false);
  });

  // Prime the control once so it renders selection using "focused" colors even at startup.
  // Do it after creation/layout so FindFocus() is meaningful.
  tree->CallAfter([tree] {
    wxWindow *prev = wxWindow::FindFocus();

    // Give the tree focus once to switch it into the "focused selection" painting mode.
    tree->SetFocus();
    tree->Refresh(false);

    // Restore focus back where it was (so we don't steal focus from editor/text control).
    if (prev && prev != tree)
      prev->SetFocus();
  });
}
