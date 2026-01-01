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

#include "ard_edit.hpp"
#include "ard_ap.hpp"
#include "ard_ed_frm.hpp"
#include "ard_esymov.hpp"
#include "ard_ev.hpp"
#include "ard_refactor.hpp"
#include "utils.hpp"
#include <algorithm>
#include <wx/artprov.h>
#include <wx/notebook.h>
#include <wx/richmsgdlg.h>

enum {
  ID_TIMER_COMPLETETION = wxID_HIGHEST + 4000,
  ID_TIMER_FLASH,
  ID_TIMER_SYMBOL_HIGHLIGHT,
  ID_MENU_REFACTOR_RENAME,
  ID_MENU_REFACTOR_INTRODUCE_VAR,
  ID_MENU_REFACTOR_INLINE_VAR,
  ID_MENU_REFACTOR_GENERATE_FUNC,
  ID_MENU_REFACTOR_CREATE_DECL,
  ID_MENU_REFACTOR_ORG_INCLUDES,
  ID_MENU_REFACTOR_EXTRACT_FUNC,
  ID_MENU_REFACTOR_FORMAT_SELECTION,
  ID_MENU_REFACTOR_FORMAT_WHOLE_FILE,
  ID_MENU_AI_EXPLAIN_SELECTION,
  ID_MENU_AI_GENERATE_COMMENT,
  ID_MENU_AI_GENERATE_DOC_FUNC,
  ID_MENU_AI_GENERATE_DOC_CLASS,
  ID_MENU_AI_OPTIMIZE_FUNC_METHOD,
  ID_MENU_NAV_PREV_OCCURRENCE,
  ID_MENU_NAV_NEXT_OCCURRENCE,
  ID_MENU_NAV_SYM_OCCURRENCE,
  ID_MENU_NAV_GOTO_DEFINITION
};

enum {
  STC_FLASH_INDICATOR = 8,
  STC_INDIC_DIAG_ERROR,
  STC_INDIC_DIAG_WARNING,
  STC_INDIC_SYMBOL_OCCURRENCE
};

enum {
  ESM_SYMBOL = 1,
  ESM_ERROR,
  ESM_WARNING
};

#define FLASH_TIME 500

#ifdef __APPLE__
// Returns true if it is an emoji (most are outside the BMP, i.e. > U+FFFF)
static inline bool IsDangerousEmoji(wxUniChar ch) {
  // Emojis are for the most part outside the basic multilingual plane.
  if (ch > 0xFFFF)
    return true;

  // Some emojis are directly in the BMP (typically U+2600..U+27FF),
  // so we add a simple range filter:
  if ((ch >= 0x2600 && ch <= 0x27BF)) // Misc symbols + Dingbats
    return true;

  return false;
}
#endif

namespace {

// Map libclang cursor kind to our image IDs registered in InitCodeCompletionIcons().
static inline int CompletionImageType(CXCursorKind kind) {
  switch (kind) {
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
      return 1; // function
    case CXCursor_VarDecl:
    case CXCursor_FieldDecl:
    case CXCursor_ParmDecl:
      return 2; // variable/parameter
    case CXCursor_StructDecl:
    case CXCursor_ClassDecl:
    case CXCursor_UnionDecl:
    case CXCursor_EnumDecl:
      return 3; // type
    case CXCursor_MacroDefinition:
      return 4; // macro
    default:
      return 0;
  }
}

static wxString BuildCompletionList(const std::vector<CompletionItem> &completions) {
  wxString list;
  for (size_t i = 0; i < completions.size(); ++i) {
    if (i > 0)
      list << '\n';

    const auto &item = completions[i];
    list << wxString::FromUTF8(item.label.c_str());
    list << wxT("?") << CompletionImageType(item.kind);
  }
  return list;
}

// --- Identifier helpers used by symbol-highlight code ------------------------

static inline bool AEIsSpaceChar(int c) {
  return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static inline bool AEIsIdentStart(unsigned char c) {
  return (c == '_') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline bool AEIsIdentChar(unsigned char c) {
  return (c == '_') ||
         (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9');
}

struct AeIdentSpan {
  int symPos = -1; // position used for line/col (may be caret-1 if caret is just after identifier)
  int start = -1;  // identifier start
  int end = -1;    // identifier end (exclusive)
};

static bool AEValidateIdentifierSpan(wxStyledTextCtrl *ed, int start, int end, int textLen) {
  if (!ed || start < 0 || end <= start || end > textLen) {
    return false;
  }

  unsigned char c0 = (unsigned char)ed->GetCharAt(start);
  if (!AEIsIdentStart(c0)) {
    return false;
  }

  for (int p = start; p < end; ++p) {
    if (!AEIsIdentChar((unsigned char)ed->GetCharAt(p))) {
      return false;
    }
  }
  return true;
}

// Extract a valid C/C++ identifier at/near caret.
// - Reject whitespace.
// - Allow caret just after identifier (e.g. "foo(|", "foo;|") by backing up one char.
// - Uses STC WordStart/WordEnd but then *strictly validates* by AEIsIdent* rules.
static bool AEExtractIdentifierAtOrBeforeCaret(wxStyledTextCtrl *ed,
                                               int caretPos,
                                               AeIdentSpan &out,
                                               int textLen) {
  if (!ed || textLen <= 0) {
    return false;
  }

  int symPos = caretPos;
  if (symPos < 0 || symPos >= textLen) {
    return false;
  }

  const int c0 = ed->GetCharAt(symPos);

  if (AEIsSpaceChar(c0)) {
    return false;
  }

  // Allow caret just *after* identifier (e.g. foo(|, foo; foo) etc.)
  if (!AEIsIdentChar((unsigned char)c0) && symPos > 0) {
    const int cPrev = ed->GetCharAt(symPos - 1);
    if (AEIsIdentChar((unsigned char)cPrev)) {
      symPos--;
    }
  }

  if (symPos < 0 || symPos >= textLen) {
    return false;
  }
  if (!AEIsIdentChar((unsigned char)ed->GetCharAt(symPos))) {
    return false;
  }

  const int wordStart = ed->WordStartPosition(symPos, true);
  const int wordEnd = ed->WordEndPosition(symPos, true);
  if (!AEValidateIdentifierSpan(ed, wordStart, wordEnd, textLen)) {
    return false;
  }

  out.symPos = symPos;
  out.start = wordStart;
  out.end = wordEnd;
  return true;
}

// Clang JumpTarget for occurrences points to the *start* of the symbol.
// So in OnSymbolOccurrencesReady we can compute length by scanning only forward.
static int AEIdentifierLengthFrom(wxStyledTextCtrl *ed, int startPos, int textLen) {
  if (!ed || startPos < 0 || startPos >= textLen) {
    return 0;
  }

  unsigned char c0 = (unsigned char)ed->GetCharAt(startPos);
  if (!AEIsIdentStart(c0)) {
    return 0;
  }

  int p = startPos + 1;
  while (p < textLen && AEIsIdentChar((unsigned char)ed->GetCharAt(p))) {
    ++p;
  }
  return p - startPos;
}

} // namespace

// -------------------------------------------------------------------------------
ArduinoEditor::ArduinoEditor(wxWindow *parent,
                             ArduinoCli *cli,
                             ArduinoCodeCompletion *comp,
                             wxConfigBase *config,
                             const std::string &filename,
                             const std::string &fullPath)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL),
      m_readOnly(false),
      m_completionTimer(this, ID_TIMER_COMPLETETION),
      m_scheduledPos(-1),
      m_flashTimer(this, ID_TIMER_FLASH),
      m_symbolTimer(this, ID_TIMER_SYMBOL_HIGHLIGHT),
      m_lastSymbolPos(-1) {
  this->arduinoCli = cli;
  this->m_config = config;
  this->m_filename = filename;
  this->completion = comp;

  // real filesystem path
  if (!fullPath.empty()) {
    m_filePath = fullPath;
  } else {
    std::string sep(1, wxFileName::GetPathSeparator());
    m_filePath = arduinoCli->GetSketchPath() + sep + m_filename;
  }

  wxBoxSizer *mainSizer = new wxBoxSizer(wxHORIZONTAL);
  m_editor = new wxStyledTextCtrl(this);
  m_symbolOverview = new SymbolOverviewBar(this, m_editor);

  mainSizer->Add(m_symbolOverview, 0, wxEXPAND);
  mainSizer->Add(m_editor, 1, wxEXPAND);

  SetSizer(mainSizer);

  SetupStyledTextCtrl(m_editor, m_config);

  m_editor->AutoCompSetIgnoreCase(false);
  m_editor->AutoCompSetAutoHide(false);
  m_editor->AutoCompSetChooseSingle(false);
  m_editor->AutoCompSetMaxHeight(15);
  m_editor->AutoCompSetSeparator('\n');
  m_editor->UsePopUp(wxSTC_POPUP_NEVER);
  m_editor->SetCaretLineVisibleAlways(true);

  InitCodeCompletionIcons();

  EditorSettings s;
  s.Load(m_config);

  ApplyBaseSettings(s);

  // Binding events
  m_editor->Bind(wxEVT_STC_CHARADDED, &ArduinoEditor::OnCharAdded, this);
  m_editor->Bind(wxEVT_STC_AUTOCOMP_COMPLETED, &ArduinoEditor::OnAutoCompCompleted, this);
  m_editor->Bind(wxEVT_STC_AUTOCOMP_SELECTION, &ArduinoEditor::OnAutoCompSelection, this);
  m_editor->Bind(wxEVT_STC_AUTOCOMP_CANCELLED, &ArduinoEditor::OnAutoCompCancelled, this);
  // - symbol highlight
  m_editor->Bind(wxEVT_STC_UPDATEUI, &ArduinoEditor::OnEditorUpdateUI, this);
  Bind(EVT_SYMBOL_OCCURRENCES_READY, &ArduinoEditor::OnSymbolOccurrencesReady, this);
  // - symbol usages search
  Bind(EVT_SYMBOL_USAGES_READY, &ArduinoEditor::OnSymbolUsagesReady, this);
  // - context menu on right mouse button
  m_editor->Bind(wxEVT_CONTEXT_MENU, &ArduinoEditor::OnContextMenu, this);
  // - save + change
  m_editor->Bind(wxEVT_STC_SAVEPOINTLEFT, &ArduinoEditor::OnSavePointLeft, this);
  m_editor->Bind(wxEVT_STC_SAVEPOINTREACHED, &ArduinoEditor::OnSavePointReached, this);
  m_editor->Bind(wxEVT_STC_MODIFIED, &ArduinoEditor::OnChanged, this);
  // - disable font resizing
  m_editor->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent &evt) {
#if defined(__APPLE__)
    if (evt.CmdDown())
#else
    if (evt.ControlDown())
#endif
    {
      m_editor->SetZoom(0);
      return; // block zooming
    }
    evt.Skip();
  });

  // def. find
  m_editor->Bind(wxEVT_LEFT_DOWN, &ArduinoEditor::OnEditorLeftDown, this);

  Bind(wxEVT_TIMER, &ArduinoEditor::OnCompletionTimer, this, m_completionTimer.GetId());

  // Keyboard shortcuts - captures keys before the children
  Bind(wxEVT_CHAR_HOOK, &ArduinoEditor::OnCharHook, this);

  // Autocompletion
  Bind(EVT_COMPLETION_READY, &ArduinoEditor::OnCompletionReady, this);

  // calltips hide
  m_editor->Bind(wxEVT_KILL_FOCUS, &ArduinoEditor::OnEditorKillFocus, this);
  m_editor->Bind(wxEVT_LEAVE_WINDOW, &ArduinoEditor::OnEditorMouseLeave, this);

  // Hover
  m_editor->SetMouseDwellTime(500); // 500 ms, how long the mouse must remain still

  // Flash
  Bind(wxEVT_TIMER, &ArduinoEditor::OnFlashTimer, this, m_flashTimer.GetId());

  // timer for symbol highlight
  Bind(wxEVT_TIMER, &ArduinoEditor::OnSymbolTimer, this, m_symbolTimer.GetId());

  // Tooltips/hovers
  m_editor->Bind(wxEVT_STC_DWELLSTART, &ArduinoEditor::OnDwellStart, this);
  m_editor->Bind(wxEVT_STC_DWELLEND, &ArduinoEditor::OnDwellEnd, this);

  // AI - every editor has own interface
  m_aiActions = new ArduinoAiActions(this);

  std::string fn = GetFilePath();
  wxString wxFn = wxString::FromUTF8(fn.c_str());
  if (!m_editor->LoadFile(wxFn)) {
    ModalMsgDialog(wxString::Format(_("Unable to load file %s."), wxFn));
  } else {
#ifdef __WXMAC__
    {
      // On Apple, Scintilla cannot display pictorial emojis and it crashes with SigBus somewhere
      // in CopyImage. Therefore, it is specially filtered here and replaced with ?.
      wxString txt = m_editor->GetText();
      bool changed = false;

      for (size_t i = 0; i < txt.length(); ++i) {
        if (IsDangerousEmoji(txt[i])) {
          txt[i] = '?';
          changed = true;
        }
      }

      if (changed) {
        m_editor->SetText(txt);
      }
    }
#endif
    m_editor->SetSavePoint();
  }

  // r/o stuff (external non-project files)
  m_normalBgColor = m_editor->StyleGetBackground(wxSTC_STYLE_DEFAULT);
  m_hasReadOnlyBg = false;
}

ArduinoEditor::~ArduinoEditor() {
  delete m_aiActions;
  m_aiActions = nullptr;
}

void ArduinoEditor::SetFilePath(const std::string &filepath) {
  m_filePath = filepath;

  std::string sketchPath = arduinoCli->GetSketchPath();
  if (m_filePath.rfind(sketchPath, 0) == 0) {
    m_filename = m_filePath.substr(sketchPath.size());

    if (!m_filename.empty() && (m_filename[0] == '/' || m_filename[0] == '\\')) {
      m_filename = m_filename.substr(1);
    }
  } else {
    m_filename = std::filesystem::path(m_filePath).filename().string();
  }
}

std::string ArduinoEditor::GetFileName() const {
  return m_filename;
}

std::string ArduinoEditor::GetFilePath() const {
  return m_filePath;
}

void ArduinoEditor::SetReadOnly(bool ro) {
  if (m_readOnly == ro && m_editor && m_editor->GetReadOnly() == ro) {
    return;
  }

  m_readOnly = ro;

  if (m_editor) {
    m_editor->SetReadOnly(ro);
  }

  UpdateReadOnlyColors();
  UpdateTabReadOnlyIndicator();
}

bool ArduinoEditor::IsReadOnly() const {
  return m_readOnly;
}

std::string ArduinoEditor::GetText() const {
  return wxToStd(m_editor->GetText());
}

void ArduinoEditor::OpenFindDialog(bool replace) {
  if (!m_findData) {
    m_findData = new wxFindReplaceData();
    m_findData->SetFlags(wxFR_DOWN);
  }

  long style = replace ? wxFR_REPLACEDIALOG : 0;

  // If the dialog exists but the mode (find/replace) does not match, destroy it
  if (m_findDlg && m_findIsReplace != replace) {
    m_findDlg->Destroy();
    m_findDlg = nullptr;
  }

  if (!m_findDlg) {
    m_findDlg = new wxFindReplaceDialog(
        this,
        m_findData,
        replace ? _("Find and Replace") : _("Find"),
        style);

    m_findIsReplace = replace;

    m_findDlg->Bind(wxEVT_FIND, &ArduinoEditor::OnFind, this);
    m_findDlg->Bind(wxEVT_FIND_NEXT, &ArduinoEditor::OnFindNext, this);
    m_findDlg->Bind(wxEVT_FIND_REPLACE, &ArduinoEditor::OnReplace, this);
    m_findDlg->Bind(wxEVT_FIND_REPLACE_ALL, &ArduinoEditor::OnReplaceAll, this);
    m_findDlg->Bind(wxEVT_FIND_CLOSE, &ArduinoEditor::OnFindClose, this);
  }

  // If the mode fits and the dialog already exists, just show it
  m_findDlg->Show();
  m_findDlg->Raise();
}

void ArduinoEditor::OnCharHook(wxKeyEvent &event) {
  int key = event.GetKeyCode();
  int mods = event.GetModifiers();

  // Primary modifier: Ctrl on Win/Linux, Cmd on macOS
  bool primary = (mods & wxMOD_CMD) != 0;
  // Real Ctrl on macOS (on Win/Linux it will typically be 0)
  bool rawCtrl = (mods & wxMOD_RAW_CONTROL) != 0;
  bool alt = (mods & wxMOD_ALT) != 0;
  bool shift = (mods & wxMOD_SHIFT) != 0;

  APP_DEBUG_LOG("EDIT: key=%d, mods=%d, primary=%d, rawCtrl=%d, alt=%d, shift=%d",
                key, mods, primary, rawCtrl, alt, shift);

  bool anyCtrl = primary || rawCtrl; // "some" control modifier

  // --- MANUAL COMPLETION (Ctrl/Cmd + Space, Ctrl + .) ---
  if (!m_readOnly && (m_clangSettings.completionMode != noCompletion)) {
    // Ctrl/Cmd + Space, without additional modifiers
    if (!alt && !shift && anyCtrl && key == WXK_SPACE) {
      ShowAutoCompletion();
      return;
    }

    // Ctrl + . - here we want the "real" Ctrl (on Mac), not Cmd
    if (!alt && !shift && rawCtrl && key == '.') {
      ShowAutoCompletion();
      return;
    }
  }

  // --- BACKSPACE while completion popup is visible: keep it alive and re-filter from cache ---
  // Scintilla's AutoComp window tends to close on backspace. If we already have completion items
  // in memory, we can instantly re-show the popup with a shorter prefix, so a small typo doesn't
  // make the whole thing disappear.
  if (!alt && !anyCtrl && !shift && key == WXK_BACK &&
      m_popupMode == PopupMode::Completion &&
      !m_completionMetadata.m_lastCompletions.empty()) {

    // Let the editor actually delete the character first.
    event.Skip();

    const int wordStart = m_completionMetadata.m_lastWordStart;

    CallAfter([this, wordStart]() {
      if (!m_editor)
        return;

      if (m_completionMetadata.m_lastCompletions.empty()) {
        if (m_editor->AutoCompActive()) {
          m_editor->AutoCompCancel();
        }
        return;
      }

      CancelCallTip();

      const int curPos = m_editor->GetCurrentPos();

      // If the user deleted "behind" the start (or moved elsewhere), just cancel the popup.
      if (wordStart < 0 || curPos < wordStart) {
        if (m_editor->AutoCompActive()) {
          m_editor->AutoCompCancel();
        }

        m_popupMode = PopupMode::None;
        return;
      }

      // Do not keep the popup when the caret jumped to a different line.
      if (m_editor->LineFromPosition(curPos) != m_editor->LineFromPosition(wordStart)) {
        if (m_editor->AutoCompActive()) {
          m_editor->AutoCompCancel();
        }

        m_popupMode = PopupMode::None;
        return;
      }

      int lengthEntered = curPos - wordStart;
      if (lengthEntered < 0) {
        lengthEntered = 0;
      }

      m_popupMode = PopupMode::Completion;

      // After backspace, we leave interactive code completion. The popup will remain open
      // and the user can do whatever they want with it, including constructing a completely
      // different expression from the root.

      ScheduleAutoCompletion();

      wxString list = BuildCompletionList(m_completionMetadata.m_lastCompletions);
      if (!list.IsEmpty()) {
        m_editor->AutoCompShow(lengthEntered, list);
        m_completionMetadata.m_lastLengthEntered = lengthEntered;
      }
    });

    return;
  }

  // ---- Alt + Up/Down -> jump to previous/next occurrence of symbol ----
  if (alt && !anyCtrl && !shift && (key == WXK_DOWN || key == WXK_UP)) {
    NavigateSymbolOccurrence(key == WXK_DOWN);
    return;
  }

  // ---- FIND (Cmd/Ctrl + F, without additional modifiers) ----
  if (primary && !alt && !shift && (key == 'F' || key == 'f')) {
    OpenFindDialog(false);
    return;
  }

  // ---- REPLACE (Cmd/Ctrl + Alt + F) ----
  if (primary && alt && !shift && (key == 'F' || key == 'f')) {
    OpenFindDialog(true);
    return;
  }

  // ---- FIND NEXT (Cmd/Ctrl + G) ----
  if (primary && !alt && !shift && (key == 'G' || key == 'g')) {
    if (m_findData)
      DoFind(m_findData->GetFindString(), m_findData->GetFlags(), false);
    return;
  }

  // ---- FIND PREVIOUS (Cmd/Ctrl + Shift + G) ----
  if (primary && !alt && shift && (key == 'G' || key == 'g')) {
    if (m_findData) {
      int flags = m_findData->GetFlags() ^ wxFR_DOWN;
      DoFind(m_findData->GetFindString(), flags, false);
    }
    return;
  }

  // ---- FIND USAGES (Shift + F12) ----
  if (!anyCtrl && !alt && shift && key == WXK_F12) {
    FindSymbolUsagesAtCursor();
    return;
  }

  // ---- F3 / Shift+F3 alias for Win/Linux (also works fine on Mac) ----
  if (!primary && !alt && !shift && key == WXK_F3) {
    if (m_findData)
      DoFind(m_findData->GetFindString(), m_findData->GetFlags(), false);
    return;
  }
  if (!primary && !alt && shift && key == WXK_F3) {
    if (m_findData) {
      int flags = m_findData->GetFlags() ^ wxFR_DOWN;
      DoFind(m_findData->GetFindString(), flags, false);
    }
    return;
  }

  // ---- Cmd/Ctrl + Z undo/redo ----
  if (!m_readOnly && primary && !alt && (key == 'Z' || key == 'z')) {
    if (shift) {
      if (m_editor->CanRedo()) {
        m_editor->Redo();
      }
    } else {
      if (m_editor->CanUndo()) {
        m_editor->Undo();
      }
    }
    return;
  }

  event.Skip();
}

void ArduinoEditor::NavigateSymbolOccurrence(bool forward) {
  if (m_symbolOccPositions.empty()) {
    wxBell();
    return;
  }

  const int caretPos = m_editor->GetCurrentPos();

  // If the caret is inside a symbol, use symbol boundaries to decide what is
  // "previous/next", so we don't just jump to the start of the current occurrence.
  const int wordStart = m_editor->WordStartPosition(caretPos, true);
  const int wordEnd = m_editor->WordEndPosition(caretPos, true);

  const int comparePos = forward
                             ? ((wordEnd > wordStart) ? wordEnd : caretPos)
                             : ((wordEnd > wordStart) ? wordStart : caretPos);

  int targetPos = -1;
  if (forward) {
    // Find the first occurrence strictly after the current symbol.
    for (int p : m_symbolOccPositions) {
      if (p > comparePos) {
        targetPos = p;
        break;
      }
    }
    // no next occurrence, no movement
  } else {
    // Find the last occurrence strictly before the current symbol.
    for (int i = (int)m_symbolOccPositions.size() - 1; i >= 0; --i) {
      int p = m_symbolOccPositions[i];
      if (p < comparePos) {
        targetPos = p;
        break;
      }
    }
    // we are on first occurrence
  }

  if (targetPos < 0) {
    return;
  }

  m_editor->GotoPos(targetPos);
  m_editor->SetCurrentPos(targetPos);
  m_editor->SetSelection(targetPos, targetPos);
  m_editor->EnsureCaretVisible();
}

void ArduinoEditor::FindSymbolUsagesAtCursor() {
  if (!completion) {
    return;
  }

  int line = 0;
  int column = 0;
  GetCurrentCursor(line, column); // 1-based

  std::string code = wxToStd(m_editor->GetText());

  // new request ID
  uint64_t seq = ++m_usagesSeq;
  m_usagesPendingSeq = seq;

  std::vector<SketchFileBuffer> files;
  GetOwnerFrame()->CollectEditorSources(files);

  completion->FindSymbolOccurrencesProjectWideAsync(
      files,
      m_filePath,
      wxToStd(m_editor->GetText()),
      line,
      column,
      /*onlyFromSketch=*/false,
      this,
      seq,
      EVT_SYMBOL_USAGES_READY);
}

void ArduinoEditor::OnSymbolUsagesReady(wxThreadEvent &event) {
  uint64_t seq = (uint64_t)event.GetInt();

  // we have a newer request -> this is old, we'll throw it away
  if (seq != m_usagesPendingSeq) {
    return;
  }

  auto usages = event.GetPayload<std::vector<JumpTarget>>();
  if (usages.empty()) {
    return;
  }

  ShowUsagesPopup(usages);
}

// Helper: find SketchFileBuffer for a given file path.
static const SketchFileBuffer *FindBufferForFile(const std::vector<SketchFileBuffer> &files,
                                                 const std::string &filePath) {
  // Exact match first
  for (const auto &f : files) {
    if (f.filename == filePath) {
      return &f;
    }
  }

  // Fallback – match by basename if absolute paths differ but the file name is unique.
  wxFileName targetFn(wxString::FromUTF8(filePath.c_str()));
  wxString targetBase = targetFn.GetFullName();

  for (const auto &f : files) {
    wxFileName bufFn(wxString::FromUTF8(f.filename.c_str()));
    if (bufFn.GetFullName() == targetBase) {
      return &f;
    }
  }

  return nullptr;
}

// Helper: build a short context snippet around the symbol usage.
static wxString MakeUsageContext(const SketchFileBuffer &buf,
                                 unsigned line,
                                 unsigned column,
                                 size_t maxLen = 48) {
  if (maxLen == 0) {
    return wxString();
  }

  const std::string &code = buf.code;
  if (code.empty() || line == 0) {
    return wxString();
  }

  // Find the start/end of the requested line (1-based line index).
  size_t start = 0;
  unsigned currentLine = 1;

  while (currentLine < line && start < code.size()) {
    size_t nl = code.find('\n', start);
    if (nl == std::string::npos) {
      return wxString(); // line out of range
    }
    start = nl + 1;
    ++currentLine;
  }

  if (start >= code.size()) {
    return wxString();
  }

  size_t end = code.find('\n', start);
  if (end == std::string::npos) {
    end = code.size();
  }

  if (end <= start) {
    return wxString();
  }

  std::string lineText = code.substr(start, end - start);

  // Replace tabs with spaces so the popup looks nicer.
  for (char &ch : lineText) {
    if (ch == '\t') {
      ch = ' ';
    }
  }

  if (lineText.empty()) {
    return wxString();
  }

  // If the line is short enough, show it as is.
  if (lineText.size() <= maxLen) {
    return wxString::FromUTF8(lineText.c_str());
  }

  // Use column (1-based) as a "center" when possible.
  size_t center = 0;
  if (column > 0) {
    center = static_cast<size_t>(column - 1);
    if (center >= lineText.size()) {
      center = lineText.size() - 1;
    }
  }

  const size_t half = maxLen / 2;
  size_t windowStart = 0;

  if (center > half) {
    windowStart = center - half;
  } else {
    windowStart = 0;
  }

  if (windowStart + maxLen > lineText.size()) {
    if (lineText.size() > maxLen) {
      windowStart = lineText.size() - maxLen;
    } else {
      windowStart = 0;
    }
  }

  std::string window = lineText.substr(windowStart,
                                       std::min(maxLen, lineText.size() - windowStart));

  bool addLeftEllipsis = windowStart > 0;
  bool addRightEllipsis = windowStart + window.size() < lineText.size();

  std::string result;
  if (addLeftEllipsis) {
    result += "...";
  }
  result += window;
  if (addRightEllipsis) {
    result += "...";
  }

  return wxString::FromUTF8(result.c_str());
}

void ArduinoEditor::ShowUsagesPopup(const std::vector<JumpTarget> &usages) {
  m_lastUsages = usages;
  m_popupMode = PopupMode::Usages;

  // Collect all current editor sources so we can show a short context snippet.
  std::vector<SketchFileBuffer> files;
  GetOwnerFrame()->CollectEditorSources(files);

  // helper
  auto PadAfterLine = [](unsigned line, int width = 5) {
    // Convert to string without padding
    wxString s;
    s.Printf(wxT("%u"), line);

    int pad = width - (int)s.length();
    if (pad < 0)
      pad = 0;

    // Append `pad` spaces
    while (pad-- > 0) {
      s.Append(' ');
    }
    return s;
  };

  wxString list;
  for (size_t i = 0; i < usages.size(); ++i) {
    if (i > 0)
      list << '\n';

    const auto &jt = usages[i];

    wxFileName fn(wxString::FromUTF8(jt.file.c_str()));
    wxString base = fn.GetFullName();

    wxString lineStr = PadAfterLine(jt.line);   // e.g. "74  ", "653 "
    wxString label = base + wxT(":") + lineStr; // → "file.cpp:74  "

    // Try to append a short context snippet taken from the line where the symbol occurs.
    if (!files.empty()) {
      if (const SketchFileBuffer *buf = FindBufferForFile(files, jt.file)) {
        wxString ctx = MakeUsageContext(*buf, jt.line, jt.column, 48);
        ctx.Trim(true).Trim(false);
        if (!ctx.IsEmpty()) {
          label << wxT(" ") << ctx;
        }
      }
    }

    list << label;
  }

  // Make sure no hover calltip is active when we are about to show the usages popup,
  // because CallTipShow/CallTipCancel may interfere with the AutoComp window.
  CancelCallTip();

  // lengthEntered = 0 -> we don't replace anything, we just show the popup
  m_editor->AutoCompShow(0, list);
}

void ArduinoEditor::OnAutoCompSelection(wxStyledTextEvent &evt) {
  auto *stc = static_cast<wxStyledTextCtrl *>(evt.GetEventObject());

  if (m_popupMode != PopupMode::Usages) {
    evt.Skip();
    return;
  }

  int currentIndex = stc->AutoCompGetCurrent();
  APP_DEBUG_LOG("EDIT: usages popup selection index=%d", currentIndex);

  stc->AutoCompCancel();
  m_popupMode = PopupMode::None;

  if (currentIndex < 0 || currentIndex >= (int)m_lastUsages.size()) {
    return;
  }

  if (auto *frame = GetOwnerFrame()) {
    int curLine, curCol;
    GetCurrentCursor(curLine, curCol);
    frame->PushNavLocation(m_filePath, curLine, curCol);

    const JumpTarget &tgt = m_lastUsages[currentIndex];
    frame->HandleGoToLocation(tgt);
  }
}

void ArduinoEditor::OnAutoCompCancelled(wxStyledTextEvent &evt) {
  APP_DEBUG_LOG("EDIT: AUTOCOMP_CANCELLED (popupMode was %d)", (int)m_popupMode);
  m_popupMode = PopupMode::None;

  ArduinoEditorFrame *frame = GetOwnerFrame();
  if (frame && !m_clangSettings.resolveDiagOnlyAfterSave) {
    frame->ScheduleDiagRefresh();
  }

  evt.Skip();
}

bool ArduinoEditor::DoFind(const wxString &what, int flags, bool fromStart) {
  if (!m_editor || what.IsEmpty())
    return false;

  const int textLength = m_editor->GetTextLength();

  // Setting search flags (case, whole word)
  int stcFlags = 0;
  if (flags & wxFR_MATCHCASE)
    stcFlags |= wxSTC_FIND_MATCHCASE;
  if (flags & wxFR_WHOLEWORD)
    stcFlags |= wxSTC_FIND_WHOLEWORD;

  m_editor->SetSearchFlags(stcFlags);

  // Determining direction
  const bool down = (flags & wxFR_DOWN) != 0;

  int startPos;
  if (fromStart) {
    startPos = down ? 0 : textLength;
  } else {
    // we start after the current one (or before the current one when searching upwards)
    if (down)
      startPos = m_editor->GetSelectionEnd();
    else
      startPos = m_editor->GetSelectionStart();
  }

  // Simple strategy: we use FindText forward for searching
  // upwards we do "find the last occurrence before current".
  int foundPos = -1;

  if (down) {
    foundPos = m_editor->FindText(startPos, textLength, what, stcFlags);
    if (foundPos == -1 && !fromStart) {
      // wrap to the beginning
      foundPos = m_editor->FindText(0, textLength, what, stcFlags);
    }
  } else {
    // Searching upwards - by iteration we find the last occurrence before startPos
    int pos = 0;
    int lastFound = -1;
    while (true) {
      int p = m_editor->FindText(pos, textLength, what, stcFlags);
      if (p == -1 || p >= startPos)
        break;
      lastFound = p;
      pos = p + 1;
    }

    if (lastFound == -1 && !fromStart) {
      // wrap: search from the end
      startPos = textLength;
      pos = 0;
      while (true) {
        int p = m_editor->FindText(pos, textLength, what, stcFlags);
        if (p == -1 || p >= startPos)
          break;
        lastFound = p;
        pos = p + 1;
      }
    }

    foundPos = lastFound;
  }

  if (foundPos == -1) {
    wxBell();
    return false;
  }

  // Mark the found text
  m_editor->SetSelection(foundPos, foundPos + what.Length());
  m_editor->EnsureCaretVisible();
  return true;
}

void ArduinoEditor::OnFind(wxFindDialogEvent &event) {
  DoFind(event.GetFindString(), event.GetFlags(), false);
}

void ArduinoEditor::OnFindNext(wxFindDialogEvent &event) {
  DoFind(event.GetFindString(), event.GetFlags(), false);
}

void ArduinoEditor::OnReplace(wxFindDialogEvent &event) {
  if (!m_editor)
    return;

  const wxString findStr = event.GetFindString();
  const wxString replaceStr = event.GetReplaceString();
  const int flags = event.GetFlags();

  // if the current selection = the searched text, replace it
  wxString sel = m_editor->GetSelectedText();
  if (!sel.IsEmpty()) {
    bool equal = sel == findStr;
    if ((flags & wxFR_MATCHCASE) == 0) {
      equal = sel.CmpNoCase(findStr) == 0;
    }

    if (equal) {
      m_editor->ReplaceSelection(replaceStr);
    }
  }

  // find the next occurrence
  DoFind(findStr, flags, false);
}

void ArduinoEditor::OnReplaceAll(wxFindDialogEvent &event) {
  if (!m_editor)
    return;

  const wxString findStr = event.GetFindString();
  const wxString replaceStr = event.GetReplaceString();
  const int flags = event.GetFlags();

  int stcFlags = 0;
  if (flags & wxFR_MATCHCASE)
    stcFlags |= wxSTC_FIND_MATCHCASE;
  if (flags & wxFR_WHOLEWORD)
    stcFlags |= wxSTC_FIND_WHOLEWORD;

  m_editor->SetSearchFlags(stcFlags);

  int pos = 0;
  int textLength = m_editor->GetTextLength();

  m_editor->BeginUndoAction();

  while (true) {
    int foundPos = m_editor->FindText(pos, textLength, findStr, stcFlags);
    if (foundPos == -1)
      break;

    m_editor->SetSelection(foundPos, foundPos + findStr.Length());
    m_editor->ReplaceSelection(replaceStr);

    // move the position after the just inserted text
    pos = foundPos + replaceStr.Length();
    textLength = m_editor->GetTextLength();
  }

  m_editor->EndUndoAction();
}

void ArduinoEditor::RefactorRenameSymbolAtCursor() {
  ArduinoRefactoring r(this);
  r.RefactorRenameSymbolAtCursor();
}

void ArduinoEditor::RefactorIntroduceVariable() {
  ArduinoRefactoring r(this);
  r.RefactorIntroduceVariable();
}

void ArduinoEditor::RefactorInlineVariable() {
  ArduinoRefactoring r(this);
  r.RefactorInlineVariable();
}

void ArduinoEditor::RefactorGenerateFunctionFromCursor() {
  ArduinoRefactoring r(this);
  r.RefactorGenerateFunctionFromCursor();
}

void ArduinoEditor::RefactorCreateDeclarationInHeader() {
  ArduinoRefactoring r(this);
  r.RefactorCreateDeclarationInHeader();
}

void ArduinoEditor::RefactorOrganizeIncludes() {
  ArduinoRefactoring r(this);
  r.RefactorOrganizeIncludes();
}

void ArduinoEditor::RefactorExtractFunction() {
  ArduinoRefactoring r(this);
  r.RefactorExtractFunction();
}

void ArduinoEditor::RefactorFormatSelection() {
  ArduinoRefactoring r(this);
  r.RefactorFormatSelection();
}

void ArduinoEditor::RefactorFormatWholeFile() {
  ArduinoRefactoring r(this);
  r.RefactorFormatWholeFile();
}

int ArduinoEditor::RefactorRenameSymbol(std::vector<JumpTarget> occs,
                                        const std::string &oldName,
                                        const std::string &newName) {
  ArduinoRefactoring r(this);
  return r.RefactorRenameSymbol(occs, oldName, newName);
}

void ArduinoEditor::AiExplainSelection() {
  if (m_aiActions) {
    m_aiActions->ExplainSelection();
  }
}

void ArduinoEditor::AiGenerateDocCommentForSymbol() {
  if (m_aiActions) {
    m_aiActions->GenerateDocCommentForSymbol();
  }
}

void ArduinoEditor::OnFindClose(wxFindDialogEvent &WXUNUSED(event)) {
  if (m_findDlg) {
    m_findDlg->Hide();
  }
}

void ArduinoEditor::OnContextMenu(wxContextMenuEvent &event) {
  m_editor->CallTipCancel();

  // Position in the client coordinates of the editor
  wxPoint pt = event.GetPosition();
  if (pt == wxDefaultPosition) {
    pt = wxGetMousePosition();
    pt = m_editor->ScreenToClient(pt);
  } else {
    pt = m_editor->ScreenToClient(pt);
  }

  wxMenu menu;

  // Standard items with icons
  AddMenuItemWithArt(&menu, wxID_UNDO, _("&Undo\tCtrl+Z"), wxEmptyString, wxAEArt::Undo);
  AddMenuItemWithArt(&menu, wxID_REDO, _("&Redo\tCtrl+Y"), wxEmptyString, wxAEArt::Redo);
  menu.AppendSeparator();

  AddMenuItemWithArt(&menu, wxID_CUT, _("Cu&t\tCtrl+X"), wxEmptyString, wxAEArt::Cut);
  AddMenuItemWithArt(&menu, wxID_COPY, _("&Copy\tCtrl+C"), wxEmptyString, wxAEArt::Copy);
  AddMenuItemWithArt(&menu, wxID_PASTE, _("&Paste\tCtrl+V"), wxEmptyString, wxAEArt::Paste);
  AddMenuItemWithArt(&menu, wxID_DELETE, _("&Delete\tDel"), wxEmptyString, wxAEArt::Delete);
  menu.AppendSeparator();

  AddMenuItemWithArt(&menu, wxID_SELECTALL, _("Select &All\tCtrl+A"), wxEmptyString, wxAEArt::SelectAll);

  menu.AppendSeparator();

  // Find / Replace
  AddMenuItemWithArt(&menu, wxID_FIND, _("&Find...\tCtrl+F"), wxEmptyString, wxAEArt::Find);
  AddMenuItemWithArt(&menu, wxID_REPLACE, _("R&eplace...\tCtrl+Alt+F"), wxEmptyString, wxAEArt::FindReplace);

  menu.AppendSeparator();

  // Navigate menu
  auto *navMenu = new wxMenu;

  AddMenuItemWithArt(navMenu,
                     ID_MENU_NAV_GOTO_DEFINITION,
#ifdef __WXMAC__
                     _("Go to definition (Cmd+Click)"),
#else
                     _("Go to definition\tCtrl+Click"),
#endif
                     _("Jump to the definition of the symbol under the cursor."),
                     wxAEArt::GoToParent);

  navMenu->AppendSeparator();

  AddMenuItemWithArt(navMenu,
                     ID_MENU_NAV_PREV_OCCURRENCE,
                     _("Previous symbol occurrence\tCtrl+Up"),
                     _("Jump to the previous symbol occurrence in the current file."),
                     wxAEArt::GoUp);

  AddMenuItemWithArt(navMenu,
                     ID_MENU_NAV_NEXT_OCCURRENCE,
                     _("Next symbol occurrence\tCtrl+Down"),
                     _("Jump to the next symbol occurrence in the current file."),
                     wxAEArt::GoDown);

  navMenu->AppendSeparator();

  AddMenuItemWithArt(navMenu,
                     ID_MENU_NAV_SYM_OCCURRENCE,
                     _("Show symbol occurrences\tShift+F12"),
                     _("Shows list of all project-wide symbol occurrences."),
                     wxAEArt::FindAll);

  menu.AppendSubMenu(navMenu, _("Navigate"));

  // Refactor submenu
  auto *refMenu = new wxMenu;
  AddMenuItemWithArt(refMenu,
                     ID_MENU_REFACTOR_RENAME,
                     _("Rename symbol..."),
                     _("Rename the selected symbol across the project."),
                     wxAEArt::Edit);

  AddMenuItemWithArt(refMenu,
                     ID_MENU_REFACTOR_INTRODUCE_VAR,
                     _("Introduce variable..."),
                     _("Extract the selected expression into a new local variable."),
                     wxAEArt::ListView);

  AddMenuItemWithArt(refMenu,
                     ID_MENU_REFACTOR_INLINE_VAR,
                     _("Inline variable"),
                     _("Replace all usages of the variable with its initializer."),
                     wxAEArt::ListView);

  AddMenuItemWithArt(refMenu,
                     ID_MENU_REFACTOR_GENERATE_FUNC,
                     _("Generate function implementation..."),
                     _("Insert an implementation stub for the selected function."),
                     wxAEArt::Plus);

  AddMenuItemWithArt(refMenu,
                     ID_MENU_REFACTOR_CREATE_DECL,
                     _("Create declaration in header..."),
                     _("Add a missing function declaration to the appropriate header file."),
                     wxAEArt::Plus);

  AddMenuItemWithArt(refMenu,
                     ID_MENU_REFACTOR_ORG_INCLUDES,
                     _("Organize includes..."),
                     _("Detect and clean up unused or duplicate #include directives."),
                     wxAEArt::Minus);

  AddMenuItemWithArt(refMenu,
                     ID_MENU_REFACTOR_EXTRACT_FUNC,
                     _("Extract function..."),
                     _("Create a new function from the selected block of code."),
                     wxAEArt::GoToParent);

  refMenu->AppendSeparator();

  AddMenuItemWithArt(refMenu,
                     ID_MENU_REFACTOR_FORMAT_SELECTION,
                     _("Format Selection"),
                     _("Format the selected text using clang-format."),
                     wxAEArt::SourceFormat);

  AddMenuItemWithArt(refMenu,
                     ID_MENU_REFACTOR_FORMAT_WHOLE_FILE,
                     _("Format File"),
                     _("Format the current file using clang-format."),
                     wxAEArt::SourceFormat);

  menu.AppendSubMenu(refMenu, _("Refactor"));

  // --- AI submenu ---
  wxMenu *aiMenu = nullptr;
  if (m_aiSettings.enabled) {
    aiMenu = new wxMenu;

    AddMenuItemWithArt(aiMenu,
                       ID_MENU_AI_EXPLAIN_SELECTION,
                       _("Explain selection"),
                       _("AI explains the meaning and behavior of the selected code."),
                       wxAEArt::Question);
    aiMenu->AppendSeparator();
    AddMenuItemWithArt(aiMenu,
                       ID_MENU_AI_GENERATE_COMMENT,
                       _("Generate documentation for Symbol"),
                       _("AI generates a documentation comment for the symbol under the cursor."),
                       wxAEArt::Plus);

    AddMenuItemWithArt(aiMenu,
                       ID_MENU_AI_GENERATE_DOC_FUNC,
                       _("Auto-Document All Functions in File"),
                       _("AI generates documentation comments for all functions in this file."),
                       wxAEArt::Plus);

    AddMenuItemWithArt(aiMenu,
                       ID_MENU_AI_GENERATE_DOC_CLASS,
                       _("Document All Methods in Class"),
                       _("AI generates documentation for all methods of the selected class."),
                       wxAEArt::Plus);
    aiMenu->AppendSeparator();
    AddMenuItemWithArt(aiMenu,
                       ID_MENU_AI_OPTIMIZE_FUNC_METHOD,
                       _("Optimize function or method"),
                       _("AI optimizes function or method."),
                       wxAEArt::Edit);

    menu.AppendSubMenu(aiMenu, _("AI"));
  }

  // Enable/disable according to the editor state
  menu.Enable(wxID_UNDO, m_editor->CanUndo());
  menu.Enable(wxID_REDO, m_editor->CanRedo());

  bool hasSel = m_editor->GetSelectionStart() != m_editor->GetSelectionEnd();
  menu.Enable(wxID_CUT, hasSel);
  menu.Enable(wxID_COPY, hasSel);
  menu.Enable(wxID_DELETE, hasSel);

  bool readOnly = m_editor->GetReadOnly();

  if (readOnly) {
    // in r/o mode it makes no sense to change anything
    menu.Enable(wxID_UNDO, false);
    menu.Enable(wxID_REDO, false);
    menu.Enable(wxID_CUT, false);
    menu.Enable(wxID_PASTE, false);
    menu.Enable(wxID_DELETE, false);
    menu.Enable(wxID_REPLACE, false);
    refMenu->Enable(ID_MENU_REFACTOR_RENAME, false);
    refMenu->Enable(ID_MENU_REFACTOR_INTRODUCE_VAR, false);
    refMenu->Enable(ID_MENU_REFACTOR_INLINE_VAR, false);
    refMenu->Enable(ID_MENU_REFACTOR_GENERATE_FUNC, false);
    refMenu->Enable(ID_MENU_REFACTOR_CREATE_DECL, false);
    refMenu->Enable(ID_MENU_REFACTOR_ORG_INCLUDES, false);
    refMenu->Enable(ID_MENU_REFACTOR_EXTRACT_FUNC, false);
    refMenu->Enable(ID_MENU_REFACTOR_FORMAT_SELECTION, false);
    refMenu->Enable(ID_MENU_REFACTOR_FORMAT_WHOLE_FILE, false);
  } else {
    refMenu->Enable(ID_MENU_REFACTOR_RENAME, true);
    refMenu->Enable(ID_MENU_REFACTOR_INTRODUCE_VAR, hasSel);
    refMenu->Enable(ID_MENU_REFACTOR_INLINE_VAR, true);
    refMenu->Enable(ID_MENU_REFACTOR_GENERATE_FUNC, true);
    refMenu->Enable(ID_MENU_REFACTOR_CREATE_DECL, true);
    refMenu->Enable(ID_MENU_REFACTOR_ORG_INCLUDES, true);
    refMenu->Enable(ID_MENU_REFACTOR_EXTRACT_FUNC, hasSel);
    refMenu->Enable(ID_MENU_REFACTOR_FORMAT_SELECTION, hasSel);
    refMenu->Enable(ID_MENU_REFACTOR_FORMAT_WHOLE_FILE, true);
  }

  if (aiMenu) {
    aiMenu->Enable(ID_MENU_AI_EXPLAIN_SELECTION, hasSel);
    aiMenu->Enable(ID_MENU_AI_GENERATE_COMMENT, !readOnly);
    aiMenu->Enable(ID_MENU_AI_GENERATE_DOC_FUNC, !readOnly);
    aiMenu->Enable(ID_MENU_AI_GENERATE_DOC_CLASS, !readOnly);
    aiMenu->Enable(ID_MENU_AI_OPTIMIZE_FUNC_METHOD, !readOnly);
  }

  Bind(wxEVT_MENU, &ArduinoEditor::OnPopupMenu, this);

  ArduinoEditorFrame *frame = GetOwnerFrame();
  wxString oldTxt = frame->GetStatusBar()->GetStatusText();

  menu.Bind(wxEVT_MENU_HIGHLIGHT, &ArduinoEditor::OnPopupMenuHighlight, this); // for statusbar help

  m_editor->PopupMenu(&menu, pt);

  // After using the menu, we unbind the handler to prevent accumulating binds
  menu.Unbind(wxEVT_MENU_HIGHLIGHT, &ArduinoEditor::OnPopupMenuHighlight, this);
  Unbind(wxEVT_MENU, &ArduinoEditor::OnPopupMenu, this);

  // return old statusbar text
  frame->GetStatusBar()->SetStatusText(oldTxt);
}

void ArduinoEditor::OnPopupMenuHighlight(wxMenuEvent &event) {
  wxString help;

  if (auto *menu = wxDynamicCast(event.GetEventObject(), wxMenu)) {
    wxMenuItem *item = menu->FindItem(event.GetId());
    if (item) {
      help = item->GetHelp();
    } else {
      help.Clear();
    }
  }

  GetOwnerFrame()->GetStatusBar()->SetStatusText(help);
}

void ArduinoEditor::OnPopupMenu(wxCommandEvent &event) {
  bool readOnly = m_editor->GetReadOnly();

  switch (event.GetId()) {
    case wxID_UNDO:
      if (!readOnly && m_editor->CanUndo())
        m_editor->Undo();
      break;

    case wxID_REDO:
      if (!readOnly && m_editor->CanRedo())
        m_editor->Redo();
      break;

    case wxID_CUT:
      if (!readOnly)
        m_editor->Cut();
      break;

    case wxID_PASTE:
      if (!readOnly)
        m_editor->Paste();
      break;

    case wxID_DELETE:
      if (!readOnly)
        m_editor->Clear();
      break;

    case wxID_SELECTALL:
      m_editor->SelectAll();
      break;

    case wxID_FIND:
      OpenFindDialog(/*replace=*/false);
      break;

    case wxID_REPLACE:
      if (!readOnly)
        OpenFindDialog(/*replace=*/true);
      break;

    case ID_MENU_REFACTOR_RENAME:
      if (!readOnly)
        RefactorRenameSymbolAtCursor();
      break;

    case ID_MENU_REFACTOR_INTRODUCE_VAR:
      if (!readOnly)
        RefactorIntroduceVariable();
      break;

    case ID_MENU_REFACTOR_GENERATE_FUNC:
      if (!readOnly)
        RefactorGenerateFunctionFromCursor();
      break;

    case ID_MENU_REFACTOR_CREATE_DECL:
      if (!readOnly)
        RefactorCreateDeclarationInHeader();
      break;

    case ID_MENU_REFACTOR_ORG_INCLUDES:
      if (!readOnly)
        RefactorOrganizeIncludes();
      break;

    case ID_MENU_REFACTOR_EXTRACT_FUNC:
      if (!readOnly)
        RefactorExtractFunction();
      break;

    case ID_MENU_REFACTOR_FORMAT_SELECTION:
      if (!readOnly)
        RefactorFormatSelection();
      break;

    case ID_MENU_REFACTOR_FORMAT_WHOLE_FILE:
      if (!readOnly)
        RefactorFormatWholeFile();
      break;

    case ID_MENU_REFACTOR_INLINE_VAR:
      if (!readOnly)
        RefactorInlineVariable();
      break;

    case ID_MENU_AI_EXPLAIN_SELECTION:
      AiExplainSelection();
      break;

    case ID_MENU_AI_GENERATE_COMMENT:
      if (!readOnly)
        AiGenerateDocCommentForSymbol();
      break;

    case ID_MENU_AI_GENERATE_DOC_FUNC:
      if (!readOnly)
        AiGenerateDocCommentsForCurrentFile();
      break;

    case ID_MENU_AI_GENERATE_DOC_CLASS:
      if (!readOnly)
        AiGenerateDocCommentsForCurrentClass();
      break;

    case ID_MENU_AI_OPTIMIZE_FUNC_METHOD:
      if (!readOnly)
        AiOptimizeFunctionOrMethod();
      break;

    case ID_MENU_NAV_GOTO_DEFINITION:
      GotoSymbolDefinition();
      break;

    case ID_MENU_NAV_PREV_OCCURRENCE:
      NavigateSymbolOccurrence(false);
      break;

    case ID_MENU_NAV_NEXT_OCCURRENCE:
      NavigateSymbolOccurrence(true);
      break;

    case ID_MENU_NAV_SYM_OCCURRENCE:
      FindSymbolUsagesAtCursor();
      break;

    default:
      event.Skip();
      break;
  }
}

void ArduinoEditor::ScheduleAutoCompletion() {
  if (m_clangSettings.completionMode != always)
    return;

  // caret position at the moment of scheduling
  m_scheduledPos = m_editor->GetCurrentPos();

  // one-shot timer, after the last character
  m_completionTimer.Stop();

  // If the completion has already been invoked and the corresponding options have been loaded,
  // we will only make a short pause before re-evaluating.
  m_completionTimer.Start(m_editor->AutoCompActive() ? 250 : m_clangSettings.autocompletionDelay, wxTIMER_ONE_SHOT);
}

void ArduinoEditor::OnCompletionTimer(wxTimerEvent &WXUNUSED(evt)) {
  if (m_scheduledPos != m_editor->GetCurrentPos()) {
    return;
  }

  ShowAutoCompletion();
}

void ArduinoEditor::OnCompletionReady(wxThreadEvent &event) {
  uint64_t seq = (uint64_t)event.GetInt();

  // if we have a newer request in metadata, we discard this one
  if (seq != m_completionMetadata.m_pendingRequestId) {
    return;
  }

  auto completions = event.GetPayload<std::vector<CompletionItem>>();

  int wordStart = (int)event.GetExtraLong();
  int lengthEntered = wxAtoi(event.GetString());

  if (completions.empty()) {
    if (m_editor->AutoCompActive()) {
      m_editor->AutoCompCancel();
    }
    return;
  }

  m_completionMetadata.m_lastCompletions = completions;
  m_completionMetadata.m_lastWordStart = wordStart;
  m_completionMetadata.m_lastLengthEntered = lengthEntered;

  wxString completionList = BuildCompletionList(completions);

  if (!completionList.IsEmpty()) {
    m_editor->AutoCompShow(lengthEntered, completionList);
  }
}

void ArduinoEditor::ApplyBaseSettings(const EditorSettings &s) {
  m_editor->IndicatorSetStyle(STC_FLASH_INDICATOR, wxSTC_INDIC_ROUNDBOX);
  m_editor->IndicatorSetForeground(STC_FLASH_INDICATOR, s.GetColors().warning);
  m_editor->IndicatorSetAlpha(STC_FLASH_INDICATOR, 80);
  m_editor->IndicatorSetOutlineAlpha(STC_FLASH_INDICATOR, 200);

  m_editor->IndicatorSetStyle(STC_INDIC_DIAG_ERROR, wxSTC_INDIC_SQUIGGLE);
  m_editor->IndicatorSetForeground(STC_INDIC_DIAG_ERROR, s.GetColors().error);
  m_editor->IndicatorSetUnder(STC_INDIC_DIAG_ERROR, true);

  m_editor->IndicatorSetStyle(STC_INDIC_DIAG_WARNING, wxSTC_INDIC_SQUIGGLE);
  m_editor->IndicatorSetForeground(STC_INDIC_DIAG_WARNING, s.GetColors().warning);
  m_editor->IndicatorSetUnder(STC_INDIC_DIAG_WARNING, true);

  // --- indicator pro symbol highlighting ---
  m_editor->IndicatorSetStyle(STC_INDIC_SYMBOL_OCCURRENCE, wxSTC_INDIC_ROUNDBOX);
  m_editor->IndicatorSetForeground(STC_INDIC_SYMBOL_OCCURRENCE, s.GetColors().symbolHighlight);
  m_editor->IndicatorSetAlpha(STC_INDIC_SYMBOL_OCCURRENCE, 80);
  m_editor->IndicatorSetOutlineAlpha(STC_INDIC_SYMBOL_OCCURRENCE, 200);

  if (m_symbolOverview) {
    m_symbolOverview->RegisterMarker(ESM_SYMBOL, s.GetColors().symbolHighlight);
    m_symbolOverview->RegisterMarker(ESM_ERROR, s.GetColors().error);
    m_symbolOverview->RegisterMarker(ESM_WARNING, s.GetColors().warning);
  }

  m_autoIndentEnabled = s.autoIndent;
  m_symbolHighlightEnabled = s.highlightSymbols;
}

void ArduinoEditor::ApplySettings(const EditorSettings &s) {
  // Common settings
  ApplyStyledTextCtrlSettings(m_editor, s);

  ApplyBaseSettings(s);

  m_normalBgColor = m_editor->StyleGetBackground(wxSTC_STYLE_DEFAULT);
  m_hasReadOnlyBg = false;
  m_highlightMatchingBraces = s.highlightMatchingBraces;
  m_displayHoverInfo = s.displayHoverInfo;

  UpdateReadOnlyColors();
}

void ArduinoEditor::ApplySettings(const ClangSettings &settings) {
  m_clangSettings = settings;
}

void ArduinoEditor::ApplySettings(const AiSettings &settings) {
  m_aiSettings = settings;
  m_aiActions->SetFullInfoRequest(settings.fullInfoRequest);
  m_aiActions->SetFloatingWindow(settings.floatingWindow);
}

void ArduinoEditor::ApplyBoardChange() {
  // Currently nothing to do
}

int ArduinoEditor::ModalMsgDialog(const wxString &message, const wxString &caption, int styles) {
  wxRichMessageDialog dlg(this, message, caption, styles);
  return dlg.ShowModal();
}

bool ArduinoEditor::Save() {
  if (m_editor->GetReadOnly()) {
    ModalMsgDialog(_("File is opened as read-only. Enable editing first if you want to save changes."),
                   _("Read-only file"));
    return false;
  }

  wxString fn = wxString::FromUTF8(GetFilePath());
  if (!m_editor->SaveFile(fn)) {
    ModalMsgDialog(wxString::Format(_("Unable to save file %s."), fn));
    return false;
  }

  m_editor->SetSavePoint();
  return true;
}

bool ArduinoEditor::IsModified() {
  return m_editor->IsModified();
}

bool ArduinoEditor::IsAutoCompActive() {
  return m_editor->AutoCompActive();
}

#ifndef __WXMAC__
wxBitmap ArduinoEditor::MakeCircleBitmap(int diameter,
                                         const wxColour &fill,
                                         const wxColour &border,
                                         int borderWidth,
                                         int offsetX,
                                         int offsetY) {
  if (diameter <= 0)
    diameter = 16;

  int padLeft = offsetX > 0 ? offsetX : 0;
  int padRight = offsetX < 0 ? -offsetX : 0;
  int padTop = offsetY > 0 ? offsetY : 0;
  int padBottom = offsetY < 0 ? -offsetY : 0;

  int canvasW = diameter + padLeft + padRight;
  int canvasH = diameter + padTop + padBottom;

  wxBitmap bmp(canvasW, canvasH, wxBITMAP_SCREEN_DEPTH);

  wxColour maskCol(255, 0, 255);

  wxMemoryDC dc(bmp);
  dc.SetBackground(wxBrush(maskCol));
  dc.Clear();

  int radius = diameter / 2;
  int cx = padLeft + radius;
  int cy = padTop + radius;

  int r = radius - borderWidth;
  if (r < 0)
    r = 0;

  if (borderWidth > 0)
    dc.SetPen(wxPen(border, borderWidth));
  else
    dc.SetPen(*wxTRANSPARENT_PEN);

  dc.SetBrush(wxBrush(fill));
  dc.DrawCircle(cx, cy, r);

  dc.SelectObject(wxNullBitmap);

  wxImage img = bmp.ConvertToImage();
  img.SetMaskColour(maskCol.Red(), maskCol.Green(), maskCol.Blue());

  return wxBitmap(img);
}
#else
wxBitmap ArduinoEditor::MakeCircleBitmap(int diameter,
                                         const wxColour &fill,
                                         const wxColour &border,
                                         int borderWidth,
                                         int offsetX,
                                         int offsetY) {
  if (diameter <= 0)
    diameter = 16;

  // Positive offsets = we inflate from top/left
  // Negative offsets = inflate from the bottom/right
  int padLeft = offsetX > 0 ? offsetX : 0;
  int padRight = offsetX < 0 ? -offsetX : 0;
  int padTop = offsetY > 0 ? offsetY : 0;
  int padBottom = offsetY < 0 ? -offsetY : 0;

  int canvasW = diameter + padLeft + padRight;
  int canvasH = diameter + padTop + padBottom;

  wxBitmap bmp(canvasW, canvasH, wxBITMAP_SCREEN_DEPTH);

  // clear the entire bitmap -> fully transparent
  {
    wxAlphaPixelData data(bmp);
    if (data) {
      wxAlphaPixelData::Iterator p(data);
      for (int y = 0; y < canvasH; ++y) {
        wxAlphaPixelData::Iterator rowStart = p;
        for (int x = 0; x < canvasW; ++x, ++p) {
          p.Red() = 0;
          p.Green() = 0;
          p.Blue() = 0;
          p.Alpha() = 0;
        }
        p = rowStart;
        p.OffsetY(data, 1);
      }
    }
  }

  wxMemoryDC dc(bmp);
  dc.SetBackground(*wxTRANSPARENT_BRUSH);
  dc.Clear();

  int radius = diameter / 2;

  // centering the wheel inside the canvas + padding
  int centerX = padLeft + radius;
  int centerY = padTop + radius;

  if (borderWidth > 0)
    dc.SetPen(wxPen(border, borderWidth));
  else
    dc.SetPen(*wxTRANSPARENT_PEN);

  dc.SetBrush(wxBrush(fill));
  dc.DrawCircle(centerX, centerY, radius - borderWidth);

  dc.SelectObject(wxNullBitmap);
  return bmp;
}
#endif

void ArduinoEditor::ShowAutoCompletion() {
  if (completion) {
    APP_DEBUG_LOG("EDIT: ShowAutoCompletion()");
    m_popupMode = PopupMode::Completion;
    completion->ShowAutoCompletionAsync(m_editor, m_filePath, m_completionMetadata, this);
  }
}

void ArduinoEditor::InitCodeCompletionIcons() {
  const int size = 14;

  // Icon IDs - must match what is then "?<id>" in the completion list
  enum {
    IMG_FUNC = 1,
    IMG_VAR = 2,
    IMG_CLASS = 3,
    IMG_MACRO = 4
  };

  // TODO: it should be linked to EditorSettings
  wxBitmap funcBmp = MakeCircleBitmap(size, wxColour(80, 120, 255));   // blue
  wxBitmap varBmp = MakeCircleBitmap(size, wxColour(100, 200, 120));   // green
  wxBitmap classBmp = MakeCircleBitmap(size, wxColour(255, 180, 80));  // orange
  wxBitmap macroBmp = MakeCircleBitmap(size, wxColour(180, 120, 200)); // purple

  m_editor->RegisterImage(IMG_FUNC, funcBmp);
  m_editor->RegisterImage(IMG_VAR, varBmp);
  m_editor->RegisterImage(IMG_CLASS, classBmp);
  m_editor->RegisterImage(IMG_MACRO, macroBmp);

  // just to be sure - the default is '?', but let's make it explicit
  m_editor->AutoCompSetTypeSeparator('?');
}

void ArduinoEditor::AutoIndent(char ch) {
  // Helper: return editor's indent step in spaces (fallbacks included)
  auto GetIndentStep = [](wxStyledTextCtrl *stc) -> int {
    int step = stc->GetIndent();
    if (step <= 0)
      step = stc->GetTabWidth();
    if (step <= 0)
      step = 2; // last resort
    return step;
  };

  // Helper: strip trailing whitespace and trailing // comment (simple heuristic)
  auto StripLineTail = [](const wxString &line) -> wxString {
    wxString s = line;

    // Drop trailing newline chars if present
    while (!s.IsEmpty() && (s.Last() == '\n' || s.Last() == '\r'))
      s.RemoveLast();

    // Remove // comment (naive, but works well for typical code)
    int cpos = s.Find(wxT("//"));
    if (cpos != wxNOT_FOUND) {
      s = s.Left(cpos);
    }

    s.Trim(true); // right trim
    return s;
  };

  // Helper: does line (after trimming comment/ws) end with '{' ?
  auto LineEndsWithOpenBrace = [&](wxStyledTextCtrl *stc, int line) -> bool {
    if (line < 0)
      return false;
    wxString s = StripLineTail(stc->GetLine(line));
    if (s.IsEmpty())
      return false;
    return s.Last() == '{';
  };

  // Helper: is '}' the first non-space/tab char on the given line?
  auto LineStartsWithCloseBrace = [](wxStyledTextCtrl *stc, int line) -> bool {
    wxString s = stc->GetLine(line);
    s.Trim(false); // left trim
    if (s.IsEmpty())
      return false;
    return s[0] == '}';
  };

  // 1) Enter: copy previous indentation; if prev line ends with '{', add one level.
  if (ch == '\n' || ch == '\r') {
    int currentLine = m_editor->GetCurrentLine();
    if (currentLine > 0) {
      int prevIndent = m_editor->GetLineIndentation(currentLine - 1);
      int indent = std::max(0, prevIndent);

      // If we just created a new line and it starts with '}', don't auto-indent it deeper.
      const bool curStartsWithClose = LineStartsWithCloseBrace(m_editor, currentLine);

      if (!curStartsWithClose && LineEndsWithOpenBrace(m_editor, currentLine - 1)) {
        indent += GetIndentStep(m_editor);
      }

      m_editor->SetLineIndentation(currentLine, indent);
      m_editor->GotoPos(m_editor->GetLineIndentPosition(currentLine));
    }
  }

  // 2) Typed '}' : if it's the first non-whitespace on the line -> dedent to matching '{'
  if (ch == '}') {
    int posAfter = m_editor->GetCurrentPos();
    int bracePos = posAfter - 1;
    if (bracePos >= 0) {
      int line = m_editor->LineFromPosition(bracePos);
      int lineStart = m_editor->PositionFromLine(line);

      // Check that before '}' there is only whitespace
      bool onlyWsBefore = true;
      for (int p = lineStart; p < bracePos; ++p) {
        int c = m_editor->GetCharAt(p);
        if (c != ' ' && c != '\t') {
          onlyWsBefore = false;
          break;
        }
      }

      if (onlyWsBefore) {
        int matchPos = m_editor->BraceMatch(bracePos);
        if (matchPos >= 0) {
          int matchLine = m_editor->LineFromPosition(matchPos);
          int targetIndent = m_editor->GetLineIndentation(matchLine);
          if (targetIndent < 0)
            targetIndent = 0;

          m_editor->SetLineIndentation(line, targetIndent);

          // Place caret after the '}' (which is now at the indentation position)
          int newBracePos = m_editor->GetLineIndentPosition(line);
          m_editor->GotoPos(newBracePos + 1);
        } else {
          // Fallback: unindent by one level if no matching brace found
          int curIndent = m_editor->GetLineIndentation(line);
          int step = GetIndentStep(m_editor);
          int targetIndent = std::max(0, curIndent - step);
          m_editor->SetLineIndentation(line, targetIndent);
          int newBracePos = m_editor->GetLineIndentPosition(line);
          m_editor->GotoPos(newBracePos + 1);
        }
      }
    }
  }
}

void ArduinoEditor::OnCharAdded(wxStyledTextEvent &event) {
  char ch = event.GetKey();

  if (m_autoIndentEnabled) {
    AutoIndent(ch);
  }

  if (m_clangSettings.completionMode == always) {
    const int currentPos = m_editor->GetCurrentPos();

    auto isIdentChar = [](char c) {
      return wxIsalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    const bool isArrow =
        (ch == '>' && currentPos >= 2 &&
         m_editor->GetCharAt(currentPos - 2) == '-');

    const bool isScope =
        (ch == ':' && currentPos >= 2 &&
         m_editor->GetCharAt(currentPos - 2) == ':');

    // Trigger after '.', '->', '::'
    if (ch == '.' || isArrow || isScope) {
      ShowAutoCompletion();
    } else if (isIdentChar(ch)) {
      int wordStart = m_editor->WordStartPosition(currentPos, true);
      if (currentPos - wordStart >= 2) {
        ScheduleAutoCompletion();
      }
    } else {
      // Anything that cannot continue an identifier/prefix -> close completion
      m_completionTimer.Stop();
      if (m_editor->AutoCompActive()) {
        m_editor->AutoCompCancel();
      }
    }
  }

  event.Skip();
}

void ArduinoEditor::OnChanged(wxStyledTextEvent &event) {
  const int mod = event.GetModificationType();

  // We are only interested in changing the text, not the style/fold/etc.
  if (!(mod & (wxSTC_MOD_INSERTTEXT |
               wxSTC_MOD_DELETETEXT |
               wxSTC_PERFORMED_UNDO |
               wxSTC_PERFORMED_REDO))) {
    event.Skip();
    return;
  }

  ArduinoEditorFrame *frame = GetOwnerFrame();
  if (frame && !m_clangSettings.resolveDiagOnlyAfterSave) {
    frame->ScheduleDiagRefresh();
  }

  event.Skip();
}

void ArduinoEditor::OnEditorUpdateUI(wxStyledTextEvent &event) {
  event.Skip();

  int pos = m_editor->GetCurrentPos();

  if (m_highlightMatchingBraces) {
    int ch = m_editor->GetCharAt(pos - 1);

    if (ch == '(' || ch == ')' ||
        ch == '{' || ch == '}' ||
        ch == '[' || ch == ']') {
      int match = m_editor->BraceMatch(pos - 1);

      m_editor->BraceHighlight(wxSTC_INVALID_POSITION, wxSTC_INVALID_POSITION);
      m_editor->BraceBadLight(wxSTC_INVALID_POSITION);

      if (match >= 0)
        m_editor->BraceHighlight(pos - 1, match);
      else
        m_editor->BraceBadLight(pos - 1);
    } else {
      m_editor->BraceHighlight(wxSTC_INVALID_POSITION, wxSTC_INVALID_POSITION);
      m_editor->BraceBadLight(wxSTC_INVALID_POSITION);
    }
  }

  if (!completion) {
    return;
  }

  if (pos == m_lastSymbolPos) {
    return;
  }

  m_lastSymbolPos = pos;

  if (m_symbolTimer.IsRunning()) {
    m_symbolTimer.Stop();
  }

  if (!m_editor->AutoCompActive()) {
    m_symbolTimer.Start(500, wxTIMER_ONE_SHOT);
  }

  if (m_symbolOverview) {
    m_symbolOverview->Refresh();
  }
}

void ArduinoEditor::OnSymbolTimer(wxTimerEvent &WXUNUSED(event)) {
  if (!completion) {
    return;
  }

  const int textLen = m_editor->GetTextLength();

  // caret moves, ignore (but also clear cached positions)
  const int curPos = m_editor->GetCurrentPos();
  if (curPos != m_lastSymbolPos) {
    m_symbolOccPositions.clear();
    return;
  }

  if (!m_symbolHighlightEnabled || textLen == 0) {
    ClearSymbolOccurrences();
    return;
  }

  // ---- Fast "meaningful symbol under caret?" filter ----
  AeIdentSpan id;
  if (!AEExtractIdentifierAtOrBeforeCaret(m_editor, curPos, id, textLen)) {
    ClearSymbolOccurrences();
    return;
  }

  const int wordStart = id.start;
  const int symPos = id.symPos;

  // ---- Reuse check: if caret is inside an already highlighted occurrence, don't re-query ----
  // m_symbolOccPositions stores *start positions* of occurrences. So reuse should be based on current wordStart.
  if (!m_symbolOccPositions.empty()) {
    auto it = std::find(m_symbolOccPositions.begin(), m_symbolOccPositions.end(), wordStart);
    if (it != m_symbolOccPositions.end()) {
      APP_DEBUG_LOG("EDIT: SymbolHighlight: caret in existing symbol -> reuse, no async query");
      return;
    }
  }

  // Build 1-based (line, column) from the symbol position (symPos), not necessarily caret pos.
  const int line = m_editor->LineFromPosition(symPos) + 1;
  const int column = m_editor->GetColumn(symPos) + 1;

  std::string code = wxToStd(m_editor->GetText());

  // new request id
  uint64_t seq = ++m_symbolHighlightSeq;
  m_symbolHighlightPendingSeq = seq;

  completion->FindSymbolOccurrencesAsync(m_filePath,
                                         code,
                                         line,
                                         column,
                                         /*onlyFromSketch=*/true,
                                         this,
                                         seq);
}

void ArduinoEditor::ClearSymbolOccurrences() {
  const int textLen = m_editor->GetTextLength();

  m_editor->SetIndicatorCurrent(STC_INDIC_SYMBOL_OCCURRENCE);
  m_editor->IndicatorClearRange(0, textLen);

  if (m_symbolOverview) {
    m_symbolOverview->ClearMarker(ESM_SYMBOL);
  }

  m_symbolOccPositions.clear();
}

void ArduinoEditor::OnSymbolOccurrencesReady(wxThreadEvent &event) {
  uint64_t seq = (uint64_t)event.GetInt();

  // We have a newer request - this is an old result, ignore it
  if (seq != m_symbolHighlightPendingSeq) {
    return;
  }

  auto occurrences = event.GetPayload<std::vector<JumpTarget>>();

  const int textLen = m_editor->GetTextLength();

  // If caret moved since we requested this async query, ignore the result.
  const int curPos = m_editor->GetCurrentPos();
  if (curPos != m_lastSymbolPos) {
    APP_DEBUG_LOG("EDIT: SymbolHighlight: async result but caret moved -> ignore");
    return;
  }

  // Same identifier-under-caret validation as in OnSymbolTimer.
  AeIdentSpan caretId;
  if (!AEExtractIdentifierAtOrBeforeCaret(m_editor, curPos, caretId, textLen)) {
    ClearSymbolOccurrences();
    APP_DEBUG_LOG("EDIT: SymbolHighlight: async result but caret not on identifier -> clear");
    return;
  }

  // If the editor is empty or highlight is turned off -> we just delete any old highlights
  if (!m_symbolHighlightEnabled || textLen == 0) {
    ClearSymbolOccurrences();
    APP_DEBUG_LOG("EDIT: SymbolHighlight: disabled or empty editor");
    return;
  }

  // If there are no occurrences, we just clean it up too
  if (occurrences.empty()) {
    ClearSymbolOccurrences();
    APP_DEBUG_LOG("EDIT: SymbolHighlight: no symbol occurrences for async result");
    return;
  }

  APP_DEBUG_LOG("EDIT: SymbolHighlight: %zu occurrences (async)",
                occurrences.size());

  int lineCount = m_editor->GetLineCount();

  struct SymbolRange {
    int start;
    int len;
    int line;
  };

  std::vector<SymbolRange> ranges;
  std::vector<int> newOccPositions;
  std::vector<int> overviewLines;

  // We will prepare new ranges and positions, but we are NOT DRAWING ANYTHING yet.
  for (size_t i = 0; i < occurrences.size(); ++i) {
    const auto &jt = occurrences[i];

    if (jt.file != m_filePath) {
      continue; // skip foreign files
    }

    unsigned jLine = jt.line;
    unsigned jCol = jt.column;

    int ln = (jLine > 0) ? (int)jLine - 1 : 0;
    if (ln < 0)
      ln = 0;
    if (ln >= lineCount)
      ln = lineCount - 1;
    if (ln < 0 || ln >= lineCount)
      continue;

    int lineStart = m_editor->PositionFromLine(ln);
    if (lineStart < 0) {
      continue;
    }

    int col0 = (jCol > 0) ? (int)jCol - 1 : 0;
    if (col0 < 0)
      col0 = 0;

    int pos = lineStart + col0;
    if (pos < 0)
      pos = 0;
    if (pos >= textLen)
      pos = textLen - 1;

    int start = pos;
    int len = AEIdentifierLengthFrom(m_editor, start, textLen);
    if (len <= 0) {
      continue;
    }

    ranges.push_back(SymbolRange{start, len, ln});
    newOccPositions.push_back(start);
    overviewLines.push_back(ln);

    APP_DEBUG_LOG("  [%zu] candidate highlight %s:%u:%u (pos=%d, len=%d)",
                  i,
                  jt.file.c_str(),
                  (unsigned)jt.line,
                  (unsigned)jt.column,
                  start,
                  len);
  }

  // It could be that all occurrences were in different files
  if (ranges.empty()) {
    ClearSymbolOccurrences();
    APP_DEBUG_LOG("EDIT: SymbolHighlight: no occurrences in this file");
    return;
  }

  // We normalize the set of positions.
  std::sort(newOccPositions.begin(), newOccPositions.end());
  newOccPositions.erase(std::unique(newOccPositions.begin(), newOccPositions.end()),
                        newOccPositions.end());

  // If the set of positions has not changed, we leave the existing highlight as is.
  if (newOccPositions == m_symbolOccPositions) {
    APP_DEBUG_LOG("EDIT: SymbolHighlight: occurrences unchanged -> skip redraw");
    return;
  }

  // The set of occurrences has changed -> we delete the old highlight and draw a new one.
  m_editor->SetIndicatorCurrent(STC_INDIC_SYMBOL_OCCURRENCE);
  m_editor->IndicatorClearRange(0, textLen);
  if (m_symbolOverview) {
    m_symbolOverview->ClearMarker(ESM_SYMBOL);
  }

  for (const auto &r : ranges) {
    m_editor->SetIndicatorCurrent(STC_INDIC_SYMBOL_OCCURRENCE);
    m_editor->IndicatorFillRange(r.start, r.len);
  }

  if (m_symbolOverview) {
    m_symbolOverview->SetMarkers(ESM_SYMBOL, overviewLines);
  }

  // save positions for Alt+Up/Down navigation
  m_symbolOccPositions = std::move(newOccPositions);
}

void ArduinoEditor::OnAutoCompCompleted(wxStyledTextEvent &evt) {
  auto *m_editor = static_cast<wxStyledTextCtrl *>(evt.GetEventObject());

  // ----- USAGES mode -----
  if (m_popupMode == PopupMode::Usages) {
    APP_DEBUG_LOG("EDIT: AUTOCOMP_COMPLETED in usages mode -> ignored");
    return;
  }

  m_popupMode = PopupMode::None;

  wxString chosen = evt.GetText();
  std::string chosenUtf8 = wxToStd(chosen);

  int idx = -1;
  for (size_t i = 0; i < m_completionMetadata.m_lastCompletions.size(); ++i) {
    if (m_completionMetadata.m_lastCompletions[i].label == chosenUtf8) {
      idx = (int)i;
      break;
    }
  }
  if (idx < 0)
    return;

  const auto &item = m_completionMetadata.m_lastCompletions[idx];

  // The beginning of the word to be replaced - what we have stored
  int start = m_completionMetadata.m_lastWordStart;
  int end = m_editor->GetCurrentPos(); // end of what Scintilla inserted

  m_editor->SetTargetStart(start);
  m_editor->SetTargetEnd(end);
  m_editor->ReplaceTarget(wxString::FromUTF8(item.text)); // e.g. "Serial" or "println"

  // position AFTER the inserted text
  int pos = start + (int)item.text.size();
  m_editor->GotoPos(pos);

  // parentheses for functions
  if (item.kind == CXCursor_FunctionDecl ||
      item.kind == CXCursor_CXXMethod) {

    m_editor->InsertText(pos, wxT("()"));

    const auto bracePosStart = item.label.find('(');
    const auto bracePosEnd = item.label.find(')');

    if (bracePosStart != std::string::npos &&
        bracePosEnd != std::string::npos) {

      if (bracePosStart + 1 == bracePosEnd) {
        // no parameters - cursor after "()"
        m_editor->GotoPos(pos + 2);
      } else {
        // at least one parameter - cursor inside
        m_editor->GotoPos(pos + 1);
      }
    } else {
      // fallback - cursor after '('
      m_editor->GotoPos(pos + 1);
    }
  }

  ArduinoEditorFrame *frame = GetOwnerFrame();
  if (frame && !m_clangSettings.resolveDiagOnlyAfterSave) {
    frame->ScheduleDiagRefresh();
  }
}

void ArduinoEditor::UpdateTabModifiedIndicator(bool modified) {
  if (ArduinoEditorFrame *frame = GetOwnerFrame()) {
    frame->UpdateEditorTabIcon(this, modified, m_readOnly);
  }
}

void ArduinoEditor::OnSavePointLeft(wxStyledTextEvent &evt) {
  UpdateTabModifiedIndicator(true); // file modified
  evt.Skip();
}

void ArduinoEditor::OnSavePointReached(wxStyledTextEvent &evt) {
  UpdateTabModifiedIndicator(false); // file "clean"
  evt.Skip();
}

void ArduinoEditor::OnDwellStart(wxStyledTextEvent &event) {
  if (!m_editor->HasFocus() || !m_displayHoverInfo) {
    return;
  }

  // When any popup (completion/usages) is visible or in progress, do not show hover tooltips.
  // CallTipShow tends to interfere with AutoComp popup lifetime.
  if (m_popupMode != PopupMode::None) {
    if (!m_editor->AutoCompActive()) {
      APP_DEBUG_LOG("EDIT: popupMode stale (%d) but AutoComp not active -> reset", (int)m_popupMode);
      m_popupMode = PopupMode::None;
    } else {
      return;
    }
  }

  int pos = event.GetPosition();
  if (pos < 0) {
    return;
  }

  int line = m_editor->LineFromPosition(pos) + 1; // 1-based
  int column = m_editor->GetColumn(pos) + 1;      // 1-based

  std::string code = wxToStd(m_editor->GetText());

  HoverInfo info;
  if (!completion->GetHoverInfo(m_filename, code, line, column, info)) {
    return;
  }

  APP_DEBUG_LOG("EDIT: Hover: name='%s', sig='%s', type='%s', kind='%s', brief='%s', full='%s'",
                info.name.c_str(),
                info.signature.c_str(),
                info.type.c_str(),
                info.kind.c_str(),
                info.briefComment.c_str(),
                info.fullComment.c_str());

  wxString wxTip = wxString::FromUTF8(info.ToHoverString());
  wxTip.Trim(true).Trim(false);
  m_editor->CallTipShow(pos, wxTip);
}

void ArduinoEditor::CancelCallTip() {
  if (m_editor->CallTipActive()) {
    m_editor->CallTipCancel();
  }
}

void ArduinoEditor::OnDwellEnd(wxStyledTextEvent &WXUNUSED(event)) {
  CancelCallTip();
}

void ArduinoEditor::OnEditorLeftDown(wxMouseEvent &event) {
#ifdef __WXMAC__
  bool goTo = event.CmdDown();
#else
  bool goTo = event.ControlDown();
#endif

  if (!goTo) {
    event.Skip();
    return;
  }

  // Determine where we clicked (according to the mouse)
  int x = event.GetX();
  int y = event.GetY();
  int pos = m_editor->PositionFromPointClose(x, y);
  if (pos == wxSTC_INVALID_POSITION) {
    event.Skip();
    return;
  }

  // also move the cursor visually to the click position
  m_editor->GotoPos(pos);
  m_editor->SetCurrentPos(pos);
  m_editor->SetSelection(pos, pos);

  GotoSymbolDefinition();
}

void ArduinoEditor::GotoSymbolDefinition() {
  // Find the definition using libclang
  std::string code = wxToStd(m_editor->GetText());

  int line, column;
  GetCurrentCursor(line, column); // 1-based

  // Save this position as "back" (i.e., where we came from)
  if (auto *frame = GetOwnerFrame()) {
    frame->PushNavLocation(m_filePath, line, column);
  }

  JumpTarget target;
  if (!completion->FindDefinition(m_filePath, code, line, column, target)) {
    FindSymbolUsagesAtCursor();
    return;
  }

  APP_DEBUG_LOG("EDIT: FindDefinition() -> file=%s, line=%d, column=%d", target.file.c_str(), target.line, target.column);

  if ((target.file == m_filePath) && (target.line == line) /* not compare curCol */) {
    FindSymbolUsagesAtCursor();
    return;
  }

  // Perform a jump to the target definition
  HandleGoToLocation(target);
}

void ArduinoEditor::OnFlashTimer(wxTimerEvent &WXUNUSED(event)) {
  if (m_flashLine < 0)
    return;

  int startPos = m_editor->PositionFromLine(m_flashLine);
  int endPos = m_editor->GetLineEndPosition(m_flashLine);

  m_editor->SetIndicatorCurrent(STC_FLASH_INDICATOR);
  m_editor->IndicatorClearRange(startPos, endPos - startPos);

  m_flashLine = -1;
}

void ArduinoEditor::HandleGoToLocation(const JumpTarget &tgt) {
  ArduinoEditorFrame *frame = GetOwnerFrame();
  if (!frame) {
    return;
  }

  frame->HandleGoToLocation(tgt);
}

void ArduinoEditor::FlashLine(int line) {
  m_flashLine = line - 1; // Scintilla is 0-based
  int startPos = m_editor->PositionFromLine(m_flashLine);
  int endPos = m_editor->GetLineEndPosition(m_flashLine);

  m_editor->SetIndicatorCurrent(STC_FLASH_INDICATOR);
  m_editor->IndicatorClearRange(0, m_editor->GetTextLength());
  m_editor->IndicatorFillRange(startPos, endPos - startPos);

  // Start timer to clear the highlight
  m_flashTimer.Start(FLASH_TIME, wxTIMER_ONE_SHOT);
}

void ArduinoEditor::ClearDiagnosticsIndicators() {
  const int length = m_editor->GetTextLength();

  m_editor->SetIndicatorCurrent(STC_INDIC_DIAG_ERROR);
  m_editor->IndicatorClearRange(0, length);

  m_editor->SetIndicatorCurrent(STC_INDIC_DIAG_WARNING);
  m_editor->IndicatorClearRange(0, length);

  if (m_symbolOverview) {
    m_symbolOverview->ClearMarker(ESM_ERROR);
    m_symbolOverview->ClearMarker(ESM_WARNING);
  }
}

void ArduinoEditor::AddDiagnosticUnderline(unsigned line, unsigned column, bool isError) {
  int textLen = m_editor->GetTextLength();

  if (textLen == 0) {
    return;
  }

  int lineCount = m_editor->GetLineCount();

  // clang sends 1-based line/column, wxSTC uses 0-based
  int ln = (line > 0) ? (int)line - 1 : 0;
  if (ln < 0) {
    ln = 0;
  }

  if (ln >= lineCount) {
    ln = m_editor->GetLineCount() - 1;
  }

  int lineStart = m_editor->PositionFromLine(ln);
  if (lineStart < 0) {
    return;
  }

  int col = (column > 0) ? (int)column - 1 : 0;
  if (col < 0) {
    col = 0;
  }

  int pos = lineStart + col;
  if (pos < 0) {
    pos = 0;
  }
  if (pos >= textLen) {
    pos = textLen - 1;
  }

  // Let's try to mark the entire "word" / token, not just a single character
  int start = pos;
  int end = pos;

  auto isDelim = [this, textLen](int p) -> bool {
    if (p < 0 || p >= textLen)
      return true;
    wxChar ch = m_editor->GetCharAt(p);
    return wxIsspace(ch) || ch == '(' || ch == ')' || ch == '{' || ch == '}' ||
           ch == ';' || ch == ',' || ch == '.' || ch == ':' || ch == '[' ||
           ch == ']';
  };

  while (start > 0 && !isDelim(start - 1)) {
    --start;
  }
  while (end < textLen && !isDelim(end)) {
    ++end;
  }

  int len = end - start;
  if (len <= 0) {
    // fallback marks the entire line
    start = m_editor->PositionFromLine(ln);
    end = m_editor->GetLineEndPosition(ln);
  }

  m_editor->SetIndicatorCurrent(isError ? STC_INDIC_DIAG_ERROR : STC_INDIC_DIAG_WARNING);
  m_editor->IndicatorFillRange(start, len);

  if (m_symbolOverview) {
    m_symbolOverview->AddMarker(isError ? ESM_ERROR : ESM_WARNING, line);
  }
}

void ArduinoEditor::Goto(int line, int column) {
  int targetLine = line - 1; // wxSTC is 0-based
  if (targetLine < 0)
    targetLine = 0;

  int pos = m_editor->PositionFromLine(targetLine) + (column - 1);
  if (pos < 0)
    pos = m_editor->PositionFromLine(targetLine);

  // classic jump + caret
  m_editor->GotoPos(pos);
  m_editor->SetCurrentPos(pos);
  m_editor->SetSelection(pos, pos);

  // --- centering the line on the screen ---
  int linesOnScreen = m_editor->LinesOnScreen();
  if (linesOnScreen > 0) {
    int desiredFirst = targetLine - linesOnScreen / 2;
    if (desiredFirst < 0)
      desiredFirst = 0;

    m_editor->ScrollToLine(desiredFirst);
  }

  if (m_symbolOverview) {
    m_symbolOverview->Refresh();
  }
}

void ArduinoEditor::GetCurrentCursor(int &line, int &column) {
  int pos = m_editor->GetCurrentPos();
  line = m_editor->LineFromPosition(pos) + 1; // libclang is 1-based
  column = m_editor->GetColumn(pos) + 1;
}

void ArduinoEditor::OnEditorKillFocus(wxFocusEvent &event) {
  CancelCallTip();
  event.Skip();
}

void ArduinoEditor::OnEditorMouseLeave(wxMouseEvent &event) {
  CancelCallTip();
  event.Skip();
}

ArduinoEditorFrame *ArduinoEditor::GetOwnerFrame() {
  for (wxWindow *w = GetParent(); w != nullptr; w = w->GetParent()) {
    if (auto *frame = dynamic_cast<ArduinoEditorFrame *>(w)) {
      return frame;
    }
  }
  return nullptr;
}

// simple colormix
static wxColour MixColors(const wxColour &a, const wxColour &b, double ratio) {
  double inv = 1.0 - ratio;
  unsigned char r = (unsigned char)(a.Red() * inv + b.Red() * ratio);
  unsigned char g = (unsigned char)(a.Green() * inv + b.Green() * ratio);
  unsigned char bch = (unsigned char)(a.Blue() * inv + b.Blue() * ratio);
  return wxColour(r, g, bch);
}

void ArduinoEditor::UpdateReadOnlyColors() {
  if (!m_editor)
    return;

  // base bg color from current theme
  wxColour baseBg = m_editor->StyleGetBackground(wxSTC_STYLE_DEFAULT);
  if (!baseBg.IsOk()) {
    baseBg = m_normalBgColor;
  }

  if (!baseBg.IsOk()) {
    // fallback, system color
    baseBg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
  }

  if (m_readOnly) {
    // depending on whether the theme is light or dark, we will generate a color
    int avg = (baseBg.Red() + baseBg.Green() + baseBg.Blue()) / 3;

    wxColour target;
    if (avg < 128) {
      target = MixColors(baseBg, *wxWHITE, 0.12);
    } else {
      target = MixColors(baseBg, *wxBLACK, 0.10);
    }

    m_readOnlyBgColor = target;
    m_hasReadOnlyBg = true;

    for (int style = 0; style <= wxSTC_STYLE_MAX; ++style) {
      wxColour cur = m_editor->StyleGetBackground(style);
      if (!cur.IsOk() || cur == baseBg) {
        m_editor->StyleSetBackground(style, target);
      }
    }

    wxColour caretLine = m_editor->GetCaretLineBackground();
    if (!caretLine.IsOk()) {
      caretLine = baseBg;
    }
    wxColour caretTarget = MixColors(target, caretLine, 0.5);
    m_editor->SetCaretLineBackground(caretTarget);
  } else {
    if (!m_hasReadOnlyBg) {
      m_editor->StyleSetBackground(wxSTC_STYLE_DEFAULT, baseBg);
      return;
    }

    for (int style = 0; style <= wxSTC_STYLE_MAX; ++style) {
      wxColour cur = m_editor->StyleGetBackground(style);
      if (cur == m_readOnlyBgColor) {
        m_editor->StyleSetBackground(style, baseBg);
      }
    }

    wxColour caretBase = MixColors(baseBg, wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT), 0.08);
    m_editor->SetCaretLineBackground(caretBase);
  }
}

void ArduinoEditor::UpdateTabReadOnlyIndicator() {
  if (ArduinoEditorFrame *frame = GetOwnerFrame()) {
    bool modified = m_editor && m_editor->GetModify();
    frame->UpdateEditorTabIcon(this, modified, m_readOnly);
  }
}

void ArduinoEditor::AiSolveError(const ArduinoParseError &error) {
  if (m_aiActions && m_aiSettings.enabled) {
    m_aiActions->SolveProjectError(error);
  }
}

void ArduinoEditor::AiGenerateDocCommentsForCurrentFile() {
  if (m_aiActions && m_aiSettings.enabled) {
    m_aiActions->GenerateDocCommentsForCurrentFile();
  }
}

void ArduinoEditor::AiGenerateDocCommentsForCurrentClass() {
  if (m_aiActions && m_aiSettings.enabled) {
    m_aiActions->GenerateDocCommentsForCurrentClass();
  }
}

void ArduinoEditor::AiOptimizeFunctionOrMethod() {
  if (m_aiActions && m_aiSettings.enabled) {
    m_aiActions->OptimizeFunctionOrMethod();
  }
}
