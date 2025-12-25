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

// arduino_board_options_dlg.hpp
#pragma once

#include "ard_cli.hpp"
#include <wx/choice.h>
#include <wx/wx.h>

class ArduinoBoardOptionsDialog : public wxDialog {
public:
  ArduinoBoardOptionsDialog(wxWindow *parent,
                            const std::vector<ArduinoBoardOption> &options,
                            const wxString &title = _("Board options"));

  const std::vector<ArduinoBoardOption> &GetOptions() const { return m_options; }

private:
  void OnOk(wxCommandEvent &);

  std::vector<ArduinoBoardOption> m_options;
  std::vector<wxChoice *> m_choices;
};
