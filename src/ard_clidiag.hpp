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

#include <wx/button.h>
#include <wx/config.h>
#include <wx/dialog.h>
#include <wx/event.h>

#include <string>
#include <vector>

#include "ard_diagview.hpp"

struct ArduinoParseError;

class ArduinoCliDiagnosticsDialog : public wxDialog {
public:
  ArduinoCliDiagnosticsDialog(wxWindow *parent,
                              wxConfigBase *config,
                              wxWindowID id = wxID_ANY,
                              const wxString &title = _("arduino-cli build diagnostics"),
                              const wxPoint &pos = wxDefaultPosition,
                              const wxSize &size = wxSize(900, 520),
                              long style = wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

  void SetSketchRoot(std::string sketchRoot);
  void SetDiagnostics(const std::vector<ArduinoParseError> &errors);

private:
  void BuildUi();
  void BindEvents();

  void OnCloseButton(wxCommandEvent &evt);
  void OnCloseWindow(wxCloseEvent &evt);

  void OnDiagJump(wxEvent &evt);
  void OnDiagSolveAi(wxEvent &evt);

private:
  wxConfigBase *m_config;
  ArduinoDiagnosticsView *m_diagView = nullptr;
  wxButton *m_btnClose = nullptr;

  std::string m_sketchRoot;
};
