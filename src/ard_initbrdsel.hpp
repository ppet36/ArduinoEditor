#pragma once

#include <string>
#include <vector>

#include <wx/dialog.h>

class wxChoice;
class wxButton;

class ArduinoInitialBoardSelectDialog : public wxDialog {
public:
  ArduinoInitialBoardSelectDialog(wxWindow *parent, const wxString &sketchName);

  void SetBoardHistory(const std::vector<std::string> &historyFqbns,
                       const std::string &currentFqbn = std::string());

  std::string GetSelectedFqbn() const;

private:
  void RebuildChoice();
  void UpdateUiState();

  void OnChoice(wxCommandEvent &evt);
  void OnUseSelected(wxCommandEvent &evt);
  void OnSelectManual(wxCommandEvent &evt);
  void OnContinue(wxCommandEvent &evt);
  void OnClose(wxCloseEvent &evt);

private:
  wxChoice *m_choice = nullptr;
  wxButton *m_btnUseSelected = nullptr;
  wxButton *m_btnSelectManual = nullptr;
  wxButton *m_btnContinue = nullptr;

  std::vector<std::string> m_history;
  std::string m_currentFqbn;
};
