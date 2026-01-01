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

#include "ard_setdlg.hpp"
#include <wx/control.h>
#include <wx/timer.h>

enum class ArduinoActivityState {
  Idle,
  Background,
  Busy
};

// Right/bottom dot in statusbar.
class ArduinoActivityDotCtrl : public wxControl {
public:
  ArduinoActivityDotCtrl(wxWindow *parent,
                         wxWindowID id = wxID_ANY,
                         const wxPoint &pos = wxDefaultPosition,
                         const wxSize &size = wxDefaultSize,
                         long style = 0);

  void SetState(ArduinoActivityState state);
  ArduinoActivityState GetState() const { return m_state; }

  void EnablePulse(bool enable);
  void StartProcess(const wxString &name, int id, ArduinoActivityState state, bool canBeTerminated = false);
  void StopProcess(int id);
  void StopAllProcesses();

  void ApplySettings(const EditorSettings &settings);

private:
  void OnPaint(wxPaintEvent &event);
  void OnSize(wxSizeEvent &event);
  void OnTimer(wxTimerEvent &event);
  void OnMouseClick(wxMouseEvent &event);
  void OnPopupMenu(wxCommandEvent &event);

  void UpdateTooltip();
  wxString BuildTooltipText() const;

  wxColour GetDotColour() const;
  int GetDotRadius() const;

  bool ShouldPulse() const;
  void AfterStateChanged();
  void UpdatePulseTimer();

  void RecomputeStateFromProcesses();

private:
  struct ActivityProcess {
    int id{0};
    wxString name;
    bool canBeTerminated{false};
    ArduinoActivityState state{ArduinoActivityState::Background};
  };

  EditorSettings m_settings;

  ArduinoActivityState m_state{ArduinoActivityState::Idle};
  wxTimer m_timer;
  bool m_pulseEnabled{true};
  int m_pulsePhase{0};

  std::vector<ActivityProcess> m_processes;

  wxDECLARE_NO_COPY_CLASS(ArduinoActivityDotCtrl);
};

class ArduinoActivityScope {
public:
  ArduinoActivityScope(ArduinoActivityDotCtrl *ctrl,
                       ArduinoActivityState state,
                       int id,
                       const wxString &name);

  ArduinoActivityScope(ArduinoActivityDotCtrl *ctrl,
                       int id,
                       ArduinoActivityState state);

  ~ArduinoActivityScope();

private:
  ArduinoActivityDotCtrl *m_ctrl{nullptr};
  int m_id{-1};
};
