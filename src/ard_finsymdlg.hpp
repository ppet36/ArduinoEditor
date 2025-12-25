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

#include <functional>
#include <vector>

#include <wx/config.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include "ard_cc.hpp"

class FindSymbolDialog : public wxDialog {
public:
  FindSymbolDialog(wxWindow *parent,
                   wxConfigBase *config,
                   const std::vector<SymbolInfo> &symbols);

  // Returns the selected symbol (current item in the list)
  bool GetSelectedSymbol(SymbolInfo &out) const;

  // Sets the callback called on double-click / Enter on a symbol
  void SetOnSymbolActivated(const std::function<void(const SymbolInfo &)> &cb) {
    m_onActivated = cb;
  }

  // Update of the symbol list (e.g. after reparse)
  void SetSymbols(const std::vector<SymbolInfo> &symbols);

private:
  void OnSearchChanged(wxCommandEvent &event);
  void OnItemActivated(wxListEvent &event);
  void OnClose(wxCloseEvent &event);
  void OnCharHook(wxKeyEvent &event);
  void OnSearchTimer(wxTimerEvent &event);

  void ApplyFilterAndRebuild();
  void RebuildList();

  wxTextCtrl *m_search = nullptr;
  wxListCtrl *m_list = nullptr;

  wxConfigBase *m_config = nullptr;
  wxString m_cfgPrefix;

  std::vector<SymbolInfo> m_allSymbols;
  std::vector<SymbolInfo> m_filteredSymbols;

  std::function<void(const SymbolInfo &)> m_onActivated;

  wxTimer m_searchTimer;
};
