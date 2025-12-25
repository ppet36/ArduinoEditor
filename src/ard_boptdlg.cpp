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

#include "ard_boptdlg.hpp"

ArduinoBoardOptionsDialog::ArduinoBoardOptionsDialog(wxWindow *parent,
                                                     const std::vector<ArduinoBoardOption> &options,
                                                     const wxString &title)
    : wxDialog(parent, wxID_ANY, title,
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_options(options) {
  auto *topSizer = new wxBoxSizer(wxVERTICAL);
  auto *grid = new wxFlexGridSizer(2, 5, 10); // 2 columns, spaces

  grid->AddGrowableCol(1, 1);

  for (const auto &opt : m_options) {
    // Label
    wxString label = wxString::FromUTF8(opt.label.c_str());
    grid->Add(new wxStaticText(this, wxID_ANY, label),
              0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

    // Choice
    wxChoice *choice = new wxChoice(this, wxID_ANY);
    int selectedIndex = -1;
    for (size_t i = 0; i < opt.values.size(); ++i) {
      const auto &v = opt.values[i];

      wxString text;
      if (!v.label.empty()) {
        text = wxString::FromUTF8(v.label.c_str());
      } else if (!v.id.empty()) {
        // if we don't have a label, at least display the option ID
        text = wxString::FromUTF8(v.id.c_str());
      } else {
        text = _("<unknown>");
      }

      choice->Append(text);

      if (v.selected && selectedIndex == -1) {
        selectedIndex = static_cast<int>(i);
      }
    }

    if (selectedIndex >= 0) {
      choice->SetSelection(selectedIndex);
    } else if (!opt.values.empty()) {
      choice->SetSelection(0);
    }

    m_choices.push_back(choice);
    grid->Add(choice, 1, wxEXPAND);
  }

  topSizer->Add(grid, 1, wxALL | wxEXPAND, 10);

  // OK/Cancel
  auto *btnSizer = new wxStdDialogButtonSizer();
  btnSizer->AddButton(new wxButton(this, wxID_OK, _("OK")));
  btnSizer->AddButton(new wxButton(this, wxID_CANCEL, _("Cancel")));
  btnSizer->Realize();

  topSizer->Add(btnSizer, 0, wxALL | wxALIGN_RIGHT, 10);

  Bind(wxEVT_BUTTON, &ArduinoBoardOptionsDialog::OnOk, this, wxID_OK);

  SetSizerAndFit(topSizer);
  CentreOnParent();
}

void ArduinoBoardOptionsDialog::OnOk(wxCommandEvent &WXUNUSED(event)) {
  // Reflect from wxChoice back into m_options
  for (size_t i = 0; i < m_options.size() && i < m_choices.size(); ++i) {
    int sel = m_choices[i]->GetSelection();
    if (sel < 0 || sel >= static_cast<int>(m_options[i].values.size())) {
      continue;
    }

    for (auto &v : m_options[i].values) {
      v.selected = false;
    }
    m_options[i].values[sel].selected = true;
  }

  EndModal(wxID_OK);
}
