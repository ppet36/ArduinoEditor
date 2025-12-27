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

// NOTE:
// Arduino Editor bundles a full copy of arduino-cli with all official releases
// on all supported platforms (macOS, Windows, Linux, Raspberry Pi).
// The arduino-cli binary is therefore always expected to be located next to
// the Arduino Editor executable.
//
// As a result, most of the installation / discovery logic in this file is
// currently not used in normal release builds and exists mainly for:
//
//  - historical reasons (early development before bundling was introduced),
//  - fallback or development scenarios,
//  - potential future changes in distribution strategy.
//
// Before removing this code, consider that it may still be useful for
// non-bundled builds, custom distributions, or debugging setups.
// If bundling remains a permanent design decision, this code can likely
// be simplified or removed.


#include "ard_cliinst.hpp"
#include "utils.hpp"
#include <memory>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/regex.h>
#include <wx/richmsgdlg.h>
#include <wx/stdpaths.h>

#define MIN_CLI_VERSION "1.3.0"

namespace {
static wxString QuoteCliPathForShell(const wxString &path) {
#if defined(_WIN32)
  wxString p = path;
  p.Replace(wxT("\""), wxT("\\\""));
  return wxString::Format(wxT("\"%s\""), p);
#else
  // POSIX-safe single-quote escaping:  '  ->  '\''
  wxString p = path;
  p.Replace(wxT("'"), wxT("'\\''"));
  return wxString::Format(wxT("'%s'"), p);
#endif
}

static wxString BuildCliCmd(const wxString &cliPath, const wxString &args) {
  return QuoteCliPathForShell(cliPath) + wxT(" ") + args;
}
} // namespace

ArduinoCliInstaller::ArduinoCliInstaller(wxWindow *owner, wxConfigBase *config) : m_config(config), m_owner(owner) {
}

int ArduinoCliInstaller::ModalMsgDialog(const wxString &message, const wxString &caption, int styles) {
  wxRichMessageDialog dlg(m_owner, message, caption, styles);
  return dlg.ShowModal();
}

bool ArduinoCliInstaller::CheckBaseToolchainInstalled(const wxString &cliPath,
                                                      bool *outNeedsInstall,
                                                      bool *outHasAvr) {
  if (outNeedsInstall)
    *outNeedsInstall = false;
  if (outHasAvr)
    *outHasAvr = false;

  std::string output;

  auto tryCoreList = [&](const wxString &args) -> int {
    output.clear();
    wxString cmd = BuildCliCmd(cliPath, args);
    return ArduinoCli::ExecuteCommand(wxToStd(cmd), output);
  };

  // Prefer "--format json"
  int rc = tryCoreList(wxT("core list --format json"));
  if (rc != 0) {
    rc = tryCoreList(wxT("core list --json"));
  }

  if (rc != 0) {
    int dr = ModalMsgDialog(
        _("Unable to check installed Arduino cores (arduino-cli core list failed).\n\n"
          "Do you want Arduino Editor to run the initial setup now?\n"
          "(Update core index + install Arduino AVR core + update library index)"),
        _("Arduino toolchain setup"),
        wxYES_NO | wxICON_WARNING);

    if (dr == wxID_YES) {
      if (outNeedsInstall)
        *outNeedsInstall = true;
      return true;
    }
    if (outNeedsInstall)
      *outNeedsInstall = false;
    return true;
  }

  wxString outWx = wxString::FromUTF8(output);
  wxString outLower = outWx.Lower();

  // Has AVR?
  bool hasAvr = outLower.Contains(wxT("arduino:avr"));

  // Empty platforms?
  wxString trimmed = outWx;
  trimmed.Trim(true).Trim(false);

  wxRegEx reEmptyPlatforms(wxT("\"platforms\"\\s*:\\s*\\[\\s*\\]"));
  wxRegEx reEmptyArray(wxT("^\\[\\s*\\]$"));

  bool empty = false;
  if (reEmptyPlatforms.IsValid() && reEmptyPlatforms.Matches(outWx)) {
    empty = true;
  } else if (reEmptyArray.IsValid() && reEmptyArray.Matches(trimmed)) {
    empty = true;
  }

  if (outHasAvr)
    *outHasAvr = hasAvr;

  // “Base” = we want to have at least AVR. If it is completely empty, or AVR is missing -> install.
  bool needs = empty || !hasAvr;
  if (outNeedsInstall)
    *outNeedsInstall = needs;

  return true;
}

bool ArduinoCliInstaller::InstallBaseToolchain(const wxString &cliPath, wxProgressDialog *existingProg) {
  wxProgressDialog *prog = existingProg;
  std::unique_ptr<wxProgressDialog> owned;

  if (!prog) {
    owned.reset(new wxProgressDialog(
        _("Preparing Arduino toolchain"),
        _("Preparing..."),
        100,
        m_owner,
        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME | wxPD_CAN_ABORT));
    prog = owned.get();
  }

  auto step = [&](int pct, const wxString &msg) -> bool {
    ::wxYield();
    bool cont = prog->Update(pct, msg);
    ::wxYield();
    return cont;
  };

  std::string out;

  auto runCli = [&](const wxString &args, const wxString &what) -> bool {
    wxString cmd = BuildCliCmd(cliPath, args);
    out.clear();
    int rc = ArduinoCli::ExecuteCommand(wxToStd(cmd), out);
    if (rc != 0) {
      prog->Update(100);

      wxString details = wxString::FromUTF8(out);
      if (details.Length() > 4000)
        details = details.Left(4000) + wxT("\n...");

      ModalMsgDialog(
          wxString::Format(_("arduino-cli command failed while %s:\n\n%s\n\nOutput:\n%s"),
                           what, cmd, details),
          _("arduino-cli setup error"),
          wxOK | wxICON_ERROR);
      return false;
    }
    return true;
  };

  if (!step(50, _("Updating core index..."))) {
    ModalMsgDialog(_("Toolchain setup was cancelled."), _("Cancelled"), wxOK | wxICON_INFORMATION);
    return false;
  }
  if (!runCli(wxT("core update-index"), _("updating core index")))
    return false;

  if (!step(65, _("Installing Arduino AVR core..."))) {
    ModalMsgDialog(_("Toolchain setup was cancelled."), _("Cancelled"), wxOK | wxICON_INFORMATION);
    return false;
  }
  if (!runCli(wxT("core install arduino:avr"), _("installing Arduino AVR core")))
    return false;

  if (!step(85, _("Updating library index..."))) {
    ModalMsgDialog(_("Toolchain setup was cancelled."), _("Cancelled"), wxOK | wxICON_INFORMATION);
    return false;
  }
  if (!runCli(wxT("lib update-index"), _("updating library index")))
    return false;

  step(100, _("Done."));
  return true;
}

bool ArduinoCliInstaller::EnsureBaseToolchainInstalled(const wxString &cliPath) {
  bool needs = false;
  bool hasAvr = false;

  if (!CheckBaseToolchainInstalled(cliPath, &needs, &hasAvr)) {
    return false;
  }
  if (!needs) {
    return true;
  }
  return InstallBaseToolchain(cliPath, nullptr);
}

ArduinoCli *ArduinoCliInstaller::GetCli(const std::string &sketchPath) {
  // Try to get the arduino-cli path from the config
  std::string cfgCliPath;
  if (m_config) {
    wxString s;
    if (m_config->Read(wxT("ArduinoCliPath"), &s) && !s.IsEmpty()) {
      cfgCliPath = wxToStd(s);
    }
  }

  std::string cliVersion;
  std::string cliPath = ArduinoCli::DetectCliExecutable(cfgCliPath, &cliVersion);

  // If arduino-cli cannot be found, we will offer Install / Browse / Cancel
  while (cliPath.empty()) {
    wxRichMessageDialog dlg(
        m_owner,
        _("arduino-cli was not found.\n\n"
          "Arduino Editor can automatically download the official "
          "arduino-cli binary from the Arduino servers, extract it into "
          "the same directory as this application and use it only for "
          "this editor.\n\n"
          "Alternatively, you can select an existing arduino-cli binary on disk.\n\n"
          "How do you want to proceed?"),
        _("arduino-cli missing"),
        wxYES_NO | wxCANCEL | wxICON_QUESTION);

    dlg.SetYesNoCancelLabels(_("Install"), _("Browse"), _("Exit"));

    int dr = dlg.ShowModal();

    if (dr == wxID_YES) {
      // Install
      wxString installedPath;
      if (!DownloadArduinoCli(installedPath)) {
        // Install failed; we return to main dialog.
        continue;
      }

      std::string tmpVersion;
      std::string tmpPath =
          ArduinoCli::DetectCliExecutable(wxToStd(installedPath), &tmpVersion);

      if (!tmpPath.empty()) {
        cliPath = tmpPath;
        cliVersion = tmpVersion;

        if (m_config) {
          m_config->Write(wxT("ArduinoCliPath"), installedPath);
          m_config->Flush();
        }
        break; // we have valid cliPath
      }

      ModalMsgDialog(
          _("arduino-cli was downloaded, but it does not appear to be a valid "
            "arduino-cli binary.\n\n"
            "Please install arduino-cli manually or select the binary manually."),
          _("Invalid arduino-cli binary"),
          wxOK | wxICON_ERROR);

      // cliPath is still empty -> cycle displays main dialog again
    } else if (dr == wxID_NO) {
      // Browse - file dialog
      while (true) {
        wxFileDialog fileDlg(
            m_owner,
#if defined(_WIN32)
            _("Select arduino-cli executable"),
#else
            _("Select arduino-cli binary"),
#endif
            wxEmptyString,
#if defined(_WIN32)
            wxT("arduino-cli.exe"),
#else
            wxT("arduino-cli"),
#endif
#if defined(_WIN32)
            _("arduino-cli executable (*.exe)|*.exe|All files (*.*)|*.*"),
#else
            _("All files (*)|*"),
#endif
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);

        if (fileDlg.ShowModal() != wxID_OK) {
          // User canceled Browse -> back to main Install/Browse/Cancel dialog
          break;
        }

        wxString chosenPathWx = fileDlg.GetPath();
        std::string chosenPath = wxToStd(chosenPathWx);

        std::string tmpVersion;
        std::string tmpPath =
            ArduinoCli::DetectCliExecutable(chosenPath, &tmpVersion);

        if (!tmpPath.empty()) {
          cliPath = tmpPath;
          cliVersion = tmpVersion;

          if (m_config) {
            m_config->Write(wxT("ArduinoCliPath"), chosenPathWx);
            m_config->Flush();
          }
          break; // we have valid cliPath
        }

        int retry = ModalMsgDialog(
            _("The selected file does not appear to be a valid arduino-cli binary.\n\n"
              "Do you want to try selecting a different file?"),
            _("Invalid arduino-cli binary"),
            wxYES_NO | wxICON_ERROR);

        if (retry != wxID_YES) {
          // abort Browse, return to main dialog
          break;
        }
      }

      if (!cliPath.empty()) {
        // Valid binary selected by user -> end of main while
        break;
      }

      // otherwise we continue in the main while, Install/Browse/Exit will be displayed again
    } else {
      ModalMsgDialog(
          _("Arduino Editor cannot run without arduino-cli.\n\n"
            "Please install arduino-cli and restart the application."),
          _("arduino-cli not found"),
          wxOK | wxICON_ERROR);
      return nullptr;
    }
  }

  // Check minimum version (MIN_CLI_VERSION)
  if (CompareVersions(cliVersion, MIN_CLI_VERSION) < 0) {
    if (m_config) {
      m_config->DeleteEntry(wxT("ArduinoCliPath"));
    }

    wxString msg = wxString::Format(
        _("Installed arduino-cli version is %s.\n\n"
          "This Arduino Editor requires arduino-cli version %s or newer.\n\n"
          "Please update arduino-cli and start the application again."),
        wxString::FromUTF8(cliVersion),
        wxString::FromUTF8(MIN_CLI_VERSION));

    ModalMsgDialog(msg, _("Unsupported arduino-cli version"));
    return nullptr;
  }

  // Ensure base toolchain is available for this user (important when arduino-cli exists system-wide)
  if (!EnsureBaseToolchainInstalled(wxString::FromUTF8(cliPath))) {
    return nullptr;
  }

  return new ArduinoCli(sketchPath, cliPath);
}

bool ArduinoCliInstaller::CheckArduinoCli(const std::string &cliPath) {
  std::string cliVersion;
  std::string detectedPath = ArduinoCli::DetectCliExecutable(cliPath, &cliVersion);

  return !detectedPath.empty() && (CompareVersions(cliVersion, MIN_CLI_VERSION) >= 0);
}

bool ArduinoCliInstaller::DownloadArduinoCli(wxString &outPath) {
#if defined(__WXGTK__)
  return DownloadArduinoCliLinux(outPath);
#elif defined(__WXMSW__)
  return DownloadArduinoCliWindows(outPath);
#elif defined(__WXMAC__)
  return DownloadArduinoCliMac(outPath);
#else
  outPath = wxEmptyString;
  ModalMsgDialog(
      _("Automatic arduino-cli installation is not supported on this platform.\n\n"
        "Please install arduino-cli manually and restart the application."),
      _("arduino-cli install"),
      wxOK | wxICON_INFORMATION);
  return false;
#endif
}

#ifdef __WXGTK__
bool ArduinoCliInstaller::DownloadArduinoCliLinux(wxString &outPath) {
  // --------------------------
  // Show progress dialog
  // --------------------------
  wxProgressDialog prog(
      _("Installing arduino-cli"),
      _("Preparing..."),
      100,
      m_owner,
      wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME);

  // Directory where ArduinoEditor binary lives
  wxStandardPaths &paths = wxStandardPaths::Get();
  wxString exePath = paths.GetExecutablePath();
  wxFileName exeFn(exePath);
  wxString appDir = exeFn.GetPath();

  if (!wxDirExists(appDir)) {
    ModalMsgDialog(_("Cannot determine application directory.\nPlease install arduino-cli manually."));
    return false;
  }

  wxString tmpDir = wxFileName::GetTempDir();
  wxString archivePath = tmpDir;
  if (!archivePath.EndsWith(wxFILE_SEP_PATH)) {
    archivePath += wxFILE_SEP_PATH;
  }
  archivePath += wxT("arduino-cli.tar.gz");

  wxString url = wxT("https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Linux_64bit.tar.gz");

  // ---------------------------------------
  // Step 1: Download
  // ---------------------------------------
  prog.Update(10, _("Downloading arduino-cli..."));
  ::wxYield();

  std::string output;

  {
    wxString cmd = wxString::Format(wxT("curl -L '%s' -o '%s'"), url, archivePath);
    int rc = ArduinoCli::ExecuteCommand(wxToStd(cmd), output);
    if (rc != 0) {
      prog.Update(100);
      ModalMsgDialog(_("Downloading arduino-cli failed.\n\nCommand was:\n") + cmd,
                     _("Download error"));
      return false;
    }
  }

  // ---------------------------------------
  // Step 2: Extract
  // ---------------------------------------
  prog.Update(30, _("Extracting arduino-cli..."));
  ::wxYield();

  {
    wxString cmd = wxString::Format(wxT("tar -xzf '%s' -C '%s'"), archivePath, appDir);
    long rc = wxExecute(cmd, wxEXEC_SYNC);
    if (rc != 0) {
      prog.Update(100);
      ModalMsgDialog(_("Extracting arduino-cli failed.\n\nCommand was:\n") + cmd,
                     _("Extraction error"));
      return false;
    }
  }

  // ---------------------------------------
  // Step 3: Verify
  // ---------------------------------------
  prog.Update(40, _("Verifying arduino-cli..."));
  ::wxYield();

  wxFileName cliFile(appDir, wxT("arduino-cli"));
  if (!cliFile.IsFileReadable()) {
    prog.Update(100);
    ModalMsgDialog(_("After extraction, the arduino-cli executable was not found at:\n") + cliFile.GetFullPath());
    return false;
  }

  ::chmod(wxToStd(cliFile.GetFullPath()).c_str(), 0755);

  wxString cliPath = cliFile.GetFullPath();

  // ---------------------------------------
  // Step 4: Initial toolchain setup (shared)
  // ---------------------------------------
  if (!InstallBaseToolchain(cliPath, &prog)) {
    return false;
  }

  // Done
  prog.Update(100, _("Done."));
  ::wxYield();

  outPath = cliPath;
  return true;
}
#endif

#ifdef __WXMAC__
bool ArduinoCliInstaller::DownloadArduinoCliMac(wxString &outPath) {
  wxProgressDialog prog(
      _("Installing arduino-cli"),
      _("Preparing..."),
      100,
      m_owner,
      wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME);

  prog.Update(10, _("Looking for Homebrew (brew)..."));
  ::wxYield();

  // Let's try to find brew in typical places for Intel / Apple Silicon
  wxString brewPath;
  const wxString brewCandidates[] = {
      wxT("/opt/homebrew/bin/brew"),
      wxT("/usr/local/bin/brew")};

  for (const auto &candidate : brewCandidates) {
    if (wxFileExists(candidate)) {
      brewPath = candidate;
      break;
    }
  }

  if (brewPath.empty()) {
    prog.Update(100);

    ModalMsgDialog(
        _("Homebrew (brew) was not found in standard locations.\n\n"
          "Please install Homebrew first (https://brew.sh/), then run:\n\n"
          "  brew install arduino-cli\n\n"
          "and finally use the 'Browse' option to select the arduino-cli binary."),
        _("arduino-cli install"),
        wxOK | wxICON_INFORMATION);
    return false;
  }

  // brew install arduino-cli
  prog.Update(40, _("Installing arduino-cli via Homebrew..."));
  ::wxYield();

  wxString installCmd = wxString::Format(wxT("'%s' install arduino-cli"), brewPath);
  long rc = wxExecute(installCmd, wxEXEC_SYNC);
  if (rc != 0) {
    prog.Update(100);

    ModalMsgDialog(
        wxString::Format(_("Homebrew command failed:\n\n%s"
                           "\n\nPlease run this command in Terminal and then use 'Browse' "
                           "to select the arduino-cli binary."),
                         installCmd),
        _("Homebrew error"),
        wxOK | wxICON_ERROR);
    return false;
  }

  // We can find the Homebrew prefix /opt/homebrew or /usr/local etc.
  prog.Update(70, _("Locating arduino-cli binary..."));
  ::wxYield();

  wxArrayString output;
  wxString prefixCmd = wxString::Format(wxT("'%s' --prefix"), brewPath);
  rc = wxExecute(prefixCmd, output, wxEXEC_SYNC);

  wxFileName cliFile;
  if (rc == 0 && !output.empty()) {
    wxString prefix = output[0];
    prefix.Trim(true).Trim(false); // trim both ends
    cliFile.Assign(prefix + wxT("/bin"), wxT("arduino-cli"));
  }

  // Fallback: typical location where the prefix would not work
  if (!cliFile.IsFileReadable()) {
    wxFileName f1(wxT("/opt/homebrew/bin"), wxT("arduino-cli"));
    wxFileName f2(wxT("/usr/local/bin"), wxT("arduino-cli"));
    if (f1.IsFileReadable()) {
      cliFile = f1;
    } else if (f2.IsFileReadable()) {
      cliFile = f2;
    }
  }

  if (!cliFile.IsFileReadable()) {
    prog.Update(100);
    ModalMsgDialog(
        _("Homebrew seems to have installed arduino-cli, but the binary "
          "could not be located in standard paths.\n\n"
          "Please run 'brew --prefix' or 'brew --prefix arduino-cli' in Terminal\n"
          "to locate the installation and then use the 'Browse' option."),
        _("arduino-cli not found"),
        wxOK | wxICON_ERROR);
    return false;
  }

  prog.Update(100, _("Done."));
  ::wxYield();

  outPath = cliFile.GetFullPath();
  return true;
}
#endif // __WXMAC__

#ifdef __WXMSW__
bool ArduinoCliInstaller::DownloadArduinoCliWindows(wxString &outPath) {
  wxProgressDialog prog(
      _("Installing arduino-cli"),
      _("Preparing..."),
      100,
      m_owner,
      wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME);

  wxStandardPaths &paths = wxStandardPaths::Get();
  wxString exePath = paths.GetExecutablePath();
  wxFileName exeFn(exePath);
  wxString appDir = exeFn.GetPath();

  if (!wxDirExists(appDir)) {
    ModalMsgDialog(_("Cannot determine application directory.\nPlease install arduino-cli manually."));
    return false;
  }

  wxString tmpDir = wxFileName::GetTempDir();
  wxString archivePath = tmpDir;
  if (!archivePath.EndsWith(wxFILE_SEP_PATH)) {
    archivePath += wxFILE_SEP_PATH;
  }
  archivePath += wxT("arduino-cli.zip");

  wxString url = wxT("https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip");

  // 1) Download
  prog.Update(10, _("Downloading arduino-cli..."));
  ::wxYield();

  std::string output;

  {
    wxString cmd = wxString::Format(wxT("powershell -NoProfile -Command \"Invoke-WebRequest -UseBasicParsing -Uri '%s' -OutFile '%s'\""), url, archivePath);
    int rc = ArduinoCli::ExecuteCommand(wxToStd(cmd), output);
    if (rc != 0) {
      prog.Update(100);
      ModalMsgDialog(
          _("Downloading arduino-cli failed.\n\nCommand was:\n") + cmd,
          _("Download error"),
          wxOK | wxICON_ERROR);
      return false;
    }
  }

  // 2) Extract
  prog.Update(30, _("Extracting arduino-cli..."));
  ::wxYield();

  {
    wxString cmd = wxString::Format(wxT("powershell -NoProfile -Command \"Expand-Archive -Path '%s' -DestinationPath '%s' -Force\""), archivePath, appDir);
    int rc = ArduinoCli::ExecuteCommand(wxToStd(cmd), output);
    if (rc != 0) {
      prog.Update(100);
      ModalMsgDialog(
          _("Extracting arduino-cli failed.\n\nCommand was:\n") + cmd,
          _("Extraction error"),
          wxOK | wxICON_ERROR);
      return false;
    }
  }

  // 3) Verify - in the official ZIP there is arduino-cli.exe in the root
  prog.Update(40, _("Verifying arduino-cli..."));
  ::wxYield();

  wxFileName cliFile(appDir, wxT("arduino-cli.exe"));
  if (!cliFile.IsFileReadable()) {
    prog.Update(100);
    ModalMsgDialog(
        _("After extraction, the arduino-cli executable was not found at:\n") + cliFile.GetFullPath(),
        _("arduino-cli not found"),
        wxOK | wxICON_ERROR);
    return false;
  }

  wxString cliPath = cliFile.GetFullPath();

  // 4) Shared initial toolchain setup
  if (!InstallBaseToolchain(cliPath, &prog)) {
    return false;
  }

  prog.Update(100, _("Done."));
  ::wxYield();

  outPath = cliPath;
  return true;
}
#endif // __WXMSW__
