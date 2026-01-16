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

#include "ard_aipnl.hpp"
#include "ard_ai.hpp"
#include "ard_ap.hpp"
#include "ard_ed_frm.hpp"
#include "ard_ev.hpp"
#include "ard_mdwidget.hpp"
#include "ard_setdlg.hpp"
#include "utils.hpp"
#include <wx/button.h>
#include <wx/choice.h>

namespace {
const int kStylePlaceholder = 1;
}

static void FreeChoiceClientData(wxChoice *c) {
  if (!c)
    return;
  for (unsigned i = 0; i < c->GetCount(); ++i) {
    auto *p = (std::string *)c->GetClientData(i);
    delete p;
    c->SetClientData(i, nullptr);
  }
}

static wxString NormalizeNewlines(wxString s) {
  s.Replace(wxT("\r\n"), wxT("\n"));
  s.Replace(wxT("\r"), wxT("\n"));
  return s;
}

static void AppendEscapedHtmlChar(wxString &out, wxChar ch) {
  switch (ch) {
    case '&':
      out += wxT("&amp;");
      break;
    case '<':
      out += wxT("&lt;");
      break;
    case '>':
      out += wxT("&gt;");
      break;
    case '"':
      out += wxT("&quot;");
      break;
    default:
      out += ch;
      break;
  }
}

// - \n => <br>
// - tab => 4× &nbsp;
// - escapuje <>&"
static wxString UiUserTextToHtmlNbspBr(const wxString &raw) {
  wxString s = NormalizeNewlines(raw);

  wxString out;
  out.reserve(s.length() * 2);

  bool atLineStart = true;
  bool prevWasSpace = false;

  out += wxT("<p>");

  for (size_t i = 0; i < s.length(); ++i) {
    const wxChar ch = s[i];

    if (ch == '\n') {
      out += wxT("<br>\n");
      atLineStart = true;
      prevWasSpace = false;
      continue;
    }

    if (ch == '\t') {
      out += wxT("&nbsp;&nbsp;&nbsp;&nbsp;");
      atLineStart = false;
      prevWasSpace = true;
      continue;
    }

    if (ch == ' ') {
      if (atLineStart || prevWasSpace)
        out += wxT("&nbsp;");
      else
        out += wxT(" ");
      atLineStart = false;
      prevWasSpace = true;
      continue;
    }

    AppendEscapedHtmlChar(out, ch);
    atLineStart = false;
    prevWasSpace = false;
  }

  out += wxT("</p>");
  return out;
}

static wxString GetIntro() {
  return _("AI chat ready.\n"
           "You can ask coding questions or request refactoring.\n"
           "\n"
           "The chat has access to all sketch files.\n"
           "It may request files, propose modifications as patches,\n"
           "and - with your confirmation - create new files.\n"
           "\n"
           "Type your message and press Enter to send.\n"
           "(Shift+Enter inserts a new line.)");
}

ArduinoAiChatPanel::ArduinoAiChatPanel(wxWindow *parent, ArduinoAiActions *actions, wxConfigBase *config)
    : wxPanel(parent, wxID_ANY),
      m_config(config), m_historyPanel(nullptr), m_inputCtrl(nullptr), m_refreshTimer(this), m_isBusy(false), m_actions(actions) {
  InitUi();

  Bind(wxEVT_TIMER, &ArduinoAiChatPanel::OnRefreshTimer, this);
  Bind(wxEVT_AI_SIMPLE_CHAT_SUCCESS, &ArduinoAiChatPanel::OnChatSuccess, this);
  Bind(wxEVT_AI_SIMPLE_CHAT_ERROR, &ArduinoAiChatPanel::OnChatError, this);
  Bind(wxEVT_AI_SIMPLE_CHAT_PROGRESS, &ArduinoAiChatPanel::OnChatProgress, this);
  Bind(wxEVT_AI_SUMMARIZATION_UPDATED, &ArduinoAiChatPanel::OnSessionTitleUpdated, this);
  Bind(wxEVT_SYS_COLOUR_CHANGED, &ArduinoAiChatPanel::OnSysColourChanged, this);

  m_switchModelBtn->Bind(wxEVT_BUTTON, &ArduinoAiChatPanel::OnSwitchModelClicked, this);
  m_historyPanel->Bind(wxEVT_HTML_LINK_CLICKED, &ArduinoAiChatPanel::OnHtmlLinkClicked, this);

  std::vector<AiModelSettings> models;
  ArduinoEditorSettingsDialog::LoadAiModels(m_config, models);
  if (m_switchModelBtn) {
    m_switchModelBtn->Enable(models.size() > 1);
  }
}

ArduinoAiChatPanel::~ArduinoAiChatPanel() {
  SaveUiState();

  if (m_sessionChoice) {
    FreeChoiceClientData(m_sessionChoice);
  }
}

void ArduinoAiChatPanel::ApplySettings(const AiSettings &WXUNUSED(settings)) {
  std::vector<AiModelSettings> models;
  ArduinoEditorSettingsDialog::LoadAiModels(m_config, models);

  if (m_switchModelBtn) {
    m_switchModelBtn->Enable(models.size() > 1);
  }
}

void ArduinoAiChatPanel::InitUi() {
  auto *sizer = new wxBoxSizer(wxVERTICAL);

  // --- Top bar: session chooser ---
  auto *topBar = new wxBoxSizer(wxHORIZONTAL);

  m_sessionChoice = new wxChoice(this, wxID_ANY);
  topBar->Add(m_sessionChoice, 1, wxEXPAND | wxALL, 5);

  m_switchModelBtn = new wxBitmapButton(this, wxID_ANY, AEGetArtBundle(wxAEArt::SwitchModel));

  m_switchModelBtn->SetToolTip(_("Switch AI model"));
  m_switchModelBtn->SetCanFocus(false);
  topBar->Add(m_switchModelBtn, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, 5);

  sizer->Add(topBar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);

  m_sessionChoice->Bind(wxEVT_CHOICE, &ArduinoAiChatPanel::OnSessionChoice, this);

  // --- Splitter: history (top) vs input (bottom) ---
  m_splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxSP_LIVE_UPDATE | wxSP_3D);
  m_splitter->SetMinimumPaneSize(60);

  // History panel (markdown -> HTML)
  m_historyPanel = new ArduinoMarkdownPanel(m_splitter);

  // Input area wrapped in a panel (better splitter behavior + future toolbar)
  m_inputPanel = new wxPanel(m_splitter, wxID_ANY);
  auto *inputSizer = new wxBoxSizer(wxVERTICAL);

  // Chat-like input: wxStyledTextCtrl allows adding extra spacing between lines.
  // wxTextCtrl is native and doesn't reliably support line spacing across platforms.
  m_inputCtrl = new wxStyledTextCtrl(m_inputPanel, wxID_ANY);

  // Wrapping + no horizontal scroll gives a "chat composer" feel.
  m_inputCtrl->SetWrapMode(wxSTC_WRAP_WORD);
  m_inputCtrl->SetUseHorizontalScrollBar(false);

  // Visual padding inside the control.
  m_inputCtrl->SetMarginWidth(0, 0);
  m_inputCtrl->SetMarginLeft(10);
  m_inputCtrl->SetMarginRight(10);

  for (int i = 0; i < 5; ++i) {
    m_inputCtrl->SetMarginWidth(i, 0);
  }

  m_inputCtrl->SetMarginType(0, wxSTC_MARGIN_SYMBOL);
  m_inputCtrl->SetMarginMask(0, 0);
  m_inputCtrl->SetMarginSensitive(0, false);

  m_inputCtrl->SetProperty(wxT("fold"), wxT("0"));
  m_inputCtrl->SetMarginWidth(2, 0);

  // Extra space above/below each line => better readability for longer inputs.
  m_inputCtrl->SetExtraAscent(2);
  m_inputCtrl->SetExtraDescent(2);

  // Keep it simple (no editor chrome).
  m_inputCtrl->SetIndentationGuides(wxSTC_IV_NONE);
  m_inputCtrl->SetEdgeMode(wxSTC_EDGE_NONE);
  m_inputCtrl->SetViewWhiteSpace(wxSTC_WS_INVISIBLE);

  m_inputCtrl->SetMinSize(wxSize(-1, 60));

  wxString normalFace;
#ifdef __WXMAC__
  normalFace = wxT("Helvetica Neue");
#elif defined(__WXMSW__)
  normalFace = wxT("Segoe UI");
#else
  normalFace = wxT("DejaVu Sans");
#endif

  EditorSettings settings;
  settings.Load(m_config);

  wxFont inputFont(settings.GetFont().GetPointSize() + 2,
                   wxFONTFAMILY_DEFAULT,
                   wxFONTSTYLE_NORMAL,
                   wxFONTWEIGHT_NORMAL,
                   false,
                   normalFace);

  // Apply font to STC.
  // NOTE: For wxStyledTextCtrl, the displayed text uses Scintilla "styles".
  // StyleClearAll() propagates wxSTC_STYLE_DEFAULT to other styles, so we must
  // set the default style *before* calling it, otherwise the control will keep
  // the built-in (usually smaller) default font.
  m_inputCtrl->SetFont(inputFont); // helps with sizing metrics on some platforms
  m_inputCtrl->SetLexer(wxSTC_LEX_NULL);
  m_inputCtrl->StyleSetFont(wxSTC_STYLE_DEFAULT, inputFont);
  m_inputCtrl->StyleSetSize(wxSTC_STYLE_DEFAULT, inputFont.GetPointSize());
  // Be explicit: style 0 is what plain text typically uses.
  m_inputCtrl->StyleSetFont(0, inputFont);
  m_inputCtrl->StyleSetSize(0, inputFont.GetPointSize());

  m_inputCtrl->StyleSetFont(wxSTC_STYLE_LINENUMBER, inputFont);
  m_inputCtrl->StyleSetSize(wxSTC_STYLE_LINENUMBER, inputFont.GetPointSize());
  m_inputCtrl->SetCaretLineVisible(false);
  m_inputCtrl->SetTabWidth(2);
  m_inputCtrl->SetUseTabs(false);

  inputSizer->Add(m_inputCtrl, 1, wxEXPAND | wxALL, 5);
  m_inputPanel->SetSizer(inputSizer);

  // Split: history on top, input at bottom
  m_splitter->SplitHorizontally(m_historyPanel, m_inputPanel, -140);

  sizer->Add(m_splitter, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

  SetSizerAndFit(sizer);

  m_inputCtrl->Bind(wxEVT_CHAR_HOOK, &ArduinoAiChatPanel::OnCharHook, this);

  // intro
  if (m_historyPanel) {
    m_historyPanel->Clear();
    m_historyPanel->AppendMarkdown(GetIntro(), AiMarkdownRole::Info);
  }

  LoadUiState();

  RefreshSessionList();

  ApplyInputColors();

  m_refreshTimer.StartOnce(3000);

  m_inputCtrl->SetFocus();
}

void ArduinoAiChatPanel::LoadUiState() {
  if (!m_splitter)
    return;

  long sash = -1;
  if (m_config->Read(wxString::FromUTF8(CFG_AI_CHAT_SASH), &sash) && sash > 0) {
    // only if already split
    if (m_splitter->IsSplit()) {
      m_splitter->SetSashPosition((int)sash);
    }
  }
}

void ArduinoAiChatPanel::SaveUiState() {
  if (!m_splitter)
    return;

  if (m_splitter->IsSplit()) {
    m_config->Write(wxString::FromUTF8(CFG_AI_CHAT_SASH), (long)m_splitter->GetSashPosition());
  }
}

void ArduinoAiChatPanel::RefreshSessionList() {
  if (!m_sessionChoice || !m_actions)
    return;

  // --- selection policy ---
  // Prefer:
  //   1) a pending forced selection (e.g. after title update / new session),
  //   2) last known current session id,
  //   3) whatever is currently selected in the choice.
  std::string selId;

  if (!m_pendingSelectSessionId.empty()) {
    selId = m_pendingSelectSessionId;
    m_pendingSelectSessionId.clear(); // one-shot
  } else if (!m_currentSessionId.empty()) {
    selId = m_currentSessionId;
  } else {
    int oldSel = m_sessionChoice->GetSelection();
    if (oldSel != wxNOT_FOUND) {
      auto *p = (std::string *)m_sessionChoice->GetClientData(oldSel);
      if (p) {
        selId = *p;
      }
    }
  }

  // IMPORTANT: free old client data before Clear() to avoid leaks
  FreeChoiceClientData(m_sessionChoice);
  m_sessionChoice->Clear();

  // Always add the "action" item at index 0
  m_sessionChoice->Append(wxEmptyString); // selecting this == Reset chat

  auto sessions = m_actions->ListStoredChatSessions();
  if (sessions.empty()) {
    // Keep choice enabled so user can still pick the empty item.
    m_sessionChoice->Append(_("(no saved sessions)")); // client data = nullptr
    m_sessionChoice->Enable(true);
    m_sessionChoice->SetSelection(1);

    m_currentSessionId.clear();
    m_pendingSelectSessionId.clear();
    return;
  }

  m_sessionChoice->Enable(true);

  // Display: "TITLE  (N msgs)"  (real sessions start at index 1)
  for (const auto &s : sessions) {
    wxString label = wxString::Format(wxT("%s  (%d msgs)"),
                                      s.GetTitle(),
                                      s.messageCount);
    m_sessionChoice->Append(label, (void *)new std::string(s.id));
  }

  // restore / force selection (search only real items: 1..count-1)
  int found = wxNOT_FOUND;
  if (!selId.empty()) {
    for (unsigned i = 1; i < m_sessionChoice->GetCount(); ++i) {
      auto *p = (std::string *)m_sessionChoice->GetClientData(i);
      if (p && *p == selId) {
        found = (int)i;
        break;
      }
    }
  }

  if (found != wxNOT_FOUND) {
    m_sessionChoice->SetSelection(found);
  } else {
    m_sessionChoice->SetSelection(0); // first real session
  }

  // remember current selection
  int curSel = m_sessionChoice->GetSelection();
  if (curSel > 0) { // ignore action item
    auto *p = (std::string *)m_sessionChoice->GetClientData(curSel);
    if (p) {
      m_currentSessionId = *p;
    } else {
      m_currentSessionId.clear();
    }
  } else {
    m_currentSessionId.clear();
  }
}

void ArduinoAiChatPanel::ResetChat() {
  if (m_isBusy) {
    return;
  }

  if (m_actions) {
    m_actions->ResetInteractiveChat();
  }

  // New chat will get a new session id/title later.
  m_currentSessionId.clear();
  m_pendingSelectSessionId.clear();

  Clear();

  if (m_sessionChoice && m_sessionChoice->GetCount() > 0) {
    m_sessionChoice->SetSelection(0);
  }

  RefreshSessionList();
}

void ArduinoAiChatPanel::OnRefreshTimer(wxTimerEvent &) {
  RefreshSessionList();
}

void ArduinoAiChatPanel::OnSwitchModelClicked(wxCommandEvent &) {
  std::vector<AiModelSettings> models;
  ArduinoEditorSettingsDialog::LoadAiModels(m_config, models);

  AiSettings aiSettings;
  aiSettings.Load(m_config);

  wxMenu menu;

  for (const auto &m : models) {
    const wxWindowID id = wxWindow::NewControlId();

    wxMenuItem *it = menu.AppendRadioItem(id, m.name);
    if (m.id == aiSettings.id) {
      it->Check(true);
    }

    menu.Bind(
        wxEVT_MENU,
        [this, isModel = m, &aiSettings](wxCommandEvent &) mutable {
          ArduinoEditorSettingsDialog::ApplyModelToAiSettings(isModel, aiSettings);
          aiSettings.Save(m_config);
          if (auto *frame = wxDynamicCast(GetParent(), ArduinoEditorFrame)) {
            frame->ApplySettings(aiSettings);
          }
        },
        id);
  }

  wxRect r = m_switchModelBtn->GetRect();
  wxPoint pos(r.GetLeft(), r.GetBottom());

  PopupMenu(&menu, pos);
}

void ArduinoAiChatPanel::OnHtmlLinkClicked(wxHtmlLinkEvent &event) {
  wxString href = event.GetLinkInfo().GetHref();
  href.Trim(true).Trim(false);

  APP_DEBUG_LOG("AIPNL: OnHtmlLinkClicked (href=%s)", wxToStd(href).c_str());

  wxString lower = href.Lower();

  // Internal scheme
  if (lower.StartsWith(wxT("ai://"))) {
    // Expected: ai://review_patch/<id>
    static const wxString kPrefix = wxT("ai://review_patch/");
    if (lower.StartsWith(kPrefix)) {
      wxString id = href.Mid(kPrefix.length());
      id.Trim(true).Trim(false);

      if (!id.empty() && m_actions) {
        m_actions->OpenPendingPatchReview(wxToStd(id));
      }
    }
    return;
  }

  event.Skip(true);
}

void ArduinoAiChatPanel::OnRefreshSessions(wxCommandEvent &) {
  RefreshSessionList();
}

void ArduinoAiChatPanel::OnSessionChoice(wxCommandEvent &) {
  if (m_isBusy || !m_sessionChoice) {
    return;
  }

  int sel = m_sessionChoice->GetSelection();
  if (sel == wxNOT_FOUND) {
    return;
  }

  // Action item: empty string => reset chat / start new session
  if (sel == 0) {
    ResetChat();
    return;
  }

  // Normal sessions: require client data (session id)
  auto *p = (std::string *)m_sessionChoice->GetClientData(sel);
  if (p) {
    m_currentSessionId = *p;
  } else {
    m_currentSessionId.clear();
    return;
  }

  LoadSelectedSession();
}

void ArduinoAiChatPanel::LoadSelectedSession() {
  if (!m_actions || !m_sessionChoice || m_sessionChoice->GetSelection() == wxNOT_FOUND) {
    return;
  }

  int sel = m_sessionChoice->GetSelection();
  auto *p = (std::string *)m_sessionChoice->GetClientData(sel);
  if (!p) {
    return;
  }

  m_currentSessionId = *p;

  wxString transcript;
  if (!m_actions->LoadChatSession(*p, transcript)) {
    if (m_historyPanel) {
      m_historyPanel->AppendMarkdown(_("[AI] Failed to load saved session."), AiMarkdownRole::Error);
    }
    return;
  }

  m_actions->RestoreInteractiveChatFromTranscript(transcript, *p);

  std::vector<AiChatUiItem> items;
  if (m_actions->LoadChatSessionUi(*p, items)) {
    if (m_historyPanel) {
      m_historyPanel->Clear();
      m_historyPanel->AppendMarkdown(GetIntro(), AiMarkdownRole::Info);

      for (const auto &it : items) {
        AiMarkdownRole role = AiMarkdownRole::Info;
        if (it.role == "user") {
          role = AiMarkdownRole::User;
        } else if (it.role == "assistant") {
          role = AiMarkdownRole::Assistant;
        } else if (it.role == "error") {
          role = AiMarkdownRole::Error;
        }

        wxString text = it.text;
        if (role == AiMarkdownRole::User) {
          text = UiUserTextToHtmlNbspBr(text);
        }

        m_historyPanel->AppendMarkdown(text, role, it.GetTokenInfo(), it.GetCreatedDateByLocale());
      }
    }
  }

  if (m_inputCtrl) {
    m_inputCtrl->SetFocus();
  }
}

void ArduinoAiChatPanel::Clear() {
  if (m_historyPanel) {
    m_historyPanel->Clear();
    m_historyPanel->AppendMarkdown(GetIntro(), AiMarkdownRole::Info);
  }

  if (m_inputCtrl) {
    m_inputCtrl->SetText(wxEmptyString);
    m_inputCtrl->SetFocus();
  }
}

void ArduinoAiChatPanel::SetBusyUi(bool busy) {
  if (!m_inputCtrl)
    return;

  if (busy) {
    m_isBusy = true;

    m_inputCtrl->SetCaretWidth(0);

    const wxString msg = wxT("  ") + _("AI model thinking...") + wxT("  ");

    m_inputCtrl->SetText(msg);
    m_inputCtrl->SetReadOnly(true);

    m_inputCtrl->StartStyling(0);
    m_inputCtrl->SetStyling((int)msg.length() * 2, kStylePlaceholder);

    m_inputCtrl->GotoPos(0);
    return;
  }

  m_isBusy = false;

  m_inputCtrl->SetCaretWidth(1);
  m_inputCtrl->SetReadOnly(false);

  m_inputCtrl->SetText(wxEmptyString);

  ApplyInputColors();

  m_inputCtrl->SetFocus();
}

void ArduinoAiChatPanel::ApplyInputColors() {
  EditorSettings settings;
  settings.Load(m_config);

  EditorColorScheme c = settings.GetColors();

  m_inputCtrl->StyleSetForeground(wxSTC_STYLE_DEFAULT, c.text);
  m_inputCtrl->StyleSetBackground(wxSTC_STYLE_DEFAULT, c.background);

  m_inputCtrl->SetBackgroundColour(c.background);
  m_inputCtrl->SetCaretForeground(c.text);

  m_inputCtrl->StyleClearAll();

  m_inputCtrl->StyleSetForeground(0, c.text);
  m_inputCtrl->StyleSetBackground(0, c.background);

  m_inputCtrl->StyleSetForeground(kStylePlaceholder, c.note);
  m_inputCtrl->StyleSetBackground(kStylePlaceholder, c.background);
  m_inputCtrl->StyleSetItalic(kStylePlaceholder, true);
}

void ArduinoAiChatPanel::OnSysColourChanged(wxSysColourChangedEvent &event) {
  m_historyPanel->Render(/*scrollToEnd=*/false);

  ApplyInputColors();

  if (m_switchModelBtn) {
    m_switchModelBtn->SetBitmap(AEGetArtBundle(wxAEArt::SwitchModel));
  }

  Layout();

  event.Skip();
}

void ArduinoAiChatPanel::OnCharHook(wxKeyEvent &event) {
  if (!m_inputCtrl) {
    event.Skip();
    return;
  }

  const int keyCode = event.GetKeyCode();

  if (m_isBusy) {
    event.Skip();
    return;
  }

  if (keyCode == WXK_RETURN) {
    if (event.GetModifiers() & wxMOD_SHIFT) {
      event.Skip();
      return;
    } else {
      SendCurrentInput();
      return;
    }
  }

  event.Skip();
}

void ArduinoAiChatPanel::SendCurrentInput() {
  if (!m_inputCtrl || m_isBusy)
    return;

  // wxStyledTextCtrl doesn't implement wxTextEntry; use GetText().
  wxString trimmed = TrimCopy(m_inputCtrl->GetText());

  if (trimmed.IsEmpty()) {
    return;
  }

  // Append user text
  if (m_historyPanel) {
    wxDateTime now = wxDateTime::Now();

    wxString time = now.Format(
        wxT("%x %X"), // locale-dependent date + time
        wxDateTime::Local);

    wxString uiText = UiUserTextToHtmlNbspBr(trimmed);
    m_historyPanel->AppendMarkdown(uiText, AiMarkdownRole::User, wxEmptyString, time);
  }

  // Clean input
  m_inputCtrl->SetText(wxEmptyString);

  if (!m_actions) {
    if (m_historyPanel) {
      m_historyPanel->AppendMarkdown(
          _("[AI] Interactive actions are not available."),
          AiMarkdownRole::Error);
    }
    return;
  }

  SetBusyUi(true);

  // Run async request
  if (!m_actions->StartInteractiveChat(trimmed, this)) {
    // Error.
    SetBusyUi(false);

    if (m_historyPanel) {
      m_historyPanel->AppendMarkdown(
          _("[AI] Failed to start interactive chat request."),
          AiMarkdownRole::Error);
    }
  }
}

void ArduinoAiChatPanel::OnChatSuccess(wxThreadEvent &event) {
  const wxString answer = event.GetString();

  if (m_inputCtrl) {
    SetBusyUi(false);
  }

  if (m_historyPanel && !answer.IsEmpty()) {
    wxString info;
    wxString model = m_actions->GetSettings().model;
    AiTokenTotals totals = event.GetPayload<AiTokenTotals>();

    if (totals.HasAny()) {
      if (totals.calls <= 1) {
        info = wxString::Format(_("%s (in: %lld, out: %lld, tot: %lld) tokens"),
                                model,
                                totals.input,
                                totals.output,
                                totals.total);
      } else {
        info = wxString::Format(_("%s (in: %lld, out: %lld, tot: %lld) tokens, %d calls"),
                                model,
                                totals.input,
                                totals.output,
                                totals.total,
                                totals.calls);
      }
    } else {
      // Fallback (should be rare): use the last call's token info directly from the client.
      auto *cl = m_actions->GetClient();
      if (cl) {
        int inTok = cl->GetLastInputTokens();
        int outTok = cl->GetLastOutputTokens();
        int totTok = cl->GetLastTotalTokens();
        if ((inTok > -1) && (outTok > -1) && (totTok > -1)) {
          info = wxString::Format(_("%s (in: %d, out: %d, tot: %d) tokens"),
                                  model, inTok, outTok, totTok);
        }
      }
    }

    wxDateTime now = wxDateTime::Now();

    wxString time = now.Format(
        wxT("%x %X"), // locale-dependent date + time
        wxDateTime::Local);

    m_historyPanel->AppendMarkdown(answer, AiMarkdownRole::Assistant, info, time);
  }

  RefreshSessionList();
}

void ArduinoAiChatPanel::OnChatError(wxThreadEvent &event) {
  const wxString err = event.GetString();

  if (m_inputCtrl) {
    SetBusyUi(false);
  }

  wxString msg;
  if (err.IsEmpty()) {
    msg = _("[AI ERROR] Unknown error");
  } else {
    msg = wxString::Format(_("[AI ERROR] %s"), err);
  }

  if (m_historyPanel) {
    m_historyPanel->AppendMarkdown(msg, AiMarkdownRole::Error);
  }

  // Keep the choice labels (message counts) in sync even on errors.
  RefreshSessionList();
}

void ArduinoAiChatPanel::OnSessionTitleUpdated(wxThreadEvent &event) {
  if (!m_sessionChoice)
    return;

  wxString payload = TrimCopy(event.GetString());

  if (payload.IsEmpty())
    return;

  wxString sessionLine = TrimCopy(payload.BeforeFirst('\n'));
  wxString titleLine = TrimCopy(payload.AfterFirst('\n'));

  if (sessionLine.IsEmpty() || titleLine.IsEmpty())
    return;

  std::string sid = wxToStd(sessionLine);

  // New/updated title belongs to this session -> select it.
  // (Typical case: after starting a new chat, summarization sets the first title.)
  m_pendingSelectSessionId = sid;
  RefreshSessionList();

  // After refresh, enforce the new title in the label while keeping the "(N msgs)" suffix.
  int found = wxNOT_FOUND;
  for (unsigned i = 0; i < m_sessionChoice->GetCount(); ++i) {
    auto *p = (std::string *)m_sessionChoice->GetClientData(i);
    if (p && *p == sid) {
      found = (int)i;
      break;
    }
  }
  if (found == wxNOT_FOUND)
    return;

  wxString oldLabel = m_sessionChoice->GetString((unsigned)found);
  wxString suffix;
  int parenPos = oldLabel.Find(wxT("  ("));
  if (parenPos != wxNOT_FOUND) {
    suffix = oldLabel.Mid(parenPos); // includes "  (N msgs)"
  }

  wxString newLabel = titleLine;
  if (!suffix.IsEmpty()) {
    newLabel << suffix;
  }

  m_sessionChoice->SetString((unsigned)found, newLabel);
  m_sessionChoice->SetSelection(found);
  m_currentSessionId = sid;
}

void ArduinoAiChatPanel::OnChatProgress(wxThreadEvent &WXUNUSED(event)) {
  // Currently not supported
}
