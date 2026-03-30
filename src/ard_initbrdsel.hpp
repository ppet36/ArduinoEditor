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
