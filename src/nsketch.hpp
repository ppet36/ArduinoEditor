
#include <wx/config.h>
#include <wx/wx.h>

class NewSketchDialog : public wxDialog {
public:
  NewSketchDialog(wxWindow *parent, wxConfigBase *config, const wxString &initialDir);

  wxString GetSketchName() const { return m_nameCtrl->GetValue(); }
  wxString GetDirectory() const { return m_dir; }

private:
  void OnBrowse(wxCommandEvent &);
  void OnNameChanged(wxCommandEvent &);
  void UpdateValidation();

  wxTextCtrl *m_nameCtrl = nullptr;
  wxStaticText *m_dirText = nullptr;
  wxStaticText *m_errorText = nullptr;
  wxString m_dir;
};
