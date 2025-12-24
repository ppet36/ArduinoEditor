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
