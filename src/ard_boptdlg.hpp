// arduino_board_options_dlg.hpp
#pragma once

#include "ard_cli.hpp"
#include <wx/choice.h>
#include <wx/wx.h>

class ArduinoBoardOptionsDialog : public wxDialog {
public:
  ArduinoBoardOptionsDialog(wxWindow *parent,
                            const std::vector<ArduinoBoardOption> &options,
                            const wxString &title = _("Board options"));

  const std::vector<ArduinoBoardOption> &GetOptions() const { return m_options; }

private:
  void OnOk(wxCommandEvent &);

  std::vector<ArduinoBoardOption> m_options;
  std::vector<wxChoice *> m_choices;
};
