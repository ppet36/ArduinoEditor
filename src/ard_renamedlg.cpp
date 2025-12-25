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

#include "ard_renamedlg.hpp"
#include "ard_cc.hpp"
#include "utils.hpp"
#include <fstream>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>

// Helper function - reads a line from a file and returns a short preview
// We will assume that the files are saved, which may not be true :)
static wxString ReadLinePreview(const std::string &file, int line) {
  if (line <= 0) {
    return wxEmptyString;
  }

  std::ifstream ifs(file);
  if (!ifs.is_open()) {
    return wxEmptyString;
  }

  std::string txt;
  int current = 1;
  while (current < line && std::getline(ifs, txt)) {
    ++current;
  }

  if (current == line && std::getline(ifs, txt)) {
    // nothing, we read too much - a little hack: when the loop ended at current=line,
    // getline has already been executed; for simplicity, let's leave it like this:
  } else if (current == line && !txt.empty()) {
    // the row was already loaded in the previous step
  } else if (current != line) {
    return wxEmptyString;
  }

  // trim whitespace and truncate
  wxString wxTxt = wxString::FromUTF8(txt.c_str());
  wxTxt.Trim(true).Trim(false);
  if (wxTxt.Length() > 120) {
    wxTxt = wxTxt.Mid(0, 117) + wxT("...");
  }
  return wxTxt;
}

ArduinoRenameSymbolDialog::ArduinoRenameSymbolDialog(wxWindow *parent,
                                                     wxConfigBase *config,
                                                     const wxString &oldName,
                                                     const std::vector<JumpTarget> &occurrences,
                                                     const std::string &sketchRoot)
    : wxDialog(parent,
               wxID_ANY,
               _("Rename symbol"),
               wxDefaultPosition,
               wxSize(700, 450),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_config(config) {
  InitLayout();

  if (m_newNameCtrl) {
    m_newNameCtrl->SetValue(oldName);
    m_newNameCtrl->SelectAll();
    m_newNameCtrl->SetFocus();
  }

  PopulateList(occurrences, sketchRoot);

  if (m_okButton) {
    m_okButton->SetDefault();
  }

  // Enter in text = OK (if not empty)
  if (m_newNameCtrl) {
    m_newNameCtrl->Bind(wxEVT_TEXT_ENTER,
                        &ArduinoRenameSymbolDialog::OnTextEnter,
                        this);
  }

  // Double-click on an item in the list = OK (if not empty)
  if (m_listCtrl) {
    m_listCtrl->Bind(wxEVT_LIST_ITEM_ACTIVATED,
                     &ArduinoRenameSymbolDialog::OnListItemActivated,
                     this);
  }

  SetMinSize(wxSize(600, 350));
  Layout();

  if (!LoadWindowSize(wxT("RenameSymbolDlg"), this, m_config)) {
    CentreOnParent();
  }
}

ArduinoRenameSymbolDialog::~ArduinoRenameSymbolDialog() {
  SaveWindowSize(wxT("RenameSymbolDlg"), this, m_config);
}

void ArduinoRenameSymbolDialog::InitLayout() {
  auto *mainSizer = new wxBoxSizer(wxVERTICAL);

  // --- Top line: label + text box ---
  auto *nameSizer = new wxBoxSizer(wxHORIZONTAL);
  auto *label = new wxStaticText(this, wxID_ANY, _("New name:"));
  nameSizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

  m_newNameCtrl = new wxTextCtrl(
      this,
      wxID_ANY,
      wxEmptyString,
      wxDefaultPosition,
      wxDefaultSize,
      wxTE_PROCESS_ENTER);

  nameSizer->Add(m_newNameCtrl, 1, wxALIGN_CENTER_VERTICAL);

  mainSizer->Add(nameSizer, 0, wxEXPAND | wxALL, 8);

  // --- List of occurrences ---
  m_listCtrl = new wxListCtrl(
      this,
      wxID_ANY,
      wxDefaultPosition,
      wxDefaultSize,
      wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);

  m_listCtrl->EnableCheckBoxes(true);

  m_listCtrl->InsertColumn(0, _("File"));
  m_listCtrl->InsertColumn(1, _("Line"));
  m_listCtrl->InsertColumn(2, _("Column"));
  m_listCtrl->InsertColumn(3, _("Preview"));

  mainSizer->Add(m_listCtrl, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  // --- OK / Cancel buttons ---
  auto *btnSizer = new wxStdDialogButtonSizer();

  m_okButton = new wxButton(this, wxID_OK);
  auto *cancelButton = new wxButton(this, wxID_CANCEL);

  btnSizer->AddButton(m_okButton);
  btnSizer->AddButton(cancelButton);
  btnSizer->Realize();

  mainSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 8);

  SetSizer(mainSizer);
}

void ArduinoRenameSymbolDialog::PopulateList(const std::vector<JumpTarget> &occurrences,
                                             const std::string &sketchRoot) {
  if (!m_listCtrl) {
    return;
  }

  // save them for later filtering
  m_occurrences = occurrences;

  m_listCtrl->Freeze();
  m_listCtrl->DeleteAllItems();

  // normalize the root to cut off the prefix
  std::string normRoot = sketchRoot;
  if (!normRoot.empty() &&
      normRoot.back() != '/' &&
      normRoot.back() != '\\') {
    normRoot.push_back('/');
  }

  long row = 0;
  for (const auto &jt : occurrences) {
    std::string file = jt.file;
    wxString fileWx;

    if (!normRoot.empty() &&
        file.rfind(normRoot, 0) == 0) {
      std::string rel = file.substr(normRoot.size());
      fileWx = wxString::FromUTF8(rel.c_str());
    } else {
      fileWx = wxString::FromUTF8(file.c_str());
    }

    wxString lineWx = wxString::Format(wxT("%d"), jt.line);
    wxString colWx = wxString::Format(wxT("%d"), jt.column);
    wxString prevWx = ReadLinePreview(file, jt.line);

    long idx = m_listCtrl->InsertItem(row, fileWx);
    m_listCtrl->SetItem(idx, 1, lineWx);
    m_listCtrl->SetItem(idx, 2, colWx);
    m_listCtrl->SetItem(idx, 3, prevWx);

    // implicitly check everything
    if (m_listCtrl->HasCheckBoxes()) {
      m_listCtrl->CheckItem(idx, true);
    }

    ++row;
  }

  m_listCtrl->SetColumnWidth(0, 220);
  m_listCtrl->SetColumnWidth(1, wxLIST_AUTOSIZE_USEHEADER);
  m_listCtrl->SetColumnWidth(2, wxLIST_AUTOSIZE_USEHEADER);
  m_listCtrl->SetColumnWidth(3, wxLIST_AUTOSIZE);

  m_listCtrl->Thaw();
}

wxString ArduinoRenameSymbolDialog::GetNewName() const {
  if (!m_newNameCtrl) {
    return wxEmptyString;
  }
  wxString val = m_newNameCtrl->GetValue();
  wxString copy = val;
  copy.Trim(true).Trim(false);
  return copy;
}

void ArduinoRenameSymbolDialog::GetSelectedOccurrences(std::vector<JumpTarget> &out) const {
  out.clear();

  if (!m_listCtrl) {
    return;
  }

  const long count = m_listCtrl->GetItemCount();
  if (count <= 0) {
    return;
  }

  // safety - the number of rows must fit
  if (m_occurrences.size() != static_cast<size_t>(count)) {
    out = m_occurrences;
    return;
  }

  // if checkbox feature is not available, behave as "all selected"
  if (!m_listCtrl->HasCheckBoxes()) {
    out = m_occurrences;
    return;
  }

  for (long i = 0; i < count; ++i) {
    if (m_listCtrl->IsItemChecked(i)) {
      out.push_back(m_occurrences[static_cast<size_t>(i)]);
    }
  }
}

void ArduinoRenameSymbolDialog::OnTextEnter(wxCommandEvent &WXUNUSED(evt)) {
  wxString name = GetNewName();
  if (!name.IsEmpty()) {
    EndModal(wxID_OK);
  }
}

void ArduinoRenameSymbolDialog::OnListItemActivated(wxListEvent &evt) {
  if (!m_listCtrl || !m_listCtrl->HasCheckBoxes()) {
    return;
  }

  long idx = evt.GetIndex();
  if (idx < 0 || idx >= m_listCtrl->GetItemCount()) {
    return;
  }

  bool checked = m_listCtrl->IsItemChecked(idx);
  m_listCtrl->CheckItem(idx, !checked);
}
