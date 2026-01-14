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

#include <string>
#include <wx/artprov.h>
#include <wx/bmpbuttn.h>
#include <wx/config.h>
#include <wx/splitter.h>
#include <wx/stc/stc.h>
#include <wx/wx.h>

class ArduinoAiActions;
struct AiSettings;
class ArduinoMarkdownPanel;

// AI Chat panel / AUI widget
class ArduinoAiChatPanel : public wxPanel {
public:
  ArduinoAiChatPanel(wxWindow *parent, ArduinoAiActions *actions, wxConfigBase *config);
  ~ArduinoAiChatPanel();

  void ApplySettings(const AiSettings &settings);

  void Clear();

private:
  wxConfigBase *m_config;
  ArduinoMarkdownPanel *m_historyPanel;
  wxStyledTextCtrl *m_inputCtrl;
  wxSplitterWindow *m_splitter = nullptr;
  wxPanel *m_inputPanel = nullptr;
  wxBitmapButton *m_switchModelBtn = nullptr;

  wxChoice *m_sessionChoice = nullptr;
  std::string m_currentSessionId;       // selected/active session (kept across refreshes)
  std::string m_pendingSelectSessionId; // one-shot forced selection (e.g. after title update)

  wxTimer m_refreshTimer;

  bool m_isBusy{false};

  ArduinoAiActions *m_actions = nullptr;

  // config keys
  static constexpr const char *CFG_AI_CHAT_SASH = "AI/ChatPanelSash";
  static constexpr const char *CFG_AI_CHAT_SPLITMODE = "AI/ChatPanelSplitMode";

  void LoadUiState();
  void SaveUiState();
  void RefreshSessionList();
  void ResetChat();
  void OnRefreshSessions(wxCommandEvent &);
  void OnRefreshTimer(wxTimerEvent &);
  void OnSwitchModelClicked(wxCommandEvent &);

  void OnSysColourChanged(wxSysColourChangedEvent &event);

  void OnSessionChoice(wxCommandEvent &);
  void LoadSelectedSession();

  void InitUi();
  void OnCharHook(wxKeyEvent &event);
  void SendCurrentInput();

  void OnChatSuccess(wxThreadEvent &event);
  void OnChatError(wxThreadEvent &event);
  void OnSessionTitleUpdated(wxThreadEvent &event);
  void OnChatProgress(wxThreadEvent &event);
};
