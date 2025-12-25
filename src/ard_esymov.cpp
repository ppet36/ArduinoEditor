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

#include "ard_esymov.hpp"

#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <wx/sysopt.h>

SymbolOverviewBar::SymbolOverviewBar(wxWindow *parent, wxStyledTextCtrl *editor)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      m_editor(editor) {
  SetMinSize(wxSize(10, -1)); // zk prouek
  SetBackgroundStyle(wxBG_STYLE_PAINT);

  Bind(wxEVT_PAINT, &SymbolOverviewBar::OnPaint, this);
  Bind(wxEVT_LEFT_DOWN, &SymbolOverviewBar::OnMouseLeftDown, this);
}

void SymbolOverviewBar::RegisterMarker(int id, const wxColour &color) {
  MarkerData &md = m_markers[id];
  md.color = color;
  md.lines.clear();
  Refresh();
}

void SymbolOverviewBar::ClearMarker(int id) {
  auto it = m_markers.find(id);
  if (it == m_markers.end())
    return;
  it->second.lines.clear();
  Refresh();
}

void SymbolOverviewBar::SetMarkers(int id, const std::vector<int> &lines) {
  auto it = m_markers.find(id);
  if (it == m_markers.end())
    return;
  it->second.lines = lines;
  Refresh();
}

void SymbolOverviewBar::AddMarker(int id, int line) {
  auto it = m_markers.find(id);
  if (it == m_markers.end())
    return;

  if (line < 0)
    return;

  auto &vec = it->second.lines;
  if (std::find(vec.begin(), vec.end(), line) == vec.end()) {
    vec.push_back(line);
  }

  Refresh();
}

void SymbolOverviewBar::OnPaint(wxPaintEvent &WXUNUSED(evt)) {
  wxAutoBufferedPaintDC dc(this);

  wxSize sz = GetClientSize();
  int w = sz.x;
  int h = sz.y;

  // 1) Background by system/parent (to match the statusbar)
  wxColour bg;
  if (GetParent())
    bg = GetParent()->GetBackgroundColour();
  if (!bg.IsOk())
    bg = wxSystemSettings::GetColour(wxSYS_COLOUR_3DFACE);

  dc.SetBackground(wxBrush(bg));
  dc.Clear();

  if (!m_editor)
    return;

  int totalLines = m_editor->GetLineCount();
  if (totalLines <= 0 || h <= 0)
    return;

  double scale = (double)h / (double)totalLines;

  // 2) Viewport
  int firstVisible = m_editor->GetFirstVisibleLine();
  int visible = m_editor->LinesOnScreen();

  if (visible > 0) {
    int y1 = (int)(firstVisible * scale);
    int y2 = (int)((firstVisible + visible) * scale);
    if (y2 <= y1)
      y2 = y1 + 2;

    wxColour vpCol = bg;
    vpCol = vpCol.ChangeLightness(90);

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(vpCol));
    dc.DrawRectangle(0, y1, w, y2 - y1);
  }

  // 3) Draw markers
  dc.SetPen(*wxTRANSPARENT_PEN);

  for (const auto &pair : m_markers) {
    const MarkerData &md = pair.second;
    if (!md.color.IsOk())
      continue;

    dc.SetBrush(wxBrush(md.color));

    for (int ln : md.lines) {
      if (ln < 0 || ln >= totalLines)
        continue;

      int y = (int)(ln * scale);
      int hMark = std::max(2, (int)scale);
      if (y + hMark > h)
        hMark = h - y;
      if (hMark <= 0)
        continue;

      dc.DrawRectangle(0, y, w, hMark);
    }
  }
}

void SymbolOverviewBar::OnMouseLeftDown(wxMouseEvent &evt) {
  if (!m_editor)
    return;

  wxSize sz = GetClientSize();
  int h = sz.y;
  if (h <= 0)
    return;

  int y = evt.GetY();
  if (y < 0)
    y = 0;
  if (y > h)
    y = h;

  int totalLines = m_editor->GetLineCount();
  if (totalLines <= 0)
    return;

  double ratio = (double)y / (double)h;
  int targetLine = (int)(ratio * totalLines);

  if (targetLine < 0)
    targetLine = 0;
  if (targetLine >= totalLines)
    targetLine = totalLines - 1;

  // let's move the caret and try to align it approximately to the middle of the screen
  int pos = m_editor->PositionFromLine(targetLine);
  m_editor->GotoPos(pos);

  int linesOnScreen = m_editor->LinesOnScreen();
  int first = targetLine - linesOnScreen / 2;
  if (first < 0)
    first = 0;
  m_editor->ScrollToLine(first);
}
