#pragma once

#include <string>
#include <vector>
#include <wx/config.h>
#include <wx/listctrl.h>
#include <wx/wx.h>

struct JumpTarget;

class ArduinoRenameSymbolDialog : public wxDialog {
public:
  ArduinoRenameSymbolDialog(wxWindow *parent,
                            wxConfigBase *config,
                            const wxString &oldName,
                            const std::vector<JumpTarget> &occurrences,
                            const std::string &sketchRoot);
  ~ArduinoRenameSymbolDialog();

  wxString GetNewName() const;

  void GetSelectedOccurrences(std::vector<JumpTarget> &out) const;

private:
  void InitLayout();
  void PopulateList(const std::vector<JumpTarget> &occurrences,
                    const std::string &sketchRoot);

  void OnTextEnter(wxCommandEvent &evt);
  void OnListItemActivated(wxListEvent &evt);

  wxConfigBase *m_config = nullptr;
  wxTextCtrl *m_newNameCtrl = nullptr;
  wxListCtrl *m_listCtrl = nullptr;
  wxButton *m_okButton = nullptr;

  std::vector<JumpTarget> m_occurrences;
};
