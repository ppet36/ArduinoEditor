#pragma once

#include <wx/config.h>
#include <wx/frame.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stc/stc.h>

#include "ard_cli.hpp"

class ArduinoExamplesFrame : public wxFrame {
public:
  ArduinoExamplesFrame(wxWindow *parent, ArduinoCli *cli, wxConfigBase *config);
  ~ArduinoExamplesFrame() override = default;

  // call when the list of libraries changes (LoadLibrariesAsync finished)
  void RefreshLibraries();

private:
  struct ExampleRow {
    const ArduinoLibraryInfo *lib;
    std::string examplePath;
  };

  ArduinoCli *m_cli = nullptr;
  wxConfigBase *m_config = nullptr;

  wxSplitterWindow *m_mainSplitter = nullptr;
  wxSplitterWindow *m_rightSplitter = nullptr;

  wxListCtrl *m_libList = nullptr;
  wxListCtrl *m_exampleList = nullptr;
  wxStyledTextCtrl *m_preview = nullptr;
  wxTextCtrl *m_libFilterCtrl = nullptr;
  wxTimer m_filterTimer;

  std::vector<const ArduinoLibraryInfo *> m_libRows;
  std::vector<const ArduinoLibraryInfo *> m_allLibRows; // unfiltered list
  std::vector<ExampleRow> m_exampleRows;

  int ModalMsgDialog(const wxString &message, const wxString &caption = _("Install example"), int styles = wxOK | wxICON_ERROR);

  void CreateLayout();
  void BindEvents();

  void ApplyLibraryFilter(const wxString &filter);

  void PopulateLibraries();
  void PopulateExamplesForLibrary(const ArduinoLibraryInfo *lib);
  void LoadExamplePreview(const std::string &examplePath);

  void OnLibraryItemSelected(wxListEvent &evt);
  void OnExampleItemActivated(wxListEvent &evt);
  void OnExampleItemSelected(wxListEvent &evt);
  void OnLibraryItemActivated(wxListEvent &evt);
  void OnExampleContextMenu(wxContextMenuEvent &evt);
  void OnInstallExample(wxCommandEvent &evt);
  void OnFilterText(wxCommandEvent &evt);
  void OnFilterTimer(wxTimerEvent &evt);

  void LoadLayout();
  void SaveLayout();

  void InstallSelectedExample();

  void OnClose(wxCloseEvent &evt);
};
