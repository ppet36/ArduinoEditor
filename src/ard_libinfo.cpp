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

#include "ard_libinfo.hpp"
#include "ard_libman.hpp"
#include "utils.hpp"
#include <wx/filename.h>
#include <wx/hyperlink.h>

static void OpenInFileManager(const wxString &fullPath) {
  if (fullPath.empty())
    return;

  wxFileName fn(fullPath);
  wxString dir;

  if (fn.DirExists()) {
    // it's already a directory
    dir = fn.GetFullPath();
  } else {
    fn.Normalize(wxPATH_NORM_DOTS |
                 wxPATH_NORM_TILDE |
                 wxPATH_NORM_ENV_VARS |
                 wxPATH_NORM_CASE |
                 wxPATH_NORM_ABSOLUTE);
    dir = fn.GetPath();
  }

  if (!dir.empty()) {
    wxLaunchDefaultApplication(dir);
  }
}

void ArduinoLibraryDetailDialog::ShowInstalledLibraryInfo(wxWindow *parent,
                                                          const ArduinoLibraryInfo *installedInfo,
                                                          wxConfigBase *config,
                                                          ArduinoCli *cli) {
  if (!cli || !installedInfo) {
    return;
  }

  const auto &libs = cli->GetLibraries();
  if (libs.empty()) {
    return;
  }

  const ArduinoLibraryInfo *info = nullptr;
  for (const auto &lib : libs) {
    if (installedInfo->name == lib.name) {
      info = &lib; // here you take the address of the element in the vector
      break;
    }
  }

  if (!info) {
    return;
  }

  // constructor needs const ArduinoLibraryInfo& -> you dereference the pointer
  ArduinoLibraryDetailDialog dialog(parent, *info, installedInfo,
                                    config, wxEmptyString, false);
  dialog.ShowModal();
}

ArduinoLibraryDetailDialog::ArduinoLibraryDetailDialog(wxWindow *parent,
                                                       const ArduinoLibraryInfo &info,
                                                       const ArduinoLibraryInfo *installedInfo,
                                                       wxConfigBase *config,
                                                       const wxString &typeLabel,
                                                       bool installUninstallButton)
    : wxDialog(parent, wxID_ANY,
               wxString::FromUTF8(info.name.c_str()),
               wxDefaultPosition, wxSize(500, 400),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_installedInfo(installedInfo),
      m_config(config) {

  const auto &rel = info.latest;

  const ArduinoLibraryRelease *instRel =
      m_installedInfo ? &m_installedInfo->latest : nullptr;

  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  // header + version
  {
    auto *hdrSizer = new wxBoxSizer(wxHORIZONTAL);
    auto *nameTxt = new wxStaticText(this, wxID_ANY,
                                     wxString::FromUTF8(info.name.c_str()));
    nameTxt->SetFont(nameTxt->GetFont().Bold());

    wxString verLabel = _("Version: ") + wxString::FromUTF8(rel.version.c_str());
    auto *verTxt = new wxStaticText(this, wxID_ANY, verLabel);

    hdrSizer->Add(nameTxt, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 10);
    hdrSizer->Add(verTxt, 0, wxALIGN_CENTER_VERTICAL);

    topSizer->Add(hdrSizer, 0, wxALL | wxEXPAND, 8);
  }

  // Category + Maintainer + Installed + Type
  {
    wxString cat = _("Category: ") + wxString::FromUTF8(rel.category.c_str());
    wxString maint = _("Maintainer: ") + wxString::FromUTF8(rel.maintainer.c_str());

    topSizer->Add(new wxStaticText(this, wxID_ANY, cat),
                  0, wxLEFT | wxRIGHT | wxTOP, 8);
    topSizer->Add(new wxStaticText(this, wxID_ANY, maint),
                  0, wxLEFT | wxRIGHT | wxTOP, 4);

    if (!typeLabel.empty()) {
      wxString typeStr = _("Type: ") + typeLabel;
      topSizer->Add(new wxStaticText(this, wxID_ANY, typeStr),
                    0, wxLEFT | wxRIGHT | wxTOP, 4);
    }

    if (instRel) {
      wxString instVer;
      if (!instRel->version.empty()) {
        instVer = _("Installed version: ") + wxString::FromUTF8(instRel->version.c_str());
      } else {
        instVer = _("Installed: yes");
      }

      auto *instTxt = new wxStaticText(this, wxID_ANY, instVer);
      topSizer->Add(instTxt, 0, wxLEFT | wxRIGHT | wxTOP, 4);

      if (instRel->isLegacy) {
        auto *legacyTxt = new wxStaticText(this, wxID_ANY,
                                           _("Legacy library (old layout)"));
        legacyTxt->SetForegroundColour(wxColour(160, 0, 0));
        topSizer->Add(legacyTxt, 0, wxLEFT | wxRIGHT | wxTOP, 2);
      }
    }
  }

  // Website as a hyperlink (if present)
  if (!rel.website.empty()) {
    auto *link = new wxHyperlinkCtrl(this, wxID_ANY,
                                     wxString::FromUTF8(rel.website.c_str()),
                                     wxString::FromUTF8(rel.website.c_str()));
    topSizer->Add(link, 0, wxALL, 8);
  }

  // Description (sentence + paragraph)
  {
    wxString text;

    if (!rel.sentence.empty()) {
      text << wxString::FromUTF8(rel.sentence.c_str()) << wxT("\n\n");
    }
    if (!rel.paragraph.empty()) {
      text << wxString::FromUTF8(rel.paragraph.c_str()) << wxT("\n");
    }

    auto *desc = new wxTextCtrl(this, wxID_ANY, text,
                                wxDefaultPosition, wxDefaultSize,
                                wxTE_MULTILINE | wxTE_READONLY | wxBORDER_SIMPLE);
    desc->SetMinSize(wxSize(-1, 120));
    topSizer->Add(desc, 1, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 8);
  }

  // Architectures + includes + version
  {
    auto *gridSizer = new wxFlexGridSizer(0, 2, 4, 4);
    gridSizer->AddGrowableCol(1, 1);

    // Architectures
    {
      wxString label = _("Architectures:");
      wxString value;
      for (size_t i = 0; i < rel.architectures.size(); ++i) {
        if (i)
          value << wxT(", ");
        value << wxString::FromUTF8(rel.architectures[i].c_str());
      }
      gridSizer->Add(new wxStaticText(this, wxID_ANY, label),
                     0, wxALIGN_TOP);
      gridSizer->Add(new wxStaticText(this, wxID_ANY, value),
                     0, wxALIGN_TOP | wxEXPAND);
    }

    // Includes
    {
      wxString label = _("Provides includes:");
      wxString value;
      for (size_t i = 0; i < rel.providesIncludes.size(); ++i) {
        if (i)
          value << wxT(", ");
        value << wxString::FromUTF8(rel.providesIncludes[i].c_str());
      }
      gridSizer->Add(new wxStaticText(this, wxID_ANY, label),
                     0, wxALIGN_TOP);
      gridSizer->Add(new wxStaticText(this, wxID_ANY, value),
                     0, wxALIGN_TOP | wxEXPAND);
    }

    // Available versions
    {
      wxString label = _("Available versions:");
      wxString value;
      for (size_t i = 0; i < info.availableVersions.size(); ++i) {
        if (i)
          value << wxT(", ");
        value << wxString::FromUTF8(info.availableVersions[i].c_str());
      }
      gridSizer->Add(new wxStaticText(this, wxID_ANY, label),
                     0, wxALIGN_TOP);
      gridSizer->Add(new wxStaticText(this, wxID_ANY, value),
                     0, wxALIGN_TOP | wxEXPAND);
    }

    topSizer->Add(gridSizer, 0, wxALL | wxEXPAND, 8);

    // Location
    if (instRel && !instRel->location.empty()) {
      wxString label = _("Location:");
      wxString value = wxString::FromUTF8(instRel->location.c_str());

      gridSizer->Add(new wxStaticText(this, wxID_ANY, label),
                     0, wxALIGN_TOP);
      gridSizer->Add(new wxStaticText(this, wxID_ANY, value),
                     0, wxALIGN_TOP | wxEXPAND);
    }

    // Dependencies
    {
      wxString label = _("Dependencies:");
      wxString value;

      for (size_t i = 0; i < rel.dependencies.size(); ++i) {
        if (i)
          value << wxT(", ");
        value << wxString::FromUTF8(rel.dependencies[i].c_str());
      }

      // If you don't want "(none)", feel free to remove that ternary operator
      if (value.empty()) {
        value = _("(none)");
      }

      gridSizer->Add(new wxStaticText(this, wxID_ANY, label),
                     0, wxALIGN_TOP);
      gridSizer->Add(new wxStaticText(this, wxID_ANY, value),
                     0, wxALIGN_TOP | wxEXPAND);
    }

    // Install dir
    if (instRel && !instRel->installDir.empty()) {
      wxString labelText = _("Install dir:");
      wxString path = wxString::FromUTF8(instRel->installDir.c_str());

      // the label will be clickable
      auto *labelCtrl = new wxStaticText(this, wxID_ANY, labelText);

      // value as a hyperlink (without an actual URL, handled in the handler)
      auto *linkCtrl = new wxHyperlinkCtrl(this, wxID_ANY, path, wxEmptyString);

      // click on the label opens the file manager
      labelCtrl->Bind(wxEVT_LEFT_DOWN,
                      [path](wxMouseEvent &evt) {
                        OpenInFileManager(path);
                        // we do not want to propagate the click further so that it does not do anything else
                        evt.Skip(false);
                      });

      // click on the hyperlink as well
      linkCtrl->Bind(wxEVT_HYPERLINK,
                     [path](wxHyperlinkEvent &WXUNUSED(evt)) {
                       OpenInFileManager(path);
                     });

      gridSizer->Add(labelCtrl, 0, wxALIGN_TOP);
      gridSizer->Add(linkCtrl, 0, wxALIGN_TOP | wxEXPAND);
    }
  }

  // Buttons
  auto *btnSizer = new wxBoxSizer(wxHORIZONTAL);
  if (installUninstallButton) {
    // Install
    auto *installBtn = new wxButton(this, wxID_ANY, _("Install..."));
    installBtn->Bind(wxEVT_BUTTON,
                     [this, &info, installBtn](wxCommandEvent &) {
                       auto *manager =
                           wxDynamicCast(GetParent(), ArduinoLibraryManagerFrame);
                       if (!manager)
                         return;

                       wxMenu menu;

                       const int latestId = 1;
                       menu.Append(latestId, _("Latest"));
                       menu.Bind(
                           wxEVT_MENU,
                           [manager, &info](wxCommandEvent &) {
                             manager->StartInstallRepo(info, std::string{});
                           },
                           latestId);

                       if (!info.availableVersions.empty()) {
                         menu.AppendSeparator();
                         int baseId = 1000;
                         for (size_t i = 0; i < info.availableVersions.size(); ++i) {
                           int id = baseId + (int)i;
                           wxString v = wxString::FromUTF8(
                               info.availableVersions[i].c_str());
                           menu.Append(id, v);
                           std::string ver = info.availableVersions[i];
                           menu.Bind(
                               wxEVT_MENU,
                               [manager, &info, ver](wxCommandEvent &) {
                                 manager->StartInstallRepo(info, ver);
                               },
                               id);
                         }
                       }

                       wxPoint pos = installBtn->GetPosition();
                       pos.y += installBtn->GetSize().y;
                       PopupMenu(&menu, pos);
                     });

    btnSizer->Add(installBtn, 0, wxALL, 8);

    // Uninstall - only if the library is installed
    if (m_installedInfo) {
      auto *uninstallBtn = new wxButton(this, wxID_ANY, _("Uninstall"));

      uninstallBtn->Bind(wxEVT_BUTTON,
                         [this, &info](wxCommandEvent &) {
                           auto *manager =
                               wxDynamicCast(GetParent(), ArduinoLibraryManagerFrame);
                           if (!manager)
                             return;

                           manager->StartUninstallRepo(info);
                         });

      btnSizer->Add(uninstallBtn, 0, wxTOP | wxBOTTOM | wxRIGHT, 8);
    }
  }

  btnSizer->AddStretchSpacer(1);
  btnSizer->Add(new wxButton(this, wxID_OK, _("Close")), 0, wxALL, 8);
  topSizer->Add(btnSizer, 0, wxEXPAND);

  SetSizerAndFit(topSizer);

  // load position/size from config
  if (!LoadWindowSize(wxT("LibraryDetail"), this, m_config)) {
    CentreOnParent();
  }
}

ArduinoLibraryDetailDialog::~ArduinoLibraryDetailDialog() {
  SaveWindowSize(wxT("LibraryDetail"), this, m_config);
}
