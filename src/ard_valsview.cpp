#include "ard_valsview.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cmath>

#include <wx/datetime.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

// -------------------------------------------------------------------------------------------------
// ArduinoValuesView::ValueChip
// -------------------------------------------------------------------------------------------------

class ArduinoValuesView::ValueChip : public wxPanel {
public:
  ValueChip(wxWindow *parent,
            const wxString &name,
            const wxFont &baseFont,
            const wxFont &valueFont,
            const wxFont &timeFont)
      : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSIMPLE_BORDER),
        m_name(name) {

    m_nameText = new wxStaticText(this, wxID_ANY, name + wxT(":"));
    m_valueText = new wxStaticText(this, wxID_ANY, wxT("—"));
    m_timeText = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_timeText->Hide();

    m_nameText->SetFont(baseFont);
    m_valueText->SetFont(valueFont);
    m_timeText->SetFont(timeFont);

    auto *row1 = new wxBoxSizer(wxHORIZONTAL);
    row1->Add(m_nameText, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
    row1->Add(m_valueText, 0, wxALIGN_CENTER_VERTICAL, 0);

    auto *root = new wxBoxSizer(wxVERTICAL);
    root->Add(row1, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    root->Add(m_timeText, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

    SetSizer(root);
    root->Fit(this);
    SetMinSize(GetBestSize());
  }

  void SetSample(double value, wxLongLong nowMs) {
    m_value = value;

    if (m_lastUpdateMs != wxLongLong(0)) {
      const wxLongLong dt = nowMs - m_lastUpdateMs;
      if (dt > 0) {
        const double d = (double)dt.GetValue();
        if (m_emaPeriodMs <= 0.0)
          m_emaPeriodMs = d;
        else
          m_emaPeriodMs = (m_emaPeriodMs * 0.8) + (d * 0.2);
      }
    }

    m_lastUpdateMs = nowMs;
    m_dirty = true;
  }

  bool HasSample() const { return m_lastUpdateMs != wxLongLong(0); }

  wxLongLong GetLastUpdateMs() const { return m_lastUpdateMs; }

  double GetEmaPeriodMs() const { return m_emaPeriodMs; }

  double GetShowAgeAfterMs() const {
    // Hide noisy age label for fast-updating signals. Show only when value is stale.
    const double ema = m_emaPeriodMs;
    const double derived = (ema > 0.0) ? (2.0 * ema) : 0.0;
    return std::max(2000.0, derived);
  }

  void SetTimeVisible(bool visible) {
    if (visible == m_timeVisible)
      return;
    m_timeVisible = visible;
    m_timeText->Show(visible);
    m_needsFit = true;
    m_dirty = true;
  }

  void SetAgeText(const wxString &s) {
    if (s == m_lastAgeText)
      return;
    m_lastAgeText = s;
    m_timeText->SetLabel(s);
    m_needsFit = true;
    m_dirty = true;
  }

  void SetValueText(const wxString &s) {
    if (s == m_lastValueText)
      return;
    m_lastValueText = s;
    m_valueText->SetLabel(s);
    m_needsFit = true;
    m_dirty = true;
  }

  void SetTextColor(const wxColour &c) {
    if (m_lastText.IsOk() && c == m_lastText)
      return;
    m_lastText = c;
    // m_nameText->SetForegroundColour(c);
    m_valueText->SetForegroundColour(c);
    // m_timeText->SetForegroundColour(c);
    m_dirty = true;
  }

  bool ConsumeDirty() {
    if (!m_dirty)
      return false;
    m_dirty = false;
    return true;
  }

  bool ConsumeNeedsFit() {
    if (!m_needsFit)
      return false;
    m_needsFit = false;
    return true;
  }

private:
  wxString m_name;

  wxStaticText *m_nameText = nullptr;
  wxStaticText *m_valueText = nullptr;
  wxStaticText *m_timeText = nullptr;

  double m_value = 0.0;
  wxLongLong m_lastUpdateMs = 0;

  wxString m_lastValueText;
  wxString m_lastAgeText;
  wxColour m_lastText;
  double m_emaPeriodMs = 0.0;
  bool m_timeVisible = false;
  bool m_needsFit = true;

  bool m_dirty = true;
};

// -------------------------------------------------------------------------------------------------
// ArduinoValuesView
// -------------------------------------------------------------------------------------------------

static wxLongLong NowMs() {
#if wxCHECK_VERSION(3, 1, 0)
  return wxGetUTCTimeMillis();
#else
  const wxDateTime now = wxDateTime::UNow();
  return (wxLongLong)now.GetValue().GetValue();
#endif
}

ArduinoValuesView::ArduinoValuesView(wxWindow *parent,
                                     wxWindowID id,
                                     const wxPoint &pos,
                                     const wxSize &size,
                                     long style,
                                     const wxString &name)
    : wxPanel(parent, id, pos, size, style, name),
      m_timer(this) {

  m_baseColor = wxColour(200, 255, 200);

  m_scroller = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxHSCROLL | wxVSCROLL | wxBORDER_NONE);
  m_scroller->SetScrollRate(10, 10);

  m_scroller->SetAutoLayout(false);
  m_scroller->Bind(wxEVT_SIZE, &ArduinoValuesView::OnScrollerSize, this);

  auto *root = new wxBoxSizer(wxVERTICAL);
  root->Add(m_scroller, 1, wxEXPAND, 0);
  SetSizer(root);

  // Ensure a visible height even when empty; parent sizer uses our best/min size.
  SetMinSize(wxSize(-1, FromDIP(60)));

  Bind(wxEVT_TIMER, &ArduinoValuesView::OnTimer, this);
  m_timer.Start(100);
}

std::string ArduinoValuesView::KeyFromName(const wxString &name) {
  return wxToStd(name);
}

ArduinoValuesView::ValueChip *ArduinoValuesView::GetOrCreateChip(const wxString &name) {
  const std::string key = KeyFromName(name);
  auto it = m_chips.find(key);
  if (it != m_chips.end())
    return it->second;

  const wxFont baseFont = GetFont();

  wxFont valueFont = baseFont;
  valueFont.SetWeight(wxFONTWEIGHT_BOLD);

  wxFont timeFont = baseFont;
  const int pts = std::max(6, baseFont.GetPointSize() - 2);
  timeFont.SetPointSize(pts);

  auto *chip = new ValueChip(m_scroller, name, baseFont, valueFont, timeFont);
  chip->Layout();
  chip->Fit();
  chip->SetMinSize(chip->GetBestSize());
  chip->SetSize(chip->GetBestSize());

  m_order.push_back(chip);
  m_chips.emplace(key, chip);

  RelayoutChips();
  Layout();

  // Our parent sizer may have given us a near-zero height when we were empty.
  // After the first chip is created, force a relayout up the chain.
  InvalidateBestSize();
  if (wxWindow *p = GetParent()) {
    p->Layout();
    p->SendSizeEvent();
  }

  return chip;
}

void ArduinoValuesView::AddSample(const wxString &name, double value, bool refresh) {
  APP_TRACE_LOG("AVW: AddSample (%s, %f, %d)", wxToStd(name).c_str(), value, refresh);
  if (name.empty())
    return;

  ValueChip *chip = GetOrCreateChip(name);
  const wxLongLong nowMs = NowMs();

  chip->SetSample(value, nowMs);
  chip->SetValueText(FormatValue(value));
  // Age text is updated by timer (and shown only when stale).
  chip->SetTimeVisible(false);
  chip->SetAgeText(wxEmptyString);
  chip->SetTextColor(ComputeFadeColor(0.0));

  if (refresh) {
    // Refresh the chip and keep the scroller's virtual size in sync.    if (chip->ConsumeNeedsFit()) {
    chip->Layout();
    chip->Fit();
    chip->SetMinSize(chip->GetBestSize());
  }

  chip->Refresh(false);
  RelayoutChips();
  Layout();
  if (wxWindow *p = GetParent()) {
    p->Layout();
  }
}

void ArduinoValuesView::SetFadeFactor(double fadeFactorMs) {
  if (fadeFactorMs == 0.0)
    fadeFactorMs = 1.0;
  m_fadeFactorMs = fadeFactorMs;
}

void ArduinoValuesView::SetFadeBaseColor(const wxColour &c) {
  if (!c.IsOk())
    return;
  m_baseColor = c;
}

void ArduinoValuesView::Clear() {
  for (auto &kv : m_chips) {
    if (kv.second)
      kv.second->Destroy();
  }
  m_chips.clear();

  m_order.clear();
  RelayoutChips();

  InvalidateBestSize();
  if (wxWindow *p = GetParent()) {
    p->Layout();
    p->SendSizeEvent();
  }
}

wxString ArduinoValuesView::FormatValue(double value) {
  const double r = std::round(value);
  if (std::isfinite(value) && std::fabs(value - r) < 1e-9) {
    return wxString::Format(wxT("%lld"), (long long)r);
  }
  return wxString::Format(wxT("%.6g"), value);
}

wxString ArduinoValuesView::FormatAge(double ageMs) {
  if (ageMs < 0)
    ageMs = 0;

  if (ageMs <= 999.0) {
    return wxString::Format(wxT("%.0f ms"), ageMs);
  }

  const double sec = ageMs / 1000.0;
  if (sec < 60.0) {
    if (sec < 10.0)
      return wxString::Format(wxT("%.2f s"), sec);
    return wxString::Format(wxT("%.1f s"), sec);
  }

  const double min = sec / 60.0;
  if (min < 10.0)
    return wxString::Format(wxT("%.2f min"), min);
  return wxString::Format(wxT("%.1f min"), min);
}

wxColour ArduinoValuesView::ComputeFadeColor(double ageMs) const {
  const double span = std::max(1.0, std::fabs(m_fadeFactorMs));
  double t = ageMs / span;
  if (t < 0.0)
    t = 0.0;
  if (t > 1.0)
    t = 1.0;

  const int r0 = m_baseColor.Red();
  const int g0 = m_baseColor.Green();
  const int b0 = m_baseColor.Blue();

  auto lerp = [](int a, int b, double t) -> int {
    const double v = (double)a + ((double)b - (double)a) * t;
    int iv = (int)std::lround(v);
    if (iv < 0)
      iv = 0;
    if (iv > 255)
      iv = 255;
    return iv;
  };

  const double k = std::min(1.0, std::max(0.0, m_fadeStrength * t));

  if (m_fadeFactorMs > 0.0) {
    // Darken toward black
    return wxColour(lerp(r0, 0, k), lerp(g0, 0, k), lerp(b0, 0, k));
  }

  // Lighten toward white
  return wxColour(lerp(r0, 255, k), lerp(g0, 255, k), lerp(b0, 255, k));
}

void ArduinoValuesView::OnScrollerSize(wxSizeEvent &e) {
  e.Skip();
  RelayoutChips();
}

void ArduinoValuesView::RelayoutChips() {
  if (!m_scroller)
    return;

  const wxSize client = m_scroller->GetClientSize();
  const int cw = std::max(1, client.GetWidth());

  const int pad = FromDIP(4);
  const int gap = FromDIP(6);

  int x = pad;
  int y = pad;
  int rowH = 0;

  // Place chips left-to-right with wrapping. Chips are sized to their best size.
  for (ValueChip *chip : m_order) {
    if (!chip || !chip->IsShown())
      continue;

    // Ensure the chip has an up-to-date best size
    chip->Layout();
    const wxSize bs = chip->GetBestSize();

    const int w = std::max(FromDIP(40), bs.GetWidth());
    const int h = std::max(FromDIP(24), bs.GetHeight());

    if (x != pad && x + w > cw - pad) {
      // Wrap to next row
      x = pad;
      y += rowH + gap;
      rowH = 0;
    }

    chip->SetSize(x, y, w, h);

    x += w + gap;
    rowH = std::max(rowH, h);
  }

  const int totalH = y + rowH + pad;
  m_scroller->SetVirtualSize(wxSize(cw, std::max(totalH, FromDIP(10))));
  m_scroller->Refresh(false);
}

void ArduinoValuesView::OnTimer(wxTimerEvent &e) {
  (void)e;

  if (m_chips.empty())
    return;

  const wxLongLong nowMs = NowMs();
  bool anyDirty = false;

  for (auto &kv : m_chips) {
    ValueChip *chip = kv.second;
    if (!chip || !chip->HasSample())
      continue;

    const wxLongLong last = chip->GetLastUpdateMs();
    const wxLongLong ageLL = nowMs - last;
    const double ageMs = std::max(0.0, (double)ageLL.GetValue());

    // Fade is applied to text color (not background).
    chip->SetTextColor(ComputeFadeColor(ageMs));

    // Show age only when value is "stale" relative to its typical update period.
    const bool showAge = (ageMs >= chip->GetShowAgeAfterMs());
    chip->SetTimeVisible(showAge);
    if (showAge)
      chip->SetAgeText(FormatAge(ageMs));
    else
      chip->SetAgeText(wxEmptyString);

    const bool dirty = chip->ConsumeDirty();
    const bool needFit = chip->ConsumeNeedsFit();
    if (dirty || needFit) {
      if (wxSizer *sz = chip->GetSizer())
        sz->Layout();
      chip->Fit();
      chip->SetMinSize(chip->GetBestSize());
      chip->Layout();
      chip->Fit();
      chip->SetMinSize(chip->GetBestSize());
      chip->SetSize(chip->GetBestSize());
      chip->Refresh(false);
      anyDirty = true;
    }
  }

  if (anyDirty) {
    RelayoutChips();
    Layout();
  }
}
