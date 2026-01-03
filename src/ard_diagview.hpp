#pragma once

#include "ard_cc.hpp"
#include "ard_setdlg.hpp"
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/listctrl.h>
#include <wx/panel.h>

#include <string>
#include <vector>

class ArduinoDiagnosticsActionEvent : public wxCommandEvent {
public:
  ArduinoDiagnosticsActionEvent(wxEventType type = wxEVT_NULL, int id = 0)
      : wxCommandEvent(type, id) {}

  ArduinoDiagnosticsActionEvent(const ArduinoDiagnosticsActionEvent &o) = default;

  wxEvent *Clone() const override { return new ArduinoDiagnosticsActionEvent(*this); }

  void SetJumpTarget(const JumpTarget &t) {
    m_jump = t;
    m_hasJump = true;
  }
  const JumpTarget &GetJumpTarget() const { return m_jump; }
  bool HasJumpTarget() const { return m_hasJump; }

  void SetDiagnostic(const ArduinoParseError &d) {
    m_diag = d;
    m_hasDiag = true;
  }
  const ArduinoParseError &GetDiagnostic() const { return m_diag; }
  bool HasDiagnostic() const { return m_hasDiag; }

private:
  JumpTarget m_jump{};
  ArduinoParseError m_diag{};

  bool m_hasJump{false};
  bool m_hasDiag{false};
};

wxDECLARE_EVENT(EVT_ARD_DIAG_JUMP, ArduinoDiagnosticsActionEvent);
wxDECLARE_EVENT(EVT_ARD_DIAG_SOLVE_AI, ArduinoDiagnosticsActionEvent);

class ArduinoDiagnosticsView : public wxPanel {
public:
  ArduinoDiagnosticsView(wxWindow *parent, wxConfigBase *config);

  void SetSketchRoot(std::string sketchRoot) { m_sketchRoot = std::move(sketchRoot); }

  void SetDiagnostics(const std::vector<ArduinoParseError> &diags);

  void ShowMessage(const wxString &message);

  bool GetDiagnosticsAt(const std::string &filename, unsigned row, unsigned col, ArduinoParseError &outDiagnostics);

  void ApplySettings(const EditorSettings &settings);
  void ApplySettings(const AiSettings &settings);

private:
  void OnItemActivated(wxListEvent &event);
  void OnContextMenu(wxContextMenuEvent &evt);

  void UpdateColors(const EditorColorScheme &colors);

  void CopySelected();
  void CopyAll();
  void RequestSolveAi();

  wxString GetRowText(long row) const;
  long GetSelectedRow() const;

  void OnSysColourChanged(wxSysColourChangedEvent &event);

private:
  wxConfigBase *m_config;

  wxListCtrl *m_list{nullptr};

  wxImageList *m_imgList{nullptr};
  int m_imgError{-1};
  int m_imgWarning{-1};
  int m_imgNote{-1};

  bool m_aiEnabled{false};
  std::string m_sketchRoot;

  std::vector<ArduinoParseError> m_current;
};
