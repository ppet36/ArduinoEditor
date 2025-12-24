#pragma once

#include <string>
#include <wx/artprov.h>
#include <wx/bmpbuttn.h>
#include <wx/config.h>
#include <wx/splitter.h>
#include <wx/wx.h>

class ArduinoAiActions;
class ArduinoMarkdownPanel;

// AI Chat panel / AUI widget
class ArduinoAiChatPanel : public wxPanel {
public:
  ArduinoAiChatPanel(wxWindow *parent, ArduinoAiActions *actions, wxConfigBase *config);
  ~ArduinoAiChatPanel();

  void Clear();

  void OnSysColourChanged();

private:
  wxConfigBase *m_config;
  ArduinoMarkdownPanel *m_historyPanel;
  wxTextCtrl *m_inputCtrl;
  wxSplitterWindow *m_splitter = nullptr;
  wxPanel *m_inputPanel = nullptr;

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
