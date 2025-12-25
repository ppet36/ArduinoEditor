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

#include "ard_indic.hpp"

#include <algorithm>
#include <cmath>
#include <wx/dcbuffer.h>

namespace {

constexpr int ACTIVITY_TIMER_INTERVAL_MS = 150;

constexpr int MIN_CONTROL_SIZE = 14;

} // namespace

ArduinoActivityDotCtrl::ArduinoActivityDotCtrl(wxWindow *parent,
                                               wxWindowID id,
                                               const wxPoint &pos,
                                               const wxSize &size,
                                               long style)
    : wxControl(parent, id, pos, size, style | wxBORDER_NONE),
      m_timer(this) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);

  if (!size.IsFullySpecified()) {
    SetMinSize(wxSize(MIN_CONTROL_SIZE, MIN_CONTROL_SIZE));
  }

  SetBackgroundColour(parent->GetBackgroundColour());

  // Event binding
  Bind(wxEVT_PAINT, &ArduinoActivityDotCtrl::OnPaint, this);
  Bind(wxEVT_SIZE, &ArduinoActivityDotCtrl::OnSize, this);
  m_timer.Bind(wxEVT_TIMER, &ArduinoActivityDotCtrl::OnTimer, this);

  UpdateTooltip();
  StartTimerIfNeeded();
}

void ArduinoActivityDotCtrl::SetState(ArduinoActivityState state) {
  if (m_state == state && m_processes.empty())
    return;

  m_state = state;
  m_pulsePhase = 0;
  UpdateTooltip();
  StartTimerIfNeeded();
  Refresh();
  Update();
}

// ----------------------------------------------------------
// Process management
// ----------------------------------------------------------

void ArduinoActivityDotCtrl::StartProcess(const wxString &name, int id,
                                          ArduinoActivityState state) {
  // It makes no sense to run an "Idle" task -> remap it to Background
  if (state == ArduinoActivityState::Idle) {
    state = ArduinoActivityState::Background;
  }

  ActivityProcess p;
  p.id = id;
  p.name = name;
  p.state = state;

  APP_DEBUG_LOG("INDIC: START: (%d) %s", id, wxToStd(name).c_str());

  m_processes.push_back(p);
  RecomputeStateFromProcesses();
}

void ArduinoActivityDotCtrl::StopProcess(int id) {
  if (id <= 0)
    return;

  auto itName = std::find_if(
      m_processes.begin(), m_processes.end(),
      [id](const ActivityProcess &p) { return p.id == id; });

  if (itName == m_processes.end()) {
    APP_DEBUG_LOG("INDIC: STOP: (%d) NOT_FOUND", id);
    return;
  }

  const std::string name = wxToStd(itName->name);
  APP_DEBUG_LOG("INDIC: STOP: (%d) %s", itName->id, name.c_str());

  auto it = std::remove_if(
      m_processes.begin(), m_processes.end(),
      [id](const ActivityProcess &p) { return p.id == id; });

  if (it != m_processes.end()) {
    m_processes.erase(it, m_processes.end());
    RecomputeStateFromProcesses();
  }
}

void ArduinoActivityDotCtrl::StopAllProcesses() {
  if (m_processes.empty())
    return;

  m_processes.clear();
  RecomputeStateFromProcesses();
}

void ArduinoActivityDotCtrl::ApplySettings(const EditorSettings &settings) {
  m_settings = settings;
  Refresh();
}

void ArduinoActivityDotCtrl::RecomputeStateFromProcesses() {
  // We derive the "worst case" state from running processes.
  ArduinoActivityState newState = ArduinoActivityState::Idle;

  bool hasBackground = false;
  bool hasBusy = false;

  for (const auto &p : m_processes) {
    if (p.state == ArduinoActivityState::Busy) {
      hasBusy = true;
    } else if (p.state == ArduinoActivityState::Background) {
      hasBackground = true;
    }
  }

  if (hasBusy) {
    newState = ArduinoActivityState::Busy;
  } else if (hasBackground) {
    newState = ArduinoActivityState::Background;
  } else {
    newState = ArduinoActivityState::Idle;
  }

  if (newState != m_state) {
    m_state = newState;
    m_pulsePhase = 0;
  }

  UpdateTooltip();
  StartTimerIfNeeded();
  Refresh();
  Update();
}

void ArduinoActivityDotCtrl::EnablePulse(bool enable) {
  if (m_pulseEnabled == enable)
    return;

  m_pulseEnabled = enable;
  StartTimerIfNeeded();
  Refresh();
}

void ArduinoActivityDotCtrl::OnPaint(wxPaintEvent &WXUNUSED(event)) {
  wxAutoBufferedPaintDC dc(this);

#ifdef __WXMSW__
  dc.SetBackground(wxBrush(GetBackgroundColour()));
#else
  dc.SetBackground(*wxTRANSPARENT_BRUSH);
#endif

  dc.Clear();

  const wxSize size = GetClientSize();
  const int w = size.GetWidth();
  const int h = size.GetHeight();
  if (w <= 0 || h <= 0)
    return;

  const int radius = GetDotRadius();
  const int cx = w / 2;
  const int cy = h / 2;

  wxColour baseColour = GetDotColour();

  unsigned char alpha = 255;
  if (m_pulseEnabled && m_state != ArduinoActivityState::Idle) {
    const double t = (m_pulsePhase % 20) / 20.0; // 0..1
    const double s = 0.6 + 0.4 * std::sin(t * 2.0 * M_PI);
    alpha = static_cast<unsigned char>(255 * s);
  }

  wxBrush brush(baseColour);
  brush.SetStyle(wxBRUSHSTYLE_SOLID);

  dc.SetPen(*wxTRANSPARENT_PEN);
  dc.SetBrush(brush);

#if wxCHECK_VERSION(3, 1, 0)
  dc.SetBrush(wxBrush(wxColour(baseColour.Red(),
                               baseColour.Green(),
                               baseColour.Blue(),
                               alpha)));
#endif

  dc.DrawCircle(cx, cy, radius);
}

void ArduinoActivityDotCtrl::OnSize(wxSizeEvent &event) {
  event.Skip();
  Refresh();
}

void ArduinoActivityDotCtrl::OnTimer(wxTimerEvent &WXUNUSED(event)) {
  if (!m_pulseEnabled || m_state == ArduinoActivityState::Idle) {
    StopTimerIfNeeded();
    return;
  }

  ++m_pulsePhase;
  if (m_pulsePhase > 1000000) {
    m_pulsePhase = 0;
  }

  Refresh(false);
}

// ----------------------------------------------------------
// Tooltips & colors
// ----------------------------------------------------------

void ArduinoActivityDotCtrl::UpdateTooltip() {
  SetToolTip(BuildTooltipText());
}

wxString ArduinoActivityDotCtrl::BuildTooltipText() const {
  wxString text;

  if (m_processes.empty()) {
    switch (m_state) {
      case ArduinoActivityState::Idle:
        text = _("No background activity");
        break;
      case ArduinoActivityState::Background:
        text = _("Background activity in progress (e.g. AI, parsing)");
        break;
      case ArduinoActivityState::Busy:
        text = _("Busy: heavy operation running");
        break;
    }
    return text;
  }

  text = _("Running tasks:");
  text += wxT("\n");

  for (const auto &p : m_processes) {
    wxString stateStr;
    switch (p.state) {
      case ArduinoActivityState::Idle:
        stateStr = _("Idle");
        break;
      case ArduinoActivityState::Background:
        stateStr = _("Background");
        break;
      case ArduinoActivityState::Busy:
        stateStr = _("Busy");
        break;
    }

    text += wxT("  ");
    text += stateStr;
    text += wxT(": ");
    text += p.name;
    text += wxT("\n");
  }

  return text;
}

wxColour ArduinoActivityDotCtrl::GetDotColour() const {
  EditorColorScheme cs = m_settings.GetColors();

  switch (m_state) {
    case ArduinoActivityState::Idle:
      return cs.symbolHighlight;
    case ArduinoActivityState::Background:
      return cs.warning;
    case ArduinoActivityState::Busy:
      return cs.error;
  }

  return *wxLIGHT_GREY;
}

int ArduinoActivityDotCtrl::GetDotRadius() const {
  const wxSize size = GetClientSize();
  const int d = std::min(size.GetWidth(), size.GetHeight());
  // Small margin from borders
  return std::max(2, (d - 4) / 2);
}

void ArduinoActivityDotCtrl::StartTimerIfNeeded() {
  if (!m_pulseEnabled) {
    StopTimerIfNeeded();
    return;
  }

  if (m_state == ArduinoActivityState::Idle) {
    StopTimerIfNeeded();
    return;
  }

  if (!m_timer.IsRunning()) {
    m_timer.Start(ACTIVITY_TIMER_INTERVAL_MS);
  }
}

void ArduinoActivityDotCtrl::StopTimerIfNeeded() {
  if (m_timer.IsRunning()) {
    m_timer.Stop();
  }
}

// ----------------------------------------------------------
// ArduinoActivityScope
// ----------------------------------------------------------

ArduinoActivityScope::ArduinoActivityScope(ArduinoActivityDotCtrl *ctrl,
                                           ArduinoActivityState state,
                                           int id,
                                           const wxString &name)
    : m_ctrl(ctrl), m_id(id) {
  if (m_ctrl) {
    m_ctrl->StartProcess(name, id, state);
  }
}

ArduinoActivityScope::ArduinoActivityScope(ArduinoActivityDotCtrl *ctrl,
                                           int id,
                                           ArduinoActivityState state)
    : ArduinoActivityScope(ctrl, state, id, _("Background activity")) {}

ArduinoActivityScope::~ArduinoActivityScope() {
  if (m_ctrl && m_id > -1) {
    m_ctrl->StopProcess(m_id);
  }
}
