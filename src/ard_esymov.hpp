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

#include <map>
#include <vector>
#include <wx/panel.h>
#include <wx/stc/stc.h>

// Symbol/position overview bar at left side of editor
class SymbolOverviewBar : public wxPanel {
public:
  SymbolOverviewBar(wxWindow *parent, wxStyledTextCtrl *editor);

  void RegisterMarker(int id, const wxColour &color);

  void ClearMarker(int id);

  void SetMarkers(int id, const std::vector<int> &lines);

  void AddMarker(int id, int line);

private:
  struct MarkerData {
    wxColour color;
    std::vector<int> lines;
  };

  wxStyledTextCtrl *m_editor;
  std::map<int, MarkerData> m_markers;

  void OnPaint(wxPaintEvent &evt);
  void OnMouseLeftDown(wxMouseEvent &evt);
};
