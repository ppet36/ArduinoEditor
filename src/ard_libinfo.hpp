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
#include <wx/config.h>
#include <wx/wx.h>

class ArduinoLibraryDetailDialog : public wxDialog {
public:
  ArduinoLibraryDetailDialog(wxWindow *parent, const ArduinoLibraryInfo &info, const ArduinoLibraryInfo *installedInfo, wxConfigBase *config, const wxString &typeLabel = wxEmptyString, bool installUninstallButton = true);
  ~ArduinoLibraryDetailDialog();

  static void ShowInstalledLibraryInfo(wxWindow *parent, const ArduinoLibraryInfo *installedInfo, wxConfigBase *config, ArduinoCli *cli);

private:
  const ArduinoLibraryInfo *m_installedInfo = nullptr;
  wxConfigBase *m_config = nullptr;
};
