
#include "ard_cli.hpp"
#include <wx/config.h>
#include <wx/wx.h>

class ArduinoLibraryDetailDialog : public wxDialog {
public:
  ArduinoLibraryDetailDialog(wxWindow *parent, const ArduinoLibraryInfo &info, const ArduinoLibraryInfo *installedInfo, wxConfigBase *config, const wxString &typeLabel = wxEmptyString, bool installUninstallButton = true);
  ~ArduinoLibraryDetailDialog();

  static void ShowInstalledLibraryInfo(wxWindow *parent, const ArduinoLibraryInfo *installedInfo, wxConfigBase *config, ArduinoCli *cli);

private:
  const ArduinoLibraryInfo *m_installedInfo = nullptr;
  wxConfigBase *m_config = nullptr;
};
