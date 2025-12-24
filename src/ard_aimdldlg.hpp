#pragma once

#include <atomic>
#include <thread>
#include <wx/dialog.h>
#include <wx/string.h>
#include <wx/wx.h>

class wxConfigBase;
class wxStyledTextCtrl;
class wxTextCtrl;
class wxCheckBox;
class wxSpinCtrl;
class wxButton;

struct AiModelSettings;

// AI model definition/edit dialog.
class ArduinoAiModelDialog : public wxDialog {
public:
  ArduinoAiModelDialog(wxWindow *parent, AiModelSettings &model, wxConfigBase *config);
  ~ArduinoAiModelDialog();

  bool WasDeleted() const { return m_deleted; }

private:
  void BuildUi();
  void UpdateAuthUi();
  void LoadKeyHint();
  void OnClearStoredKey();
  void OnExport();
  void OnTest();
  void OnDelete();
  void OnOk();
  void OnClose(wxCloseEvent &e);
  void FinishTestUi(bool ok, const wxString &err);

  void SaveApiKey();

  wxString KeyService() const; // "ArduinoEditor/AI/<model.id>"
  wxString KeyUser() const;    // "api_key"

  int ModalMsgDialog(const wxString &message, const wxString &caption = _("Error"), int styles = wxOK | wxICON_ERROR);

private:
  AiModelSettings &m_model;
  wxConfigBase *m_config = nullptr;
  std::atomic_bool m_testInProgress{false};
  std::thread m_testThread;

  wxTextCtrl *m_nameCtrl = nullptr;
  wxTextCtrl *m_endpointCtrl = nullptr;
  wxTextCtrl *m_modelCtrl = nullptr;

  wxSpinCtrl *m_maxIterCtrl = nullptr;
  wxTextCtrl *m_timeoutCtrl = nullptr;

  wxCheckBox *m_forceQueryRange = nullptr;
  wxCheckBox *m_fullInfoRequest = nullptr;
  wxCheckBox *m_floatingWindow = nullptr;

  wxCheckBox *m_hasAuth = nullptr;
  wxTextCtrl *m_keyCtrl = nullptr;
  wxButton *m_clearKeyBtn = nullptr;
  bool m_hasStoredKey = false;

  wxStyledTextCtrl *m_extraJsonStc = nullptr;

  wxButton *m_btnOk = nullptr;
  wxButton *m_btnCancel = nullptr;
  wxButton *m_btnDelete = nullptr;
  wxButton *m_btnExport = nullptr;
  wxButton *m_btnTest = nullptr;

  bool m_deleted = false;
};
