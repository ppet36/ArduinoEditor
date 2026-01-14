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
#include <wx/button.h>
#include <wx/choice.h>

static void FreeChoiceClientData(wxChoice *c) {
  if (!c)
    return;
  for (unsigned i = 0; i < c->GetCount(); ++i) {
    auto *p = (std::string *)c->GetClientData(i);
    delete p;
    c->SetClientData(i, nullptr);
  }
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

  m_inputCtrl = new wxTextCtrl(
      m_inputPanel,
      wxID_ANY,
      wxEmptyString,
      wxDefaultPosition,
      wxDefaultSize,
      wxTE_MULTILINE | wxTE_PROCESS_ENTER);

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

  wxFont inputFont(settings.GetFont().GetPointSize() + 2, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, normalFace);
  m_inputCtrl->SetFont(inputFont);

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

        m_historyPanel->AppendMarkdown(it.text, role, it.GetTokenInfo(), it.GetCreatedDateByLocale());
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
    m_inputCtrl->Clear();
    m_inputCtrl->SetFocus();
  }
}

void ArduinoAiChatPanel::OnSysColourChanged(wxSysColourChangedEvent &event) {
  m_historyPanel->Render(/*scrollToEnd=*/false);

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

  wxString trimmed = TrimCopy(m_inputCtrl->GetValue());

  if (trimmed.IsEmpty()) {
    return;
  }

  // Append user text
  if (m_historyPanel) {
    wxDateTime now = wxDateTime::Now();

    wxString time = now.Format(
        wxT("%x %X"), // locale-dependent date + time
        wxDateTime::Local);

    m_historyPanel->AppendMarkdown(trimmed, AiMarkdownRole::User, wxEmptyString, time);
  }

  // Clean input
  m_inputCtrl->Clear();

  if (!m_actions) {
    if (m_historyPanel) {
      m_historyPanel->AppendMarkdown(
          _("[AI] Interactive actions are not available."),
          AiMarkdownRole::Error);
    }
    return;
  }

  m_isBusy = true;
  m_inputCtrl->Enable(false);

  // Run async request
  if (!m_actions->StartInteractiveChat(trimmed, this)) {
    // Error.
    m_inputCtrl->Enable(true);
    m_isBusy = false;
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
    m_inputCtrl->Enable(true);
    m_inputCtrl->SetFocus();
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

  m_isBusy = false;
}

void ArduinoAiChatPanel::OnChatError(wxThreadEvent &event) {
  const wxString err = event.GetString();

  if (m_inputCtrl) {
    m_inputCtrl->Enable(true);
    m_inputCtrl->SetFocus();
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

  m_isBusy = false;
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
