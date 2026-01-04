#include "ard_initbrdsel.hpp"
#include "utils.hpp"
#include <algorithm>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

ArduinoInitialBoardSelectDialog::ArduinoInitialBoardSelectDialog(wxWindow *parent, const wxString &sketchName)
    : wxDialog(parent, wxID_ANY, _("Select development board"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {

  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  auto *msg = new wxStaticText(
      this, wxID_ANY, wxString::Format(_("No development board is configured for this sketch.\n\n"
                                         "Sketch: %s\n\n"
                                         "You can select one from your recently used boards, or open the full board selector."),
                                       sketchName));
  msg->Wrap(520);
  topSizer->Add(msg, 0, wxALL | wxEXPAND, 12);

  auto *label = new wxStaticText(this, wxID_ANY, _("Recently used boards:"));
  topSizer->Add(label, 0, wxLEFT | wxRIGHT | wxTOP, 12);

  m_choice = new wxChoice(this, wxID_ANY);
  topSizer->Add(m_choice, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 12);

  auto *btnSizer = new wxBoxSizer(wxHORIZONTAL);

  m_btnUseSelected = new wxButton(this, wxID_OK, _("Use selected"));
  m_btnSelectManual = new wxButton(this, wxID_YES, _("Select board..."));
  m_btnContinue = new wxButton(this, wxID_NO, _("Continue without board"));

  btnSizer->Add(m_btnUseSelected, 0, wxRIGHT, 8);
  btnSizer->Add(m_btnSelectManual, 0, wxRIGHT, 8);
  btnSizer->AddStretchSpacer(1);
  btnSizer->Add(m_btnContinue, 0);

  topSizer->Add(btnSizer, 0, wxALL | wxEXPAND, 12);

  SetSizerAndFit(topSizer);
  SetMinSize(wxSize(560, -1));

  // ---- Bind handlers (no event table) ----
  // Choice
  m_choice->Bind(wxEVT_CHOICE, &ArduinoInitialBoardSelectDialog::OnChoice, this);

  // Buttons
  m_btnUseSelected->Bind(wxEVT_BUTTON, &ArduinoInitialBoardSelectDialog::OnUseSelected, this);
  m_btnSelectManual->Bind(wxEVT_BUTTON, &ArduinoInitialBoardSelectDialog::OnSelectManual, this);
  m_btnContinue->Bind(wxEVT_BUTTON, &ArduinoInitialBoardSelectDialog::OnContinue, this);

  // Close (X button)
  Bind(wxEVT_CLOSE_WINDOW, &ArduinoInitialBoardSelectDialog::OnClose, this);

  UpdateUiState();

  CentreOnParent();
}

void ArduinoInitialBoardSelectDialog::SetBoardHistory(
    const std::vector<std::string> &historyFqbns,
    const std::string &currentFqbn) {

  m_currentFqbn = currentFqbn;
  m_history.clear();
  m_history.reserve(historyFqbns.size());

  for (const auto &fqbn : historyFqbns) {
    if (fqbn.empty())
      continue;
    if (std::find(m_history.begin(), m_history.end(), fqbn) != m_history.end())
      continue;
    m_history.push_back(fqbn);
  }

  RebuildChoice();
  UpdateUiState();
}

void ArduinoInitialBoardSelectDialog::RebuildChoice() {
  if (!m_choice)
    return;

  m_choice->Freeze();
  m_choice->Clear();

  for (const auto &fqbn : m_history) {
    m_choice->Append(wxString::FromUTF8(fqbn));
  }

  if (!m_history.empty()) {
    int sel = 0;
    if (!m_currentFqbn.empty()) {
      for (size_t i = 0; i < m_history.size(); i++) {
        if (m_history[i] == m_currentFqbn) {
          sel = (int)i;
          break;
        }
      }
    }
    m_choice->SetSelection(sel);
  }

  m_choice->Thaw();
}

void ArduinoInitialBoardSelectDialog::UpdateUiState() {
  const bool hasHistory = (m_choice && m_choice->GetCount() > 0);
  const bool hasSelection = hasHistory && (m_choice->GetSelection() >= 0);

  if (m_choice)
    m_choice->Enable(hasHistory);

  if (m_btnUseSelected)
    m_btnUseSelected->Enable(hasSelection);

  if (!hasHistory) {
    if (m_btnSelectManual)
      m_btnSelectManual->SetDefault();
  } else {
    if (m_btnUseSelected)
      m_btnUseSelected->SetDefault();
  }
}

std::string ArduinoInitialBoardSelectDialog::GetSelectedFqbn() const {
  if (!m_choice)
    return std::string();

  int sel = m_choice->GetSelection();
  if (sel < 0)
    return std::string();

  wxString s = m_choice->GetString((unsigned)sel);
  return wxToStd(s);
}

void ArduinoInitialBoardSelectDialog::OnChoice(wxCommandEvent &WXUNUSED(evt)) {
  UpdateUiState();
}

void ArduinoInitialBoardSelectDialog::OnUseSelected(wxCommandEvent &WXUNUSED(evt)) {
  EndModal(wxID_OK);
}

void ArduinoInitialBoardSelectDialog::OnSelectManual(wxCommandEvent &WXUNUSED(evt)) {
  EndModal(wxID_YES);
}

void ArduinoInitialBoardSelectDialog::OnContinue(wxCommandEvent &WXUNUSED(evt)) {
  EndModal(wxID_NO);
}

void ArduinoInitialBoardSelectDialog::OnClose(wxCloseEvent &WXUNUSED(evt)) {
  EndModal(wxID_NO);
}
