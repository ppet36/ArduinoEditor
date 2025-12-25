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

#include "ard_pop.hpp"
#include "ard_mdwidget.hpp"
#include "utils.hpp"
#include <wx/wx.h>

AiExplainPopup::AiExplainPopup(wxWindow *parent,
                               wxConfigBase *config,
                               const wxString &text)
    : wxDialog(parent,
               wxID_ANY,
               _("AI explanation"),
               wxDefaultPosition,
               wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_config(config) {
  SetMinSize(wxSize(320, 200));

  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  auto *mdPanel = new ArduinoMarkdownPanel(this);
  mdPanel->AppendMarkdown(text, AiMarkdownRole::Assistant);

  topSizer->Add(mdPanel, 1, wxEXPAND | wxALL, 6);

  auto *btnSizer = CreateStdDialogButtonSizer(wxOK);
  if (btnSizer) {
    topSizer->Add(btnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
  }

  SetSizer(topSizer);
  topSizer->SetSizeHints(this);

  if (!LoadWindowSize(wxT("ArduinoPopDlg"), this, m_config)) {
    SetSize(700, 600);
    CentreOnParent();
  }

  // ESC closes dialog
  Bind(wxEVT_CHAR_HOOK, &AiExplainPopup::OnCharHook, this);
}

AiExplainPopup::~AiExplainPopup() {
  SaveWindowSize(wxT("ArduinoPopDlg"), this, m_config);
}

void AiExplainPopup::OnCharHook(wxKeyEvent &evt) {
  if (evt.GetKeyCode() == WXK_ESCAPE) {
    if (IsModal())
      EndModal(wxID_CANCEL);
    else
      Close();
  } else {
    evt.Skip();
  }
}
