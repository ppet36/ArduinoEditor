#pragma once

#include <wx/config.h>
#include <wx/dialog.h>
#include <wx/string.h>

#include "ard_cli.hpp" // ArduinoCoreInfo

class ArduinoCoreManagerFrame;

class ArduinoCoreDetailDialog : public wxDialog {
public:
  ArduinoCoreDetailDialog(wxWindow *parent,
                          const ArduinoCoreInfo &info,
                          wxConfigBase *config,
                          const wxString &typeLabel = wxEmptyString,
                          bool installUninstallButton = true);
  ~ArduinoCoreDetailDialog();

  // helper, in case you ever want to "ShowInstalledCoreInfo"
  static void ShowCoreInfo(wxWindow *parent,
                           const ArduinoCoreInfo &info,
                           wxConfigBase *config);

private:
  wxConfigBase *m_config = nullptr;
};
