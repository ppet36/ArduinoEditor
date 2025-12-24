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
