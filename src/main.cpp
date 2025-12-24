
#include "main.hpp"
#include "ard_ap.hpp"
#include "ard_ed_frm.hpp"
#include "utils.hpp"
#include <wx/artprov.h>
#include <wx/cmdline.h>
#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/richmsgdlg.h>
#include <wx/stdpaths.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

bool g_debugLogging = false;
bool g_verboseLogging = false;
std::unique_ptr<wxLocale> g_locale;
ArduinoArtProvider *g_artProvider = new ArduinoArtProvider();

enum {
  TIMER_START = wxID_HIGHEST + 1,
  TIMER_STOP
};

#if defined(__WXMSW__)
#include <fstream>
#include <windows.h>

static std::ofstream g_logFile;

static HANDLE g_singleInstanceMutex = nullptr;

// Base title of main frame
static const wchar_t *kMainWindowTitlePrefix = L"Arduino Editor";

static void EnablePerMonitorV2() {
  HMODULE user32 = ::LoadLibraryW(L"user32.dll");
  if (!user32)
    return;

  using Fn = decltype(&::SetProcessDpiAwarenessContext);
  auto p = reinterpret_cast<Fn>(
      reinterpret_cast<void *>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));

  if (p) {
    p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  }

  ::FreeLibrary(user32);
}

static void InitLogFileIfNeeded() {
  if (g_logFile.is_open())
    return;

  wxString exePath = wxStandardPaths::Get().GetExecutablePath();
  wxFileName fn(exePath);
  fn.SetFullName(wxT("arduino_editor_debug.log"));

  wxString logPath = fn.GetFullPath();
  g_logFile.open(wxToStd(logPath), std::ios::out | std::ios::app);
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
  const size_t bufSize = 512;
  wchar_t title[bufSize];
  int len = GetWindowTextW(hwnd, title, bufSize);
  if (len <= 0)
    return TRUE; // continue

  // Seach prefix "Arduino Editor"
  size_t prefixLen = wcslen(kMainWindowTitlePrefix);
  if (len >= (int)prefixLen && wcsncmp(title, kMainWindowTitlePrefix, prefixLen) == 0) {
    // we found window -> lParam
    *reinterpret_cast<HWND *>(lParam) = hwnd;
    return FALSE; // break enumeration
  }

  return TRUE; // continue
}

static HWND FindMainEditorWindow() {
  HWND found = nullptr;
  EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&found));
  return found;
}

static bool SendOpenSketchToExistingInstance(const wxString &path) {
  HWND hwnd = FindMainEditorWindow();
  if (!hwnd)
    return false;

  std::string utf8 = wxToStd(path);

  COPYDATASTRUCT cds;
  cds.dwData = 1; // message type (1 = open sketch)
  cds.cbData = static_cast<DWORD>(utf8.size() + 1);
  cds.lpData = (void *)utf8.c_str();

  SendMessageW(hwnd, WM_COPYDATA, (WPARAM) nullptr, (LPARAM)&cds);

  ShowWindow(hwnd, SW_SHOWNORMAL);
  SetForegroundWindow(hwnd);
  return true;
}

static bool ActivateExistingInstance() {
  HWND hwnd = FindMainEditorWindow();
  if (!hwnd)
    return false;

  ShowWindow(hwnd, SW_SHOWNORMAL);
  SetForegroundWindow(hwnd);
  return true;
}

// Returns true = continue as primary instance, false = terminate
static bool EnsureSingleInstanceAndForwardIfNeeded(const wxString &sketchToOpen) {
  g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"ArduinoEditor_SingleInstance_Mutex");
  if (!g_singleInstanceMutex) {
    // When something goes wrong, we prefer not to force single-instance
    return true;
  }

  if (GetLastError() != ERROR_ALREADY_EXISTS) {
    // We are the first instance
    return true;
  }

  // Another instance - we will try to pass parameters to it
  bool forwarded = false;
  if (!sketchToOpen.IsEmpty()) {
    forwarded = SendOpenSketchToExistingInstance(sketchToOpen);
  } else {
    forwarded = ActivateExistingInstance();
  }

  // If the transfer was successful, we will terminate this instance.
  if (forwarded) {
    return false;
  }

  // Fallback - mutex exists, but window not found (previous instance crashed)
  // -> let it continue and "take over" the role of the main instance.
  return true;
}
#endif // __WXMSW__

static const wxCmdLineEntryDesc g_cmdLineDesc[] = {
    // --verbose
    {wxCMD_LINE_SWITCH,
     nullptr,
     "verbose",
     "generate verbose log messages",
     wxCMD_LINE_VAL_NONE,
     0},

    // --debug
    {wxCMD_LINE_SWITCH,
     nullptr,
     "debug",
     "enable debug logging",
     wxCMD_LINE_VAL_NONE,
     0},

    // Unnamed parameter - OPTIONAL
    {wxCMD_LINE_PARAM,
     nullptr,
     nullptr,
     "path to sketch directory or *.ino file",
     wxCMD_LINE_VAL_STRING,
     wxCMD_LINE_PARAM_OPTIONAL},

    wxCMD_LINE_DESC_END};

ArduinoEditApp::ArduinoEditApp() : wxApp(), m_startTimer(this, TIMER_START), m_stopTimer(this, TIMER_STOP) {
}

bool ArduinoEditApp::OnInit() {
#ifdef __WXGTK__
  GtkSettings *settings = gtk_settings_get_default();
  if (settings) {
    g_object_set(G_OBJECT(settings), "gtk-menu-images", TRUE, NULL);
    g_object_set(G_OBJECT(settings), "gtk-button-images", TRUE, NULL);
  }
#endif

  if (!wxApp::OnInit()) {
    return false;
  }

  wxDisableAsserts();

#if defined(__WXMSW__)
  if (!EnsureSingleInstanceAndForwardIfNeeded(m_sketchToBeOpen)) {
    return false;
  }
#endif

  // icons
  wxArtProvider::Push(g_artProvider);

  cfg = wxConfig::Get();

  wxString langPref = wxT("system");
  if (cfg) {
    cfg->Read(wxT("Language"), &langPref, wxT("system"));
  }

  int langId;

  if (langPref == wxT("system")) {
    langId = wxLocale::GetSystemLanguage();
  } else {
    const wxLanguageInfo *info = wxLocale::FindLanguageInfo(langPref);
    if (info) {
      langId = info->Language;
    } else {
      langId = wxLocale::GetSystemLanguage();
    }
  }

  g_locale = std::make_unique<wxLocale>();
  if (!g_locale->Init(langId, wxLOCALE_DONT_LOAD_DEFAULT)) {
    APP_DEBUG_LOG("wxLocale init failed for lang=%d", langId);
  }

  // path + catalogs
  wxString locBase = GetLocalizationBaseDir();
  wxLocale::AddCatalogLookupPathPrefix(locBase);
  g_locale->AddCatalog(wxT("ArduinoEditor"));
  g_locale->AddCatalog(wxT("wxstd"));

  m_splash = new SplashFrame();
  m_splash->Present();

  Bind(wxEVT_TIMER, &ArduinoEditApp::OnStartTimer, this, TIMER_START);
  Bind(wxEVT_TIMER, &ArduinoEditApp::OnStopTimer, this, TIMER_STOP);

  m_startTimer.Start(100, true);

  return true;
}

int ArduinoEditApp::OnExit() {
#if defined(__WXMSW__)
  if (g_singleInstanceMutex) {
    ReleaseMutex(g_singleInstanceMutex);
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
  }
#endif
  return wxApp::OnExit();
}

void ArduinoEditApp::OnStartTimer(wxTimerEvent &WXUNUSED(event)) {

  if (g_debugLogging) {
#if defined(__WXMSW__)
    InitLogFileIfNeeded();

    if (g_logFile.is_open()) {
      auto *fileLog = new wxLogStream(&g_logFile);
      wxLog::SetActiveTarget(fileLog);
      wxLog::SetVerbose(true);
    } else {
      // Fallback
      wxLog::SetActiveTarget(new wxLogStderr());
      wxLog::SetVerbose(true);
    }
#else
    wxLog::SetActiveTarget(new wxLogStderr());
    wxLog::SetVerbose(true);
#endif

    APP_DEBUG_LOG("Debug mode enabled");
  }

  SetSplashMessage(_("Initializing UI..."));

  auto frame = new ArduinoEditorFrame(cfg);
  frame->Show(true);

  // 1) Open sketch requested from cmdline
  if (!m_sketchToBeOpen.IsEmpty()) {
    std::string s = wxToStd(m_sketchToBeOpen);
    APP_DEBUG_LOG("SketchToBeOpen: %s", s.c_str());

    wxFileName fn(m_sketchToBeOpen);
    if (fn.FileExists() && fn.GetExt() == wxT("ino")) {
      wxString sketchDir = fn.GetPath();
      frame->OpenSketch(wxToStd(sketchDir));
      return;
    } else if (fn.DirExists()) {
      frame->OpenSketch(wxToStd(m_sketchToBeOpen));
      return;
    }
  }

  // 2) If LastSketchPath is saved, then open last sketch
  wxString lastSketchPath;
  if (cfg->Read(wxT("LastSketchPath"), &lastSketchPath)) {
    wxFileName fn = wxFileName::DirName(lastSketchPath);
    if (fn.DirExists()) {
      std::string s = wxToStd(lastSketchPath);
      APP_DEBUG_LOG("LastSketchPath: %s", s.c_str());

      frame->OpenSketch(s);
      return;
    }
    // if the directory does not exist, we take it as "we don't have a last sketch"
  }

  // 3) If we have SketchesDir, we'll try NewSketch directly
  wxString sketchesDir;
  bool hasSketchesDir = cfg->Read(wxT("SketchesDir"), &sketchesDir) && !sketchesDir.IsEmpty();

  if (hasSketchesDir) {
    if (frame->NewSketch(/*inNewWindow=*/false)) {
      return;
    }
    // if NewSketch fails (user cancels), we fall back to
    // standard OpenSketchDialog fallback
  } else {
    // 4) Special case - no LastSketchPath and no SketchesDir -> first run
    wxRichMessageDialog dlg(
        frame,
        _("Thank you for trying Arduino Editor!\n\n"
          "It looks like this is your first time running the application.\n\n"
          "You can now either:\n"
          " - Open an existing Arduino sketch directory, or\n"
          " - Create a new sketch in your sketchbook folder.\n\n"
          "What would you like to do?"),
        _("Welcome to Arduino Editor"),
        wxYES_NO | wxCANCEL | wxICON_INFORMATION);

    dlg.SetYesNoCancelLabels(_("Open sketch"), _("New sketch"), _("Exit"));

    int rc = dlg.ShowModal();

    if (rc == wxID_YES) {
      // Open existing sketch
      if (!frame->OpenSketchDialog(/*inNewWindow=*/false)) {
        // user didn't open anything -> we just quit
        exit(0);
      }
      return;
    } else if (rc == wxID_NO) {
      // Create new sketch - prepare the default SketchesDir in Documents/Arduino/sketchbook
      wxStandardPaths &stdp = wxStandardPaths::Get();
      wxFileName sketchbookDir(stdp.GetDocumentsDir(), wxEmptyString);
      sketchbookDir.AppendDir(wxT("Arduino"));
      sketchbookDir.AppendDir(wxT("sketchbook"));

      wxString sketchbookPath = sketchbookDir.GetPath();

      if (!wxDirExists(sketchbookPath)) {
        if (!wxFileName::Mkdir(sketchbookPath, 0777, wxPATH_MKDIR_FULL)) {
          wxRichMessageDialog dlg(frame,
                                  wxString::Format(_("Failed to create the default sketchbook folder:\n\n%s"
                                                     "\n\nPlease create a sketch folder manually and use 'Open sketch'."),
                                                   sketchbookPath),
                                  _("Error creating sketchbook"),
                                  wxOK | wxICON_ERROR);

          dlg.ShowModal();

          // Fallback - we'll try the classic OpenSketchDialog
          if (!frame->OpenSketchDialog(/*inNewWindow=*/false)) {
            exit(0);
          }
          return;
        }
      }

      // Save SketchesDir to the configuration
      cfg->Write(wxT("SketchesDir"), sketchbookPath);
      cfg->Flush();

      // And we will create a new sketch within this sketchbook
      if (!frame->NewSketch(/*inNewWindow=*/false)) {
        // user cancels dialog for new sketch -> we just end it
        exit(0);
      }
      return;
    } else {
      // Exit
      exit(0);
    }
  }

  // 5) Fallback - we are not in the first run (SketchesDir exists),
  // but NewSketch failed, or we have no open sketch -> we will offer to open
  if (!frame->OpenSketchDialog(/*inNewWindow=*/false)) {
    exit(0);
  }
}

void ArduinoEditApp::SetSplashMessage(const wxString &message) {
  if (m_splash) {
    APP_DEBUG_LOG("SF: message=%s", wxToStd(message).c_str());
    m_splash->SetMessage(message);
  }
}

void ArduinoEditApp::OnStopTimer(wxTimerEvent &WXUNUSED(event)) {
  if (m_splash) {
    m_splash->Destroy();
    m_splash = nullptr;
  }
}

void ArduinoEditApp::HideSplash() {
  m_stopTimer.Start(2000, true);
}

void ArduinoEditApp::OpenSketch(const wxString &path) {
  ArduinoEditorFrame *frame = new ArduinoEditorFrame(cfg);
  frame->Show(true);

  std::string stdPath = std::string(path.ToUTF8().data());
  frame->OpenSketch(stdPath);
}

void ArduinoEditApp::OnInitCmdLine(wxCmdLineParser &parser) {
  wxApp::OnInitCmdLine(parser);

  parser.SetDesc(g_cmdLineDesc);
  parser.SetSwitchChars(wxT("-")); // so that both -x and --xxx work
}

bool ArduinoEditApp::OnCmdLineParsed(wxCmdLineParser &parser) {
  bool hasDebug = parser.Found(wxT("debug"));
  bool hasVerbose = parser.Found(wxT("verbose"));

  g_verboseLogging = hasVerbose;
  g_debugLogging = hasDebug || hasVerbose;

  if (parser.GetParamCount() > 0) {
    m_sketchToBeOpen = parser.GetParam(0);
  }

  return wxApp::OnCmdLineParsed(parser);
}

wxIMPLEMENT_APP_NO_MAIN(ArduinoEditApp);

#ifdef __WXMSW__
int main(int argc, char **argv) {
  EnablePerMonitorV2();
  return wxEntry(argc, argv);
}
#else
int main(int argc, char **argv) {
  return wxEntry(argc, argv);
}
#endif
