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
#include <wx/config.h>
#include <wx/listctrl.h>
#include <wx/wx.h>

struct JumpTarget;

class ArduinoRenameSymbolDialog : public wxDialog {
public:
  ArduinoRenameSymbolDialog(wxWindow *parent,
                            wxConfigBase *config,
                            const wxString &oldName,
                            const std::vector<JumpTarget> &occurrences,
                            const std::string &sketchRoot);
  ~ArduinoRenameSymbolDialog();

  wxString GetNewName() const;

  void GetSelectedOccurrences(std::vector<JumpTarget> &out) const;

private:
  void InitLayout();
  void PopulateList(const std::vector<JumpTarget> &occurrences,
                    const std::string &sketchRoot);

  void OnTextEnter(wxCommandEvent &evt);
  void OnListItemActivated(wxListEvent &evt);

  wxConfigBase *m_config = nullptr;
  wxTextCtrl *m_newNameCtrl = nullptr;
  wxListCtrl *m_listCtrl = nullptr;
  wxButton *m_okButton = nullptr;

  std::vector<JumpTarget> m_occurrences;
};
