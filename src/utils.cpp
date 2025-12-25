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
#include "ard_setdlg.hpp"
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/statline.h>
#include <wx/stdpaths.h>

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
    wxBitmapBundle bmp = wxArtProvider::GetBitmapBundle(artId, wxASCII_STR(wxART_MENU));
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

  // XXY

  // Regex for line #include
  //   #include <Foo.h>
  //   #include "subdir/Bar.hpp"
  static const std::regex includeRegex(R"(^\s*#\s*include\s*([<"])\s*([^>"]+)\s*[>"])");

  fs::path sketchDir(sketchPath);

  // a unique list of header names to be resolved through libraries
  std::unordered_set<std::string> headerNames;

  for (const auto &sf : files) {
    const std::string &code = sf.code;
    APP_DEBUG_LOG("Resolving includes in %s", sf.filename.c_str());

    std::size_t pos = 0;
    const std::size_t len = code.size();

    while (pos < len) {
      std::size_t lineEnd = code.find('\n', pos);
      if (lineEnd == std::string::npos)
        lineEnd = len;

      std::string line = code.substr(pos, lineEnd - pos);

      std::smatch m;
      // if (std::regex_match(line, m, includeRegex) && m.size() >= 3) {
      if (std::regex_search(line, m, includeRegex) && m.size() >= 3) {
        char delim = m[1].str()[0];
        std::string inner = m[2].str();
        TrimInPlace(inner);

        if (inner.empty()) {
          pos = (lineEnd < len) ? lineEnd + 1 : len;
          continue;
        }

        // We are only interested in *.h / *.hpp
        fs::path hp(inner);
        std::string ext = hp.extension().string();
        if (ext != ".h" && ext != ".hpp") {
          pos = (lineEnd < len) ? lineEnd + 1 : len;
          continue;
        }

        bool isQuoted = (delim == '"');

        bool isLocalHeader = false;
        if (isQuoted) {
          // for "aaa.h" or "bbb/ccc.h":
          // 1) convert to fs::path
          fs::path relPath(inner);
          fs::path candidate = sketchDir / relPath;

          std::error_code ec;
          if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
            // local header in sketch -> NOT solved as a library
            isLocalHeader = true;
          }
        }

        if (!isLocalHeader) {
          // this is a library candidate
          headerNames.insert(inner);
          APP_DEBUG_LOG(" - resolved %s", inner.c_str());
        }
      }

      pos = (lineEnd < len) ? lineEnd + 1 : len;
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

std::string NormalizeFilename(const std::string &sketchPath,
                              const std::string &filename) {
  namespace fs = std::filesystem;

  // 1) Replace ".ino.cpp" suffix with ".ino"
  std::string cleaned = filename;
  constexpr const char inoCppSuffix[] = ".ino.cpp";
  constexpr std::size_t inoCppLen = sizeof(inoCppSuffix) - 1; // length without null terminator

  if (cleaned.size() >= inoCppLen &&
      cleaned.compare(cleaned.size() - inoCppLen, inoCppLen, inoCppSuffix) == 0) {
    // Remove only the trailing ".cpp" part → ".ino.cpp" becomes ".ino"
    cleaned.erase(cleaned.size() - 4);
  }

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

  const std::string ext1 = ".ino.cpp";
  const std::string ext2 = ".ino.hpp";

  if (f.size() >= ext1.size() && f.compare(f.size() - ext1.size(), ext1.size(), ext1) == 0) {
    f.erase(f.size() - ext1.size() + 4); // keep ".ino"
  } else if (f.size() >= ext2.size() && f.compare(f.size() - ext2.size(), ext2.size(), ext2) == 0) {
    f.erase(f.size() - ext2.size() + 4); // keep ".ino"
  }

  return f;
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
