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

#include "ard_coreinfo.hpp"
#include "ard_coreman.hpp"
#include "utils.hpp"

#include <wx/button.h>
#include <wx/hyperlink.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

void ArduinoCoreDetailDialog::ShowCoreInfo(wxWindow *parent,
                                           const ArduinoCoreInfo &info,
                                           wxConfigBase *config) {
  ArduinoCoreDetailDialog dlg(parent, info, config, wxEmptyString, false);
  dlg.ShowModal();
}

ArduinoCoreDetailDialog::ArduinoCoreDetailDialog(wxWindow *parent,
                                                 const ArduinoCoreInfo &info,
                                                 wxConfigBase *config,
                                                 const wxString &typeLabel,
                                                 bool installUninstallButton)
    : wxDialog(parent, wxID_ANY,
               wxString::FromUTF8(info.id.c_str()),
               wxDefaultPosition, wxSize(520, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_config(config) {

  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  // header: ID + latest / installed
  {
    auto *hdrSizer = new wxBoxSizer(wxHORIZONTAL);
    auto *nameTxt = new wxStaticText(this, wxID_ANY,
                                     wxString::FromUTF8(info.id.c_str()));
    nameTxt->SetFont(nameTxt->GetFont().Bold());

    wxString verLabel;
    if (!info.latestVersion.empty()) {
      verLabel << _("Latest: ") << wxString::FromUTF8(info.latestVersion.c_str());
    }
    if (!info.installedVersion.empty()) {
      if (!verLabel.empty())
        verLabel << wxT("   ");
      verLabel << _("Installed: ") << wxString::FromUTF8(info.installedVersion.c_str());
    }

    auto *verTxt = new wxStaticText(this, wxID_ANY, verLabel);

    hdrSizer->Add(nameTxt, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 10);
    hdrSizer->Add(verTxt, 0, wxALIGN_CENTER_VERTICAL);

    topSizer->Add(hdrSizer, 0, wxALL | wxEXPAND, 8);
  }

  // Maintainer + Email + Type
  {
    wxString maint = _("Maintainer: ") + wxString::FromUTF8(info.maintainer.c_str());
    wxString email;
    if (!info.email.empty()) {
      email = _("Email: ") + wxString::FromUTF8(info.email.c_str());
    }

    topSizer->Add(new wxStaticText(this, wxID_ANY, maint),
                  0, wxLEFT | wxRIGHT | wxTOP, 8);

    if (!email.empty()) {
      topSizer->Add(new wxStaticText(this, wxID_ANY, email),
                    0, wxLEFT | wxRIGHT | wxTOP, 4);
    }

    if (!typeLabel.empty()) {
      wxString typeStr = _("Type: ") + typeLabel;
      topSizer->Add(new wxStaticText(this, wxID_ANY, typeStr),
                    0, wxLEFT | wxRIGHT | wxTOP, 4);
    }

    if (info.indexed) {
      auto *idxTxt = new wxStaticText(this, wxID_ANY, _("Indexed in package index"));
      topSizer->Add(idxTxt, 0, wxLEFT | wxRIGHT | wxTOP, 4);
    }
  }

  // Website as a hyperlink
  if (!info.website.empty()) {
    wxString ws = wxString::FromUTF8(info.website.c_str());
    auto *link = new wxHyperlinkCtrl(this, wxID_ANY, ws, ws);
    topSizer->Add(link, 0, wxALL, 8);
  }

  // list of versions and boards in the text field
  {
    wxString text;

    // Available versions
    if (!info.availableVersions.empty()) {
      text << _("Available versions:\n");
      for (const auto &v : info.availableVersions) {
        text << wxT("  - ") << wxString::FromUTF8(v);
        // mark currently installed / latest
        bool isInst = (!info.installedVersion.empty() &&
                       info.installedVersion == v);
        bool isLatest = (!info.latestVersion.empty() &&
                         info.latestVersion == v);
        if (isInst || isLatest) {
          text << wxT("  (");
          if (isInst) {
            text << _("installed");
            if (isLatest)
              text << _(", latest");
          } else if (isLatest) {
            text << _("latest");
          }
          text << wxT(")");
        }

        text << wxT("\n");
      }
      text << wxT("\n");
    }

    // Boards
    if (!info.releases.empty()) {
      text << _("Boards:\n");

      for (const auto &rel : info.releases) {
        if (!rel.version.empty()) {
          text << _("Version ") << wxString::FromUTF8(rel.version.c_str()) << wxT(":\n");
        }

        for (const auto &b : rel.boards) {
          text << wxT("  - ") << wxString::FromUTF8(b.name.c_str());
          if (!b.fqbn.empty()) {
            text << wxT("  [") << wxString::FromUTF8(b.fqbn.c_str()) << wxT("]");
          }
          text << wxT("\n");
        }

        if (!rel.boards.empty())
          text << wxT("\n");
      }
    }

    auto *desc = new wxTextCtrl(this, wxID_ANY, text,
                                wxDefaultPosition, wxDefaultSize,
                                wxTE_MULTILINE | wxTE_READONLY | wxBORDER_SIMPLE);
    desc->SetMinSize(wxSize(-1, 180));
    topSizer->Add(desc, 1, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 8);
  }

  // buttons
  auto *btnSizer = new wxBoxSizer(wxHORIZONTAL);

  if (installUninstallButton) {
    // Install...
    auto *installBtn = new wxButton(this, wxID_ANY, _("Install..."));
    installBtn->Bind(wxEVT_BUTTON,
                     [this, &info, installBtn](wxCommandEvent &) {
                       auto *manager =
                           wxDynamicCast(GetParent(), ArduinoCoreManagerFrame);
                       if (!manager)
                         return;

                       wxMenu menu;

                       const int latestId = 1;
                       menu.Append(latestId, _("Latest"));
                       menu.Bind(
                           wxEVT_MENU,
                           [manager, &info](wxCommandEvent &) {
                             manager->StartInstallCore(info, std::string{});
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
                                 manager->StartInstallCore(info, ver);
                               },
                               id);
                         }
                       }

                       wxPoint pos = installBtn->GetPosition();
                       pos.y += installBtn->GetSize().y;
                       PopupMenu(&menu, pos);
                     });

    btnSizer->Add(installBtn, 0, wxALL, 8);

    // Uninstall only if something is installed
    if (!info.installedVersion.empty()) {
      auto *uninstallBtn = new wxButton(this, wxID_ANY, _("Uninstall"));

      uninstallBtn->Bind(wxEVT_BUTTON,
                         [this, &info](wxCommandEvent &) {
                           auto *manager =
                               wxDynamicCast(GetParent(), ArduinoCoreManagerFrame);
                           if (!manager)
                             return;

                           manager->StartUninstallCore(info);
                         });

      btnSizer->Add(uninstallBtn, 0, wxTOP | wxBOTTOM | wxRIGHT, 8);
    }
  }

  btnSizer->AddStretchSpacer(1);
  btnSizer->Add(new wxButton(this, wxID_OK, _("Close")), 0, wxALL, 8);
  topSizer->Add(btnSizer, 0, wxEXPAND);

  SetSizerAndFit(topSizer);

  // position/size from config
  if (!LoadWindowSize(wxT("CoreDetail"), this, m_config)) {
    CentreOnParent();
  }
}

ArduinoCoreDetailDialog::~ArduinoCoreDetailDialog() {
  SaveWindowSize(wxT("CoreDetail"), this, m_config);
}
