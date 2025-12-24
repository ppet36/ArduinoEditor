#include "ard_mdwidget.hpp"
#include "ard_setdlg.hpp"
#include "utils.hpp"

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
  wxString text = input;
  text.Replace(wxT("\r\n"), wxT("\n"));
  text.Replace(wxT("\r"), wxT("\n"));

  wxString escaped;
  escaped.reserve(text.length() * 12 / 10);

  enum CodeMode { CODE_NONE,
                  CODE_INLINE,
                  CODE_BLOCK };
  CodeMode mode = CODE_NONE;

  bool bold = false; // **bold**

  const size_t len = text.length();
  for (size_t i = 0; i < len; ++i) {
    wxChar ch = text[i];

    // block: ```code```
    if (ch == '`' && i + 2 < len && text[i + 1] == '`' && text[i + 2] == '`') {
      if (mode == CODE_BLOCK) {
        escaped << wxT("</code></pre>");
        mode = CODE_NONE;
      } else if (mode == CODE_NONE) {
        mode = CODE_BLOCK;
        escaped << wxT("<pre><code>");
        // skip "```"
        i += 2;
        // skip optional language id (until EOL)
        while (i + 1 < len && text[i + 1] != '\n' && text[i + 1] != '\r') {
          ++i;
        }
      } else {
        escaped << wxT("```");
        i += 2;
      }
      continue;
    }

    // inline: `code`
    if (ch == '`' && mode != CODE_BLOCK) {
      if (mode == CODE_INLINE) {
        escaped << wxT("</code>");
        mode = CODE_NONE;
      } else {
        escaped << wxT("<code>");
        mode = CODE_INLINE;
      }
      continue;
    }

    // **bold** (ignore inside code)
    if (mode == CODE_NONE && ch == '*' && i + 1 < len && text[i + 1] == '*') {
      if (bold)
        escaped << wxT("</b>");
      else
        escaped << wxT("<b>");
      bold = !bold;
      ++i; // skip second '*'
      continue;
    }

    switch (ch) {
      case '&':
        escaped << wxT("&amp;");
        break;
      case '<':
        escaped << wxT("&lt;");
        break;
      case '>':
        escaped << wxT("&gt;");
        break;
      case '"':
        escaped << wxT("&quot;");
        break;
      default:
        escaped << ch;
        break;
    }
  }

  if (mode == CODE_INLINE) {
    escaped << wxT("</code>");
  } else if (mode == CODE_BLOCK) {
    escaped << wxT("</code></pre>");
  }

  // close unbalanced **
  if (bold) {
    escaped << wxT("</b>");
  }

  std::vector<wxString> paragraphs;
  wxString current;
  wxString line;

  auto flushLine = [&](bool endOfText) {
    wxString trimmed = line;
    trimmed.Trim(true).Trim(false);

    if (trimmed.IsEmpty()) {
      if (!current.IsEmpty()) {
        paragraphs.push_back(current);
        current.clear();
      }
    } else {
      if (!current.IsEmpty()) {
        current << wxT("\n") << line;
      } else {
        current = line;
      }
    }
    line.clear();

    if (endOfText && !current.IsEmpty()) {
      paragraphs.push_back(current);
      current.clear();
    }
  };

  for (size_t i = 0; i < escaped.length(); ++i) {
    wxChar ch = escaped[i];
    if (ch == '\n') {
      flushLine(false);
    } else {
      line << ch;
    }
  }
  if (!line.IsEmpty() || !current.IsEmpty()) {
    wxString trimmedLine = line;
    trimmedLine.Trim(true).Trim(false);
    if (!trimmedLine.IsEmpty()) {
      if (!current.IsEmpty()) {
        current << wxT("\n") << line;
      } else {
        current = line;
      }
    }
    if (!current.IsEmpty()) {
      paragraphs.push_back(current);
      current.clear();
    }
  }

  // Heuristic: models sometimes glue headings like "...sentence.### Step 1".
  // Insert a newline before '#... ' if it doesn't start a line.
  auto normalizeGluedHeadings = [&](const wxString &in) -> wxString {
    wxString out;
    out.reserve(in.length() + 8);

    const size_t n = in.length();
    for (size_t i = 0; i < n; ++i) {
      wxChar c = in[i];

      if (c == '#' && i + 1 < n) {
        // count 1..6 hashes
        size_t j = i;
        int level = 0;
        while (j < n && in[j] == '#' && level < 6) {
          ++level;
          ++j;
        }
        // heading needs at least one space after hashes
        if (level >= 1 && level <= 6 && j < n && in[j] == ' ') {
          // if not at start of line, force newline
          if (i > 0 && in[i - 1] != '\n') {
            out << wxT("\n");
          }
          out << in.Mid(i, j - i + 1); // "### "
          i = j;                       // continue after the space
          continue;
        }
      }

      out << c;
    }
    return out;
  };

  auto isHeadingLine = [&](const wxString &ln, int &levelOut, wxString &contentOut) -> bool {
    // allow up to 3 leading spaces
    size_t pos = 0;
    while (pos < ln.length() && pos < 3 && ln[pos] == ' ')
      ++pos;

    size_t j = pos;
    int level = 0;
    while (j < ln.length() && ln[j] == '#' && level < 6) {
      ++level;
      ++j;
    }
    if (level < 1 || level > 6)
      return false;
    if (j >= ln.length() || ln[j] != ' ')
      return false;

    wxString content = ln.Mid(j + 1);
    content.Trim(true).Trim(false);
    if (content.IsEmpty())
      return false;

    levelOut = level;
    contentOut = content;
    return true;
  };

  wxString body;
  for (size_t i = 0; i < paragraphs.size(); ++i) {
    if (!body.IsEmpty())
      body << wxT("\n\n");

    wxString p = paragraphs[i];

    if (p.Contains(wxT("<pre>"))) {
      body << p;
      continue;
    }

    p = normalizeGluedHeadings(p);

    // Split paragraph into lines, emit headings as <hN>, rest as <p> with <br>.
    wxString paraText;
    auto flushParaText = [&]() {
      if (paraText.IsEmpty())
        return;
      wxString t = paraText;
      t.Replace(wxT("\n"), wxT("<br>\n"));
      body << wxT("<p>") << t << wxT("</p>");
      paraText.clear();
    };

    wxString curLine;
    for (size_t k = 0; k < p.length(); ++k) {
      wxChar ch = p[k];
      if (ch == '\n') {
        int level = 0;
        wxString htxt;
        if (isHeadingLine(curLine, level, htxt)) {
          flushParaText();
          body << wxString::Format(wxT("<h%d>"), level) << htxt
               << wxString::Format(wxT("</h%d>"), level);
        } else {
          if (!paraText.IsEmpty())
            paraText << wxT("\n");
          paraText << curLine;
        }
        curLine.clear();
      } else {
        curLine << ch;
      }
    }

    // last line
    if (!curLine.IsEmpty()) {
      int level = 0;
      wxString htxt;
      if (isHeadingLine(curLine, level, htxt)) {
        flushParaText();
        body << wxString::Format(wxT("<h%d>"), level) << htxt
             << wxString::Format(wxT("</h%d>"), level);
      } else {
        if (!paraText.IsEmpty())
          paraText << wxT("\n");
        paraText << curLine;
      }
    }

    flushParaText();
  }

  return body;
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
