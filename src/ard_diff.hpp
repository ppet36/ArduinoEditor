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

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/collpane.h>
#include <wx/dialog.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/string.h>

#include <wx/stc/stc.h>

#include "ard_cli.hpp"
#include "ard_mdwidget.hpp" // ArduinoMarkdownPanel
#include "lcs.hpp"          // ArduinoLcsDiffAligner
#include "utils.hpp"        // SketchFileBuffer

enum DiffLineKind { Same,
                    Added,
                    Removed,
                    Modified,
                    Header };

// A resizable dialog that previews changes file-by-file.
// It takes old/new in-memory buffers and shows a 2-column aligned view
// for modified/deleted files and a single preview for new files.
class ArduinoDiffDialog : public wxDialog {
public:
  ArduinoDiffDialog(wxWindow *parent,
                    const std::vector<SketchFileBuffer> &buffersOld,
                    const std::vector<SketchFileBuffer> &buffersNew,
                    ArduinoCli *cli,
                    wxConfigBase *config,
                    const wxString &aiComment);
  ~ArduinoDiffDialog();

  wxString GetAdditionalInfo();

private:
  struct FileViewData {
    wxString fileKey;     // tab title
    wxString resolvedKey; // normalized key used for matching buffers
    bool isNewFile = false;
    bool isDeletedFile = false;

    wxStyledTextCtrl *left = nullptr;  // original (existing files)
    wxStyledTextCtrl *right = nullptr; // patched (existing files) OR single (new file)

    bool syncing = false; // guard against recursion in scroll sync

    wxString oldText;
    wxString newText;
    wxCheckBox *chkShowFull = nullptr;
  };

private:
  ArduinoLcsDiffAligner m_lcsAligner;

  void BuildUi(const std::vector<SketchFileBuffer> &buffersOld,
               const std::vector<SketchFileBuffer> &buffersNew);

  wxPanel *CreateExistingFileTab(wxNotebook *nb,
                                 FileViewData &v,
                                 const wxString &oldText,
                                 const wxString &newText);

  wxPanel *CreateNewFileTab(wxNotebook *nb,
                            FileViewData &v,
                            const wxString &newText);

  void SetupReadOnlyDiffCtrl(wxStyledTextCtrl *stc);

  void BindScrollSync(FileViewData &v);

  // ---- Core formatting helpers (line aligned view) ----
  wxString NormalizeKey(const wxString &path);

  std::vector<wxString> SplitLinesKeepLogical(const wxString &text);
  wxString JoinLines(const std::vector<wxString> &lines);

  void BuildAlignedExistingFileView(const wxString &oldText,
                                   const wxString &newText,
                                   wxString &outOldAligned,
                                   wxString &outNewAligned);

  void BuildContextAlignedExistingFileView(const wxString &oldText,
                                          const wxString &newText,
                                          int contextLines,
                                          wxString &outLeft,
                                          wxString &outRight);

  void UpdateExistingTabView(FileViewData &v);

  // line highlight

  void SetupDiffIndicators(wxStyledTextCtrl *stc);
  std::vector<DiffLineKind> ComputeLineKindsFromAlignedText(const wxString &leftAligned, const wxString &rightAligned);
  void ApplyLineIndicators(wxStyledTextCtrl *stc, const std::vector<DiffLineKind> &kinds);

private:
  wxNotebook *m_notebook = nullptr;
  std::deque<FileViewData> m_views;
  ArduinoCli *m_cli = nullptr;
  wxConfigBase *m_config = nullptr;
  ArduinoMarkdownPanel *m_aiMd = nullptr;
  wxCollapsiblePane *m_aiPane = nullptr;

  int m_diffDivPos = -1; // last splitter sash position (px), -1 = default

  wxString m_aiComment;

  bool m_showFullFile = false;
  int m_contextLines = 10;
};
