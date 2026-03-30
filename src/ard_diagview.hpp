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

  bool GetDiagnosticsAt(const std::string &filename, unsigned row, unsigned col, std::vector<ArduinoParseError> &outDiagnostics);

  void ApplySettings(const EditorSettings &settings);
  void ApplySettings(const AiSettings &settings);

  void SetStale(bool stale = true);

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
  wxString m_currentMessage;
};
