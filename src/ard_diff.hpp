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

#include "ard_ai.hpp" // AiPatchHunk
#include "ard_cli.hpp"
#include "ard_mdwidget.hpp" // ArduinoMarkdownPanel
#include "lcs.hpp"          // ArduinoLcsDiffAligner
#include "utils.hpp"        // SketchFileBuffer

enum DiffLineKind { Same,
                    Added,
                    Removed,
                    Modified,
                    Header };

// A resizable dialog that previews AI patch hunks file-by-file.
// For existing files it shows a 2-column aligned view (original vs patched).
// For new files it shows a single read-only preview of the created file.
class ArduinoDiffDialog : public wxDialog {
public:
  ArduinoDiffDialog(wxWindow *parent,
                    const std::vector<AiPatchHunk> &hunks,
                    const std::vector<SketchFileBuffer> &buffers,
                    ArduinoCli *cli,
                    wxConfigBase *config,
                    const wxString &aiComment);
  ~ArduinoDiffDialog();

  wxString GetAdditionalInfo();

private:
  struct FileViewData {
    wxString fileKey;     // as provided by patch (tab title)
    wxString resolvedKey; // normalized key used for matching buffers
    bool isNewFile = false;

    wxStyledTextCtrl *left = nullptr;  // original (existing files)
    wxStyledTextCtrl *right = nullptr; // patched (existing files) OR single (new file)

    bool syncing = false; // guard against recursion in scroll sync

    wxString originalText;
    std::vector<AiPatchHunk> hunks;
    wxCheckBox *chkShowFull = nullptr;
  };

private:
  ArduinoLcsDiffAligner m_lcsAligner;

  void BuildUi(const std::vector<AiPatchHunk> &hunks,
               const std::vector<SketchFileBuffer> &buffers);

  wxPanel *CreateExistingFileTab(wxNotebook *nb,
                                 FileViewData &v,
                                 const wxString &originalText,
                                 const std::vector<AiPatchHunk> &fileHunks);

  wxPanel *CreateNewFileTab(wxNotebook *nb,
                            FileViewData &v,
                            const std::vector<AiPatchHunk> &fileHunks);

  void SetupReadOnlyDiffCtrl(wxStyledTextCtrl *stc);

  void BindScrollSync(FileViewData &v);

  // ---- Core formatting helpers (line aligned view) ----
  wxString NormalizeKey(const wxString &path);

  std::vector<wxString> SplitLinesKeepLogical(const wxString &text);
  wxString JoinLines(const std::vector<wxString> &lines);

  wxString BuildNewFileContent(const std::vector<AiPatchHunk> &fileHunks);

  // Applies hunks to original, and also builds an aligned 2-column view
  // (originalAligned, patchedAligned) by padding within changed blocks.
  void BuildAlignedExistingFileView(
      const wxString &originalText,
      const std::vector<AiPatchHunk> &fileHunks,
      wxString &outOriginalAligned,
      wxString &outPatchedAligned);

  void BuildContextAlignedExistingFileView(
      const wxString &originalText,
      const std::vector<AiPatchHunk> &fileHunks,
      int contextLines,
      wxString &outLeft,
      wxString &outRight);

  // Ensure hunks are sane and sorted for a file; clips ranges to original size.
  std::vector<AiPatchHunk> PrepareFileHunks(
      const std::vector<AiPatchHunk> &fileHunks,
      int originalLineCount);

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
