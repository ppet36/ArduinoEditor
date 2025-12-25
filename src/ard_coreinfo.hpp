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

#include <wx/config.h>
#include <wx/dialog.h>
#include <wx/string.h>

#include "ard_cli.hpp" // ArduinoCoreInfo

class ArduinoCoreManagerFrame;

class ArduinoCoreDetailDialog : public wxDialog {
public:
  ArduinoCoreDetailDialog(wxWindow *parent,
                          const ArduinoCoreInfo &info,
                          wxConfigBase *config,
                          const wxString &typeLabel = wxEmptyString,
                          bool installUninstallButton = true);
  ~ArduinoCoreDetailDialog();

  // helper, in case you ever want to "ShowInstalledCoreInfo"
  static void ShowCoreInfo(wxWindow *parent,
                           const ArduinoCoreInfo &info,
                           wxConfigBase *config);

private:
  wxConfigBase *m_config = nullptr;
};
