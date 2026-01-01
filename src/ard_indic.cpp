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

#include "ard_ev.hpp"
#include <algorithm>
#include <cmath>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>

namespace {

static constexpr int kPulseIntervalMs = 50;

constexpr int MIN_CONTROL_SIZE = 14;

} // namespace

enum {
  ID_MENU_TERMINATE_PROCESS = wxID_HIGHEST + 500,
  ID_PULSE_TIMER
};

ArduinoActivityDotCtrl::ArduinoActivityDotCtrl(wxWindow *parent,
                                               wxWindowID id,
                                               const wxPoint &pos,
                                               const wxSize &size,
                                               long style)
    : wxControl(parent, id, pos, size, style | wxBORDER_NONE),
      m_timer(this, ID_PULSE_TIMER) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);

  if (!size.IsFullySpecified()) {
    SetMinSize(wxSize(MIN_CONTROL_SIZE, MIN_CONTROL_SIZE));
  }

  SetBackgroundColour(parent->GetBackgroundColour());

  // Event binding
  Bind(wxEVT_PAINT, &ArduinoActivityDotCtrl::OnPaint, this);
  Bind(wxEVT_SIZE, &ArduinoActivityDotCtrl::OnSize, this);
  Bind(wxEVT_TIMER, &ArduinoActivityDotCtrl::OnTimer, this, ID_PULSE_TIMER);
  Bind(wxEVT_LEFT_UP, &ArduinoActivityDotCtrl::OnMouseClick, this);
  Bind(wxEVT_RIGHT_UP, &ArduinoActivityDotCtrl::OnMouseClick, this);

  SetBackgroundStyle(wxBG_STYLE_PAINT);

  UpdateTooltip();
  UpdatePulseTimer();
}

void ArduinoActivityDotCtrl::SetState(ArduinoActivityState state) {
  if (m_state == state && m_processes.empty())
    return;

  m_state = state;
  m_pulsePhase = 0;
  AfterStateChanged();
}

// ----------------------------------------------------------
// Process management
// ----------------------------------------------------------

void ArduinoActivityDotCtrl::StartProcess(const wxString &name, int id, ArduinoActivityState state, bool canBeTerminated) {
  // It makes no sense to run an "Idle" task -> remap it to Background
  if (state == ArduinoActivityState::Idle) {
    state = ArduinoActivityState::Background;
  }

  ActivityProcess p;
  p.id = id;
  p.name = name;
  p.state = state;
  p.canBeTerminated = canBeTerminated;

  APP_DEBUG_LOG("INDIC: START: (%d) %s", id, wxToStd(name).c_str());

  m_processes.push_back(p);
  RecomputeStateFromProcesses();
  AfterStateChanged();
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
  }

  RecomputeStateFromProcesses();
  AfterStateChanged();
}

void ArduinoActivityDotCtrl::StopAllProcesses() {
  if (m_processes.empty())
    return;

  m_processes.clear();
  RecomputeStateFromProcesses();
}

bool ArduinoActivityDotCtrl::ShouldPulse() const {
  return m_pulseEnabled && (m_state == ArduinoActivityState::Busy);
}

void ArduinoActivityDotCtrl::UpdatePulseTimer() {
  const bool want = ShouldPulse();

  if (want) {
    if (!m_timer.IsRunning()) {
      m_pulsePhase = 0;
      m_timer.Start(kPulseIntervalMs);
    }
  } else {
    if (m_timer.IsRunning())
      m_timer.Stop();
    m_pulsePhase = 0;
  }
}

void ArduinoActivityDotCtrl::AfterStateChanged() {
  UpdateTooltip();
  UpdatePulseTimer();
  Refresh(false);
}

void ArduinoActivityDotCtrl::RecomputeStateFromProcesses() {
  ArduinoActivityState newState = ArduinoActivityState::Idle;
  bool hasBackground = false;
  bool hasBusy = false;

  for (const auto &p : m_processes) {
    if (p.state == ArduinoActivityState::Busy)
      hasBusy = true;
    else if (p.state == ArduinoActivityState::Background)
      hasBackground = true;
  }

  if (hasBusy)
    newState = ArduinoActivityState::Busy;
  else if (hasBackground)
    newState = ArduinoActivityState::Background;
  else
    newState = ArduinoActivityState::Idle;

  if (newState != m_state) {
    m_state = newState;
    m_pulsePhase = 0;
  }
}

void ArduinoActivityDotCtrl::EnablePulse(bool enable) {
  if (m_pulseEnabled == enable)
    return;
  m_pulseEnabled = enable;
  AfterStateChanged();
}

void ArduinoActivityDotCtrl::ApplySettings(const EditorSettings &settings) {
  m_settings = settings;
  AfterStateChanged();
}

void ArduinoActivityDotCtrl::OnPaint(wxPaintEvent &WXUNUSED(event)) {
  wxAutoBufferedPaintDC pdc(this);

  // Na GTK/mac je transparentní background často zabiják pro alpha animace.
  pdc.SetBackground(wxBrush(GetBackgroundColour()));
  pdc.Clear();

#if wxUSE_GRAPHICS_CONTEXT
  wxGCDC dc(pdc); // zajistí správné alpha blending
#else
  wxDC &dc = pdc; // fallback
#endif

  const wxSize size = GetClientSize();
  const int w = size.GetWidth();
  const int h = size.GetHeight();
  if (w <= 0 || h <= 0)
    return;

  const int baseRadius = GetDotRadius();
  const int cx = w / 2;
  const int cy = h / 2;

  wxColour baseColour = GetDotColour();

  unsigned char alpha = 255;
  int radius = baseRadius;

  if (m_pulseEnabled) {
    const double t = (m_pulsePhase % 20) / 20.0;           // 0..1
    const double s = 0.6 + 0.4 * std::sin(t * 2.0 * M_PI); // 0.2..1.0-ish

    alpha = static_cast<unsigned char>(160 + 95 * s);
    radius = baseRadius + static_cast<int>(std::lround(2.0 * s));
  }

  wxColour c(baseColour.Red(), baseColour.Green(), baseColour.Blue(), alpha);

  dc.SetPen(*wxTRANSPARENT_PEN);
  dc.SetBrush(wxBrush(c));
  dc.DrawCircle(cx, cy, radius);
}

void ArduinoActivityDotCtrl::OnSize(wxSizeEvent &event) {
  event.Skip();
  Refresh();
}

void ArduinoActivityDotCtrl::OnTimer(wxTimerEvent &WXUNUSED(event)) {
  APP_TRACE_LOG("INDIC: Pulse tick phase=%d", (int)m_pulsePhase);

  if (!m_pulseEnabled || m_state == ArduinoActivityState::Idle) {
    return;
  }

  ++m_pulsePhase;
  if (m_pulsePhase > 1000000) {
    m_pulsePhase = 0;
  }

  Refresh(false);
}

void ArduinoActivityDotCtrl::OnMouseClick(wxMouseEvent &event) {
  const bool showPopup = std::any_of(
      m_processes.begin(), m_processes.end(),
      [](const ActivityProcess &p) { return p.canBeTerminated; });

  if (!showPopup) {
    event.Skip();
    return;
  }

  wxMenu menu;
  menu.Append(ID_MENU_TERMINATE_PROCESS, _("Terminate current process"));

  Bind(wxEVT_MENU, &ArduinoActivityDotCtrl::OnPopupMenu, this);

  PopupMenu(&menu, event.GetPosition());

  Unbind(wxEVT_MENU, &ArduinoActivityDotCtrl::OnPopupMenu, this);
  event.Skip();
}

void ArduinoActivityDotCtrl::OnPopupMenu(wxCommandEvent &event) {
  switch (event.GetId()) {
    case ID_MENU_TERMINATE_PROCESS: {
      wxCommandEvent evt(EVT_PROCESS_TERMINATE_REQUEST);
      wxPostEvent(GetParent(), evt);
      break;
    }

    default:
      break;
  }
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
      return cs.note;
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
