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

#include "ard_mdwidget.hpp"
#include "ard_setdlg.hpp"
#include "utils.hpp"
#include <memory>
#include <sstream>
#include <string>

#include "maddy/parser.h"

#include <wx/html/htmlwin.h>
#include <wx/platform.h>

static wxString ArduinoMarkdown_HtmlEscape(const wxString &s) {
  wxString out;
  out.reserve(s.length() * 12 / 10);

  for (size_t i = 0; i < s.length(); ++i) {
    wxChar ch = s[i];
    switch (ch) {
      case '&':
        out << wxT("&amp;");
        break;
      case '<':
        out << wxT("&lt;");
        break;
      case '>':
        out << wxT("&gt;");
        break;
      case '"':
        out << wxT("&quot;");
        break;
      default:
        out << ch;
        break;
    }
  }
  return out;
}

wxString ArduinoMarkdown_MarkdownToHtmlFragment(const wxString &input) {
  std::stringstream markdownInput("");

  std::shared_ptr<maddy::ParserConfig> config = std::make_shared<maddy::ParserConfig>();
  config->enabledParsers &= ~maddy::types::EMPHASIZED_PARSER; // disable emphasized parser

  std::shared_ptr<maddy::Parser> parser = std::make_shared<maddy::Parser>(config);

  std::string md = wxToStd(ArduinoMarkdown_HtmlEscape(input));
  std::istringstream iss(md);
  std::string htmlOutput = parser->Parse(iss);

  return wxString::FromUTF8(htmlOutput);
}

// -------------------------------------------------------------------------------------------------
//  ArduinoMarkdownPanel
// -------------------------------------------------------------------------------------------------

#include <wx/sizer.h>

ArduinoMarkdownPanel::ArduinoMarkdownPanel(wxWindow *parent,
                                           wxWindowID id,
                                           const wxPoint &pos,
                                           const wxSize &size,
                                           long style)
    : wxPanel(parent, id, pos, size, style) {
  auto *sizer = new wxBoxSizer(wxVERTICAL);

  m_html = new wxHtmlWindow(this, wxID_ANY,
                            wxDefaultPosition,
                            wxDefaultSize,
                            wxHW_SCROLLBAR_AUTO);

  InitHtmlCtrl();

  sizer->Add(m_html, 1, wxEXPAND | wxALL, 0);
  SetSizer(sizer);

  Clear();
}

void ArduinoMarkdownPanel::InitHtmlCtrl() {
#ifdef __WXMAC__
  m_html->SetFonts(wxT("Helvetica Neue"), wxT("Menlo"), nullptr);
#elif defined(__WXMSW__)
  m_html->SetFonts(wxT("Segoe UI"), wxT("Consolas"), nullptr);
#else
  m_html->SetFonts(wxT("DejaVu Sans"), wxT("DejaVu Sans Mono"), nullptr);
#endif
  wxConfigBase *config = wxConfigBase::Get();
  EditorSettings settings;
  settings.Load(config);

  wxFont font = settings.GetFont();
  int pointSize = font.GetPointSize();

  static const int sizes[] = {pointSize, pointSize + 1, pointSize + 2, pointSize + 3, pointSize + 4, pointSize + 5, pointSize + 6};
  m_html->SetFonts(wxEmptyString, wxEmptyString, sizes);

  m_html->Bind(wxEVT_HTML_LINK_CLICKED, &ArduinoMarkdownPanel::OnHtmlLinkClicked, this);
}

void ArduinoMarkdownPanel::SetBaseFonts(const wxString &normalFace,
                                        const wxString &fixedFace,
                                        const int *sizes) {
  m_html->SetFonts(normalFace, fixedFace, sizes);
  m_html->SetPage(BuildFullHtmlFromMessages());
}

void ArduinoMarkdownPanel::Clear() {
  m_msgs.clear();
  Render(false);
}

wxString ArduinoMarkdownPanel::GetRoleLabel(AiMarkdownRole role) const {
  switch (role) {
    case AiMarkdownRole::User:
      return _("You");
    case AiMarkdownRole::Assistant:
      return _("Assistant");
    case AiMarkdownRole::System:
      return _("System");
    case AiMarkdownRole::Info:
      return _("Info");
    case AiMarkdownRole::Error:
      return _("Error");
    default:
      return _("Message");
  }
}

wxString ArduinoMarkdownPanel::WrapMessage(const wxString &msgHtml,
                                           AiMarkdownRole role,
                                           const wxString &info,
                                           const wxString &time) const {
  wxString label = GetRoleLabel(role);

  wxConfigBase *config = wxConfigBase::Get();
  EditorSettings settings;
  settings.Load(config);

  EditorColorScheme colors = settings.GetColors();

  wxColour headerBg;
  switch (role) {
    case AiMarkdownRole::User:
      headerBg = colors.aiUserBg;
      break;
    case AiMarkdownRole::Assistant:
      headerBg = colors.aiAssistantBg;
      break;
    case AiMarkdownRole::System:
      headerBg = colors.aiSystemBg;
      break;
    case AiMarkdownRole::Info:
      headerBg = colors.aiInfoBg;
      break;
    case AiMarkdownRole::Error:
      headerBg = colors.aiErrorBg;
      break;
    default:
      headerBg = colors.background;
      break;
  }

  const wxString safeInfo = ArduinoMarkdown_HtmlEscape(info);
  const wxString safeTime = ArduinoMarkdown_HtmlEscape(time);

  wxString html;

  if (!safeTime.IsEmpty()) {
    html << wxT("<p></p><font color=\"#666666\" size=\"-1\">")
         << safeTime
         << wxT("</font><br>\n");
  }

  html << wxT("<table width=\"100%\" border=\"0\" cellspacing=\"0\" cellpadding=\"4\">")
       << wxT("<tr bgcolor=\"") << ColorToHex(headerBg) << wxT("\">")
       << wxT("<td width=\"20%\"><b>") << label << wxT("</b></td>");

  if (!safeInfo.IsEmpty()) {
    html << wxT("<td witdh=\"80%\" align=\"right\"><font size=\"-1\">")
         << safeInfo
         << wxT("</font></td>");
  } else {
    html << wxT("<td></td>");
  }

  html << wxT("</tr>")
       << wxT("<tr><td colspan=\"2\">")
       << msgHtml
       << wxT("</td></tr>")
       << wxT("</table>")
       << wxT("<br>\n");

  return html;
}

wxString ArduinoMarkdownPanel::BuildFullHtmlFromMessages() const {
  wxConfigBase *config = wxConfigBase::Get();
  EditorSettings settings;
  settings.Load(config);
  EditorColorScheme colors = settings.GetColors();

  // zvol si textovou barvu podle toho co v scheme máš:
  // wxColour fg = colors.foreground;  // pokud existuje
  wxColour bg = colors.background;
  wxColour fg = colors.text;

  wxString body;
  for (const auto &m : m_msgs) {
    wxString frag = ArduinoMarkdown_MarkdownToHtmlFragment(m.markdown);
    body << WrapMessage(frag, m.role, m.info, m.time) << wxT("\n");
  }

  wxString html;
  html << wxT("<html><body bgcolor=\"") << ColorToHex(bg)
       << wxT("\" text=\"") << ColorToHex(fg)
       << wxT("\">")
       << body
       << wxT("</body></html>");

  APP_DEBUG_LOG("MDW: HTML=\n%s", wxToStd(html).c_str());
  return html;
}

void ArduinoMarkdownPanel::Render(bool scrollToEnd) {
  m_html->SetPage(BuildFullHtmlFromMessages());

  if (scrollToEnd) {
    m_html->CallAfter([this]() {
      int xUnit, yUnit;
      m_html->GetScrollPixelsPerUnit(&xUnit, &yUnit);

      int xPos, yPos;
      m_html->GetViewStart(&xPos, &yPos);

      int xRange, yRange;
      m_html->GetVirtualSize(&xRange, &yRange);

      if (yUnit > 0) {
        m_html->Scroll(xPos, yRange / yUnit);
      }
    });
  } else {
    m_html->Refresh();
    m_html->Update();
  }
}

void ArduinoMarkdownPanel::AppendMarkdown(const wxString &markdown,
                                          AiMarkdownRole role,
                                          const wxString &info,
                                          const wxString &time) {
  m_msgs.push_back({markdown, role, info, time});
  Render(true);
}

void ArduinoMarkdownPanel::OnHtmlLinkClicked(wxHtmlLinkEvent &event) {
  wxString href = event.GetLinkInfo().GetHref();
  href.Trim(true).Trim(false);

  // Basic safety: only allow common external schemes
  wxString lower = href.Lower();
  if (lower.StartsWith(wxT("http://")) || lower.StartsWith(wxT("https://")) || lower.StartsWith(wxT("mailto:"))) {
    wxLaunchDefaultBrowser(href);
    return;
  }
}
