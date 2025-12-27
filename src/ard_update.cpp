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

#include "ard_update.hpp"

#include "utils.hpp"

#include <wx/app.h> // wxTheApp
#include <wx/button.h>
#include <wx/config.h>
#include <wx/datetime.h>
#include <wx/msgdlg.h>
#include <wx/richmsgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h> // wxLaunchDefaultBrowser

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <nlohmann/json.hpp>
#include <thread>

#include <curl/curl.h>

#ifndef AE_VERSION
#error "AE_VERSION must be defined (e.g. -DAE_VERSION=\"1.0.0\")"
#endif

// Configure repo at build time (recommended)
#ifndef AE_GITHUB_OWNER
#define AE_GITHUB_OWNER "ppet36"
#endif

#ifndef AE_GITHUB_REPO
#define AE_GITHUB_REPO "ArduinoEditor"
#endif

namespace {

// settings keys
static const wxChar *K_LAST_CHECK = wxT("ArduinoEditor/Updates/last_check_unix");
static const wxChar *K_SNOOZE_UNTIL = wxT("ArduinoEditor/Updates/snooze_until_unix");
static const wxChar *K_DISMISSED = wxT("ArduinoEditor/Updates/dismissed_tags_json");
static const wxChar *K_INTERVAL_HOURS = wxT("ArduinoEditor/Updates/check_interval_hours");

static long long NowUtcUnix() {
  return static_cast<long long>(std::time(nullptr));
}

static wxString ToWx(const std::string &s) {
  return wxString::FromUTF8(s.c_str());
}

static std::string ToStdUtf8(const wxString &s) {
  return std::string(s.utf8_str());
}

static wxString JoinLines(std::initializer_list<wxString> lines) {
  wxString out;
  for (auto &l : lines) {
    out += l;
    if (!out.EndsWith(wxT("\n")))
      out += wxT("\n");
  }
  return out;
}

static std::atomic_bool g_updateCheckInProgress{false};

static size_t CurlWriteCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *out = static_cast<std::string *>(userdata);
  const size_t n = size * nmemb;
  out->append(ptr, n);
  return n;
}

static bool CurlHttpGet(const wxString &url,
                        const wxString &userAgent,
                        const wxArrayString &headers,
                        long &outHttpStatus,
                        wxString &outBodyUtf8,
                        wxString &outError) {
  static std::once_flag once;
  std::call_once(once, []() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
  });

  CURL *curl = curl_easy_init();
  if (!curl) {
    outError = wxT("curl_easy_init failed");
    return false;
  }

  std::string body;
  std::string url8 = ToStdUtf8(url);
  std::string ua8 = ToStdUtf8(userAgent);

  curl_easy_setopt(curl, CURLOPT_URL, url8.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, ua8.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

  // timeouts: keep it snappy
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);

  // write body
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlWriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

  struct curl_slist *hdrs = nullptr;
  for (auto &h : headers) {
    hdrs = curl_slist_append(hdrs, ToStdUtf8(h).c_str());
  }
  if (hdrs) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  }

  CURLcode res = curl_easy_perform(curl);

  long http = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  outHttpStatus = http;

  if (hdrs)
    curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    outError = wxString::FromUTF8(curl_easy_strerror(res));
    return false;
  }

  outBodyUtf8 = wxString::FromUTF8(body.c_str());
  return true;
}

} // namespace

wxString ArduinoEditorUpdateDialog::BuildApiUrl() {
  // https://api.github.com/repos/{owner}/{repo}/releases/latest
  return wxString::Format(wxT("https://api.github.com/repos/%s/%s/releases/latest"),
                          wxString::FromUTF8(AE_GITHUB_OWNER),
                          wxString::FromUTF8(AE_GITHUB_REPO));
}

bool ArduinoEditorUpdateDialog::IsTimeToCheck(wxConfigBase &cfg, long long nowUtc) {
  // Snooze
  long long snoozeUntil = 0;
  cfg.Read(K_SNOOZE_UNTIL, &snoozeUntil, 0LL);
  if (snoozeUntil > 0 && nowUtc < snoozeUntil) {
    return false;
  }

  long long lastCheck = 0;
  cfg.Read(K_LAST_CHECK, &lastCheck, 0LL);

  long intervalHours = 24;
  cfg.Read(K_INTERVAL_HOURS, &intervalHours, 24L);
  if (intervalHours < 1) {
    return false;
  }

  const long long minDelta = static_cast<long long>(intervalHours) * 3600LL;
  if (lastCheck > 0 && (nowUtc - lastCheck) < minDelta) {
    return false;
  }
  return true;
}

void ArduinoEditorUpdateDialog::MarkCheckedNow(wxConfigBase &cfg, long long nowUtc) {
  cfg.Write(K_LAST_CHECK, nowUtc);
  cfg.Flush();
}

wxArrayString ArduinoEditorUpdateDialog::LoadDismissedTags(wxConfigBase &cfg) {
  wxString raw;
  cfg.Read(K_DISMISSED, &raw, wxEmptyString);
  wxArrayString out;

  if (raw.IsEmpty())
    return out;

  try {
    auto j = nlohmann::json::parse(ToStdUtf8(raw));
    if (!j.is_array())
      return out;
    for (auto &it : j) {
      if (it.is_string()) {
        out.Add(ToWx(it.get<std::string>()));
      }
    }
  } catch (...) {
    // ignore corrupted value
  }
  return out;
}

void ArduinoEditorUpdateDialog::SaveDismissedTags(wxConfigBase &cfg, const wxArrayString &tags) {
  nlohmann::json j = nlohmann::json::array();
  for (auto &t : tags) {
    j.push_back(ToStdUtf8(t));
  }
  cfg.Write(K_DISMISSED, ToWx(j.dump()));
  cfg.Flush();
}

bool ArduinoEditorUpdateDialog::ContainsTag(const wxArrayString &tags, const wxString &tag) {
  for (auto &t : tags) {
    if (t.CmpNoCase(tag) == 0)
      return true;
  }
  return false;
}

wxString ArduinoEditorUpdateDialog::NormalizeTagToVersion(const wxString &tagOrName) {
  wxString s = tagOrName;
  s.Trim(true).Trim(false);

  // strip leading 'v' or 'V'
  if (!s.IsEmpty() && (s[0] == 'v' || s[0] == 'V')) {
    s = s.Mid(1);
  }

  // cut off suffix like "-beta" or "+meta"
  int dash = s.Find(wxT('-'));
  if (dash != wxNOT_FOUND)
    s = s.Left(dash);
  int plus = s.Find(wxT('+'));
  if (plus != wxNOT_FOUND)
    s = s.Left(plus);

  s.Trim(true).Trim(false);
  return s;
}

static bool ParseIntSafe(const wxString &s, long &out) {
  wxString t = s;
  t.Trim(true).Trim(false);
  if (t.IsEmpty())
    return false;
  // ensure digits only
  for (wxUniChar c : t) {
    if (!wxIsdigit(c))
      return false;
  }
  return t.ToLong(&out);
}

int ArduinoEditorUpdateDialog::CompareSemver(const wxString &a, const wxString &b) {
  // returns -1 if a<b, 0 if equal, 1 if a>b
  auto split = [](const wxString &v) {
    wxArrayString parts = wxSplit(v, '.');
    long nums[3] = {0, 0, 0};
    for (size_t i = 0; i < 3 && i < parts.size(); ++i) {
      long x = 0;
      if (ParseIntSafe(parts[i], x))
        nums[i] = x;
      else
        nums[i] = 0;
    }
    return std::array<long, 3>{nums[0], nums[1], nums[2]};
  };

  auto aa = split(a);
  auto bb = split(b);

  if (aa[0] != bb[0])
    return aa[0] < bb[0] ? -1 : 1;
  if (aa[1] != bb[1])
    return aa[1] < bb[1] ? -1 : 1;
  if (aa[2] != bb[2])
    return aa[2] < bb[2] ? -1 : 1;
  return 0;
}

bool ArduinoEditorUpdateDialog::ParseLatestReleaseJson(const wxString &jsonUtf8, AeReleaseInfo &out) {
  try {
    auto j = nlohmann::json::parse(ToStdUtf8(jsonUtf8));

    // For safety: if API returns an error object, it may have "message"
    if (j.is_object() && j.contains("message") && j["message"].is_string()) {
      return false;
    }

    if (!j.is_object())
      return false;

    // Latest endpoint should already exclude drafts/prereleases, but keep a guard:
    if (j.value("draft", false))
      return false;
    if (j.value("prerelease", false))
      return false;

    out.tag = ToWx(j.value("tag_name", std::string{}));
    out.title = ToWx(j.value("name", std::string{}));
    out.htmlUrl = ToWx(j.value("html_url", std::string{}));
    out.bodyMd = ToWx(j.value("body", std::string{}));
    out.publishedAt = ToWx(j.value("published_at", std::string{}));

    if (out.tag.IsEmpty())
      return false;

    out.version = NormalizeTagToVersion(out.tag);
    if (out.version.IsEmpty())
      return false;

    return true;
  } catch (...) {
    return false;
  }
}

wxString ArduinoEditorUpdateDialog::BuildReleaseNotesMarkdown(const AeReleaseInfo &rel) {
  wxString title = rel.title;
  if (title.IsEmpty())
    title = rel.tag;

  wxString header = JoinLines({wxString::Format(wxT("## %s"), title),
                               wxString::Format(wxT("- **Version:** %s"), rel.version),
                               rel.publishedAt.IsEmpty() ? wxString() : wxString::Format(wxT("- **Published:** %s"), rel.publishedAt),
                               rel.htmlUrl.IsEmpty() ? wxString() : wxString::Format(wxT("- **GitHub:** [%s](%s)"), rel.htmlUrl, rel.htmlUrl),
                               wxString()});

  wxString body = rel.bodyMd;
  body.Trim(true).Trim(false);

  if (body.IsEmpty()) {
    body = wxT("_No release notes provided._");
  }

  return header + body + wxT("\n");
}

ArduinoEditorUpdateDialog::ArduinoEditorUpdateDialog(wxWindow *parent, wxConfigBase &cfg, const AeReleaseInfo &rel)
    : wxDialog(parent, wxID_ANY, _("Update available"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_cfg(&cfg), m_rel(rel) {

  auto *root = new wxBoxSizer(wxVERTICAL);

  wxString curVer = wxString::FromUTF8(AE_VERSION);
  auto *title = new wxStaticText(this, wxID_ANY,
                                 wxString::Format(_("A new version is available: %s (you have %s)"),
                                                  rel.version, curVer));
  title->Wrap(700);
  root->Add(title, 0, wxALL | wxEXPAND, 10);

  m_md = new ArduinoMarkdownPanel(this, wxID_ANY);
  m_md->SetMinSize(wxSize(700, 350));
  root->Add(m_md, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  // Fill markdown as "System"
  m_md->AppendMarkdown(BuildReleaseNotesMarkdown(rel), AiMarkdownRole::System);
  m_md->Render(false);

  auto *btns = new wxBoxSizer(wxHORIZONTAL);

  auto *btnOpen = new wxButton(this, wxID_ANY, _("Open on GitHub"));
  auto *btnIgnore = new wxButton(this, wxID_ANY, _("Ignore this version"));
  auto *btnLater = new wxButton(this, wxID_ANY, _("Remind me later"));

  btns->Add(btnOpen, 0, wxRIGHT, 8);
  btns->AddStretchSpacer(1);
  btns->Add(btnLater, 0, wxRIGHT, 8);
  btns->Add(btnIgnore, 0);

  root->Add(btns, 0, wxALL | wxEXPAND, 10);

  btnOpen->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnOpenGitHub(); });
  btnIgnore->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnIgnore(); });
  btnLater->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnRemindLater(); });

  SetSizer(root);

  SetMinSize(wxSize(760, 520));
  SetSize(wxSize(900, 650));

  Layout();
  CentreOnParent();
}

void ArduinoEditorUpdateDialog::OnOpenGitHub() {
  if (!m_rel.htmlUrl.IsEmpty()) {
    wxLaunchDefaultBrowser(m_rel.htmlUrl);
  }
}

void ArduinoEditorUpdateDialog::OnIgnore() {
  if (!m_cfg)
    return;

  auto tags = LoadDismissedTags(*m_cfg);
  if (!ContainsTag(tags, m_rel.tag)) {
    tags.Add(m_rel.tag);
    SaveDismissedTags(*m_cfg, tags);
  }

  // Clear snooze so ignore is immediate & persistent
  m_cfg->Write(K_SNOOZE_UNTIL, 0LL);
  m_cfg->Flush();

  EndModal(wxID_OK);
}

void ArduinoEditorUpdateDialog::OnRemindLater() {
  if (!m_cfg)
    return;

  // Simple snooze: 24 hours
  const long long nowUtc = NowUtcUnix();
  const long long snooze = nowUtc + 24LL * 3600LL;

  m_cfg->Write(K_SNOOZE_UNTIL, snooze);
  m_cfg->Flush();

  EndModal(wxID_CANCEL);
}

static void ModalMsgDialog(wxWindow *parent, const wxString &msg, const wxString &caption = _("Check for updates"), int styles = wxICON_ERROR | wxOK) {
  if (!parent || parent->IsBeingDeleted())
    return;
  wxRichMessageDialog dlg(parent, msg, caption, styles);
  dlg.ShowModal();
}

void ArduinoEditorUpdateDialog::CheckAndShowIfNeeded(wxWindow *parent, wxConfigBase &cfg, bool force) {
  const long long nowUtc = NowUtcUnix();
  if (!force && !IsTimeToCheck(cfg, nowUtc)) {
    return;
  }

  // prevent parallel checks (e.g. multiple triggers during startup)
  bool expected = false;
  if (!g_updateCheckInProgress.compare_exchange_strong(expected, true)) {
    ModalMsgDialog(parent, _("An update check is already in progress."), _("Check for updates"), wxICON_INFORMATION | wxOK);
    return;
  }

  // Mark checked now to avoid repeated checks if app triggers this again soon.
  // For force checks, you may want NOT to touch last_check_unix here.
  if (!force) {
    MarkCheckedNow(cfg, nowUtc);
  }

  const wxString apiUrl = BuildApiUrl();
  wxWindow *parentPtr = parent;
  wxConfigBase *cfgPtr = &cfg;

  // Do network + parsing off the UI thread
  std::thread([apiUrl, parentPtr, cfgPtr, nowUtc, force]() {
    AeReleaseInfo rel;
    wxString err;
    wxString body;
    long http = 0;

    wxArrayString headers;
    headers.Add(wxT("Accept: application/vnd.github+json"));
    // GitHub likes explicit UA
    wxString ua = wxString::Format(wxT("ArduinoEditor/%s"), wxString::FromUTF8(AE_VERSION));

    bool ok = CurlHttpGet(apiUrl, ua, headers, http, body, err);

    // Back to UI thread
    wxTheApp->CallAfter([ok, http, body, parentPtr, cfgPtr, nowUtc, err, force]() {
      g_updateCheckInProgress.store(false);

      if (!parentPtr || parentPtr->IsBeingDeleted() || !cfgPtr) {
        return;
      }

      // If force, record last_check now (so UI says “checked”), otherwise you already did.
      if (force) {
        MarkCheckedNow(*cfgPtr, nowUtc);
      }

      if (!ok) {
        if (force) {
          ModalMsgDialog(parentPtr, wxString::Format(_("Update check failed:\n\n%s"), err));
        }
        return;
      }

      APP_DEBUG_LOG("UPD: response body=\n%s", wxToStd(body).c_str());

      if (http != 200) {
        if (force) {
          // Try to extract GitHub "message" for nicer error
          wxString ghMsg;
          try {
            auto j = nlohmann::json::parse(std::string(body.utf8_str()));
            if (j.is_object() && j.contains("message") && j["message"].is_string()) {
              ghMsg = wxString::FromUTF8(j["message"].get<std::string>().c_str());
            }
          } catch (...) {
          }

          wxString msg = wxString::Format(_("Update check failed (HTTP %ld)."), http);
          if (!ghMsg.IsEmpty()) {
            msg += wxT("\n\n") + ghMsg;
          }
          ModalMsgDialog(parentPtr, msg);
        }
        return;
      }

      AeReleaseInfo relLocal;
      if (!ParseLatestReleaseJson(body, relLocal)) {
        if (force) {
          ModalMsgDialog(parentPtr, _("Update check failed: unexpected response format."));
        }
        return;
      }

      const wxString curVer = NormalizeTagToVersion(wxString::FromUTF8(AE_VERSION));
      if (CompareSemver(curVer, relLocal.version) >= 0) {
        if (force) {
          ModalMsgDialog(parentPtr, wxString::Format(_("You are running the latest version (%s)."), wxString::FromUTF8(AE_VERSION)), _("Check for updates"), wxICON_INFORMATION | wxOK);
        }
        return;
      }

      if (!force) {
        auto dismissed = LoadDismissedTags(*cfgPtr);
        if (ContainsTag(dismissed, relLocal.tag)) {
          return;
        }
      }

      ArduinoEditorUpdateDialog dlg(parentPtr, *cfgPtr, relLocal);
      dlg.ShowModal();
    });
  }).detach();
}
