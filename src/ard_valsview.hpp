#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/panel.h>
#include <wx/timer.h>

class wxScrolledWindow;

class ArduinoValuesView : public wxPanel {
public:
  ArduinoValuesView(wxWindow *parent,
                    wxWindowID id = wxID_ANY,
                    const wxPoint &pos = wxDefaultPosition,
                    const wxSize &size = wxDefaultSize,
                    long style = wxTAB_TRAVERSAL,
                    const wxString &name = wxT("ArduinoValuesView"));

  // API analogous to ArduinoPlotView (but simplified).
  void AddSample(const wxString &name, double value, bool refresh = true);

  // Fade factor in milliseconds. Absolute value defines the age at which fading saturates.
  // Sign controls direction: >0 darkens with age, <0 lightens with age.
  void SetFadeFactor(double fadeFactorMs);

  // Base color used for the freshest values (before fading).
  void SetFadeBaseColor(const wxColour &c);

  // Optional: clear all values.
  void Clear();

private:
  class ValueChip;

  ValueChip *GetOrCreateChip(const wxString &name);

  // Periodic UI upkeep: update relative times & faded colors even when no new samples arrive.
  void OnTimer(wxTimerEvent &e);
  void OnSysColourChanged(wxSysColourChangedEvent &event);
  void ApplyColorScheme();

  // Helpers
  static wxString FormatValue(double value);
  static wxString FormatAge(double ageMs);
  wxColour ComputeFadeColor(double ageMs) const;

  // Keying helper (stable across wxString lifetime).
  static std::string KeyFromName(const wxString &name);

private:
  wxScrolledWindow *m_scroller = nullptr;

  std::unordered_map<std::string, ValueChip *> m_chips;
  std::vector<ValueChip *> m_order;

  void RelayoutChips();
  void OnScrollerSize(wxSizeEvent &e);

  wxTimer m_timer;

  wxColour m_baseColor;
  double m_fadeFactorMs = 3000.0; // default: 3s to fully fade
  double m_fadeStrength = 0.65;   // 0..1 portion to fade towards black/white
};
