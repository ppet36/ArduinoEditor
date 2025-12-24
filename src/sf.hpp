#pragma once

#include <wx/timer.h>
#include <wx/wx.h>

#ifndef VERSION
#define VERSION "1.0.0"
#endif

class SplashFrame : public wxFrame {
public:
  SplashFrame();
  void StartClose(int delayMs = 300); // closes after a short delay

  void Present();

  void SetMessage(const wxString &msg);

private:
  wxTimer m_closeTimer;
  wxStaticText *m_messageText = nullptr;

  void OnCloseTimer(wxTimerEvent &evt);
};
