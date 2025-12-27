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

#include "ard_mdwidget.hpp"
#include <wx/dialog.h>
#include <wx/string.h>

class wxWindow;
class wxConfigBase;

struct AeReleaseInfo {
  wxString tag;         // e.g. "v1.0.1"
  wxString version;     // normalized e.g. "1.0.1"
  wxString title;       // release name
  wxString htmlUrl;     // GitHub release page
  wxString bodyMd;      // release notes markdown
  wxString publishedAt; // ISO string
};

class ArduinoEditorUpdateDialog : public wxDialog {
public:
  // Checks throttling + GitHub latest release and shows modal dialog if needed.
  // Non-blocking: network runs async and dialog is shown only when response arrives.
  static void CheckAndShowIfNeeded(wxWindow *parent, wxConfigBase &cfg, bool force = false);

private:
  ArduinoEditorUpdateDialog(wxWindow *parent, wxConfigBase &cfg, const AeReleaseInfo &rel);

  void OnOpenGitHub();
  void OnIgnore();
  void OnRemindLater();

  static bool ParseLatestReleaseJson(const wxString &jsonUtf8, AeReleaseInfo &out);
  static wxString NormalizeTagToVersion(const wxString &tagOrName);
  static int CompareSemver(const wxString &a, const wxString &b); // -1/0/1
  static bool IsTimeToCheck(wxConfigBase &cfg, long long nowUtc);
  static void MarkCheckedNow(wxConfigBase &cfg, long long nowUtc);

  static wxArrayString LoadDismissedTags(wxConfigBase &cfg);
  static void SaveDismissedTags(wxConfigBase &cfg, const wxArrayString &tags);
  static bool ContainsTag(const wxArrayString &tags, const wxString &tag);

  static wxString BuildApiUrl();
  static wxString BuildReleaseNotesMarkdown(const AeReleaseInfo &rel);

private:
  wxConfigBase *m_cfg{nullptr};
  AeReleaseInfo m_rel;

  // Forward declare to avoid include cycle
  class ArduinoMarkdownPanel *m_md{nullptr};
};
