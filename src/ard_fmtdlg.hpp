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

#include <wx/dialog.h>
#include <wx/string.h>

// Some settings from clang-format dialog.
class ArduinoClangFormatSettingsDialog : public wxDialog {
public:
  // overridesJsonIn: JSON object as string. Example:
  // {"SortIncludes":false,"AllowShortBlocksOnASingleLine":"Empty"}
  ArduinoClangFormatSettingsDialog(wxWindow *parent,
                                   const wxString &overridesJsonIn);

  // Resulting JSON (only values that differ from defaults).
  wxString GetOverridesJson() const { return m_overridesJsonOut; }

private:
  void BuildUi();
  void BuildGridFromMeta();
  void LoadFromJson();
  void SaveToJson();
  void ApplyFilter(const wxString &filterLower);

  void OnSearchChanged(wxCommandEvent &e);
  void OnReset(wxCommandEvent &e);
  void OnOk(wxCommandEvent &e);

private:
  wxString m_overridesJsonIn;
  wxString m_overridesJsonOut;

  class wxPropertyGrid *m_pg = nullptr;
  class wxSearchCtrl *m_search = nullptr;
};
