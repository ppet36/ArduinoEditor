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

#include <wx/html/htmlwin.h>
#include <wx/panel.h>
#include <wx/string.h>

class wxHtmlWindow;

enum class AiMarkdownRole {
  User,
  Assistant,
  System,
  Info,
  Error
};

wxString ArduinoMarkdown_MarkdownToHtmlFragment(const wxString &input);

// Simple markdown panel.
class ArduinoMarkdownPanel : public wxPanel {
public:
  ArduinoMarkdownPanel(wxWindow *parent,
                       wxWindowID id = wxID_ANY,
                       const wxPoint &pos = wxDefaultPosition,
                       const wxSize &size = wxDefaultSize,
                       long style = wxTAB_TRAVERSAL);

  void Clear();

  void Render(bool scrollToEnd);

  void AppendMarkdown(const wxString &markdown, AiMarkdownRole role, const wxString &info = wxEmptyString, const wxString &time = wxEmptyString);

  void SetBaseFonts(const wxString &normalFace,
                    const wxString &fixedFace,
                    const int *sizes = nullptr);

private:
  struct MdMsg {
    wxString markdown;
    AiMarkdownRole role;
    wxString info;
    wxString time;
  };

  wxHtmlWindow *m_html{nullptr};
  std::vector<MdMsg> m_msgs;

  void InitHtmlCtrl();
  wxString BuildFullHtmlFromMessages() const;
  wxString WrapMessage(const wxString &msgHtml, AiMarkdownRole role, const wxString &info, const wxString &time) const;
  wxString GetRoleLabel(AiMarkdownRole role) const;
  void OnHtmlLinkClicked(wxHtmlLinkEvent &event);
};
