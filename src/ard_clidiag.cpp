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

#include "ard_clidiag.hpp"

#include "ard_cc.hpp"
#include "utils.hpp"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>

ArduinoCliDiagnosticsDialog::ArduinoCliDiagnosticsDialog(wxWindow *parent,
                                                         wxConfigBase *config,
                                                         wxWindowID id,
                                                         const wxString &title,
                                                         const wxPoint &pos,
                                                         const wxSize &size,
                                                         long style)
    : wxDialog(parent, id, title, pos, size, style),
      m_config(config) {
  BuildUi();
  BindEvents();

  SetMinSize(wxSize(640, 250));

  if (!LoadWindowSize(wxT("ArduinoCliDiagnosticsDialog"), this, m_config)) {
    CentreOnParent();
  }
}

void ArduinoCliDiagnosticsDialog::BuildUi() {
  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  // Diagnostics view fills almost entire dialog.
  m_diagView = new ArduinoDiagnosticsView(this, m_config);
  topSizer->Add(m_diagView, 1, wxEXPAND | wxALL, 8);

  // Bottom button row
  auto *btnSizer = new wxStdDialogButtonSizer();

  m_btnClose = new wxButton(this, wxID_CLOSE, _("Close"));
  btnSizer->AddButton(m_btnClose);
  btnSizer->Realize();

  topSizer->Add(btnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  SetSizer(topSizer);
  Layout();

  // ESC closes (nice for sanity).
  SetEscapeId(wxID_CLOSE);
}

void ArduinoCliDiagnosticsDialog::BindEvents() {
  Bind(wxEVT_BUTTON, &ArduinoCliDiagnosticsDialog::OnCloseButton, this, wxID_CLOSE);
  Bind(wxEVT_CLOSE_WINDOW, &ArduinoCliDiagnosticsDialog::OnCloseWindow, this);

  // Forward these two events to parent unchanged.
  Bind(EVT_ARD_DIAG_JUMP, &ArduinoCliDiagnosticsDialog::OnDiagJump, this);
  Bind(EVT_ARD_DIAG_SOLVE_AI, &ArduinoCliDiagnosticsDialog::OnDiagSolveAi, this);
}

void ArduinoCliDiagnosticsDialog::SetSketchRoot(std::string sketchRoot) {
  if (m_diagView) {
    m_diagView->SetSketchRoot(sketchRoot);
  }
}

void ArduinoCliDiagnosticsDialog::SetDiagnostics(const std::vector<ArduinoParseError> &errors) {
  if (m_diagView) {
    m_diagView->SetDiagnostics(errors);
  }
}

void ArduinoCliDiagnosticsDialog::OnCloseButton(wxCommandEvent &WXUNUSED(evt)) {
  SaveWindowSize(wxT("ArduinoCliDiagnosticsDialog"), this, m_config);

  Hide();
}

void ArduinoCliDiagnosticsDialog::OnCloseWindow(wxCloseEvent &evt) {
  SaveWindowSize(wxT("ArduinoCliDiagnosticsDialog"), this, m_config);

  Hide();
  evt.Veto();
}

static void ForwardEventToParent(wxWindow *self, wxEvent &evt) {
  wxWindow *parent = self ? self->GetParent() : nullptr;
  if (!parent)
    return;

  wxQueueEvent(parent->GetEventHandler(), evt.Clone());
}

void ArduinoCliDiagnosticsDialog::OnDiagJump(wxEvent &evt) {
  ForwardEventToParent(this, evt);
}

void ArduinoCliDiagnosticsDialog::OnDiagSolveAi(wxEvent &evt) {
  ForwardEventToParent(this, evt);
}
