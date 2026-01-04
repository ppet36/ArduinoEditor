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

#include "ard_cli.hpp"
#include <string>
#include <wx/config.h>
#include <wx/progdlg.h>
#include <wx/wx.h>


class ArduinoCliInstaller {
private:
  wxConfigBase *m_config;
  wxWindow *m_owner;

  int ModalMsgDialog(const wxString &message, const wxString &caption = _("Error"), int styles = wxOK | wxICON_ERROR);

  bool DownloadArduinoCli(wxString &outPath);
#if defined(__WXGTK__)
  bool DownloadArduinoCliLinux(wxString &outPath);
#elif defined(__WXMSW__)
  bool DownloadArduinoCliWindows(wxString &outPath);
#elif defined(__WXMAC__)
  bool DownloadArduinoCliMac(wxString &outPath);
#endif

  bool EnsureBaseToolchainInstalled(const wxString &cliPath);
  bool CheckBaseToolchainInstalled(const wxString &cliPath, bool *outNeedsInstall, bool *outHasAvr, wxProgressDialog *prog);
  bool InstallBaseToolchain(const wxString &cliPath, wxProgressDialog *existingProg);

public:
  ArduinoCliInstaller(wxWindow *owner, wxConfigBase *config);

  ArduinoCli *GetCli(const std::string &sketchPath);

  static bool CheckArduinoCli(const std::string &cliPath);
};
