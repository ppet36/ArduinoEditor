#include "sf.hpp"

#include "sf.hpp"
#include <wx/settings.h>

SplashFrame::SplashFrame()
    : wxFrame(
          nullptr,
          wxID_ANY,
          wxT("Arduino Editor"),
          wxDefaultPosition,
          wxSize(600, 300),
          wxFRAME_NO_TASKBAR | wxSTAY_ON_TOP | wxBORDER_NONE),
      m_closeTimer(this) {
  Bind(wxEVT_TIMER, &SplashFrame::OnCloseTimer, this);

  // Base system colors
  const wxColour windowBg = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
  const wxColour windowText = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
  const wxColour highlight = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
  const wxColour highlightText = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);

  auto *panel = new wxPanel(this);
  panel->SetBackgroundColour(windowBg);
  SetBackgroundColour(windowBg);

  auto *frameSizer = new wxBoxSizer(wxVERTICAL);
  frameSizer->Add(panel, 1, wxEXPAND);
  SetSizer(frameSizer);

  // Top & bottom bars - systmov "accent"
  auto *topBar = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 50));
  auto *bottomBar = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 50));

  topBar->SetBackgroundColour(highlight);
  bottomBar->SetBackgroundColour(highlight);

  // --- message text in bottom bar ---
  m_messageText = new wxStaticText(bottomBar, wxID_ANY, wxT(""), wxDefaultPosition, wxDefaultSize);
  m_messageText->SetForegroundColour(highlightText);

  auto *bottomSizer = new wxBoxSizer(wxHORIZONTAL);
  bottomSizer->Add(m_messageText, 1, wxLEFT | wxRIGHT | wxALIGN_CENTER_VERTICAL, 15);
  bottomBar->SetSizer(bottomSizer);
  // --------------------------------------

  auto *centerPanel = new wxPanel(panel);
  centerPanel->SetBackgroundColour(windowBg);

  auto *title = new wxStaticText(centerPanel, wxID_ANY, _("Arduino Editor"));
  auto *version = new wxStaticText(
      centerPanel,
      wxID_ANY,
      wxString::Format(_("Version %s"), wxString::FromUTF8(VERSION)));

  wxFont titleFont = title->GetFont();
  titleFont.SetPointSize(24);
  titleFont.MakeBold();
  title->SetFont(titleFont);
  title->SetForegroundColour(windowText);

  wxFont verFont = version->GetFont();
  verFont.SetPointSize(16);
  version->SetFont(verFont);
  // a little more subdued than the main title
  version->SetForegroundColour(wxColour(
      (unsigned char)std::min(windowText.Red() + 40, 255),
      (unsigned char)std::min(windowText.Green() + 40, 255),
      (unsigned char)std::min(windowText.Blue() + 40, 255)));

  auto *centerSizer = new wxBoxSizer(wxVERTICAL);
  centerSizer->AddStretchSpacer(1);
  centerSizer->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 15);
  centerSizer->Add(version, 0, wxALIGN_CENTER_HORIZONTAL);
  centerSizer->AddStretchSpacer(1);
  centerPanel->SetSizer(centerSizer);

  auto *mainSizer = new wxBoxSizer(wxVERTICAL);
  mainSizer->Add(topBar, 0, wxEXPAND);
  mainSizer->Add(centerPanel, 1, wxEXPAND | wxLEFT | wxRIGHT, 20);
  mainSizer->Add(bottomBar, 0, wxEXPAND);
  panel->SetSizer(mainSizer);

  Layout();
  CentreOnScreen();
}

void SplashFrame::SetMessage(const wxString &msg) {
  if (m_messageText) {
    m_messageText->SetLabel(msg);
    m_messageText->Refresh();
    m_messageText->Update();
    ::wxYield();
  }
}

void SplashFrame::Present() {
  Show();
  Raise();
}

void SplashFrame::StartClose(int delayMs) {
  m_closeTimer.StartOnce(delayMs);
}

void SplashFrame::OnCloseTimer(wxTimerEvent &) {
  Destroy();
}
