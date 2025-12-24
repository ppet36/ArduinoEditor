#pragma once

#include "ard_ai.hpp"
#include "ard_cc.hpp"
#include "ard_cli.hpp"
#include "ard_setdlg.hpp"
#include <wx/arrstr.h>
#include <wx/fdrepdlg.h>
#include <wx/stc/stc.h>
#include <wx/timer.h>
#include <wx/wx.h>

class ArduinoEditorFrame;
class SymbolOverviewBar;
class ArduinoRefactoring;

class ArduinoEditor : public wxPanel {
  friend class ArduinoRefactoring;
  friend class ArduinoAiActions;

private:
  enum class PopupMode {
    None,
    Completion,
    Usages
  };

  PopupMode m_popupMode = PopupMode::None;

  std::string m_filename; // sketch relative filename
  std::string m_filePath; // absolute filename on disk

  bool m_readOnly = false;

  wxStyledTextCtrl *m_editor;

  SymbolOverviewBar *m_symbolOverview = nullptr;

  ArduinoCodeCompletion *completion;

  ArduinoCli *arduinoCli;

  wxConfigBase *m_config;

  ArduinoAiActions *m_aiActions = nullptr;

  CompletionMetadata m_completionMetadata;

  ClangSettings m_clangSettings;
  AiSettings m_aiSettings;

  wxTimer m_completionTimer;
  int m_scheduledPos; // caret position where we planned the completion

  // Usages
  std::vector<JumpTarget> m_lastUsages;
  uint64_t m_usagesSeq = 0;
  uint64_t m_usagesPendingSeq = 0;

  // r/o visual support
  wxColour m_normalBgColor;
  wxColour m_readOnlyBgColor;
  bool m_hasReadOnlyBg = false;
  void UpdateReadOnlyColors();
  void UpdateTabReadOnlyIndicator();

  // Find/Replace
  wxFindReplaceData *m_findData = nullptr;
  wxFindReplaceDialog *m_findDlg = nullptr;
  bool m_findIsReplace = false;

  bool m_autoIndentEnabled = false;
  bool m_symbolHighlightEnabled = true;
  bool m_highlightMatchingBraces = true;
  bool m_displayHoverInfo = true;

  wxTimer m_flashTimer;
  int m_flashLine = -1;

  // Symbol highlighting
  wxTimer m_symbolTimer;
  int m_lastSymbolPos;
  std::vector<int> m_symbolOccPositions; // pozice v STC (0-based), seazen
  uint64_t m_symbolHighlightSeq = 0;
  uint64_t m_symbolHighlightPendingSeq = 0;
  void OnSymbolTimer(wxTimerEvent &event);
  void OnSymbolOccurrencesReady(wxThreadEvent &event);

  // Handlers of the Find/Replace dialog
  void OpenFindDialog(bool replace);
  void OnFind(wxFindDialogEvent &event);
  void OnFindNext(wxFindDialogEvent &event);
  void OnReplace(wxFindDialogEvent &event);
  void OnReplaceAll(wxFindDialogEvent &event);
  void OnFindClose(wxFindDialogEvent &event);

  void OnContextMenu(wxContextMenuEvent &event);
  void OnPopupMenu(wxCommandEvent &event);
  void OnPopupMenuHighlight(wxMenuEvent &event);

  void OnSavePointLeft(wxStyledTextEvent &evt);
  void OnSavePointReached(wxStyledTextEvent &evt);
  void UpdateTabModifiedIndicator(bool modified);

  void OnEditorKillFocus(wxFocusEvent &event);
  void OnEditorMouseLeave(wxMouseEvent &event);
  void OnEditorUpdateUI(wxStyledTextEvent &event);

  // Keyboard shortcuts (Ctrl+F, Ctrl+H, F3 etc.)
  void OnCharHook(wxKeyEvent &event);

  // Custom search in the editor
  bool DoFind(const wxString &what, int flags, bool fromStart = false);

  // Hover
  void OnDwellStart(wxStyledTextEvent &event);
  void OnDwellEnd(wxStyledTextEvent &event);
  // Def. search
  void OnEditorLeftDown(wxMouseEvent &event);

  // Autocomp filter
  void AutoIndent(char ch);
  void OnCharAdded(wxStyledTextEvent &event);

  void OnChanged(wxStyledTextEvent &event);

  void OnAutoCompCompleted(wxStyledTextEvent &evt);
  void OnCompletionReady(wxThreadEvent &event);

  void HandleGoToLocation(const JumpTarget &tgt);
  void OnFlashTimer(wxTimerEvent &event);

  void InitCodeCompletionIcons();
  void ShowAutoCompletion();

  void ScheduleAutoCompletion();
  void OnCompletionTimer(wxTimerEvent &evt);

  void NavigateSymbolOccurrence(bool forward);

  ArduinoEditorFrame *GetOwnerFrame();

  int ModalMsgDialog(const wxString &message, const wxString &caption = _("Error"), int styles = wxOK | wxICON_ERROR);

  void ApplyBaseSettings(const EditorSettings &s);

  // Refactoring
  void RefactorRenameSymbolAtCursor();
  void RefactorIntroduceVariable();
  void RefactorInlineVariable();
  void RefactorGenerateFunctionFromCursor();
  void RefactorCreateDeclarationInHeader();
  void RefactorOrganizeIncludes();
  void RefactorExtractFunction();
  void RefactorFormatSelection();
  void RefactorFormatWholeFile();

  // Usages
  void FindSymbolUsagesAtCursor();
  void OnSymbolUsagesReady(wxThreadEvent &event);
  void ShowUsagesPopup(const std::vector<JumpTarget> &usages);
  void OnAutoCompSelection(wxStyledTextEvent &evt);

  // AI functions
  void AiExplainSelection();
  void AiGenerateDocCommentForSymbol();

public:
  void AiSolveError(const ArduinoParseError &error); // this is needed from main frame
private:
  void AiGenerateDocCommentsForCurrentFile();
  void AiGenerateDocCommentsForCurrentClass();
  void AiOptimizeFunctionOrMethod();

public:
  ArduinoEditor(wxWindow *parent,
                ArduinoCli *cli,
                ArduinoCodeCompletion *comp,
                wxConfigBase *config,
                const std::string &filename,
                const std::string &fullPath = std::string());

  ~ArduinoEditor();

  void SetReadOnly(bool ro);
  bool IsReadOnly() const;

  int RefactorRenameSymbol(std::vector<JumpTarget> occs, const std::string &oldName, const std::string &newName);

  // Filename
  void SetFilePath(const std::string &filepath);
  std::string GetFileName() const;
  std::string GetFilePath() const;
  std::string GetText() const;

  // Tool method for circular bitmap
  static wxBitmap MakeCircleBitmap(int diameter,
                                   const wxColour &fill,
                                   const wxColour &border = *wxBLACK,
                                   int borderWidth = 0,
                                   int offsetX = 0,
                                   int offsetY = 0);

  // External settings apply
  void ApplySettings(const EditorSettings &s);
  void ApplySettings(const ClangSettings &s);
  void ApplySettings(const AiSettings &s);
  void ApplyBoardChange();

  void Goto(int line, int column);               // 1-based
  void GetCurrentCursor(int &line, int &column); // 1-based

  void FlashLine(int line);
  void ClearDiagnosticsIndicators();
  void AddDiagnosticUnderline(unsigned line, unsigned column, bool isError);

  bool Save();
  bool IsModified();

  // Autocomplete
  bool IsAutoCompActive();
  void CancelCallTip();
};
