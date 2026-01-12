#include "ard_valsview.hpp"
#include "utils.hpp"
#include "ard_setdlg.hpp"
#include "ard_ap.hpp"

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
            const wxFont &valueFont)
      : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSIMPLE_BORDER),
        m_name(name) {

    m_nameText = new wxStaticText(this, wxID_ANY, name + wxT(":"));
    m_valueText = new wxStaticText(this, wxID_ANY, wxT("—"));

    m_nameText->SetFont(baseFont);
    m_valueText->SetFont(valueFont);

    auto *root = new wxBoxSizer(wxHORIZONTAL);
    root->Add(m_nameText, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM | wxALIGN_CENTER_VERTICAL, 6);
    root->Add(m_valueText, 0, wxRIGHT | wxTOP | wxBOTTOM | wxALIGN_CENTER_VERTICAL, 6);

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
    m_tooltipDirty = true;
    m_dirty = true;
  }

  bool HasSample() const { return m_lastUpdateMs != wxLongLong(0); }
  wxLongLong GetLastUpdateMs() const { return m_lastUpdateMs; }
  double GetEmaPeriodMs() const { return m_emaPeriodMs; }

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
    m_valueText->SetForegroundColour(c);
    m_dirty = true;
  }

  void UpdateTooltipIfNeeded(wxLongLong nowMs, double ageMs) {
    if (!m_tooltipDirty && (nowMs - m_lastTooltipUpdateMs) < wxLongLong(1000))
      return;
    if ((nowMs - m_lastTooltipUpdateMs) < wxLongLong(1000))
      return;

    const wxString age = ArduinoValuesView::FormatAge(ageMs);
    const wxString tip = wxString::Format(_("Last update: %s ago"), age);

    if (tip == m_lastTooltip && !m_tooltipDirty) {
      m_lastTooltipUpdateMs = nowMs;
      return;
    }

    m_lastTooltip = tip;
    m_lastTooltipUpdateMs = nowMs;
    m_tooltipDirty = false;

    SetToolTip(tip);
    if (m_nameText)
      m_nameText->SetToolTip(tip);
    if (m_valueText)
      m_valueText->SetToolTip(tip);
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

  double m_value = 0.0;
  wxLongLong m_lastUpdateMs = 0;

  wxString m_lastValueText;
  wxColour m_lastText;
  double m_emaPeriodMs = 0.0;

  bool m_needsFit = true;
  bool m_dirty = true;

  // Tooltip throttling
  bool m_tooltipDirty = true;
  wxLongLong m_lastTooltipUpdateMs = 0;
  wxString m_lastTooltip;
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

  m_scroller = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxHSCROLL | wxVSCROLL | wxBORDER_NONE);
  m_scroller->SetScrollRate(10, 10);

  m_scroller->SetAutoLayout(false);
  m_scroller->Bind(wxEVT_SIZE, &ArduinoValuesView::OnScrollerSize, this);

  Bind(wxEVT_SYS_COLOUR_CHANGED, &ArduinoValuesView::OnSysColourChanged, this);

  auto *root = new wxBoxSizer(wxVERTICAL);
  root->Add(m_scroller, 1, wxEXPAND, 0);
  SetSizer(root);

  ApplyColorScheme();

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

  auto *chip = new ValueChip(m_scroller, name, baseFont, valueFont);
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

  // Tooltip with last-update age is updated by timer (throttled to ~1 Hz).
  chip->SetTextColor(ComputeFadeColor(0.0));

  if (!refresh)
    return;

  const bool needFit = chip->ConsumeNeedsFit();
  if (needFit) {
    chip->Layout();
    chip->Fit();
    chip->SetMinSize(chip->GetBestSize());
  }

  chip->Refresh(false);
  if (needFit)
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

  // 0..999 ms
  if (ageMs <= 999.0) {
    const long ms = (long)llround(ageMs);
    return wxString::Format(_("%ld ms"), ms);
  }

  // Work in whole seconds from here (stable + readable).
  const long totalSec = (long)llround(ageMs / 1000.0);

  // 1..59 s
  if (totalSec < 60) {
    return wxString::Format(_("%ld sec"), totalSec);
  }

  // 1:00 .. 59:59  -> m:ssmin
  const long totalMin = totalSec / 60;
  const long sec = totalSec % 60;

  if (totalMin < 60) {
    return wxString::Format(_("%ld:%02ld min"), totalMin, sec);
  }

  // 60:00+ -> h:mmhod
  const long totalHr = totalMin / 60;
  const long min = totalMin % 60;
  return wxString::Format(_("%ld:%02ld hour"), totalHr, min);
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
  bool anyFit = false;
  bool anyRepaint = false;

  for (auto &kv : m_chips) {
    ValueChip *chip = kv.second;
    if (!chip || !chip->HasSample())
      continue;

    const wxLongLong last = chip->GetLastUpdateMs();
    const wxLongLong ageLL = nowMs - last;
    const double ageMs = std::max(0.0, (double)ageLL.GetValue());

    // Fade is applied to text color (not background).
    chip->SetTextColor(ComputeFadeColor(ageMs));

    // Show last-update age in a tooltip; throttle to ~1 Hz per chip.
    chip->UpdateTooltipIfNeeded(nowMs, ageMs);

    const bool dirty = chip->ConsumeDirty();
    const bool needFit = chip->ConsumeNeedsFit();

    if (needFit) {
      if (wxSizer *sz = chip->GetSizer())
        sz->Layout();
      chip->Fit();
      chip->SetMinSize(chip->GetBestSize());
      chip->SetSize(chip->GetBestSize());
      chip->Refresh(false);
      anyFit = true;
      anyRepaint = true;
    } else if (dirty) {
      chip->Refresh(false);
      anyRepaint = true;
    }
  }

  // Only relayout if chip geometry might have changed (i.e. label length changed).
  if (anyFit) {
    RelayoutChips();
    Layout();
  } else if (anyRepaint) {
    m_scroller->Refresh(false);
  }
}

void ArduinoValuesView::ApplyColorScheme() {
  auto *config = wxConfigBase::Get();
  EditorSettings settings;
  settings.Load(config);

  EditorColorScheme scheme = settings.GetColors();
  m_baseColor = scheme.note;
  m_fadeFactorMs = IsDarkMode() ? 3000 : -3000;
}

void ArduinoValuesView::OnSysColourChanged(wxSysColourChangedEvent &event) {
  ApplyColorScheme();
  Refresh();
  event.Skip();
}

