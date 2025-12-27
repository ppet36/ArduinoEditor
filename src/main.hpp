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

#include "sf.hpp"
#include <wx/app.h>
#include <wx/config.h>
#include <wx/timer.h>

class ArduinoEditorFrame;
class AeUserActivityFilter;

class ArduinoEditApp : public wxApp {
private:
  wxConfigBase *cfg;
  wxString m_sketchbook;
  SplashFrame *m_splash;
  wxTimer m_startTimer;
  wxTimer m_stopTimer;

  wxString m_sketchToBeOpen;

  // Updates
  wxTimer m_updateIdleTimer;
  AeUserActivityFilter *m_activityFilter = nullptr;
  bool m_updateIdleTrackingStarted = false;
  std::atomic_bool m_updateCheckInProgress{false};
  void OnUpdateIdleTimer(wxTimerEvent &event);
  void ArmUpdateIdleTimer();
  void StartUpdateIdleTracking();

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
