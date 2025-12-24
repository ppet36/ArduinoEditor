#pragma once

#include <wx/config.h>
#include <wx/dialog.h>
#include <wx/wx.h>

class AiExplainPopup : public wxDialog {
public:
  AiExplainPopup(wxWindow *parent, wxConfigBase *config, const wxString &text);
  ~AiExplainPopup();

private:
  wxConfigBase *m_config;

  void OnCharHook(wxKeyEvent &evt);
};
