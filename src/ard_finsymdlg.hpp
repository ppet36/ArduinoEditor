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
