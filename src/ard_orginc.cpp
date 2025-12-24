#include "ard_orginc.hpp"
#include "ard_edit.hpp"
#include "utils.hpp"
#include <wx/button.h>
#include <wx/intl.h>
#include <wx/stattext.h>

ArduinoRefactoringOrgIncludes::ArduinoRefactoringOrgIncludes(
    ArduinoEditor *editor,
    const std::vector<OrgIncludeItem> &items,
    wxConfigBase *config)
    : wxDialog(editor, wxID_ANY,
               _("Organize includes"),
               wxDefaultPosition,
               wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_config(config),
      m_items(items) {

  wxBoxSizer *topSizer = new wxBoxSizer(wxVERTICAL);

  wxStaticText *info = new wxStaticText(
      this,
      wxID_ANY,
      _("The following #include directives are considered unused and are selected for removal.\n"
        "Uncheck any includes you want to KEEP. Includes that remain unchecked will stay unchanged."));

  topSizer->Add(info, 0, wxALL | wxEXPAND, 8);

  long listStyle = wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES;
  m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(600, 300), listStyle);

#if wxCHECK_VERSION(3, 1, 3)
  m_list->EnableCheckBoxes(true);
#endif

  m_list->InsertColumn(0, _("Remove"));
  m_list->InsertColumn(1, _("Include"));
  m_list->InsertColumn(2, _("Line"));
  m_list->InsertColumn(3, _("Header file"));

  for (size_t i = 0; i < m_items.size(); ++i) {
    const auto &it = m_items[i];

    long idx = m_list->InsertItem((long)i, wxEmptyString);

#if wxCHECK_VERSION(3, 1, 3)
    m_list->CheckItem(idx, it.remove);
#endif

    wxString usedStr = it.usage.used ? _("(used)") : _("(unused)");

    m_list->SetItem(idx, 1, it.displayText);
    m_list->SetItem(idx, 2, wxString::Format(wxT("%u %s"), it.usage.line, usedStr));
    m_list->SetItem(idx, 3, wxString::FromUTF8(it.usage.includedFile));
  }

  m_list->SetColumnWidth(0, wxLIST_AUTOSIZE_USEHEADER);
  m_list->SetColumnWidth(1, wxLIST_AUTOSIZE_USEHEADER);
  m_list->SetColumnWidth(2, wxLIST_AUTOSIZE_USEHEADER);
  m_list->SetColumnWidth(3, wxLIST_AUTOSIZE_USEHEADER);

  topSizer->Add(m_list, 1, wxALL | wxEXPAND, 8);

  m_sortCheck = new wxCheckBox(this, wxID_ANY, _("Sort #include directives"));
  m_sortCheck->SetValue(true);
  m_sortCheck->SetToolTip(_(
      "Sorts includes so that system headers come first, followed by user headers.\n"
      "Within each group, items are ordered alphabetically."));

  topSizer->Add(m_sortCheck, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

  wxStdDialogButtonSizer *btnSizer = new wxStdDialogButtonSizer();
  wxButton *ok = new wxButton(this, wxID_OK);
  wxButton *cancel = new wxButton(this, wxID_CANCEL);
  btnSizer->AddButton(ok);
  btnSizer->AddButton(cancel);
  btnSizer->Realize();

  topSizer->Add(btnSizer, 0, wxALL | wxALIGN_RIGHT, 8);

  SetSizer(topSizer);
  topSizer->SetSizeHints(this);
  SetMinSize(wxSize(600, 350));
  Layout();

  if (!LoadWindowSize(wxT("OrgIncludesDialog"), this, m_config)) {
    CentreOnParent();
  }

  Bind(wxEVT_LIST_ITEM_ACTIVATED, &ArduinoRefactoringOrgIncludes::OnItemActivated, this);
  Bind(wxEVT_LIST_ITEM_CHECKED, &ArduinoRefactoringOrgIncludes::OnCheckChanged, this);
  Bind(wxEVT_LIST_ITEM_UNCHECKED, &ArduinoRefactoringOrgIncludes::OnCheckChanged, this);
}

ArduinoRefactoringOrgIncludes::~ArduinoRefactoringOrgIncludes() {
  SaveWindowSize(wxT("OrgIncludesDialog"), this, m_config);
}

bool ArduinoRefactoringOrgIncludes::GetSortIncludes() const {
  return m_sortCheck && m_sortCheck->GetValue();
}

void ArduinoRefactoringOrgIncludes::GetIncludesToRemove(std::vector<IncludeUsage> &out) const {
  out.clear();

  for (size_t i = 0; i < m_items.size(); ++i) {
    const auto &it = m_items[i];

    // We can easily allow deleting "used" includes, but typically the user will only leave unused ones checked.
    bool remove = it.remove;

    long idx = (long)i;
    remove = m_list->IsItemChecked(idx);

    if (remove) {
      out.push_back(it.usage);
    }
  }
}

void ArduinoRefactoringOrgIncludes::OnItemActivated(wxListEvent &evt) {
  long idx = evt.GetIndex();
  if (idx < 0 || (size_t)idx >= m_items.size())
    return;

  bool cur = m_list->IsItemChecked(idx);
  m_list->CheckItem(idx, !cur);
}

void ArduinoRefactoringOrgIncludes::OnCheckChanged(wxListEvent &evt) {
  long idx = evt.GetIndex();
  if (idx < 0 || (size_t)idx >= m_items.size())
    return;

  bool checked = m_list->IsItemChecked(idx);
  m_items[idx].remove = checked;
}
