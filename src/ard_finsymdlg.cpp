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

#include "ard_finsymdlg.hpp"
#include "utils.hpp"

#include <wx/sizer.h>

wxDEFINE_EVENT(EVT_ARD_SYMBOL_ACTIVATED, ArduinoSymbolActivatedEvent);

FindSymbolDialog::FindSymbolDialog(wxWindow *parent,
                                   wxConfigBase *config,
                                   const std::vector<SymbolInfo> &symbols)
    : wxDialog(parent,
               wxID_ANY,
               _("Find symbol"),
               wxDefaultPosition,
               wxSize(700, 400),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_config(config),
      m_allSymbols(symbols),
      m_searchTimer(this) {
  wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

  m_search = new wxTextCtrl(this, wxID_ANY);
  sizer->Add(m_search, 0, wxEXPAND | wxALL, 5);

  m_list = new wxListCtrl(this, wxID_ANY,
                          wxDefaultPosition, wxDefaultSize,
                          wxLC_REPORT | wxLC_SINGLE_SEL);
  m_list->InsertColumn(0, _("Name"), wxLIST_FORMAT_LEFT, 220);
  m_list->InsertColumn(1, _("File"), wxLIST_FORMAT_LEFT, 300);
  m_list->InsertColumn(2, _("Line"), wxLIST_FORMAT_RIGHT, 60);
  m_list->InsertColumn(3, _("Path"), wxLIST_FORMAT_LEFT, 250);

  sizer->Add(m_list, 1, wxEXPAND | wxALL, 5);

  SetSizerAndFit(sizer);

  m_search->Bind(wxEVT_TEXT, &FindSymbolDialog::OnSearchChanged, this);
  m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &FindSymbolDialog::OnItemActivated, this);
  Bind(wxEVT_CLOSE_WINDOW, &FindSymbolDialog::OnClose, this);
  Bind(wxEVT_CHAR_HOOK, &FindSymbolDialog::OnCharHook, this);
  Bind(wxEVT_TIMER, &FindSymbolDialog::OnSearchTimer, this);
  Bind(wxEVT_SHOW, &FindSymbolDialog::OnShow, this);

  m_filteredSymbols.clear();
  RebuildList();

  // Restore size/position from config
  if (!LoadWindowSize(wxT("FindSymbolDialog"), this, m_config)) {
    Centre();
  }
}

void FindSymbolDialog::OnSearchChanged(wxCommandEvent &WXUNUSED(event)) {
  // restart debounce timer
  m_searchTimer.Stop();
  m_searchTimer.Start(500, true); // 500 ms, one-shot
}

void FindSymbolDialog::OnSearchTimer(wxTimerEvent &WXUNUSED(event)) {
  ApplyFilterAndRebuild();
}

bool FindSymbolDialog::GetSelectedSymbol(SymbolInfo &out) const {
  long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (sel == -1 || sel >= static_cast<long>(m_filteredSymbols.size()))
    return false;

  out = m_filteredSymbols[sel];
  return true;
}

void FindSymbolDialog::SetSymbols(const std::vector<SymbolInfo> &symbols) {
  m_allSymbols = symbols;
  ApplyFilterAndRebuild();
}

void FindSymbolDialog::ApplyFilterAndRebuild() {
  wxString query = m_search->GetValue().Lower();

  m_filteredSymbols.clear();

  // empty query -> empty list
  if (query.Length() < 1) {
    RebuildList();
    return;
  }

  for (const auto &s : m_allSymbols) {
    wxString name = wxString::FromUTF8(s.name.c_str()).Lower();
    if (name.Contains(query)) {
      m_filteredSymbols.push_back(s);
    }
  }

  RebuildList();
}

void FindSymbolDialog::OnCharHook(wxKeyEvent &event) {
  const int key = event.GetKeyCode();

  if (key == WXK_ESCAPE) {
    // Close() -> calls OnClose, where the geometry is saved and the window is hidden
    Close();
    return;
  }

  // When typing in the search box:
  // Down or Tab moves focus to the list (and selects the first item).
  if (wxWindow::FindFocus() == m_search && (key == WXK_DOWN || key == WXK_TAB)) {
    const long count = m_list->GetItemCount();
    if (count > 0) {
      // ensure some selection exists
      long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
      if (sel < 0) {
        sel = 0;
        m_list->SetItemState(sel, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
      }
      m_list->EnsureVisible(sel);
      m_list->SetFocus();
    }
    // do not propagate (prevents caret move / tab navigation)
    return;
  }

  event.Skip();
}

void FindSymbolDialog::RebuildList() {
  m_list->Freeze();
  m_list->DeleteAllItems();

  for (size_t i = 0; i < m_filteredSymbols.size(); ++i) {
    const auto &s = m_filteredSymbols[i];

    wxString fullPath = wxString::FromUTF8(s.file.c_str());
    wxFileName fn(fullPath);
    wxString baseName = fn.GetFullName(); // "foo.cpp"
    wxString path = fn.GetPath();         // "/full/path/..."

    long idx = m_list->InsertItem(i, wxString::FromUTF8(s.display.c_str()));
    m_list->SetItem(idx, 1, baseName);
    m_list->SetItem(idx, 2, wxString::Format(wxT("%d"), s.line));
    m_list->SetItem(idx, 3, path);
  }

  // autosize columns - it can be more expensive with many rows,
  // but it doesn't matter for "Find symbol"
  int colCount = 4;
  for (int col = 0; col < colCount; ++col) {
    m_list->SetColumnWidth(col, wxLIST_AUTOSIZE_USEHEADER);
    // if you want it to be based on the content:
    int wContent = m_list->GetColumnWidth(col);
    m_list->SetColumnWidth(col, wxLIST_AUTOSIZE);
    int wBoth = m_list->GetColumnWidth(col);
    // use the larger of them
    m_list->SetColumnWidth(col, std::max(wContent, wBoth));
  }

  m_list->Thaw();
}

void FindSymbolDialog::OnItemActivated(wxListEvent &WXUNUSED(event)) {
  SymbolInfo s;
  if (!GetSelectedSymbol(s))
    return;

  ArduinoSymbolActivatedEvent evt(EVT_ARD_SYMBOL_ACTIVATED, GetId());
  evt.SetEventObject(this);
  evt.SetSymbol(s);

  wxPostEvent(GetParent(), evt);

  m_search->SetFocus();
}

void FindSymbolDialog::OnShow(wxShowEvent &event) {
  event.Skip();

  if (!event.IsShown())
    return;

  // Defer focus change to after the window is really shown,
  // otherwise it can be ignored on some platforms.
  CallAfter([this]() {
    if (!m_search)
      return;
    m_search->SetFocus();
    m_search->SelectAll();
  });
}

void FindSymbolDialog::OnClose(wxCloseEvent &event) {
  m_searchTimer.Stop();
  SaveWindowSize(wxT("FindSymbolDialog"), this, m_config);
  Hide();
  event.Veto();
}
