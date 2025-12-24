#include "ard_aboutdlg.hpp"
#include "ard_setdlg.hpp"
#include "utils.hpp"
#include <wx/html/htmlwin.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/utils.h>

#define VERSION "1.0.0"

static wxString GetAboutHtml() {
  wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
  wxString bgHex = ColorToHex(bg);

  wxString tpl = _(""
                   "<html>\n"
                   "<body bgcolor=\"%s\">\n"
                   "   <a name=\"top\"></a>\n"
                   "   <h2>Arduino Editor %s</h2>\n"
                   "\n"
                   "   <p>\n"
                   "     <b>Modern Arduino IDE powered by Clang</b>\n"
                   "     <p>\n"
                   "       <i>Fast code completion. Smart navigation. No nonsense.</i>\n"
                   "     </p>\n"
                   "   </p>\n"
                   "\n"
                   "   <hr>\n"
                   "\n"
                   "   <p>\n"
                   "     Arduino Editor is a modern development environment for Arduino projects\n"
                   "     built on the Clang toolchain. It provides fast and precise code completion,\n"
                   "     smart navigation tools, real-time error highlighting, and full support for\n"
                   "     multi-file Arduino sketches.\n"
                   "   </p>\n"
                   "\n"
                   "   <p>\n"
                   "     The goal of the project is to offer a stable, powerful and professional IDE\n"
                   "     as a drop-in alternative for the official Arduino IDE.\n"
                   "   </p>\n"
                   "\n"
                   "   <h3>Key Features</h3>\n"
                   "   <ul>\n"
                   "     <li><a href=\"#async\">Asynchronous processing</a> - the UI stays responsive even on large projects</li>\n"
                   "     <li><a href=\"#navigation\">Smart navigation</a> - go to definition, follow symbol, hover information</li>\n"
                   "     <li><a href=\"#clang\">Clang-based engine</a> for parsing, symbol resolution and code completion</li>\n"
                   "     <li><a href=\"#project\">Project-wide support</a> for structured sketches and multiple .ino/.cpp/.hpp files</li>\n"
                   "     <li><a href=\"#refactoring\">Advanced refactoring</a> - safe, semantic code transformations</li>\n"
                   "     <li><a href=\"#ai\">AI-assisted development</a> - patch-based edits with diff preview and user control</li>\n"
                   "     <li><a href=\"#editor\">Configurable editor</a> - colors, fonts and behavior can be customized</li>\n"
                   "     <li><a href=\"#cli\">Arduino CLI integration</a> for building and uploading sketches</li>\n"
                   "   </ul>\n"
                   "\n"
                   "   <hr>\n"
                   "\n"
                   "   <a name=\"async\"></a>\n"
                   "   <h3>Asynchronous Processing</h3>\n"
                   "   <p>\n"
                   "     Heavy operations such as parsing, indexing, compilation and AI requests are executed\n"
                   "     asynchronously to keep the user interface responsive.\n"
                   "   </p>\n"
                   "   <p><a href=\"#top\">Back to top</a></p>\n"
                   "\n"
                   "   <hr>\n"
                   "\n"
                   "   <a name=\"navigation\"></a>\n"
                   "   <h3>Smart Navigation</h3>\n"
                   "   <ul>\n"
                   "     <li>Go to definition / declaration</li>\n"
                   "     <li>Navigate between symbol references across files</li>\n"
                   "     <li>Navigation history with backward and forward movement</li>\n"
                   "     <li>Hover information powered by semantic analysis</li>\n"
                   "   </ul>\n"
                   "   <p><a href=\"#top\">Back to top</a></p>\n"
                   "\n"
                   "   <hr>\n"
                   "\n"
                   "   <a name=\"clang\"></a>\n"
                   "   <h3>Clang-based Engine</h3>\n"
                   "   <p>\n"
                   "     The editor uses Clang/LLVM to understand code semantically (AST-based), enabling\n"
                   "     precise completion, symbol resolution and on-the-fly diagnostics while editing.\n"
                   "   </p>\n"
                   "   <p><a href=\"#top\">Back to top</a></p>\n"
                   "\n"
                   "   <hr>\n"
                   "\n"
                   "   <a name=\"project\"></a>\n"
                   "   <h3>Project-wide Multi-file Support</h3>\n"
                   "   <p>\n"
                   "     Full support for structured sketches and multiple files (.ino/.cpp/.hpp), including\n"
                   "     project-wide indexing, navigation and explicit support for structured Arduino projects.\n"
                   "   </p>\n"
                   "   <p><a href=\"#top\">Back to top</a></p>\n"
                   "\n"
                   "   <hr>\n"
                   "\n"
                   "   <a name=\"refactoring\"></a>\n"
                   "   <h3>Advanced Refactoring</h3>\n"
                   "   <p>\n"
                   "     Clang-powered refactoring tools help you change code safely and predictably.\n"
                   "     Transformations are semantic (AST-based), not simple text replacements.\n"
                   "   </p>\n"
                   "   <ul>\n"
                   "     <li>Rename symbol across the project with full semantic awareness</li>\n"
                   "     <li>Extract function from selected code blocks</li>\n"
                   "     <li>Inline variable and introduce variable refactorings</li>\n"
                   "      <li>Create implementation from declaration and declaration from implementation</li>\n"
                   "   </ul>\n"
                   "   <p><a href=\"#top\">Back to top</a></p>\n"
                   "\n"
                   "   <hr>\n"
                   "\n"
                   "   <a name=\"ai\"></a>\n"
                   "   <h3>AI-Assisted Development</h3>\n"
                   "   <p>\n"
                   "     Arduino Editor includes a deeply integrated AI assistant designed to work directly\n"
                   "     with your project structure and source code.\n"
                   "   </p>\n"
                   "   <ul>\n"
                   "     <li>Context-aware code modifications using structured patches</li>\n"
                   "     <li>Multi-file awareness for complex sketches</li>\n"
                   "     <li>Explicit diff previews before applying any change</li>\n"
                   "     <li>AI-assisted error analysis and problem solving</li>\n"
                   "     <li>Session-based conversations with persistent history</li>\n"
                   "   </ul>\n"
                   "   <p>\n"
                   "     The AI never modifies code blindly: every change is explicit, reviewable and\n"
                   "     user-approved.\n"
                   "   </p>\n"
                   "   <p><a href=\"#top\">Back to top</a></p>\n"
                   "\n"
                   "   <hr>\n"
                   "\n"
                   "   <a name=\"editor\"></a>\n"
                   "   <h3>Configurable Editor</h3>\n"
                   "   <p>\n"
                   "     Customize colors, fonts and behavior, with support for system light and dark themes.\n"
                   "   </p>\n"
                   "   <p><a href=\"#top\">Back to top</a></p>\n"
                   "\n"
                   "   <hr>\n"
                   "\n"
                   "   <a name=\"cli\"></a>\n"
                   "   <h3>Arduino CLI Integration</h3>\n"
                   "   <p>\n"
                   "       Deep integration with arduino-cli for building, uploading, library management,\n"
                   "       board management and compiler toolchain configuration.\n"
                   "   </p>\n"
                   "   <p><a href=\"#top\">Back to top</a></p>\n"
                   "\n"
                   "   <hr>\n"
                   "\n"
                   "   <p>\n"
                   "     <b>Author:</b> Pavel Petrzela (ppet36)<br/>\n"
                   "     <b>Powered by:</b> wxWidgets, Clang/LLVM, Arduino CLI\n"
                   "   </p>\n"
                   "</body>\n"
                   "</html>\n");

  return wxString::Format(tpl, bgHex, wxString::FromUTF8(VERSION));
}

static wxString GetLicensesHtml() {
  wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
  wxString bgHex = ColorToHex(bg);

  wxString lpl = _(R"(
<html>
<body bgcolor="%s">
  <h3>Arduino Editor License (GPLv3)</h3>
  <p>
    Arduino Editor is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License version 3 (GPLv3).
  </p>
  <p>
    For the full license text see:<br>
    <a href="https://www.gnu.org/licenses/gpl-3.0.html#license-text">
      https://www.gnu.org/licenses/gpl-3.0.html
    </a>
  </p>

  <hr>

  <h3>wxWidgets License</h3>
  <p>
    wxWidgets is distributed under the wxWidgets License.
  </p>
  <p>
    For details see:<br>
    <a href="https://www.wxwidgets.org/about/licence/">
      https://www.wxwidgets.org/about/licence/
    </a>
  </p>

  <hr>

  <h3>Clang/LLVM License</h3>
  <p>
    Clang and LLVM are distributed under the Apache License v2.0
    with LLVM exceptions.
  </p>
  <p>
    For details see:<br>
    <a href="https://llvm.org/docs/DeveloperPolicy.html#license">
      https://llvm.org/docs/DeveloperPolicy.html#license
    </a>
  </p>

  <hr>

  <h3>arduino-cli License (GPLv3)</h3>
  <p>
    arduino-cli is distributed under the terms of the GNU General Public License
    version 3 (GPLv3).
  </p>
  <p>
    For the full license text see:<br>
    <a href="https://github.com/arduino/arduino-cli/blob/master/LICENSE.txt">
      https://github.com/arduino/arduino-cli/blob/master/LICENSE.txt
    </a>
  </p>
</body>
</html>
)");
  return wxString::Format(lpl, bgHex);
}

ArduinoAboutDialog::ArduinoAboutDialog(wxWindow *parent)
    : wxDialog(parent,
               wxID_ANY,
               _("About Arduino Editor"),
               wxDefaultPosition,
               wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {

  wxConfigBase *config = wxConfigBase::Get();
  EditorSettings settings;
  settings.Load(config);
  int pointSize = settings.GetFont().GetPointSize();

  auto setupHtmlFonts = [&](wxHtmlWindow *w) {
#ifdef __WXMAC__
    w->SetFonts(wxT("Helvetica Neue"), wxT("Menlo"), nullptr);
#elif defined(__WXMSW__)
    w->SetFonts(wxT("Segoe UI"), wxT("Consolas"), nullptr);
#else
    w->SetFonts(wxT("DejaVu Sans"), wxT("DejaVu Sans Mono"), nullptr);
#endif

    static const int sizes[] = {pointSize, pointSize + 1, pointSize + 2, pointSize + 3, pointSize + 4, pointSize + 5, pointSize + 6};
    w->SetFonts(wxEmptyString, wxEmptyString, sizes);
  };

  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  // Notebook
  auto *notebook = new wxNotebook(this, wxID_ANY);

  // --- 1) About page -------------------------------------------------------
  auto *aboutPanel = new wxPanel(notebook, wxID_ANY);
  auto *aboutSizer = new wxBoxSizer(wxVERTICAL);

  auto *html = new wxHtmlWindow(aboutPanel,
                                wxID_ANY,
                                wxDefaultPosition,
                                wxDefaultSize,
                                wxHW_SCROLLBAR_AUTO);

  setupHtmlFonts(html);

  wxString aboutHtml = GetAboutHtml();
  html->SetPage(aboutHtml);
  html->Bind(wxEVT_HTML_LINK_CLICKED, &ArduinoAboutDialog::OnHtmlLink, this);

  aboutSizer->Add(html, 1, wxEXPAND | wxALL, 10);
  aboutPanel->SetSizer(aboutSizer);

  notebook->AddPage(aboutPanel, _("About"), true);

  // --- 2) Licenses page ----------------------------------------------------
  auto *licPanel = new wxPanel(notebook, wxID_ANY);
  auto *licSizer = new wxBoxSizer(wxVERTICAL);

  auto *licHtml = new wxHtmlWindow(licPanel,
                                   wxID_ANY,
                                   wxDefaultPosition,
                                   wxDefaultSize,
                                   wxHW_SCROLLBAR_AUTO);
  setupHtmlFonts(licHtml);

  licHtml->SetPage(GetLicensesHtml());
  licHtml->Bind(wxEVT_HTML_LINK_CLICKED, &ArduinoAboutDialog::OnHtmlLink, this);

  licSizer->Add(licHtml, 1, wxEXPAND | wxALL, 10);
  licPanel->SetSizer(licSizer);

  notebook->AddPage(licPanel, _("Licenses"), false);

  // Notebook to the main sizer
  topSizer->Add(notebook, 1, wxEXPAND | wxALL, 10);

  // Buttons
  auto *btnSizer = CreateStdDialogButtonSizer(wxOK);
  if (btnSizer)
    topSizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, 10);

  SetSizer(topSizer);
  SetSize(wxSize(800, 600));
  CenterOnScreen();
}

void ArduinoAboutDialog::OnHtmlLink(wxHtmlLinkEvent &event) {
  const wxString href = event.GetLinkInfo().GetHref();
  if (href.IsEmpty())
    return;

  if (!href.StartsWith(wxT("#"))) {
    wxLaunchDefaultBrowser(href);
  }
  event.Skip();
}
