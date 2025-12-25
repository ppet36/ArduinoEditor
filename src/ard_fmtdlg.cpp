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

#include "ard_fmtdlg.hpp"
#include "utils.hpp"

#include <wx/button.h>
#include <wx/hyperlink.h>
#include <wx/propgrid/advprops.h>
#include <wx/propgrid/propgrid.h>
#include <wx/sizer.h>
#include <wx/srchctrl.h>
#include <wx/stattext.h>

#include <nlohmann/json.hpp>

#include <algorithm>

using json = nlohmann::json;

namespace {

struct ClangFmtOptMeta {
  enum Type { Bool,
              Int,
              Enum,
              String };

  const char *key;
  Type type;
  const char *help;

  bool defBool = false;
  int defInt = 0;
  const char *defStr = "";

  const char *const *enumLabels = nullptr; // null-terminated list
};

static const char *kShortBlocksLabels[] = {"Never", "Empty", "Always", nullptr};
static const char *kBraceStyleLabels[] = {"Attach", "Allman", "Stroustrup", "Linux", "Mozilla", "WebKit", nullptr};
static const char *kSpaceBeforeParens[] = {"Never", "ControlStatements", "Always", nullptr};
static const char *kShortIfLabels[] = {"Never", "WithoutElse", "OnlyFirstIf", "AllIfsAndElse", "Always", nullptr};
static const char *kStyleLabels[] = {"LLVM", "GNU", "Google", "Chromium", "Microsoft", "Mozilla", "WebKit", nullptr};

// Starter pack: the most used things that also make sense for Arduino/embedded.
// TODO: add other parameters that make sense.
static const ClangFmtOptMeta kClangFmtMeta[] = {
    {"BasedOnStyle", ClangFmtOptMeta::Enum, "Based on style.", false, 0, "LLVM", kStyleLabels},

    {"SortIncludes", ClangFmtOptMeta::Bool,
     "Sort #include directives.", false, 0, "", nullptr},

    {"ReflowComments", ClangFmtOptMeta::Bool,
     "Reflow and wrap comments.", false, 0, "", nullptr},

    {"AllowShortBlocksOnASingleLine", ClangFmtOptMeta::Enum,
     "Allow short blocks on a single line.", false, 0, "Empty", kShortBlocksLabels},

    {"AllowShortIfStatementsOnASingleLine", ClangFmtOptMeta::Enum,
     "Allow short if statements on a single line.", false, 0, "WithoutElse", kShortIfLabels},

    {"BreakBeforeBraces", ClangFmtOptMeta::Enum,
     "Brace breaking style.", false, 0, "Attach", kBraceStyleLabels},

    {"SpaceBeforeParens", ClangFmtOptMeta::Enum,
     "Spaces before parentheses.", false, 0, "ControlStatements", kSpaceBeforeParens},

    {"AccessModifierOffset", ClangFmtOptMeta::Int,
     "The extra indent or outdent of access modifiers, e.g. public:", false, 0, "", nullptr}};

static json ParseJsonObject(const wxString &s) {
  if (s.IsEmpty())
    return json::object();

  try {
    json j = json::parse(wxToStd(s));
    if (!j.is_object())
      return json::object();
    return j;
  } catch (...) {
    return json::object();
  }
}

static wxString DumpJsonCompact(const json &j) {
  return wxString::FromUTF8(j.dump().c_str());
}

static bool IsSameAsDefault(const ClangFmtOptMeta &m, wxPGProperty *p) {
  if (!p)
    return true;

  switch (m.type) {
    case ClangFmtOptMeta::Bool:
      return (bool)p->GetValue().GetBool() == m.defBool;

    case ClangFmtOptMeta::Int:
      return (int)p->GetValue().GetInteger() == m.defInt;

    case ClangFmtOptMeta::String:
      return p->GetValue().GetString() == wxString::FromUTF8(m.defStr);

    case ClangFmtOptMeta::Enum:
      return p->GetValueAsString() == wxString::FromUTF8(m.defStr);
  }
  return true;
}

static void FillEnumLabels(wxArrayString &labels, const char *const *arr) {
  labels.Clear();
  for (const char *const *s = arr; s && *s; ++s) {
    labels.Add(wxString::FromUTF8(*s));
  }
}

static wxString ToLowerTrim(const wxString &s) {
  wxString x = s;
  x.Trim(true).Trim(false);
  return x.Lower();
}

enum {
  ID_FMT_RESET = wxID_HIGHEST + 937
};

} // namespace

ArduinoClangFormatSettingsDialog::ArduinoClangFormatSettingsDialog(wxWindow *parent,
                                                                   const wxString &overridesJsonIn)
    : wxDialog(parent, wxID_ANY, _("Clang-format options"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_overridesJsonIn(overridesJsonIn),
      m_overridesJsonOut(overridesJsonIn) {

  BuildUi();
  BuildGridFromMeta();
  LoadFromJson();

  CentreOnParent();
}

void ArduinoClangFormatSettingsDialog::BuildUi() {
  auto *top = new wxBoxSizer(wxVERTICAL);

  auto *info = new wxStaticText(this, wxID_ANY,
                                _("These options are used only when the sketch does NOT contain .clang-format or _clang-format."));
  top->Add(info, 0, wxALL | wxEXPAND, 8);

  m_search = new wxSearchCtrl(this, wxID_ANY);
  m_search->ShowCancelButton(true);
  top->Add(m_search, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

  m_pg = new wxPropertyGrid(this, wxID_ANY, wxDefaultPosition, wxSize(560, 420),
                            wxPG_DEFAULT_STYLE | wxPG_SPLITTER_AUTO_CENTER);
  top->Add(m_pg, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

  auto *link = new wxHyperlinkCtrl(this, wxID_ANY,
                                   _("Open clang-format style options documentation"),
                                   wxT("https://clang.llvm.org/docs/ClangFormatStyleOptions.html"));
  top->Add(link, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

  // Buttons row: Reset | (spacer) | OK Cancel
  auto *btnRow = new wxBoxSizer(wxHORIZONTAL);

  auto *resetBtn = new wxButton(this, ID_FMT_RESET, _("Reset to defaults"));
  btnRow->Add(resetBtn, 0, wxRIGHT, 8);

  btnRow->AddStretchSpacer(1);

  auto *ok = new wxButton(this, wxID_OK, _("OK"));
  auto *cancel = new wxButton(this, wxID_CANCEL, _("Cancel"));
  ok->SetDefault();

  btnRow->Add(ok, 0, wxRIGHT, 8);
  btnRow->Add(cancel, 0);

  top->Add(btnRow, 0, wxALL | wxEXPAND, 8);

  SetSizerAndFit(top);

  // Events
  m_search->Bind(wxEVT_TEXT, &ArduinoClangFormatSettingsDialog::OnSearchChanged, this);
  Bind(wxEVT_BUTTON, &ArduinoClangFormatSettingsDialog::OnReset, this, ID_FMT_RESET);
  Bind(wxEVT_BUTTON, &ArduinoClangFormatSettingsDialog::OnOk, this, wxID_OK);
}

void ArduinoClangFormatSettingsDialog::BuildGridFromMeta() {
  m_pg->Clear();

  for (const auto &m : kClangFmtMeta) {
    wxPGProperty *p = nullptr;
    wxString name = wxString::FromUTF8(m.key);

    switch (m.type) {
      case ClangFmtOptMeta::Bool:
        p = m_pg->Append(new wxBoolProperty(name, name, m.defBool));
        break;

      case ClangFmtOptMeta::Int:
        p = m_pg->Append(new wxIntProperty(name, name, m.defInt));
        break;

      case ClangFmtOptMeta::String:
        p = m_pg->Append(new wxStringProperty(name, name, wxString::FromUTF8(m.defStr)));
        break;

      case ClangFmtOptMeta::Enum: {
        wxArrayString labels;
        FillEnumLabels(labels, m.enumLabels);

        p = m_pg->Append(new wxEnumProperty(name, name, labels));
        p->SetValueFromString(wxString::FromUTF8(m.defStr));
        break;
      }
    }

    if (p && m.help && *m.help) {
      p->SetHelpString(wxString::FromUTF8(m.help));
    }
  }
}

void ArduinoClangFormatSettingsDialog::LoadFromJson() {
  json j = ParseJsonObject(m_overridesJsonIn);

  for (const auto &m : kClangFmtMeta) {
    const char *key = m.key;
    if (!j.contains(key))
      continue;

    wxPGProperty *p = m_pg->GetPropertyByName(wxString::FromUTF8(key));
    if (!p)
      continue;

    try {
      switch (m.type) {
        case ClangFmtOptMeta::Bool:
          if (j[key].is_boolean())
            p->SetValue((bool)j[key]);
          break;

        case ClangFmtOptMeta::Int:
          if (j[key].is_number_integer())
            p->SetValue((int)j[key]);
          break;

        case ClangFmtOptMeta::String:
          if (j[key].is_string())
            p->SetValue(wxString::FromUTF8(j[key].get<std::string>()));
          break;

        case ClangFmtOptMeta::Enum:
          if (j[key].is_string())
            p->SetValueFromString(wxString::FromUTF8(j[key].get<std::string>()));
          break;
      }
    } catch (...) {
      // ignore broken value
    }
  }
}

void ArduinoClangFormatSettingsDialog::SaveToJson() {
  json out = json::object();

  for (const auto &m : kClangFmtMeta) {
    wxPGProperty *p = m_pg->GetPropertyByName(wxString::FromUTF8(m.key));
    if (!p)
      continue;

    // Store only non-default values (keeps JSON small and stable)
    if (IsSameAsDefault(m, p))
      continue;

    switch (m.type) {
      case ClangFmtOptMeta::Bool:
        out[m.key] = (bool)p->GetValue().GetBool();
        break;

      case ClangFmtOptMeta::Int:
        out[m.key] = (int)p->GetValue().GetInteger();
        break;

      case ClangFmtOptMeta::String:
        out[m.key] = wxToStd(p->GetValue().GetString());
        break;

      case ClangFmtOptMeta::Enum:
        out[m.key] = wxToStd(p->GetValueAsString());
        break;
    }
  }

  m_overridesJsonOut = DumpJsonCompact(out);
}

void ArduinoClangFormatSettingsDialog::ApplyFilter(const wxString &filterLower) {
  for (const auto &m : kClangFmtMeta) {
    wxPGProperty *p = m_pg->GetPropertyByName(wxString::FromUTF8(m.key));
    if (!p)
      continue;

    bool show = filterLower.IsEmpty() ||
                wxString::FromUTF8(m.key).Lower().Find(filterLower) != wxNOT_FOUND;

    p->Hide(!show);
  }

  m_pg->Refresh();
}

void ArduinoClangFormatSettingsDialog::OnSearchChanged(wxCommandEvent &e) {
  ApplyFilter(ToLowerTrim(m_search->GetValue()));
  e.Skip();
}

void ArduinoClangFormatSettingsDialog::OnReset(wxCommandEvent &e) {
  // reset to defaults by rebuilding + clearing filter + reloading defaults
  m_search->ChangeValue(wxString());
  BuildGridFromMeta();
  m_overridesJsonIn = wxString(); // effectively empty => defaults
  LoadFromJson();
  e.Skip();
}

void ArduinoClangFormatSettingsDialog::OnOk(wxCommandEvent &e) {
  SaveToJson();
  e.Skip(); // closes the dialog
}
