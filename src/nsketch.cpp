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

#include "nsketch.hpp"
#include "ard_setdlg.hpp"
#include "utils.hpp"
#include <wx/dirdlg.h>
#include <wx/filename.h>

NewSketchDialog::NewSketchDialog(wxWindow *parent, wxConfigBase *config, const wxString &initialDir)
    : wxDialog(parent, wxID_ANY, _("New sketch"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_dir(initialDir) {

  wxBoxSizer *topSizer = new wxBoxSizer(wxVERTICAL);

  auto *grid = new wxFlexGridSizer(2, 2, 5, 5);
  grid->AddGrowableCol(1, 1);

  // Sketch name
  grid->Add(new wxStaticText(this, wxID_ANY, _("Sketch name:")),
            0, wxALIGN_CENTER_VERTICAL);

  m_nameCtrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                              wxDefaultPosition,
                              wxDefaultSize,
                              wxTE_PROCESS_ENTER);

  m_nameCtrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) {
    if (auto *okBtn = wxDynamicCast(FindWindow(wxID_OK), wxButton)) {
      if (okBtn->IsEnabled()) {
        wxCommandEvent evt(wxEVT_BUTTON, wxID_OK);
        evt.SetEventObject(okBtn);
        okBtn->GetEventHandler()->ProcessEvent(evt);
      } else {
        wxBell(); // when OK is disabled
      }
    }
  });

  grid->Add(m_nameCtrl, 1, wxEXPAND);

  // Location + button
  grid->Add(new wxStaticText(this, wxID_ANY, _("Location:")),
            0, wxALIGN_CENTER_VERTICAL);

  wxBoxSizer *locSizer = new wxBoxSizer(wxHORIZONTAL);
  m_dirText = new wxStaticText(this, wxID_ANY, m_dir);

  wxButton *browseBtn = new wxButton(this, wxID_ANY, _("Change"));

  locSizer->Add(m_dirText, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
  locSizer->Add(browseBtn, 0, wxALIGN_CENTER_VERTICAL);

  grid->Add(locSizer, 1, wxEXPAND);

  topSizer->Add(grid, 0, wxALL | wxEXPAND, 10);

  // Error label under the form
  m_errorText = new wxStaticText(this, wxID_ANY, wxEmptyString);

  EditorSettings settings;
  settings.Load(config);
  EditorColorScheme colors = settings.GetColors();

  m_errorText->SetForegroundColour(colors.error);
  topSizer->Add(m_errorText, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  // OK / Cancel
  topSizer->Add(CreateLocalizedSeparatedOkCancelSizer(this),
                0, wxALL | wxEXPAND, 10);

  SetSizerAndFit(topSizer);

  // Handlers
  browseBtn->Bind(wxEVT_BUTTON, &NewSketchDialog::OnBrowse, this);
  m_nameCtrl->Bind(wxEVT_TEXT, &NewSketchDialog::OnNameChanged, this);

  // Enter = OK
  m_nameCtrl->SetFocus();
  m_nameCtrl->SetSelection(-1, -1);

  if (auto *okBtn = wxDynamicCast(FindWindow(wxID_OK), wxButton)) {
    okBtn->SetDefault();
    SetDefaultItem(okBtn);
  }

  // Initialization validation (empty name => OK disabled)
  UpdateValidation();
}

void NewSketchDialog::OnBrowse(wxCommandEvent &) {
  wxDirDialog dlg(
      this,
      _("Select sketch directory"),
      m_dir,
      wxDD_DIR_MUST_EXIST | wxDD_NEW_DIR_BUTTON);

  if (dlg.ShowModal() == wxID_OK) {
    m_dir = dlg.GetPath();
    m_dirText->SetLabel(m_dir);
    Layout();
    Fit();

    UpdateValidation(); // recalculate validity for the current name
  }
}

void NewSketchDialog::OnNameChanged(wxCommandEvent &) {
  UpdateValidation();
}

void NewSketchDialog::UpdateValidation() {
  wxString name = m_nameCtrl->GetValue();
  name.Trim(true).Trim(false);

  const bool hasInput = !name.empty();
  bool valid = false;
  wxString errorMsg;

  if (!hasInput) {
    // Nothing written -> no error, just OK forbidden
    valid = false;
    errorMsg.clear();
  } else {
    // We have some name -> let's try to build the full path and verify the existence of the directory
    wxFileName sketchDir(m_dir, name);
    wxString fullPath = sketchDir.GetFullPath();

    if (wxDirExists(fullPath)) {
      errorMsg = wxString::Format(_("Directory '%s' already exists."), fullPath);
      valid = false;
    } else {
      valid = true;
    }
  }

  // Error message below the form
  if (m_errorText) {
    m_errorText->SetLabel(errorMsg);
    m_errorText->Wrap(GetClientSize().GetWidth());
  }

  // Enable / disable OK
  if (wxWindow *okWnd = FindWindow(wxID_OK)) {
    if (auto *okBtn = wxDynamicCast(okWnd, wxButton)) {
      okBtn->Enable(valid);
    }
  }

  Layout();
}
