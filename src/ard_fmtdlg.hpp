#pragma once

#include <wx/dialog.h>
#include <wx/string.h>

// Some settings from clang-format dialog.
class ArduinoClangFormatSettingsDialog : public wxDialog {
public:
  // overridesJsonIn: JSON object as string. Example:
  // {"SortIncludes":false,"AllowShortBlocksOnASingleLine":"Empty"}
  ArduinoClangFormatSettingsDialog(wxWindow *parent,
                                   const wxString &overridesJsonIn);

  // Resulting JSON (only values that differ from defaults).
  wxString GetOverridesJson() const { return m_overridesJsonOut; }

private:
  void BuildUi();
  void BuildGridFromMeta();
  void LoadFromJson();
  void SaveToJson();
  void ApplyFilter(const wxString &filterLower);

  void OnSearchChanged(wxCommandEvent &e);
  void OnReset(wxCommandEvent &e);
  void OnOk(wxCommandEvent &e);

private:
  wxString m_overridesJsonIn;
  wxString m_overridesJsonOut;

  class wxPropertyGrid *m_pg = nullptr;
  class wxSearchCtrl *m_search = nullptr;
};
