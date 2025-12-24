
#include "sf.hpp"
#include <wx/app.h>
#include <wx/config.h>
#include <wx/timer.h>

class ArduinoEditorFrame;

class ArduinoEditApp : public wxApp {
private:
  wxConfigBase *cfg;
  wxString m_sketchbook;
  SplashFrame *m_splash;
  wxTimer m_startTimer;
  wxTimer m_stopTimer;

  wxString m_sketchToBeOpen;

  void OnStartTimer(wxTimerEvent &event);
  void OnStopTimer(wxTimerEvent &event);

public:
  ArduinoEditApp();

  virtual void OnInitCmdLine(wxCmdLineParser &parser) override;
  virtual bool OnCmdLineParsed(wxCmdLineParser &parser) override;
  virtual bool OnInit() override;
  int OnExit() override;

  void SetSplashMessage(const wxString &message);
  void HideSplash();
  void OpenSketch(const wxString &path);
};

wxDECLARE_APP(ArduinoEditApp);
