#pragma once

#include <wx/dialog.h>
#include <wx/html/htmlwin.h>

class ArduinoAboutDialog : public wxDialog {
public:
  explicit ArduinoAboutDialog(wxWindow *parent);

private:
  void OnHtmlLink(wxHtmlLinkEvent &event);
};
