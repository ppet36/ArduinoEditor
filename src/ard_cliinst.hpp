
#include "ard_cli.hpp"
#include <string>
#include <wx/config.h>
#include <wx/progdlg.h>
#include <wx/wx.h>

class ArduinoCliInstaller {
private:
  wxConfigBase *m_config;
  wxWindow *m_owner;

  int ModalMsgDialog(const wxString &message, const wxString &caption = _("Error"), int styles = wxOK | wxICON_ERROR);

  bool DownloadArduinoCli(wxString &outPath);
#if defined(__WXGTK__)
  bool DownloadArduinoCliLinux(wxString &outPath);
#elif defined(__WXMSW__)
  bool DownloadArduinoCliWindows(wxString &outPath);
#elif defined(__WXMAC__)
  bool DownloadArduinoCliMac(wxString &outPath);
#endif

  bool EnsureBaseToolchainInstalled(const wxString &cliPath);
  bool CheckBaseToolchainInstalled(const wxString &cliPath, bool *outNeedsInstall, bool *outHasAvr);
  bool InstallBaseToolchain(const wxString &cliPath, wxProgressDialog *existingProg);

public:
  ArduinoCliInstaller(wxWindow *owner, wxConfigBase *config);

  ArduinoCli *GetCli(const std::string &sketchPath);

  static bool CheckArduinoCli(const std::string &cliPath);
};
