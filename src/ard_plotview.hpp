#pragma once

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/spinctrl.h>
#include <wx/wx.h>

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ArduinoPlotView : public wxPanel {
public:
  struct Sample {
    double t_ms;  // time in milliseconds since view start
    double value; // numeric value
  };

  explicit ArduinoPlotView(wxWindow *parent,
                           wxWindowID id = wxID_ANY,
                           const wxPoint &pos = wxDefaultPosition,
                           const wxSize &size = wxDefaultSize,
                           long style = wxTAB_TRAVERSAL);

  // Add sample with current time
  void AddSample(const wxString &name, double value, bool refresh = true);
  void AddSample(const wxString &name, long value, bool refresh = true) { AddSample(name, (double)value, refresh); }

  // Add sample with explicit timestamp (ms since view start)
  void AddSampleAt(const wxString &name, double value, double t_ms, bool refresh = true);

  double GetCurrentTime() const { return (double)m_clock.Time(); }

  void Clear();

  // Visible time window (right edge is "now")
  void SetTimeWindowMs(double window_ms);
  double GetTimeWindowMs() const { return m_timeWindowMs; }

  // Hard cap per series (0 = unlimited)
  void SetMaxSamplesPerSeries(size_t maxSamples);
  size_t GetMaxSamplesPerSeries() const { return m_maxSamplesPerSeries; }

  // Optional: fixed Y range (if disabled, auto-scale based on visible data)
  void SetFixedYRange(bool enabled, double yMin = 0.0, double yMax = 1.0);
  bool HasFixedYRange() const { return m_fixedYRange; }
  void GetFixedYRange(double *yMin, double *yMax);

  // --- Visibility control (for config persistence) ---
  // Returns list of series names that are currently hidden.
  std::vector<wxString> GetHiddenSignals() const;

  // Sets which series are hidden. Works even if series don't exist yet
  // (will be applied when they appear).
  void SetHiddenSignals(const std::vector<wxString> &hiddenNames);

private:
  struct Series {
    wxString name;
    wxColour color;
    std::deque<Sample> samples;
    bool visible = true;
  };

  struct LegendHit {
    wxRect rect;         // checkbox hit box
    std::string keyUtf8; // series key
  };

  int ModalMsgDialog(const wxString &message, const wxString &caption = _("Error"), int styles = wxOK | wxICON_ERROR);

  // Events
  void OnPaint(wxPaintEvent &evt);
  void OnSize(wxSizeEvent &evt);
  void OnLeftDown(wxMouseEvent &evt);
  void OnTimeWindowSpin(wxSpinEvent &evt);
  void OnTimeWindowTextEnter(wxCommandEvent &evt);
  void OnTimeWindowKillFocus(wxFocusEvent &evt);
  void CommitTimeWindowFromText(const wxString &text);
  void OnContextMenu(wxContextMenuEvent &evt);
  void OnMenuFixedYRange(wxCommandEvent &evt);
  void OnMenuClear(wxCommandEvent &evt);
  void OnMenuSaveImage(wxCommandEvent &evt);

  // Refresh
  wxTimer m_refreshTimer;
  bool m_refreshScheduled = false;
  void OnRefreshTimer(wxTimerEvent &);
  void RequestRefresh();

  // Rendering
  void Draw(wxGraphicsContext &gc, const wxRect &rcClient);
  void DrawAxesAndGrid(wxGraphicsContext &gc, const wxRect &rcPlot,
                       double t0, double t1, double y0, double y1);
  void DrawSeries(wxGraphicsContext &gc, const wxRect &rcPlot,
                  const Series &s, double t0, double t1, double y0, double y1);
  void DrawLegend(wxGraphicsContext &gc, const wxRect &rcClient);

  // Layout helpers
  wxRect ComputePlotRect(const wxRect &rcClient) const;
  void LayoutControls();

  // Data helpers
  Series &GetOrCreateSeries(const wxString &name);
  wxColour NextColor();
  void TrimSeries(Series &s, double now_ms);

  bool ComputeVisibleRanges(double now_ms, double &t0, double &t1, double &y0, double &y1) const;

  // Formatting helpers
  static wxString FormatSeconds(double t_ms);
  static wxString FormatNumber(double v);

private:
  std::unordered_map<std::string, Series> m_series;
  std::vector<std::string> m_seriesOrder; // stable legend + draw ordering

  std::vector<wxColour> m_palette;
  size_t m_nextColorIndex = 0;

  wxStopWatch m_clock;

  double m_timeWindowMs = 10000.0; // default 10s visible window
  size_t m_maxSamplesPerSeries = 0;

  bool m_fixedYRange = false;
  double m_fixedYMin = 0.0;
  double m_fixedYMax = 1.0;

  // Controls
  wxSpinCtrl *m_spinWindowSec = nullptr;
  wxStaticText *m_spinSuffix = nullptr;
  bool m_updatingControls = false;

  // Legend interactivity
  std::vector<LegendHit> m_legendHits;

  // Persistent hidden set (keys). Applied to future series as they appear.
  std::unordered_set<std::string> m_hiddenDesiredKeys;
};
