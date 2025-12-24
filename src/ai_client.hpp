#pragma once

#include "ard_setdlg.hpp"
#include <wx/string.h>

class AiClient {
public:
  explicit AiClient(const AiSettings &settings);

  bool IsEnabled() const;

  bool TestConnection(wxString *errorOut = nullptr) const;

  bool SimpleChat(const wxString &systemPrompt,
                  const wxString &userPrompt,
                  wxString &assistantReply,
                  wxString *errorOut = nullptr) const;

  bool SimpleChatAsync(const wxString &systemPrompt,
                       const wxString &userPrompt,
                       wxEvtHandler *handler) const;

  // Validates that s is empty OR valid JSON object.
  // If errorOut != nullptr, fills it with a human readable error.
  static bool CheckExtraRequestJson(const wxString &s, wxString *errorOut = nullptr);

  int GetLastInputTokens() const;
  int GetLastOutputTokens() const;
  int GetLastTotalTokens() const;
  wxString GetModelName() const;

private:
  AiSettings m_settings;
  mutable std::atomic<int> m_lastInputTokens{0};
  mutable std::atomic<int> m_lastOutputTokens{0};
  mutable std::atomic<int> m_lastTotalTokens{0};

  bool LoadApiKey(wxString &keyOut, wxString *errorOut = nullptr) const;

  bool DoHttpPost(const wxString &bodyJson,
                  wxString &responseOut,
                  wxString *errorOut = nullptr) const;

  bool ExtractAssistantText(const wxString &responseJson,
                            wxString &assistantText,
                            wxString *errorOut = nullptr) const;
};
