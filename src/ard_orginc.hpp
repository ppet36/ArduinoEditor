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

#include <wx/checkbox.h>
#include <wx/config.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>

#include "ard_cc.hpp"
#include "ard_edit.hpp"

struct OrgIncludeItem {
  IncludeUsage usage;
  wxString displayText; // original include line (for display)
  bool remove = true;   // true = delete, false = leave
};

class ArduinoEditor;

class ArduinoRefactoringOrgIncludes : public wxDialog {
public:
  ArduinoRefactoringOrgIncludes(ArduinoEditor *editor, const std::vector<OrgIncludeItem> &items, wxConfigBase *config);
  ~ArduinoRefactoringOrgIncludes();

  bool GetSortIncludes() const;
  void GetIncludesToRemove(std::vector<IncludeUsage> &out) const;

private:
  void OnItemActivated(wxListEvent &evt);
  void OnCheckChanged(wxListEvent &evt);

  wxListCtrl *m_list = nullptr;
  wxCheckBox *m_sortCheck = nullptr;

  wxConfigBase *m_config;

  std::vector<OrgIncludeItem> m_items;
};
