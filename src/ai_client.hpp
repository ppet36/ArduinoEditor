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
