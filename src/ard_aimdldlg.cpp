#include "ard_aimdldlg.hpp"

#include "ai_client.hpp" // AiClient::CheckExtraRequestJson
#include "utils.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/config.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/richmsgdlg.h>
#include <wx/secretstore.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/sstream.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/utils.h> // wxBusyCursor
#include <wx/wfstream.h>

#include <nlohmann/json.hpp>

static wxString TrimCopy(wxString s) {
  s.Trim(true).Trim(false);
  return s;
}

static bool WriteTextFile(const wxString &path, const wxString &text) {
  wxFileOutputStream os(path);
  if (!os.IsOk())
    return false;
  wxStringInputStream is(text);
  os.Write(is);
  return os.IsOk();
}

static nlohmann::json AiModelToJson(const AiModelSettings &m) {
  nlohmann::json j;
  j["id"] = std::string(m.id.utf8_str());
  j["name"] = std::string(m.name.utf8_str());
  j["endpointUrl"] = std::string(m.endpointUrl.utf8_str());
  j["model"] = std::string(m.model.utf8_str());
  j["maxIterations"] = m.maxIterations;
  j["requestTimeout"] = m.requestTimeout;
  j["extraRequestJson"] = std::string(m.extraRequestJson.utf8_str());
  j["forceModelQueryRange"] = m.forceModelQueryRange;
  j["fullInfoRequest"] = m.fullInfoRequest;
  j["floatingWindow"] = m.floatingWindow;
  j["hasAuthentization"] = m.hasAuthentization;
  return j;
}

ArduinoAiModelDialog::ArduinoAiModelDialog(wxWindow *parent, AiModelSettings &model, wxConfigBase *config)
    : wxDialog(parent, wxID_ANY, _("AI model"), wxDefaultPosition, wxSize(760, 700),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_model(model),
      m_config(config) {

  BuildUi();
  CentreOnParent();
}

ArduinoAiModelDialog::~ArduinoAiModelDialog() {
  if (m_testThread.joinable())
    m_testThread.join();
}

wxString ArduinoAiModelDialog::KeyService() const {
  // per-model service so we can delete a single entry even if wxSecretStore::Delete()
  // supports only Delete(service)
  return wxString::Format(wxT("ArduinoEditor/AI/%s"), m_model.id);
}

wxString ArduinoAiModelDialog::KeyUser() const {
  return wxT("api_key");
}

void ArduinoAiModelDialog::BuildUi() {
  auto *mainSizer = new wxBoxSizer(wxVERTICAL);

  // --- Basic fields ---
  auto *grid = new wxFlexGridSizer(2, 5, 8);
  grid->AddGrowableCol(1, 1);

  grid->Add(new wxStaticText(this, wxID_ANY, _("Name:")), 0, wxALIGN_CENTER_VERTICAL);
  m_nameCtrl = new wxTextCtrl(this, wxID_ANY, m_model.name);
  grid->Add(m_nameCtrl, 1, wxEXPAND);

  grid->Add(new wxStaticText(this, wxID_ANY, _("Endpoint URL:")), 0, wxALIGN_CENTER_VERTICAL);
  m_endpointCtrl = new wxTextCtrl(this, wxID_ANY, m_model.endpointUrl);
  grid->Add(m_endpointCtrl, 1, wxEXPAND);

  grid->Add(new wxStaticText(this, wxID_ANY, _("Model:")), 0, wxALIGN_CENTER_VERTICAL);
  m_modelCtrl = new wxTextCtrl(this, wxID_ANY, m_model.model);
  grid->Add(m_modelCtrl, 1, wxEXPAND);

  grid->Add(new wxStaticText(this, wxID_ANY, _("Maximum AI iterations:")), 0, wxALIGN_CENTER_VERTICAL);
  m_maxIterCtrl = new wxSpinCtrl(this, wxID_ANY);
  m_maxIterCtrl->SetRange(1, 50);
  m_maxIterCtrl->SetValue(m_model.maxIterations);
  grid->Add(m_maxIterCtrl, 1, wxEXPAND);

  grid->Add(new wxStaticText(this, wxID_ANY, _("Request timeout (seconds, 0 = no limit):")), 0, wxALIGN_CENTER_VERTICAL);
  m_timeoutCtrl = new wxTextCtrl(this, wxID_ANY, wxString::Format(wxT("%d"), m_model.requestTimeout));
  grid->Add(m_timeoutCtrl, 1, wxEXPAND);

  mainSizer->Add(grid, 0, wxALL | wxEXPAND, 10);

  // --- Flags ---
  m_forceQueryRange = new wxCheckBox(this, wxID_ANY, _("Force file_range policy (AI must inspect files before PATCH)"));
  m_forceQueryRange->SetValue(m_model.forceModelQueryRange);
  mainSizer->Add(m_forceQueryRange, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  m_fullInfoRequest = new wxCheckBox(this, wxID_ANY, _("Keep full AI context (include all requested code and info)"));
  m_fullInfoRequest->SetValue(m_model.fullInfoRequest);
  mainSizer->Add(m_fullInfoRequest, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  m_floatingWindow = new wxCheckBox(this, wxID_ANY, _("Limit AI context to last applied change (floating window)"));
  m_floatingWindow->SetValue(m_model.floatingWindow);
  mainSizer->Add(m_floatingWindow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  // --- Auth + key ---
  auto *authBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Authentication"));
  m_hasAuth = new wxCheckBox(this, wxID_ANY, _("This endpoint requires an API key"));
  m_hasAuth->SetValue(m_model.hasAuthentization);
  authBox->Add(m_hasAuth, 0, wxALL | wxEXPAND, 5);

  auto *keyRow = new wxBoxSizer(wxHORIZONTAL);
  keyRow->Add(new wxStaticText(this, wxID_ANY, _("API key:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

  m_keyCtrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
  keyRow->Add(m_keyCtrl, 1, wxRIGHT | wxEXPAND, 5);

  m_clearKeyBtn = new wxButton(this, wxID_ANY, _("Clear stored key"));
  keyRow->Add(m_clearKeyBtn, 0);

  authBox->Add(keyRow, 0, wxALL | wxEXPAND, 5);

  authBox->Add(new wxStaticText(this, wxID_ANY,
                                _("The API key is stored using the system's secure keychain.")),
               0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

  mainSizer->Add(authBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  // --- Extra JSON ---
  auto *jsonBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Extra request JSON"));

  m_extraJsonStc = new wxStyledTextCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
  m_extraJsonStc->SetWrapMode(wxSTC_WRAP_NONE);
  m_extraJsonStc->SetTabWidth(2);
  m_extraJsonStc->SetUseTabs(false);
  m_extraJsonStc->SetIndent(2);
  m_extraJsonStc->SetEOLMode(wxSTC_EOL_LF);

  m_extraJsonStc->SetLexer(wxSTC_LEX_CPP);

  EditorSettings settings;
  settings.Load(m_config);

  ::ApplyStyledTextCtrlSettings(m_extraJsonStc, settings);

  m_extraJsonStc->SetText(m_model.extraRequestJson);

  jsonBox->Add(m_extraJsonStc, 1, wxALL | wxEXPAND, 5);
  mainSizer->Add(jsonBox, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  // --- Buttons ---
  auto *btnRow = new wxBoxSizer(wxHORIZONTAL);

  m_btnDelete = new wxButton(this, wxID_ANY, _("Delete"));
  m_btnExport = new wxButton(this, wxID_ANY, _("Export..."));
  m_btnTest = new wxButton(this, wxID_ANY, _("Test"));

  btnRow->Add(m_btnDelete, 0, wxRIGHT, 8);
  btnRow->Add(m_btnExport, 0);
  btnRow->Add(m_btnTest, 0, wxLEFT, 8);

  btnRow->AddStretchSpacer(1);

  m_btnOk = new wxButton(this, wxID_OK, _("OK"));
  m_btnCancel = new wxButton(this, wxID_CANCEL, _("Cancel"));
  btnRow->Add(m_btnCancel, 0, wxRIGHT, 8);
  btnRow->Add(m_btnOk, 0);

  mainSizer->Add(btnRow, 0, wxALL | wxEXPAND, 10);

  SetSizer(mainSizer);
  Layout();

  // events
  m_hasAuth->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &e) { UpdateAuthUi(); e.Skip(); });
  m_clearKeyBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnClearStoredKey(); });
  m_btnExport->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnExport(); });
  m_btnDelete->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnDelete(); });
  m_btnTest->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnTest(); });
  m_btnOk->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnOk(); });
  Bind(wxEVT_CLOSE_WINDOW, &ArduinoAiModelDialog::OnClose, this);

  LoadKeyHint();
  UpdateAuthUi();
}

void ArduinoAiModelDialog::OnClose(wxCloseEvent &e) {
  if (m_testInProgress.load()) {
    wxBell();
    e.Veto();
    return;
  }
  e.Skip();
}

int ArduinoAiModelDialog::ModalMsgDialog(const wxString &message, const wxString &caption, int styles) {
  wxRichMessageDialog dlg(this, message, caption, styles);
  return dlg.ShowModal();
}

void ArduinoAiModelDialog::UpdateAuthUi() {
  bool on = m_hasAuth && m_hasAuth->GetValue();
  if (m_keyCtrl)
    m_keyCtrl->Enable(on);
  if (m_clearKeyBtn)
    m_clearKeyBtn->Enable(on && m_hasStoredKey);
}

void ArduinoAiModelDialog::LoadKeyHint() {
  m_hasStoredKey = false;

  wxSecretStore store = wxSecretStore::GetDefault();
  wxString err;
  if (store.IsOk(&err)) {
    wxString username = KeyUser();
    wxSecretValue password;
    if (store.Load(KeyService(), username, password)) {
      m_hasStoredKey = true;
    }
  }

  if (m_keyCtrl) {
    if (m_hasStoredKey)
      m_keyCtrl->SetHint(_("A key is already stored. Leave empty to keep it."));
    else
      m_keyCtrl->SetHint(_("Enter your API key (will be stored securely)."));
  }

  if (m_clearKeyBtn)
    m_clearKeyBtn->Enable(m_hasStoredKey);
}

void ArduinoAiModelDialog::OnClearStoredKey() {
  wxSecretStore store = wxSecretStore::GetDefault();
  wxString err;
  if (store.IsOk(&err)) {
    store.Delete(KeyService());
  }
  LoadKeyHint();
  UpdateAuthUi();
}

void ArduinoAiModelDialog::OnExport() {
  // build JSON from current UI state (nejen z m_model)
  AiModelSettings tmp = m_model;

  tmp.name = TrimCopy(m_nameCtrl->GetValue());
  tmp.endpointUrl = TrimCopy(m_endpointCtrl->GetValue());
  tmp.model = TrimCopy(m_modelCtrl->GetValue());
  tmp.maxIterations = m_maxIterCtrl->GetValue();
  tmp.requestTimeout = wxAtoi(TrimCopy(m_timeoutCtrl->GetValue()));
  tmp.extraRequestJson = TrimCopy(m_extraJsonStc->GetText());

  tmp.forceModelQueryRange = m_forceQueryRange->GetValue();
  tmp.fullInfoRequest = m_fullInfoRequest->GetValue();
  tmp.floatingWindow = m_floatingWindow->GetValue();
  tmp.hasAuthentization = m_hasAuth->GetValue();

  nlohmann::json j = AiModelToJson(tmp);
  wxString pretty = wxString::FromUTF8(j.dump(2).c_str());

  wxFileDialog fd(this, _("Export model to JSON"), wxEmptyString, wxEmptyString,
                  _("JSON files (*.json)|*.json|All files|*.*"),
                  wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
  if (fd.ShowModal() != wxID_OK)
    return;

  if (!WriteTextFile(fd.GetPath(), pretty)) {
    ModalMsgDialog(_("Failed to write file."), _("Export"));
  }
}

void ArduinoAiModelDialog::OnTest() {
  if (m_testInProgress.exchange(true)) {
    return;
  }

  // Validate extra JSON first (it is sent as-is to the endpoint).
  wxString json = m_extraJsonStc ? TrimCopy(m_extraJsonStc->GetText()) : wxString();
  wxString jsonErr;
  if (!AiClient::CheckExtraRequestJson(json, &jsonErr)) {
    ModalMsgDialog(jsonErr, _("Invalid JSON"));
    return;
  }

  // Build temporary settings from current UI state (not from m_model).
  AiSettings settings;

  settings.enabled = true;
  settings.endpointUrl = TrimCopy(m_endpointCtrl->GetValue());
  settings.model = TrimCopy(m_modelCtrl->GetValue());
  settings.maxIterations = m_maxIterCtrl->GetValue();
  settings.requestTimeout = wxAtoi(TrimCopy(m_timeoutCtrl->GetValue()));
  settings.extraRequestJson = json;
  settings.hasAuthentization = m_hasAuth->GetValue();

  if (settings.hasAuthentization) {
    SaveApiKey();
  }

  settings.forceModelQueryRange = m_forceQueryRange->GetValue();
  settings.fullInfoRequest = m_fullInfoRequest->GetValue();
  settings.floatingWindow = m_floatingWindow->GetValue();

  if (settings.endpointUrl.empty()) {
    ModalMsgDialog(_("Endpoint URL is empty."), _("Validation"));
    return;
  }

  // API key: prefer the typed value, otherwise fall back to the stored key.
  if (settings.hasAuthentization) {
    wxString key = TrimCopy(m_keyCtrl->GetValue());

    if (key.empty()) {
      wxSecretStore store = wxSecretStore::GetDefault();
      wxString err;
      if (store.IsOk(&err)) {
        wxSecretValue password;
        wxString keyUser = KeyUser();
        if (store.Load(KeyService(), keyUser, password)) {
          key = password.GetAsString();
        }
      }
    }

    if (key.empty()) {
      ModalMsgDialog(_("API key is required for this endpoint (no key provided and none stored)."), _("Validation"));
      return;
    }
  }

  // Run test
  if (m_btnTest)
    m_btnTest->Disable();
  if (m_btnOk)
    m_btnOk->Disable();
  if (m_btnCancel)
    m_btnCancel->Disable();
  if (m_btnDelete)
    m_btnDelete->Disable();
  if (m_btnExport)
    m_btnExport->Disable();

  if (m_testThread.joinable()) {
    m_testThread.join();
  }

  m_testThread = std::thread([this, settings]() mutable {
    wxString err;
    bool ok = false;
    {
      AiClient client(settings);
      ok = client.TestConnection(&err);
    }

    // návrat na UI thread
    CallAfter([this, ok, err]() {
      m_testInProgress.store(false);

      if (m_btnTest)
        m_btnTest->Enable();
      if (m_btnOk)
        m_btnOk->Enable();
      if (m_btnCancel)
        m_btnCancel->Enable();
      if (m_btnDelete)
        m_btnDelete->Enable();
      if (m_btnExport)
        m_btnExport->Enable();

      if (ok) {
        ModalMsgDialog(_("Connection test succeeded."), _("AI Test"), wxOK | wxICON_INFORMATION);
      } else {
        wxRichMessageDialog dlg(this, _("Connection test failed."), _("AI Test"),
                                wxOK | wxICON_ERROR);
        if (!err.empty())
          dlg.SetExtendedMessage(err);
        dlg.ShowModal();
      }
    });
  });
  // m_testThread.detach(); - join in destructor
}

void ArduinoAiModelDialog::OnDelete() {
  m_deleted = true;
  EndModal(wxID_DELETE);
}

// nahoře v cpp budeš potřebovat:
#include <thread>

void ArduinoAiModelDialog::OnOk() {
  // validate JSON
  wxString json = m_extraJsonStc ? TrimCopy(m_extraJsonStc->GetText()) : wxString();
  wxString err;
  if (!AiClient::CheckExtraRequestJson(json, &err)) {
    ModalMsgDialog(err, _("Invalid JSON"));
    return;
  }

  wxString name = TrimCopy(m_nameCtrl->GetValue());
  wxString endpoint = TrimCopy(m_endpointCtrl->GetValue());
  wxString model = TrimCopy(m_modelCtrl->GetValue());
  if (endpoint.empty()) {
    ModalMsgDialog(_("Endpoint URL is empty."), _("Validation"));
    return;
  }

  // ---- capture auth data BEFORE EndModal (dialog may be destroyed afterwards) ----
  const bool doSaveKey = m_hasAuth->GetValue();

  // write back
  m_model.name = name;
  m_model.endpointUrl = endpoint;
  m_model.model = model;
  m_model.maxIterations = m_maxIterCtrl->GetValue();
  m_model.requestTimeout = wxAtoi(TrimCopy(m_timeoutCtrl->GetValue()));
  m_model.extraRequestJson = json;

  m_model.forceModelQueryRange = m_forceQueryRange->GetValue();
  m_model.fullInfoRequest = m_fullInfoRequest->GetValue();
  m_model.floatingWindow = m_floatingWindow->GetValue();
  m_model.hasAuthentization = doSaveKey;

#ifdef __WXGTK__
  // Fire-and-forget: avoid freezing UI on RPi/GTK keyring/DBus weirdness.
  if (doSaveKey) {
    // Cheap preflight: if there's no DBus session, secret store is likely to hang.
    wxString dbusAddr;
    const bool hasDbus = wxGetEnv(wxT("DBUS_SESSION_BUS_ADDRESS"), &dbusAddr) && !dbusAddr.empty();
    if (!hasDbus) {
      ModalMsgDialog(_("System keyring (Secret Service) is not available in this session.\n\n"
                       "The API key will NOT be saved and you will have to enter it again next time.\n\n"
                       "Tip: On Linux/Raspberry Pi this usually means there is no DBus session "
                       "(DBUS_SESSION_BUS_ADDRESS is missing) or no secret-service provider is running."),
                     _("API key not saved"),
                     wxOK | wxICON_WARNING);
      return;
    }

    const wxSecretValue apiKey(m_keyCtrl->GetValue());
    const wxString service = KeyService();
    const wxString username = KeyUser();

    std::thread([service, username, apiKey]() {
      wxSecretStore store = wxSecretStore::GetDefault();
      wxString e;
      if (!store.IsOk(&e))
        return;

      // This may still block, but only this background thread, not the UI.
      store.Save(service, username, apiKey);
    }).detach();
  }
#else
  if (doSaveKey) {
    SaveApiKey();
  }
#endif

  // close immediately (do NOT block UI on keyring)
  EndModal(wxID_OK);
}

void ArduinoAiModelDialog::SaveApiKey() {
  wxString key = TrimCopy(m_keyCtrl->GetValue());
  if (!key.empty()) {
    wxSecretStore store = wxSecretStore::GetDefault();
    wxString e;
    if (store.IsOk(&e)) {
      wxSecretValue secret(key);
      wxString username = KeyUser();
      if (!store.Save(KeyService(), username, secret)) {
        wxLogWarning(_("Failed to save AI API key to the system secret store."));
      }
    }
  }
}
