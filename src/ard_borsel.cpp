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

#include "ard_borsel.hpp"
#include "utils.hpp"
#include <algorithm>
#include <sstream>

static std::vector<std::string> Split(const std::string &s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    out.push_back(item);
  }
  return out;
}

ArduinoBoardSelectDialog::ArduinoBoardSelectDialog(wxWindow *parent,
                                                   ArduinoCli *cli,
                                                   wxConfigBase *config,
                                                   const std::string &currentFqbn)
    : wxDialog(parent,
               wxID_ANY,
               _("Select board"),
               wxDefaultPosition,
               wxSize(800, 640),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_cli(cli), m_config(config), m_filterTimer(this) {

  BuildModel();
  InitUi();
  InitSelections(currentFqbn);

  m_vendorList->Bind(wxEVT_LISTBOX,
                     &ArduinoBoardSelectDialog::OnVendorSelected,
                     this, ID_VENDOR_LIST);
  m_archList->Bind(wxEVT_LISTBOX,
                   &ArduinoBoardSelectDialog::OnArchSelected,
                   this, ID_ARCH_LIST);

  // instead of wxEVT_LISTBOX_DCLICK:
  m_boardList->Bind(wxEVT_LIST_ITEM_ACTIVATED,
                    &ArduinoBoardSelectDialog::OnBoardDClick,
                    this);

  // click on the column header - sorting
  m_boardList->Bind(wxEVT_LIST_COL_CLICK,
                    &ArduinoBoardSelectDialog::OnBoardColClick,
                    this);

  // change of row selection = refresh programmers
  m_boardList->Bind(wxEVT_LIST_ITEM_SELECTED,
                    &ArduinoBoardSelectDialog::OnBoardSelected,
                    this);

  m_boardFilter->Bind(wxEVT_TEXT, &ArduinoBoardSelectDialog::OnFilterText, this);
  Bind(wxEVT_TIMER, &ArduinoBoardSelectDialog::OnFilterTimer, this);

  Bind(EVT_PROGRAMMERS_READY, &ArduinoBoardSelectDialog::OnProgrammersReady, this);

  Bind(wxEVT_BUTTON, &ArduinoBoardSelectDialog::OnOk, this, wxID_OK);
  Bind(wxEVT_CLOSE_WINDOW, &ArduinoBoardSelectDialog::OnClose, this);
}

void ArduinoBoardSelectDialog::BuildModel() {
  if (!m_cli)
    return;

  const auto &boards = m_cli->GetAvailableBoards();
  const auto &cores = m_cli->GetCores();

  auto findCoreFor = [&cores](const std::string &vendor,
                              const std::string &arch) -> const ArduinoCoreInfo * {
    std::string id = vendor + ":" + arch;
    for (const auto &c : cores) {
      if (c.id == id) {
        return &c;
      }
    }
    return nullptr;
  };

  for (const auto &b : boards) {
    if (b.fqbn.empty())
      continue;

    auto parts = Split(b.fqbn, ':');
    if (parts.size() < 3)
      continue;

    std::string vendor = parts[0];
    std::string arch = parts[1];

    std::string boardAndOpts = parts[2];
    auto commaPos = boardAndOpts.find(',');
    std::string boardId = (commaPos == std::string::npos)
                              ? boardAndOpts
                              : boardAndOpts.substr(0, commaPos);

    ModelBoard mb;
    mb.vendor = vendor;
    mb.arch = arch;
    mb.boardId = boardId;
    mb.fqbn = b.fqbn;
    mb.name = b.name.empty() ? b.fqbn : b.name;
    mb.core = findCoreFor(vendor, arch);

    m_boards.push_back(std::move(mb));

    if (std::find(m_vendors.begin(), m_vendors.end(), vendor) == m_vendors.end()) {
      m_vendors.push_back(vendor);
    }
  }

  std::sort(m_vendors.begin(), m_vendors.end());
}

void ArduinoBoardSelectDialog::InitUi() {
  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  auto *listsSizer = new wxBoxSizer(wxHORIZONTAL);

  m_vendorList = new wxListBox(this, wxID_HIGHEST + 1);
  m_archList = new wxListBox(this, wxID_HIGHEST + 2);

  m_boardList = new wxListCtrl(
      this,
      wxID_HIGHEST + 3,
      wxDefaultPosition,
      wxDefaultSize,
      wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);

  m_vendorList->SetMinSize(wxSize(150, 200));
  m_archList->SetMinSize(wxSize(150, 200));
  m_boardList->SetMinSize(wxSize(360, 200));

  m_boardList->SetImageList(CreateListCtrlSortIndicatorImageList(m_boardList->GetForegroundColour()), wxIMAGE_LIST_SMALL);

  for (const auto &v : m_vendors) {
    m_vendorList->Append(wxString::FromUTF8(v.c_str()));
  }

  // definition of listctrl columns
  {
    wxListItem col0;
    col0.SetId(0);
    col0.SetText(_("Name"));
    col0.SetMask(wxLIST_MASK_TEXT);
    m_boardList->InsertColumn(0, col0);

    wxListItem col1;
    col1.SetId(1);
    col1.SetText(_("Board"));
    col1.SetMask(wxLIST_MASK_TEXT);
    m_boardList->InsertColumn(1, col1);
  }

  // default sort - by name, ascending
  m_boardSortColumn = 0;
  m_boardSortAscending = true;
  UpdateBoardListColumnHeaders();

  // --- sizer for filter + board list ---
  wxBoxSizer *boardsSizer = new wxBoxSizer(wxVERTICAL);

  m_boardFilter = new wxTextCtrl(this, wxID_ANY);
  m_boardFilter->SetHint(_("Filter boards..."));

  boardsSizer->Add(m_boardFilter, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
  boardsSizer->Add(m_boardList, 1, wxEXPAND | wxALL, 5);
  // -------------------------------------------

  listsSizer->Add(m_vendorList, 0, wxEXPAND | wxALL, 5);
  listsSizer->Add(m_archList, 0, wxEXPAND | wxALL, 5);
  listsSizer->Add(boardsSizer, 1, wxEXPAND, 0);

  topSizer->Add(listsSizer, 1, wxEXPAND | wxALL, 5);

  // Programmer choice under leaves
  auto *progSizer = new wxBoxSizer(wxHORIZONTAL);

  auto *progLabel = new wxStaticText(this, wxID_ANY, _("Programmer:"));
  m_programmerChoice = new wxChoice(this, wxID_ANY);
  m_programmerChoice->SetMinSize(wxSize(250, -1));
  m_programmerChoice->Enable(false); // until we select the board

  progSizer->Add(progLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
  progSizer->Add(m_programmerChoice, 0, wxALIGN_CENTER_VERTICAL);

  topSizer->Add(progSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 5);

  // Info about core under lists
  auto *infoSizer = new wxFlexGridSizer(3, 2, 2, 5);
  infoSizer->AddGrowableCol(1);

  m_maintainerText = new wxStaticText(this, wxID_ANY, wxEmptyString);
  m_websiteLink = new wxHyperlinkCtrl(this,
                                      wxID_ANY,
                                      wxT("<url>"),
                                      wxEmptyString,
                                      wxDefaultPosition,
                                      wxDefaultSize,
                                      wxHL_ALIGN_LEFT);
  m_emailText = new wxStaticText(this, wxID_ANY, wxEmptyString);

  infoSizer->Add(new wxStaticText(this, wxID_ANY, _("Maintainer:")), 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
  infoSizer->Add(m_maintainerText, 1, wxLEFT | wxEXPAND);

  infoSizer->Add(new wxStaticText(this, wxID_ANY, _("Website:")), 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
  infoSizer->Add(m_websiteLink, 1, wxLEFT | wxEXPAND);

  infoSizer->Add(new wxStaticText(this, wxID_ANY, _("Email:")), 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
  infoSizer->Add(m_emailText, 1, wxLEFT | wxEXPAND);

  topSizer->Add(infoSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  // OK / Cancel
  auto *btnSizer = CreateLocalizedSeparatedOkCancelSizer(this);
  topSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 5);

  SetSizer(topSizer);

  if (LoadWindowSize(wxT("BoardSelector"), this, m_config)) {
    return;
  } else {
    CenterOnParent();
  }
}

void ArduinoBoardSelectDialog::InitSelections(const std::string &currentFqbn) {
  // unchanged parsing currentFqbn -> curVendor, curArch, curBoard
  std::string curVendor, curArch, curBoard;
  if (!currentFqbn.empty()) {
    auto parts = Split(currentFqbn, ':');
    if (parts.size() >= 3) {
      curVendor = parts[0];
      curArch = parts[1];

      std::string boardAndOpts = parts[2];
      auto commaPos = boardAndOpts.find(',');
      curBoard = (commaPos == std::string::npos)
                     ? boardAndOpts
                     : boardAndOpts.substr(0, commaPos);
    }
  }

  int vendorSel = wxNOT_FOUND;
  if (!curVendor.empty()) {
    vendorSel = m_vendorList->FindString(wxString::FromUTF8(curVendor.c_str()));
  }
  if (vendorSel == wxNOT_FOUND && m_vendorList->GetCount() > 0) {
    vendorSel = 0;
  }

  if (vendorSel != wxNOT_FOUND) {
    m_vendorList->SetSelection(vendorSel);
    RebuildArchList();
  }

  int archSel = wxNOT_FOUND;
  if (!curArch.empty()) {
    archSel = m_archList->FindString(wxString::FromUTF8(curArch.c_str()));
  }
  if (archSel == wxNOT_FOUND && m_archList->GetCount() > 0) {
    archSel = 0;
  }

  if (archSel != wxNOT_FOUND) {
    m_archList->SetSelection(archSel);
    RebuildBoardList();
  }

  // selection of a specific board in ListCtrl
  if (!curBoard.empty() && m_boardList) {
    wxString curWx = wxString::FromUTF8(curBoard.c_str());

    int count = m_boardList->GetItemCount();
    bool found = false;
    for (int i = 0; i < count; ++i) {
      wxString tail = m_boardList->GetItemText(i, 1); // tail column

      // tail can be "esp32" or "esp32,someOption=foo" -> we take only the part before the comma
      wxString tailBase = tail.BeforeFirst(',');

      if (tailBase == curWx) {
        m_boardList->SetItemState(i,
                                  wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                  wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        m_boardList->EnsureVisible(i);
        found = true;
        break;
      }
    }

    if (!found && m_boardList->GetItemCount() > 0) {
      // fallback - when we didn't find the board, we take the first one
      m_boardList->SetItemState(0,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
      m_boardList->EnsureVisible(0);
    }
  } else if (m_boardList && m_boardList->GetItemCount() > 0) {
    m_boardList->SetItemState(0,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    m_boardList->EnsureVisible(0);
  }

  UpdateCoreInfoForSelection();
  RefreshProgrammerChoice();
}

wxString ArduinoBoardSelectDialog::GetSelectedVendor() const {
  int sel = m_vendorList->GetSelection();
  if (sel == wxNOT_FOUND)
    return {};
  return m_vendorList->GetString(sel);
}

wxString ArduinoBoardSelectDialog::GetSelectedArch() const {
  int sel = m_archList->GetSelection();
  if (sel == wxNOT_FOUND)
    return {};
  return m_archList->GetString(sel);
}

void ArduinoBoardSelectDialog::RebuildArchList() {
  wxString vendorWx = GetSelectedVendor();
  m_archList->Clear();

  if (vendorWx.IsEmpty())
    return;

  std::string vendor = wxToStd(vendorWx);
  std::vector<std::string> archs;

  for (const auto &mb : m_boards) {
    if (mb.vendor != vendor)
      continue;
    if (std::find(archs.begin(), archs.end(), mb.arch) == archs.end()) {
      archs.push_back(mb.arch);
    }
  }

  std::sort(archs.begin(), archs.end());

  for (const auto &a : archs) {
    m_archList->Append(wxString::FromUTF8(a.c_str()));
  }
}

void ArduinoBoardSelectDialog::RebuildBoardList() {
  if (!m_boardList)
    return;

  m_boardList->Freeze();
  m_boardList->DeleteAllItems();

  wxString vendorWx = GetSelectedVendor();
  wxString archWx = GetSelectedArch();

  if (vendorWx.IsEmpty() || archWx.IsEmpty()) {
    m_boardList->Thaw();
    return;
  }

  std::string vendor = wxToStd(vendorWx);
  std::string arch = wxToStd(archWx);

  // --- filter text (lowercase) ---
  wxString filter;
  if (m_boardFilter) {
    filter = m_boardFilter->GetValue();
    filter.MakeLower();
  }
  bool hasFilter = !filter.IsEmpty();
  // -------------------------------------

  // fill rows according to the selected vendor/arch
  for (const auto &mb : m_boards) {
    if (mb.vendor != vendor || mb.arch != arch)
      continue;

    wxString nameWx = wxString::FromUTF8(mb.name.c_str());

    std::string tail;
    {
      auto parts = Split(mb.fqbn, ':');
      if (parts.size() >= 3) {
        tail = parts[2]; // boardId[,option=...]
      } else {
        tail = mb.boardId;
      }
    }
    wxString tailWx = wxString::FromUTF8(tail.c_str());

    // --- filtering in both columns ---
    if (hasFilter) {
      wxString nameLower = nameWx;
      wxString tailLower = tailWx;
      nameLower.MakeLower();
      tailLower.MakeLower();

      if (nameLower.Find(filter) == wxNOT_FOUND &&
          tailLower.Find(filter) == wxNOT_FOUND) {
        continue; // does not match the filter -> skip
      }
    }
    // -------------------------------------------------

    long row = m_boardList->GetItemCount();
    long idx = m_boardList->InsertItem(row, nameWx);
    m_boardList->SetItem(idx, 1, tailWx);
  }

  // no default selection here - we leave it to the caller

  SortBoardList();

  m_boardList->SetColumnWidth(0, wxLIST_AUTOSIZE_USEHEADER);
  m_boardList->SetColumnWidth(1, wxLIST_AUTOSIZE_USEHEADER);

  m_boardList->Thaw();
}

void ArduinoBoardSelectDialog::UpdateBoardListColumnHeaders() {
  wxListItem col0;
  col0.SetId(0);
  col0.SetText(_("Name"));
  col0.SetMask(wxLIST_MASK_TEXT | wxLIST_MASK_IMAGE);

  wxListItem col1;
  col1.SetId(1);
  col1.SetText(_("Board"));
  col1.SetMask(wxLIST_MASK_TEXT | wxLIST_MASK_IMAGE);

  if (m_boardSortColumn == 0) {
    col0.SetImage(m_boardSortAscending ? IMLI_TREECTRL_ARROW_UP : IMLI_TREECTRL_ARROW_DOWN);
    col1.SetImage(-1);
  } else {
    col1.SetImage(m_boardSortAscending ? IMLI_TREECTRL_ARROW_UP : IMLI_TREECTRL_ARROW_DOWN);
    col0.SetImage(-1);
  }

  m_boardList->SetColumn(0, col0);
  m_boardList->SetColumn(1, col1);
}

void ArduinoBoardSelectDialog::SortBoardList() {
  if (!m_boardList)
    return;

  int count = m_boardList->GetItemCount();
  if (count <= 1)
    return;

  // remember the currently selected "tail" so that we can restore the selection after sorting
  int selRow = m_boardList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  wxString selTail;
  if (selRow != -1) {
    selTail = m_boardList->GetItemText(selRow, 1);
  }

  struct Row {
    wxString name;
    wxString tail;
  };

  std::vector<Row> rows;
  rows.reserve(count);

  for (int i = 0; i < count; ++i) {
    Row r;
    r.name = m_boardList->GetItemText(i, 0);

    wxListItem item;
    item.SetId(i);
    item.SetColumn(1);
    item.SetMask(wxLIST_MASK_TEXT);
    if (m_boardList->GetItem(item)) {
      r.tail = item.GetText();
    }
    rows.push_back(std::move(r));
  }

  std::sort(rows.begin(), rows.end(), [this](const Row &a, const Row &b) {
    const wxString &sa = (m_boardSortColumn == 0) ? a.name : a.tail;
    const wxString &sb = (m_boardSortColumn == 0) ? b.name : b.tail;

    int cmp = sa.CmpNoCase(sb);
    if (!m_boardSortAscending)
      cmp = -cmp;

    if (cmp != 0)
      return cmp < 0;

    // stable tie-breaker
    return a.name.CmpNoCase(b.name) < 0;
  });

  m_boardList->Freeze();
  m_boardList->DeleteAllItems();

  int newSelRow = -1;

  for (size_t i = 0; i < rows.size(); ++i) {
    long idx = m_boardList->InsertItem(i, rows[i].name);
    m_boardList->SetItem(idx, 1, rows[i].tail);

    if (!selTail.empty() && rows[i].tail == selTail) {
      newSelRow = (int)i;
    }
  }

  if (newSelRow >= 0) {
    m_boardList->SetItemState(newSelRow,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    m_boardList->EnsureVisible(newSelRow);
  }

  m_boardList->Thaw();
}

void ArduinoBoardSelectDialog::RefreshProgrammerChoice() {
  if (!m_programmerChoice) {
    return;
  }

  m_programmers.clear();
  m_programmerChoice->Clear();

  if (!m_cli) {
    m_programmerChoice->Enable(false);
    return;
  }

  std::string fqbn;
  if (!GetCurrentFqbn(fqbn)) {
    // No board selected -> disable choice
    m_programmerChoice->Enable(false);
    return;
  }

  // Placeholder + disabled until the asynchronous query completes
  m_programmerChoice->Append(_("<loading programmers...>"));
  m_programmerChoice->SetSelection(0);
  m_programmerChoice->Enable(false);

  // We start an async query for programmers
  m_cli->GetProgrammersForFqbnAsync(this, fqbn);
}

void ArduinoBoardSelectDialog::OnProgrammersReady(wxThreadEvent &evt) {
  if (!m_programmerChoice) {
    return;
  }

  // Optional: check that the event belongs to the currently selected FQBN
  // (GetProgrammersForFqbnAsync stores the FQBN into evt.SetString(...)).
  std::string currentFqbn;
  if (GetCurrentFqbn(currentFqbn)) {
    wxString evtFqbnWx = evt.GetString();
    if (!evtFqbnWx.IsEmpty()) {
      std::string evtFqbn = wxToStd(evtFqbnWx);
      if (evtFqbn != currentFqbn) {
        // Meanwhile, the user has already selected a different board -> ignore this result
        return;
      }
    }
  }

  // Result from CLI
  auto programmers = evt.GetPayload<std::vector<ArduinoProgrammerInfo>>();

  m_programmers.clear();
  m_programmerChoice->Clear();

  // We always add "<default>" as the first item = no programmer
  m_programmerChoice->Append(_("<default>"));

  if (programmers.empty()) {
    // No programmers -> only default, choice remains disabled
    m_programmerChoice->SetSelection(0);
    m_programmerChoice->Enable(false);
    return;
  }

  m_programmers = std::move(programmers);

  // Let's try to preselect the programmer based on what ArduinoCli has set up
  std::string currentProgId;
  if (m_cli) {
    currentProgId = m_cli->GetProgrammer();
  }

  int selIndex = 0; // 0 = "<default>"

  for (size_t i = 0; i < m_programmers.size(); ++i) {
    const auto &p = m_programmers[i];

    wxString label;
    if (!p.name.empty() && p.name != p.id) {
      label = wxString::FromUTF8(p.name) + wxT(" (") + wxString::FromUTF8(p.id) + wxT(")");
    } else {
      label = wxString::FromUTF8(p.id.c_str());
    }

    m_programmerChoice->Append(label);

    if (!currentProgId.empty() && p.id == currentProgId) {
      selIndex = static_cast<int>(i) + 1; // +1 due to "<default>"
    }
  }

  m_programmerChoice->Enable(true);
  m_programmerChoice->SetSelection(selIndex);
}

bool ArduinoBoardSelectDialog::GetCurrentFqbn(std::string &outFqbn) const {
  if (!m_boardList) {
    return false;
  }

  int selRow = m_boardList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (selRow == -1) {
    return false;
  }

  wxString vendorWx = GetSelectedVendor();
  wxString archWx = GetSelectedArch();
  if (vendorWx.IsEmpty() || archWx.IsEmpty()) {
    return false;
  }

  wxString tailWx = m_boardList->GetItemText(selRow, 1);
  if (tailWx.IsEmpty()) {
    return false;
  }

  outFqbn = wxToStd(vendorWx + wxT(":") + archWx + wxT(":") + tailWx);
  return !outFqbn.empty();
}

void ArduinoBoardSelectDialog::UpdateCoreInfoForSelection() {
  wxString vendorWx = GetSelectedVendor();
  wxString archWx = GetSelectedArch();

  const ArduinoCoreInfo *core = nullptr;

  if (!vendorWx.IsEmpty() && !archWx.IsEmpty()) {
    std::string vendor = wxToStd(vendorWx);
    std::string arch = wxToStd(archWx);

    for (const auto &mb : m_boards) {
      if (mb.vendor == vendor && mb.arch == arch && mb.core) {
        core = mb.core;
        break;
      }
    }
  }

  if (!core) {
    m_maintainerText->SetLabel(wxEmptyString);
    m_websiteLink->SetLabel(wxEmptyString);
    m_websiteLink->SetURL(wxEmptyString);
    m_emailText->SetLabel(wxEmptyString);
    return;
  }

  m_maintainerText->SetLabel(wxString::FromUTF8(core->maintainer.c_str()));

  wxString web = wxString::FromUTF8(core->website.c_str());
  m_websiteLink->SetLabel(web);
  m_websiteLink->SetURL(web);

  m_emailText->SetLabel(wxString::FromUTF8(core->email.c_str()));
}

void ArduinoBoardSelectDialog::OnBoardColClick(wxListEvent &evt) {
  int col = evt.GetColumn();
  if (col < 0 || col > 1)
    return;

  if (m_boardSortColumn == col) {
    m_boardSortAscending = !m_boardSortAscending;
  } else {
    m_boardSortColumn = col;
    m_boardSortAscending = true;
  }

  UpdateBoardListColumnHeaders();
  SortBoardList();
}

void ArduinoBoardSelectDialog::OnBoardSelected(wxListEvent &WXUNUSED(evt)) {
  RefreshProgrammerChoice();
}

void ArduinoBoardSelectDialog::OnVendorSelected(wxCommandEvent &) {
  RebuildArchList();
  if (m_archList->GetCount() > 0) {
    m_archList->SetSelection(0);
  }

  RebuildBoardList();

  // after switching the vendor, we select the first board by default (if it exists)
  if (m_boardList && m_boardList->GetItemCount() > 0) {
    m_boardList->SetItemState(0,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    m_boardList->EnsureVisible(0);
  }

  UpdateCoreInfoForSelection();
  RefreshProgrammerChoice();
}

void ArduinoBoardSelectDialog::OnArchSelected(wxCommandEvent &) {
  RebuildBoardList();

  if (m_boardList && m_boardList->GetItemCount() > 0) {
    m_boardList->SetItemState(0,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    m_boardList->EnsureVisible(0);
  }

  UpdateCoreInfoForSelection();
  RefreshProgrammerChoice();
}

std::string ArduinoBoardSelectDialog::GetSelectedProgrammerId() const {
  if (!m_programmerChoice) {
    return "";
  }

  int sel = m_programmerChoice->GetSelection();
  if (sel == wxNOT_FOUND || sel <= 0) {
    // sel < 0 = nothing, sel == 0 = "<default>"
    return "";
  }

  size_t idx = static_cast<size_t>(sel - 1); // shift due to "<default>"
  if (idx >= m_programmers.size()) {
    return "";
  }

  return m_programmers[idx].id;
}

void ArduinoBoardSelectDialog::OnBoardDClick(wxListEvent &WXUNUSED(evt)) {
  if (!m_boardList)
    return;

  int sel = m_boardList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (sel == -1)
    return;

  wxString vendorWx = GetSelectedVendor();
  wxString archWx = GetSelectedArch();
  if (vendorWx.IsEmpty() || archWx.IsEmpty())
    return;

  wxString tailWx = m_boardList->GetItemText(sel, 1);
  if (tailWx.IsEmpty())
    return;

  m_selectedFqbn = wxToStd(vendorWx + wxT(":") + archWx + wxT(":") + tailWx);

  if (!m_selectedFqbn.empty()) {
    if (m_cli) {
      m_cli->SetFQBN(m_selectedFqbn);

      std::string progId = GetSelectedProgrammerId();
      if (!progId.empty()) {
        m_cli->SetProgrammer(progId);
      } else {
        m_cli->SetProgrammer("");
      }
    }
    EndModal(wxID_OK);
  }
}
void ArduinoBoardSelectDialog::OnOk(wxCommandEvent &) {
  if (!m_boardList) {
    EndModal(wxID_CANCEL);
    return;
  }

  int sel = m_boardList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (sel == -1) {
    EndModal(wxID_CANCEL);
    return;
  }

  wxString vendorWx = GetSelectedVendor();
  wxString archWx = GetSelectedArch();
  if (vendorWx.IsEmpty() || archWx.IsEmpty()) {
    EndModal(wxID_CANCEL);
    return;
  }

  wxString tailWx = m_boardList->GetItemText(sel, 1);
  if (tailWx.IsEmpty()) {
    EndModal(wxID_CANCEL);
    return;
  }

  m_selectedFqbn = wxToStd(vendorWx + wxT(":") + archWx + wxT(":") + tailWx);
  if (m_selectedFqbn.empty()) {
    EndModal(wxID_CANCEL);
    return;
  }

  // Set in ArduinoCli
  if (m_cli) {
    m_cli->SetFQBN(m_selectedFqbn);

    std::string progId = GetSelectedProgrammerId();
    if (!progId.empty()) {
      m_cli->SetProgrammer(progId);
    } else {
      // if nothing is selected (or there are no programmers), leave it empty
      m_cli->SetProgrammer("");
    }
  }

  EndModal(wxID_OK);
}

void ArduinoBoardSelectDialog::EndModal(int retCode) {
  SaveWindowSize(wxT("BoardSelector"), this, m_config);
  wxDialog::EndModal(retCode);
}

void ArduinoBoardSelectDialog::OnClose(wxCloseEvent &evt) {
  evt.Skip();
}

void ArduinoBoardSelectDialog::OnFilterText(wxCommandEvent &) {
  if (m_filterTimer.IsRunning()) {
    m_filterTimer.Stop();
  }
  m_filterTimer.Start(500, wxTIMER_ONE_SHOT);
}

void ArduinoBoardSelectDialog::OnFilterTimer(wxTimerEvent &) {
  // preserve the current selection if possible (according to the tail)
  wxString prevTail;
  if (m_boardList) {
    int selRow = m_boardList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (selRow != -1) {
      prevTail = m_boardList->GetItemText(selRow, 1);
    }
  }

  RebuildBoardList();

  // after filtering, we try to return the same board
  if (m_boardList && !prevTail.IsEmpty()) {
    int count = m_boardList->GetItemCount();
    for (int i = 0; i < count; ++i) {
      wxString tail = m_boardList->GetItemText(i, 1);
      if (tail == prevTail) {
        m_boardList->SetItemState(i,
                                  wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                  wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        m_boardList->EnsureVisible(i);
        break;
      }
    }
  }

  UpdateCoreInfoForSelection();
  RefreshProgrammerChoice();
}
