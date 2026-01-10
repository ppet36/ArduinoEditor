#include "ard_plotview.hpp"
#include "ard_ap.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/datetime.h>
#include <wx/dcmemory.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/imagbmp.h>
#include <wx/image.h>
#include <wx/imagjpeg.h>
#include <wx/imagpng.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/richmsgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

//
// NOTE: This widget currently uses a fixed set of colors because there was no desire
// to pair it with customizable user colors. The colors were chosen so that it looks OK
// in both light and dark modes.
//

static std::string ToKey(const wxString &s) {
  // stable key for unordered_map: UTF-8
  return std::string(s.utf8_str());
}

namespace {

// Context menu command IDs (use stable values; they only need to be unique
// within the app).
constexpr int ID_PLOT_CTX_FIXED_Y = wxID_HIGHEST + 27110;
constexpr int ID_PLOT_CTX_SAVE_IMG = wxID_HIGHEST + 27111;
constexpr int ID_PLOT_CTX_CLEAR = wxID_HIGHEST + 27112;

static bool ParseUserDouble(const wxString &text, double &out) {
  wxString s = TrimCopy(text);
  if (s.empty())
    return false;

  // Accept both "," and "." as decimal separators.
  s.Replace(wxT(","), wxT("."));
  return s.ToDouble(&out);
}

class FixedYRangeDialog final : public wxDialog {
public:
  FixedYRangeDialog(wxWindow *parent,
                    bool enabled,
                    double yMin,
                    double yMax)
      : wxDialog(parent, wxID_ANY, _("Fixed Y range"),
                 wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {

    m_chkEnabled = new wxCheckBox(this, wxID_ANY, _("Enable fixed Y range"));
    m_chkEnabled->SetValue(enabled);

    m_editMin = new wxTextCtrl(this, wxID_ANY, wxString::Format(wxT("%.6g"), yMin));
    m_editMax = new wxTextCtrl(this, wxID_ANY, wxString::Format(wxT("%.6g"), yMax));

    auto *grid = new wxFlexGridSizer(2, 2, 8, 8);
    grid->Add(new wxStaticText(this, wxID_ANY, _("Y min:")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_editMin, 1, wxEXPAND);
    grid->Add(new wxStaticText(this, wxID_ANY, _("Y max:")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_editMax, 1, wxEXPAND);
    grid->AddGrowableCol(1, 1);

    auto *vbox = new wxBoxSizer(wxVERTICAL);
    vbox->Add(m_chkEnabled, 0, wxALL, 10);
    vbox->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    vbox->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    SetSizerAndFit(vbox);

    auto updateEnabled = [this]() {
      const bool en = m_chkEnabled->GetValue();
      m_editMin->Enable(en);
      m_editMax->Enable(en);
    };

    m_chkEnabled->Bind(wxEVT_CHECKBOX, [updateEnabled](wxCommandEvent &) { updateEnabled(); });
    updateEnabled();

    Bind(wxEVT_BUTTON, &FixedYRangeDialog::OnOk, this, wxID_OK);

    CentreOnParent();
  }

  bool IsEnabled() const { return m_enabled; }
  double GetMin() const { return m_yMin; }
  double GetMax() const { return m_yMax; }

private:
  void OnOk(wxCommandEvent &) {
    m_enabled = m_chkEnabled->GetValue();

    if (m_enabled) {
      double a = 0.0, b = 0.0;
      if (!ParseUserDouble(m_editMin->GetValue(), a) ||
          !ParseUserDouble(m_editMax->GetValue(), b)) {

        wxRichMessageDialog dlg(this, _("Please enter valid numbers for Y min / Y max."), _("Invalid input"), wxOK | wxICON_WARNING);
        dlg.ShowModal();
        return;
      }

      if (a > b)
        std::swap(a, b);

      if (a == b) {
        // Avoid zero span.
        a -= 1.0;
        b += 1.0;
      }

      m_yMin = a;
      m_yMax = b;
    }

    EndModal(wxID_OK);
  }

  wxCheckBox *m_chkEnabled = nullptr;
  wxTextCtrl *m_editMin = nullptr;
  wxTextCtrl *m_editMax = nullptr;

  bool m_enabled = false;
  double m_yMin = 0.0;
  double m_yMax = 1.0;
};

} // namespace

ArduinoPlotView::ArduinoPlotView(wxWindow *parent,
                                 wxWindowID id,
                                 const wxPoint &pos,
                                 const wxSize &size,
                                 long style)
    : wxPanel(parent, id, pos, size, style), m_refreshTimer(this) {

  // Important for flicker-free buffered painting on all platforms.
  SetBackgroundStyle(wxBG_STYLE_PAINT);

  // Bind místo EVT_* tabulky
  Bind(wxEVT_PAINT, &ArduinoPlotView::OnPaint, this);
  Bind(wxEVT_SIZE, &ArduinoPlotView::OnSize, this);
  Bind(wxEVT_LEFT_DOWN, &ArduinoPlotView::OnLeftDown, this);
  Bind(wxEVT_CONTEXT_MENU, &ArduinoPlotView::OnContextMenu, this);

  Bind(wxEVT_MENU, &ArduinoPlotView::OnMenuFixedYRange, this, ID_PLOT_CTX_FIXED_Y);
  Bind(wxEVT_MENU, &ArduinoPlotView::OnMenuSaveImage, this, ID_PLOT_CTX_SAVE_IMG);
  Bind(wxEVT_MENU, &ArduinoPlotView::OnMenuClear, this, ID_PLOT_CTX_CLEAR);

  // Dont erase background from Refresh()
  Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) { /* no-op */ });

  // delayed refresh
  Bind(wxEVT_TIMER, &ArduinoPlotView::OnRefreshTimer, this);

  // A simple palette that looks OK on dark/light-ish backgrounds.
  m_palette = {
      wxColour(0xE6, 0x3B, 0x2E), // red-ish
      wxColour(0x2E, 0x86, 0xC1), // blue
      wxColour(0x28, 0xB4, 0x63), // green
      wxColour(0xAF, 0x7A, 0xE8), // purple
      wxColour(0xF5, 0xB7, 0x1B), // orange
      wxColour(0x2E, 0xCC, 0xD3), // cyan
      wxColour(0xEC, 0x40, 0x7A), // pink
      wxColour(0x95, 0xA5, 0xA6), // gray
  };

  // Bottom-left control to adjust time window (seconds)
  // NOTE: wxTE_PROCESS_ENTER enables committing typed values via Enter.
  // We intentionally do NOT handle wxEVT_TEXT (live) to avoid fighting GTK's
  // in-progress edits (it would keep resetting the text and feel "not editable").
  m_spinWindowSec = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                   wxDefaultPosition, wxDefaultSize,
                                   wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER);
  m_spinWindowSec->SetRange(1, 600); // 1s..10min
  m_spinWindowSec->SetValue((int)std::lround(m_timeWindowMs / 1000.0));
  m_spinWindowSec->SetToolTip(_("Visible time window (seconds)"));
  m_spinWindowSec->Bind(wxEVT_SPINCTRL, &ArduinoPlotView::OnTimeWindowSpin, this);
  m_spinWindowSec->Bind(wxEVT_TEXT_ENTER, &ArduinoPlotView::OnTimeWindowTextEnter, this);
  m_spinWindowSec->Bind(wxEVT_KILL_FOCUS, &ArduinoPlotView::OnTimeWindowKillFocus, this);

  m_spinSuffix = new wxStaticText(this, wxID_ANY, wxT("s"));

  LayoutControls();
  m_clock.Start();
}

int ArduinoPlotView::ModalMsgDialog(const wxString &message, const wxString &caption, int styles) {
  wxRichMessageDialog dlg(this, message, caption, styles);
  return dlg.ShowModal();
}

void ArduinoPlotView::SetTimeWindowMs(double window_ms) {
  m_timeWindowMs = std::max(100.0, window_ms); // avoid nonsense

  // Sync control (guard against recursion)
  if (m_spinWindowSec && !m_updatingControls) {
    m_updatingControls = true;
    m_spinWindowSec->SetValue((int)std::lround(m_timeWindowMs / 1000.0));
    m_updatingControls = false;
  }

  RequestRefresh();
}

void ArduinoPlotView::SetMaxSamplesPerSeries(size_t maxSamples) {
  m_maxSamplesPerSeries = maxSamples;
  RequestRefresh();
}

void ArduinoPlotView::SetFixedYRange(bool enabled, double yMin, double yMax) {
  m_fixedYRange = enabled;
  m_fixedYMin = yMin;
  m_fixedYMax = yMax;
  RequestRefresh();
}

void ArduinoPlotView::GetFixedYRange(double *yMin, double *yMax) {
  if (yMin) {
    (*yMin) = m_fixedYMin;
  }
  if (yMax) {
    (*yMax) = m_fixedYMax;
  }
}

std::vector<wxString> ArduinoPlotView::GetHiddenSignals() const {
  std::vector<wxString> out;
  out.reserve(m_series.size());

  for (const auto &key : m_seriesOrder) {
    auto it = m_series.find(key);
    if (it == m_series.end())
      continue;
    if (!it->second.visible)
      out.push_back(it->second.name);
  }
  return out;
}

void ArduinoPlotView::SetHiddenSignals(const std::vector<wxString> &hiddenNames) {
  m_hiddenDesiredKeys.clear();
  for (const auto &n : hiddenNames) {
    m_hiddenDesiredKeys.insert(ToKey(n));
  }

  for (auto &kv : m_series) {
    const bool hidden = (m_hiddenDesiredKeys.find(kv.first) != m_hiddenDesiredKeys.end());
    kv.second.visible = !hidden;
  }

  RequestRefresh();
}

void ArduinoPlotView::Clear() {
  m_series.clear();
  m_seriesOrder.clear();
  m_nextColorIndex = 0;
  m_clock.Start();
  RequestRefresh();
}

ArduinoPlotView::Series &ArduinoPlotView::GetOrCreateSeries(const wxString &name) {
  const std::string key = ToKey(name);
  auto it = m_series.find(key);
  if (it != m_series.end())
    return it->second;

  Series s;
  s.name = name;
  s.color = NextColor();

  // Apply persisted hidden state (even if series appears later)
  if (m_hiddenDesiredKeys.find(key) != m_hiddenDesiredKeys.end())
    s.visible = false;

  auto [insIt, ok] = m_series.emplace(key, std::move(s));
  m_seriesOrder.push_back(key);
  return insIt->second;
}

wxColour ArduinoPlotView::NextColor() {
  if (m_palette.empty())
    return *wxWHITE;
  wxColour c = m_palette[m_nextColorIndex % m_palette.size()];
  m_nextColorIndex++;
  return c;
}

void ArduinoPlotView::TrimSeries(Series &s, double now_ms) {
  const double t_min = now_ms - m_timeWindowMs;

  // Keep ONE sample before t_min as an anchor, so the line reaches the left edge
  // (otherwise the first visible point may start later e.g. at -9.5s when sampling is 2Hz).
  while (s.samples.size() >= 2 && s.samples[1].t_ms < t_min) {
    s.samples.pop_front();
  }

  // Hard cap (optional)
  if (m_maxSamplesPerSeries > 0) {
    while (s.samples.size() > m_maxSamplesPerSeries) {
      s.samples.pop_front();
    }
  }
}

void ArduinoPlotView::RequestRefresh() {
  if (m_refreshScheduled)
    return;
  m_refreshScheduled = true;
  m_refreshTimer.StartOnce(250);
}

void ArduinoPlotView::OnRefreshTimer(wxTimerEvent &) {
  m_refreshScheduled = false;
  Refresh(false);
}

void ArduinoPlotView::AddSample(const wxString &name, double value, bool refresh) {
  const double now_ms = (double)m_clock.Time();
  AddSampleAt(name, value, now_ms, refresh);
}

void ArduinoPlotView::AddSampleAt(const wxString &name, double value, double t_ms, bool refresh) {
  Series &s = GetOrCreateSeries(name);
  s.samples.push_back(Sample{t_ms, value});

  // Trim per-series; cheap and keeps memory bounded.
  const double now_ms = (double)m_clock.Time();
  TrimSeries(s, now_ms);

  if (refresh)
    RequestRefresh();
}

wxRect ArduinoPlotView::ComputePlotRect(const wxRect &rcClient) const {
  // Margins for axes labels + legend breathing room.
  const int marginL = 55;
  const int marginR = 12;
  const int marginT = 10;
  const int marginB = 28;

  wxRect rcPlot = rcClient;
  rcPlot.x += marginL;
  rcPlot.y += marginT;
  rcPlot.width -= (marginL + marginR);
  rcPlot.height -= (marginT + marginB);
  rcPlot.width = std::max(1, rcPlot.width);
  rcPlot.height = std::max(1, rcPlot.height);
  return rcPlot;
}

void ArduinoPlotView::LayoutControls() {
  if (!m_spinWindowSec || !m_spinSuffix)
    return;

  const wxRect rcClient = GetClientRect();
  if (rcClient.width <= 10 || rcClient.height <= 10)
    return;

  const wxRect rcPlot = ComputePlotRect(rcClient);

  wxSize spinBest = m_spinWindowSec->GetBestSize();
  const int h = spinBest.y;
  const int w = std::max(60, spinBest.x);

  // Place it where the left-most X label would be (-window)
  int x = rcPlot.x;
  int y = rcPlot.y + rcPlot.height + 2;

  // Clamp into client area
  if (y + h > rcClient.GetBottom())
    y = rcClient.GetBottom() - h;
  if (x + w > rcClient.GetRight())
    x = rcClient.GetRight() - w;

  m_spinWindowSec->SetSize(x, y, w, h);

  wxSize sufBest = m_spinSuffix->GetBestSize();
  m_spinSuffix->SetPosition(wxPoint(x + w + 4, y + (h - sufBest.y) / 2));
}

void ArduinoPlotView::OnSize(wxSizeEvent &evt) {
  evt.Skip();
  LayoutControls();
  RequestRefresh();
}

void ArduinoPlotView::OnPaint(wxPaintEvent &WXUNUSED(evt)) {
  wxAutoBufferedPaintDC dc(this);
  dc.SetBackground(wxBrush(GetBackgroundColour()));
  dc.Clear();

  wxGraphicsContext *gc = wxGraphicsContext::Create(dc);
  if (!gc)
    return;

  const wxRect rc = GetClientRect();
  Draw(*gc, rc);

  delete gc;
}

void ArduinoPlotView::OnLeftDown(wxMouseEvent &evt) {
  const wxPoint p = evt.GetPosition();

  // Check legend checkboxes
  for (const auto &hit : m_legendHits) {
    if (hit.rect.Contains(p)) {
      auto it = m_series.find(hit.keyUtf8);
      if (it != m_series.end()) {
        it->second.visible = !it->second.visible;
        // Keep desired hidden set in sync for future/serialization
        if (!it->second.visible)
          m_hiddenDesiredKeys.insert(hit.keyUtf8);
        else
          m_hiddenDesiredKeys.erase(hit.keyUtf8);
        RequestRefresh();
      }
      return;
    }
  }

  evt.Skip();
}

void ArduinoPlotView::OnContextMenu(wxContextMenuEvent &evt) {
  wxPoint pos = evt.GetPosition();
  if (pos == wxDefaultPosition) {
    // Keyboard invocation (Shift+F10 / Menu key)
    pos = wxGetMousePosition();
  }
  pos = ScreenToClient(pos);

  wxMenu menu;
  AddMenuItemWithArt(&menu,
                     ID_PLOT_CTX_FIXED_Y,
                     _("Fixed Y range..."),
                     wxEmptyString,
                     wxAEArt::Edit);
  AddMenuItemWithArt(&menu,
                     ID_PLOT_CTX_SAVE_IMG,
                     _("Save plot as image..."),
                     wxEmptyString,
                     wxAEArt::FileSave);
  menu.AppendSeparator();
  AddMenuItemWithArt(&menu,
                     ID_PLOT_CTX_CLEAR,
                     _("Clear"),
                     wxEmptyString,
                     wxAEArt::Delete);

  PopupMenu(&menu, pos);
}

void ArduinoPlotView::OnMenuFixedYRange(wxCommandEvent &WXUNUSED(evt)) {
  // Pre-fill with what the user is CURRENTLY seeing.
  const double now_ms = (double)m_clock.Time();
  double t0 = 0, t1 = 0, y0 = 0, y1 = 0;
  if (!ComputeVisibleRanges(now_ms, t0, t1, y0, y1)) {
    y0 = 0.0;
    y1 = 1.0;
  }

  FixedYRangeDialog dlg(this, m_fixedYRange, y0, y1);
  if (dlg.ShowModal() == wxID_OK) {
    if (dlg.IsEnabled())
      SetFixedYRange(true, dlg.GetMin(), dlg.GetMax());
    else
      SetFixedYRange(false);
  }
}

void ArduinoPlotView::OnMenuSaveImage(wxCommandEvent &WXUNUSED(evt)) {
  static bool s_imgHandlersInited = false;
  if (!s_imgHandlersInited) {
    wxImage::AddHandler(new wxPNGHandler);
    wxImage::AddHandler(new wxJPEGHandler);
    wxImage::AddHandler(new wxBMPHandler);
    s_imgHandlersInited = true;
  }

  // IMPORTANT (HiDPI): GetClientSize() returns DIPs (logical units). If we
  // allocate a bitmap using the same numeric size in *pixels* and then draw
  // using DIP coordinates, the backend may apply content scaling (e.g. 2x on
  // Retina) and the right/bottom edge of the plot gets clipped.
  //
  // To export exactly what the user sees, we allocate the bitmap in pixels
  // (DIPs * content scale factor) and scale the graphics context back so that
  // all the existing drawing code continues to use DIP coordinates.
  const wxSize dipSz = GetClientSize();
  if (dipSz.x <= 0 || dipSz.y <= 0)
    return;

  double scale = 1.0;
#if wxCHECK_VERSION(3, 1, 0)
  scale = GetContentScaleFactor();
#endif
  if (scale <= 0.0)
    scale = 1.0;

  const int pxW = std::max(1, (int)std::lround(dipSz.x * scale));
  const int pxH = std::max(1, (int)std::lround(dipSz.y * scale));

  wxBitmap bmp(pxW, pxH, 32);
  if (!bmp.IsOk()) {
    ModalMsgDialog(_("Unable to create bitmap for export."), _("Save plot"));
    return;
  }

  wxMemoryDC mdc;
  mdc.SelectObject(bmp);
  mdc.SetBackground(wxBrush(GetBackgroundColour()));
  mdc.Clear();

  wxGraphicsContext *gc = wxGraphicsContext::Create(mdc);
  if (!gc) {
    mdc.SelectObject(wxNullBitmap);
    ModalMsgDialog(_("Unable to create graphics context for export."), _("Save plot"));
    return;
  }

  // Keep all Draw() math in DIPs.
  if (scale != 1.0)
    gc->Scale(scale, scale);
  Draw(*gc, wxRect(0, 0, dipSz.x, dipSz.y));
  delete gc;
  mdc.SelectObject(wxNullBitmap);

  // Default name: plot_YYYYMMDD_HHMMSS.png (helps when saving multiple snapshots).
  const wxDateTime now = wxDateTime::Now();
  const wxString defName = wxString::Format(wxT("plot_%s.png"), now.Format(wxT("%Y%m%d_%H%M%S")));

  wxFileDialog dlg(this,
                   _("Save plot as image"),
                   wxEmptyString,
                   defName,
                   _("PNG image (*.png)|*.png|JPEG image (*.jpg;*.jpeg)|*.jpg;*.jpeg|BMP image (*.bmp)|*.bmp"),
                   wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

  if (dlg.ShowModal() != wxID_OK)
    return;

  wxString path = dlg.GetPath();
  wxFileName fn(path);

  // Determine output type (prefer extension if present).
  wxBitmapType type = wxBITMAP_TYPE_PNG;
  wxString ext = fn.GetExt().Lower();

  if (ext == wxT("jpg") || ext == wxT("jpeg"))
    type = wxBITMAP_TYPE_JPEG;
  else if (ext == wxT("bmp"))
    type = wxBITMAP_TYPE_BMP;
  else if (ext == wxT("png"))
    type = wxBITMAP_TYPE_PNG;
  else {
    // No / unknown extension → pick from selected filter, and ensure extension exists.
    switch (dlg.GetFilterIndex()) {
      case 1:
        type = wxBITMAP_TYPE_JPEG;
        fn.SetExt(wxT("jpg"));
        break;
      case 2:
        type = wxBITMAP_TYPE_BMP;
        fn.SetExt(wxT("bmp"));
        break;
      default:
        type = wxBITMAP_TYPE_PNG;
        fn.SetExt(wxT("png"));
        break;
    }
    path = fn.GetFullPath();
  }

  wxImage img = bmp.ConvertToImage();
  if (!img.IsOk() || !img.SaveFile(path, type)) {
    ModalMsgDialog(_("Failed to save image file."), _("Save plot"));
    return;
  }
}

void ArduinoPlotView::OnMenuClear(wxCommandEvent &WXUNUSED(evt)) {
  Clear();
}

void ArduinoPlotView::OnTimeWindowSpin(wxSpinEvent &evt) {
  if (m_updatingControls)
    return;
  if (!m_spinWindowSec)
    return;

  const int sec = std::max(1, m_spinWindowSec->GetValue());
  SetTimeWindowMs((double)sec * 1000.0);
  evt.Skip();
}

void ArduinoPlotView::OnTimeWindowTextEnter(wxCommandEvent &evt) {
  if (m_updatingControls)
    return;
  CommitTimeWindowFromText(evt.GetString());
  evt.Skip();
}

void ArduinoPlotView::OnTimeWindowKillFocus(wxFocusEvent &evt) {
  if (!m_spinWindowSec || m_updatingControls) {
    evt.Skip();
    return;
  }

  // Commit whatever the user typed (GTK updates numeric value lazily).
  CommitTimeWindowFromText(m_spinWindowSec->GetTextValue());

  evt.Skip();
}

void ArduinoPlotView::CommitTimeWindowFromText(const wxString &text) {
  if (!m_spinWindowSec)
    return;

  wxString s = TrimCopy(text);

  if (s.empty()) {
    int curSec = (int)std::lround(m_timeWindowMs / 1000.0);
    m_spinWindowSec->SetValue(curSec);
    return;
  }

  long secLong = 0;
  bool ok = s.ToLong(&secLong);

  if (!ok) {
    wxString num;
    num.reserve(s.size());

    for (size_t i = 0; i < s.size(); i++) {
      wxChar c = s[i];
      if ((c == '+' || c == '-') && num.empty()) {
        num.Append(c);
        continue;
      }
      if (wxIsdigit(c)) {
        num.Append(c);
        continue;
      }

      if (!wxIsspace(c))
        break;
    }

    num.Trim(true).Trim(false);
    if (!num.empty())
      ok = num.ToLong(&secLong);
  }

  if (!ok) {
    int curSec = (int)std::lround(m_timeWindowMs / 1000.0);
    m_spinWindowSec->SetValue(curSec);
    return;
  }

  const int minSec = m_spinWindowSec->GetMin();
  const int maxSec = m_spinWindowSec->GetMax();

  int sec = (int)secLong;
  if (sec < minSec)
    sec = minSec;
  if (sec > maxSec)
    sec = maxSec;

  m_spinWindowSec->SetValue(sec);
  SetTimeWindowMs((double)sec * 1000.0);
}

void ArduinoPlotView::Draw(wxGraphicsContext &gc, const wxRect &rcClient) {
  if (rcClient.width <= 10 || rcClient.height <= 10)
    return;

  const wxRect rcPlot = ComputePlotRect(rcClient);

  const double now_ms = (double)m_clock.Time();

  double t0 = 0, t1 = 0, y0 = 0, y1 = 0;
  if (!ComputeVisibleRanges(now_ms, t0, t1, y0, y1)) {
    // Nothing to draw: just a frame.
    gc.SetPen(wxPen(wxColour(120, 120, 120), 1));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.DrawRectangle(rcPlot.x, rcPlot.y, rcPlot.width, rcPlot.height);
    DrawLegend(gc, rcClient);
    return;
  }

  DrawAxesAndGrid(gc, rcPlot, t0, t1, y0, y1);

  // Draw each series (stable order)
  for (const auto &key : m_seriesOrder) {
    auto it = m_series.find(key);
    if (it == m_series.end())
      continue;
    const Series &s = it->second;
    if (!s.visible)
      continue;
    DrawSeries(gc, rcPlot, s, t0, t1, y0, y1);
  }

  DrawLegend(gc, rcClient);
}

bool ArduinoPlotView::ComputeVisibleRanges(double now_ms, double &t0, double &t1, double &y0, double &y1) const {
  t1 = now_ms;
  t0 = now_ms - m_timeWindowMs;

  if (m_fixedYRange) {
    y0 = m_fixedYMin;
    y1 = m_fixedYMax;
    if (y0 == y1) {
      y0 -= 1.0;
      y1 += 1.0;
    }
    return true;
  }

  double minV = std::numeric_limits<double>::infinity();
  double maxV = -std::numeric_limits<double>::infinity();
  bool any = false;

  for (const auto &key : m_seriesOrder) {
    auto it = m_series.find(key);
    if (it == m_series.end())
      continue;
    const Series &s = it->second;
    if (!s.visible)
      continue;

    const Sample *prev = nullptr;
    bool usedPrev = false;

    for (const Sample &sm : s.samples) {
      if (sm.t_ms < t0) {
        prev = &sm;
        continue;
      }
      if (sm.t_ms > t1)
        break;

      // Include one anchor point just before t0 (so autoscale matches the drawn line)
      if (prev && !usedPrev) {
        any = true;
        minV = std::min(minV, prev->value);
        maxV = std::max(maxV, prev->value);
        usedPrev = true;
      }

      any = true;
      minV = std::min(minV, sm.value);
      maxV = std::max(maxV, sm.value);
    }
  }

  if (!any)
    return false;

  if (minV == maxV) {
    const double delta = (std::abs(minV) > 1e-9) ? std::abs(minV) * 0.1 : 1.0;
    minV -= delta;
    maxV += delta;
  } else {
    const double span = maxV - minV;
    minV -= span * 0.08;
    maxV += span * 0.08;
  }

  y0 = minV;
  y1 = maxV;
  return true;
}

void ArduinoPlotView::DrawAxesAndGrid(wxGraphicsContext &gc, const wxRect &rcPlot,
                                      double t0, double t1, double y0, double y1) {
  // Frame
  gc.SetPen(wxPen(wxColour(120, 120, 120), 1));
  gc.SetBrush(*wxTRANSPARENT_BRUSH);
  gc.DrawRectangle(rcPlot.x, rcPlot.y, rcPlot.width, rcPlot.height);

  // Grid + labels
  const int yTicks = 5;
  const int xTicks = 5;

  wxPen gridPen(wxColour(180, 180, 180), 1, wxPENSTYLE_DOT);
  gc.SetPen(gridPen);

  // Y grid lines
  for (int i = 0; i <= yTicks; i++) {
    double a = (double)i / (double)yTicks;
    int y = rcPlot.y + (int)std::lround((1.0 - a) * rcPlot.height);
    gc.StrokeLine(rcPlot.x, y, rcPlot.x + rcPlot.width, y);
  }

  // X grid lines
  for (int i = 0; i <= xTicks; i++) {
    double a = (double)i / (double)xTicks;
    int x = rcPlot.x + (int)std::lround(a * rcPlot.width);
    gc.StrokeLine(x, rcPlot.y, x, rcPlot.y + rcPlot.height);
  }

  gc.SetFont(GetFont(), wxColour(80, 80, 80));

  // Y labels
  for (int i = 0; i <= yTicks; i++) {
    double a = (double)i / (double)yTicks;
    double v = y0 + a * (y1 - y0);
    wxString txt = FormatNumber(v);

    double tw = 0, th = 0, descent = 0, extlead = 0;
    gc.GetTextExtent(txt, &tw, &th, &descent, &extlead);

    int y = rcPlot.y + (int)std::lround((1.0 - a) * rcPlot.height);
    int tx = rcPlot.x - (int)tw - 6;
    int ty = y - (int)std::lround(th / 2.0);
    gc.DrawText(txt, tx, ty);
  }

  // X labels (seconds relative to now; left = -window, right = 0)
  // We intentionally skip i=0 because a wxSpinCtrl lives there.
  for (int i = 1; i <= xTicks; i++) {
    double a = (double)i / (double)xTicks;
    double t = t0 + a * (t1 - t0);
    wxString txt = FormatSeconds(t - t1);

    double tw = 0, th = 0, descent = 0, extlead = 0;
    gc.GetTextExtent(txt, &tw, &th, &descent, &extlead);

    int x = rcPlot.x + (int)std::lround(a * rcPlot.width);
    int tx = x - (int)std::lround(tw / 2.0);
    int ty = rcPlot.y + rcPlot.height + 6;
    gc.DrawText(txt, tx, ty);
  }
}

void ArduinoPlotView::DrawSeries(wxGraphicsContext &gc, const wxRect &rcPlot,
                                 const Series &s, double t0, double t1, double y0, double y1) {
  if (s.samples.size() < 2)
    return;

  auto xOf = [&](double t_ms) -> double {
    double a = (t_ms - t0) / (t1 - t0);
    return rcPlot.x + a * rcPlot.width;
  };
  auto yOf = [&](double v) -> double {
    double a = (v - y0) / (y1 - y0);
    return rcPlot.y + (1.0 - a) * rcPlot.height;
  };

  // Find first sample >= t0
  size_t idx = 0;
  while (idx < s.samples.size() && s.samples[idx].t_ms < t0)
    idx++;

  std::vector<wxPoint2DDouble> pts;
  pts.reserve(s.samples.size() + 2);

  // Add interpolated point at t0 (if we have a sample before and after)
  if (idx > 0 && idx < s.samples.size()) {
    const Sample &a = s.samples[idx - 1];
    const Sample &b = s.samples[idx];
    if (a.t_ms < t0 && b.t_ms > a.t_ms) {
      double alpha = (t0 - a.t_ms) / (b.t_ms - a.t_ms);
      alpha = std::clamp(alpha, 0.0, 1.0);
      double v = a.value + alpha * (b.value - a.value);
      pts.emplace_back(xOf(t0), yOf(v));
    }
  }

  // Add points inside [t0, t1]
  for (size_t i = idx; i < s.samples.size(); i++) {
    const Sample &sm = s.samples[i];
    if (sm.t_ms > t1)
      break;
    pts.emplace_back(xOf(sm.t_ms), yOf(sm.value));
  }

  if (pts.size() < 2)
    return;

  gc.SetPen(wxPen(s.color, 2));
  gc.StrokeLines((int)pts.size(), &pts[0]);
}

void ArduinoPlotView::DrawLegend(wxGraphicsContext &gc, const wxRect &rcClient) {
  m_legendHits.clear();
  if (m_seriesOrder.empty())
    return;

  // Legend box in top-right
  const int pad = 8;
  const int lineH = 18;
  const int cb = 12; // checkbox size

  gc.SetFont(GetFont(), wxColour(40, 40, 40));

  int count = 0;
  double maxTextW = 0;

  for (const auto &key : m_seriesOrder) {
    auto it = m_series.find(key);
    if (it == m_series.end())
      continue;
    const Series &s = it->second;

    double tw = 0, th = 0, descent = 0, extlead = 0;
    gc.GetTextExtent(s.name, &tw, &th, &descent, &extlead);
    maxTextW = std::max(maxTextW, tw);
    count++;
  }

  if (count == 0)
    return;

  // Layout: [pad][checkbox][gap][color swatch][gap][text][pad]
  const int boxW = (int)std::lround(pad + cb + 6 + 18 + 6 + maxTextW + pad);
  const int boxH = pad + count * lineH + pad;

  int x = rcClient.x + rcClient.width - boxW - 10;
  int y = rcClient.y + 10;

  gc.SetPen(wxPen(wxColour(160, 160, 160), 1));
  gc.SetBrush(wxBrush(wxColour(245, 245, 245)));
  gc.DrawRectangle(x, y, boxW, boxH);

  int row = 0;
  for (const auto &key : m_seriesOrder) {
    auto it = m_series.find(key);
    if (it == m_series.end())
      continue;
    const Series &s = it->second;

    int yRow = y + pad + row * lineH;
    int cy = yRow + lineH / 2;

    // Checkbox rect (device pixels)
    wxRect rcCb(x + pad, yRow + (lineH - cb) / 2, cb, cb);

    // Draw checkbox
    gc.SetPen(wxPen(wxColour(90, 90, 90), 1));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.DrawRectangle(rcCb.x, rcCb.y, rcCb.width, rcCb.height);

    if (s.visible) {
      // Check mark
      gc.SetPen(wxPen(wxColour(40, 40, 40), 2));
      gc.StrokeLine(rcCb.x + 2, rcCb.y + cb / 2,
                    rcCb.x + cb / 2, rcCb.y + cb - 3);
      gc.StrokeLine(rcCb.x + cb / 2, rcCb.y + cb - 3,
                    rcCb.x + cb - 2, rcCb.y + 3);
    }

    // Store hit area
    m_legendHits.push_back({rcCb, key});

    // Color swatch line
    int swX = rcCb.GetRight() + 6;
    gc.SetPen(wxPen(s.color, s.visible ? 3 : 1));
    gc.StrokeLine(swX, cy, swX + 16, cy);

    // Name text (greyed when hidden)
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetFont(GetFont(), s.visible ? wxColour(40, 40, 40) : wxColour(140, 140, 140));
    gc.DrawText(s.name, swX + 16 + 6, yRow + 1);

    row++;
  }
}

wxString ArduinoPlotView::FormatSeconds(double t_ms) {
  double sec = t_ms / 1000.0;
  double a = std::abs(sec);
  if (a >= 10.0) {
    return wxString::Format(wxT("%.0fs"), sec);
  }
  return wxString::Format(wxT("%.1fs"), sec);
}

wxString ArduinoPlotView::FormatNumber(double v) {
  if (std::isfinite(v)) {
    double iv = std::round(v);
    if (std::abs(v - iv) < 1e-9) {
      return wxString::Format(wxT("%.0f"), v);
    }
    return wxString::Format(wxT("%.2f"), v);
  }
  if (std::isnan(v))
    return wxT("NaN");
  return (v > 0) ? wxT("+Inf") : wxT("-Inf");
}
