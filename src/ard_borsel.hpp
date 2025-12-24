#pragma once

#include <wx/config.h>
#include <wx/hyperlink.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/wx.h>

#include "ard_cli.hpp"
#include "ard_ev.hpp"

class ArduinoBoardSelectDialog : public wxDialog {
public:
  ArduinoBoardSelectDialog(wxWindow *parent,
                           ArduinoCli *cli,
                           wxConfigBase *config,
                           const std::string &currentFqbn);

  std::string GetSelectedFqbn() const { return m_selectedFqbn; }
  std::string GetSelectedProgrammerId() const;

private:
  enum {
    ID_VENDOR_LIST = wxID_HIGHEST + 1,
    ID_ARCH_LIST,
    ID_BOARD_LIST
  };

  ArduinoCli *m_cli = nullptr;
  wxConfigBase *m_config = nullptr;

  wxListBox *m_vendorList = nullptr;
  wxListBox *m_archList = nullptr;
  wxListCtrl *m_boardList = nullptr;
  wxStaticText *m_maintainerText = nullptr;
  wxHyperlinkCtrl *m_websiteLink = nullptr;
  wxStaticText *m_emailText = nullptr;
  wxChoice *m_programmerChoice = nullptr;
  wxTextCtrl *m_boardFilter = nullptr;
  wxTimer m_filterTimer;

  int m_boardSortColumn = 0; // 0 = Name, 1 = FQBN tail
  bool m_boardSortAscending = true;

  struct ModelBoard {
    std::string vendor;
    std::string arch;
    std::string boardId;
    std::string fqbn;
    std::string name;
    const ArduinoCoreInfo *core = nullptr;
  };

  std::vector<ModelBoard> m_boards;
  std::vector<std::string> m_vendors;
  std::vector<ArduinoProgrammerInfo> m_programmers; // current list for selected board

  std::string m_selectedFqbn;

  void BuildModel();
  void InitUi();
  void InitSelections(const std::string &currentFqbn);

  void RebuildArchList();
  void RebuildBoardList();
  void UpdateCoreInfoForSelection();
  void UpdateBoardListColumnHeaders();
  void SortBoardList();
  void RefreshProgrammerChoice();
  void OnProgrammersReady(wxThreadEvent &evt);
  bool GetCurrentFqbn(std::string &outFqbn) const;

  wxString GetSelectedVendor() const;
  wxString GetSelectedArch() const;

  void EndModal(int retCode) override;

  void OnVendorSelected(wxCommandEvent &);
  void OnArchSelected(wxCommandEvent &);
  void OnBoardDClick(wxListEvent &);
  void OnBoardColClick(wxListEvent &);
  void OnBoardSelected(wxListEvent &evt);
  void OnOk(wxCommandEvent &);
  void OnClose(wxCloseEvent &);
  void OnFilterText(wxCommandEvent &);
  void OnFilterTimer(wxTimerEvent &);
};
