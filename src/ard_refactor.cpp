#include "ard_refactor.hpp"

#include "ard_cc.hpp"
#include "ard_ed_frm.hpp"
#include "ard_edit.hpp"
#include "ard_orginc.hpp"
#include "utils.hpp"
#include <algorithm>
#include <fstream>
#include <set>
#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/richmsgdlg.h>
#include <wx/stdpaths.h>
#include <wx/textdlg.h>
#include <wx/tokenzr.h>

namespace {

// Trim string
static std::string TrimCopy(const std::string &s) {
  const char *ws = " \t\r\n";
  size_t start = s.find_first_not_of(ws);
  if (start == std::string::npos)
    return std::string();

  size_t end = s.find_last_not_of(ws);
  return s.substr(start, end - start + 1);
}

static wxString ExtractIndent(const wxString &lineText) {
  wxString indent;
  for (int i = 0, n = (int)lineText.Length(); i < n; ++i) {
    wxChar ch = lineText[i];
    if (ch == ' ' || ch == '\t') {
      indent += ch;
    } else {
      break;
    }
  }
  return indent;
}

// Nicely reconstruct the signature using Clang parameters from HoverInfo.
// - keep prefix (ret type + name + anything before '(')
// - keep suffix (from ')' dl - const, noexcept, etc.)
// - parameters are from info.parameters (type + name)
static std::string BuildSignatureWithHoverParams(const std::string &sigRaw,
                                                 const HoverInfo &info) {
  // If we don't get anything, let's try a fallback from type + name
  std::string sig = TrimCopy(sigRaw);
  if (sig.empty()) {
    if (!info.name.empty()) {
      std::string ret = TrimCopy(info.type);
      if (!ret.empty()) {
        sig = ret + " " + info.name + "()";
      } else {
        sig = info.name + "()";
      }
    } else {
      return std::string();
    }
  }

  auto lparen = sig.find('(');
  auto rparen = sig.rfind(')');
  if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen) {
    return sig;
  }

  std::string prefix = sig.substr(0, lparen + 1);
  std::string suffix = sig.substr(rparen);

  // We will compose the parameters from info.parameters
  std::string paramsStr;
  const auto &params = info.parameters;

  for (size_t i = 0; i < params.size(); ++i) {
    if (i > 0)
      paramsStr += ", ";

    std::string type = TrimCopy(params[i].type);
    paramsStr += type;

    if (!params[i].name.empty()) {
      paramsStr += " ";
      paramsStr += params[i].name;
    }
  }

  return prefix + paramsStr + suffix;
}

static std::string StripQualificationFromFunctionSignature(const std::string &sigRaw) {
  std::string sig = TrimCopy(sigRaw);
  auto lparen = sig.find('(');
  if (lparen == std::string::npos) {
    return sig;
  }

  std::string before = sig.substr(0, lparen);
  std::string after = sig.substr(lparen);

  size_t lastSpace = before.find_last_of(" \t");
  if (lastSpace == std::string::npos) {
    return sig;
  }

  std::string left = before.substr(0, lastSpace + 1);
  std::string namePart = TrimCopy(before.substr(lastSpace + 1));

  size_t scopePos = namePart.rfind("::");
  if (scopePos == std::string::npos) {
    return sig;
  }

  std::string methodName = namePart.substr(scopePos + 2);
  if (methodName.empty()) {
    return sig;
  }

  std::string newBefore = left + methodName;
  return newBefore + after;
}

struct ParsedInclude {
  bool isSystem = false; // <...> vs "..."
  wxString header;       // name without <> / ""
  wxString leading;      // spaces before '#'
  wxString trailing;     // comment and more mess after the closing > / "
};

static bool ParseIncludeLine(const wxString &line, ParsedInclude &out) {
  out = ParsedInclude{};

  int len = (int)line.Length();
  int i = 0;

  // leading whitespace
  while (i < len && wxIsspace(line[i])) {
    out.leading += line[i];
    ++i;
  }

  if (i >= len || line[i] != '#')
    return false;

  ++i; // '#'

  // spaces after '#'
  while (i < len && wxIsspace(line[i])) {
    ++i;
  }

  wxString kw;
  while (i < len && wxIsalpha(line[i])) {
    kw += line[i];
    ++i;
  }

  if (!(kw.CmpNoCase(wxT("include")) == 0)) {
    return false;
  }

  // spaces after "include"
  while (i < len && wxIsspace(line[i])) {
    ++i;
  }

  if (i >= len)
    return false;

  wxChar open = line[i];
  if (open != '<' && open != '"') {
    return false;
  }

  out.isSystem = (open == '<');
  ++i;

  wxChar closeCh = out.isSystem ? '>' : '"';
  int headerStart = i;

  while (i < len && line[i] != closeCh) {
    ++i;
  }

  if (i <= headerStart)
    return false;

  out.header = line.Mid(headerStart, i - headerStart);

  if (i < len && line[i] == closeCh) {
    ++i;
  }

  if (i < len) {
    out.trailing = line.Mid(i);
  }

  return true;
}

static wxString EnsureArdeditDir(const wxString &sketchRoot) {
  wxFileName fn(sketchRoot, wxEmptyString);
  fn.AppendDir(wxT(".ardedit"));

  const wxString dirPath = fn.GetFullPath();

  if (!wxDirExists(dirPath)) {
    if (!wxFileName::Mkdir(dirPath, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
      return wxString();
    }
  }

  return dirPath;
}

static wxString FindProjectClangFormatFile(const wxString &sketchRoot) {
  // prefer standard names
  const wxString cand1 = wxFileName(sketchRoot, wxT(".clang-format")).GetFullPath();
  if (wxFileExists(cand1))
    return cand1;

  const wxString cand2 = wxFileName(sketchRoot, wxT("_clang-format")).GetFullPath();
  if (wxFileExists(cand2))
    return cand2;

  // legacy / user expectation (if you already used it somewhere)
  const wxString cand3 = wxFileName(sketchRoot, wxT(".clang_format")).GetFullPath();
  if (wxFileExists(cand3))
    return cand3;

  return wxString();
}

static nlohmann::json ParseJsonObjectLenient(const wxString &s) {
  if (s.IsEmpty())
    return nlohmann::json::object();

  try {
    auto j = nlohmann::json::parse(wxToStd(s));
    if (!j.is_object())
      return nlohmann::json::object();
    return j;
  } catch (...) {
    return nlohmann::json::object();
  }
}

static wxString YamlQuoteIfNeeded(const wxString &val) {
  // For our purposes, a simple heuristic is enough:
  // - if there are spaces or "strange" characters, we put it in single quotes
  // - inside single quotes, escape by doubling ''
  bool need =
      val.Find(' ') != wxNOT_FOUND ||
      val.Find('\t') != wxNOT_FOUND ||
      val.Find(':') != wxNOT_FOUND ||
      val.Find('#') != wxNOT_FOUND ||
      val.Find('{') != wxNOT_FOUND ||
      val.Find('}') != wxNOT_FOUND ||
      val.Find('[') != wxNOT_FOUND ||
      val.Find(']') != wxNOT_FOUND ||
      val.Find('"') != wxNOT_FOUND ||
      val.Find('\'') != wxNOT_FOUND;

  if (!need)
    return val;

  wxString out = val;
  out.Replace(wxT("'"), wxT("''"));
  return wxT("'") + out + wxT("'");
}

static wxString BuildTempClangFormatFile(const wxString &ardeditDir,
                                         const EditorSettings &settings) {
  wxFileName fn(ardeditDir, wxT("ae_clang_format.yaml"));
  wxString path = fn.GetFullPath();

  int colLimit = settings.edgeColumn;
  if (colLimit < 0)
    colLimit = 0;

  wxString yaml;
  yaml << wxString::Format(wxT("IndentWidth: %d\n"), settings.tabWidth);
  yaml << wxString::Format(wxT("TabWidth: %d\n"), settings.tabWidth);
  yaml << wxT("UseTab: ") << (settings.useTabs ? wxT("ForIndentation") : wxT("Never")) << wxT("\n");
  yaml << wxString::Format(wxT("ColumnLimit: %d\n"), colLimit);

  // ---- Merge clangFormatOverridesJson (advanced options) ----
  wxString overrides = settings.clangFormatOverridesJson;
  overrides.Trim(true).Trim(false);

  if (!overrides.IsEmpty() && overrides != wxT("{}")) {
    nlohmann::json j = ParseJsonObjectLenient(overrides);

    std::vector<std::string> keys;
    keys.reserve(j.size());
    for (auto it = j.begin(); it != j.end(); ++it) {
      keys.push_back(it.key());
    }
    std::sort(keys.begin(), keys.end());

    for (const std::string &k : keys) {
      const auto &v = j[k];

      wxString key = wxString::FromUTF8(k);
      if (key.IsEmpty())
        continue;

      if (v.is_boolean()) {
        yaml << key << wxT(": ") << (v.get<bool>() ? wxT("true") : wxT("false")) << wxT("\n");
      } else if (v.is_number_integer()) {
        yaml << key << wxT(": ") << wxString::Format(wxT("%d"), (int)v.get<int>()) << wxT("\n");
      } else if (v.is_string()) {
        wxString sval = wxString::FromUTF8(v.get<std::string>());
        yaml << key << wxT(": ") << YamlQuoteIfNeeded(sval) << wxT("\n");
      } else {
        // other types (float/object/array/null) ignore
      }
    }
  }

  std::string utf8 = wxToStd(yaml);
  if (!SaveFileFromString(wxToStd(path), utf8)) {
    return wxString();
  }
  return path;
}

static wxString GetBundledClangFormatPath() {
  wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
#ifdef __WXMSW__
  exe.SetFullName(wxT("clang-format.exe"));
#else
  exe.SetFullName(wxT("clang-format"));
#endif
  return exe.GetFullPath();
}

static std::string QuoteArgSimple(const std::string &s) {
  // Good enough for our use: no trailing backslashes in paths, no weird chars.
  if (s.find_first_of(" \t\"") == std::string::npos)
    return s;

  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '"')
      out += "\\\"";
    else
      out.push_back(c);
  }
  out.push_back('"');
  return out;
}

static bool ComputeSelectedLines1Based(wxStyledTextCtrl *stc, int &fromLine1, int &toLine1) {
  int selStart = stc->GetSelectionStart();
  int selEnd = stc->GetSelectionEnd();
  if (selStart == selEnd)
    return false;
  if (selStart > selEnd)
    std::swap(selStart, selEnd);

  int from0 = stc->LineFromPosition(selStart);
  int to0 = stc->LineFromPosition(selEnd);

  // If selection ends exactly at column 0 of the next line, don't include that next line.
  if (to0 > from0 && stc->GetColumn(selEnd) == 0) {
    to0--;
  }

  fromLine1 = from0 + 1;
  toLine1 = to0 + 1;
  if (toLine1 < fromLine1)
    toLine1 = fromLine1;

  return true;
}

static wxString PickFormatTempFilename(const wxString &currentFilename) {
  wxFileName fn(currentFilename);
  wxString ext = fn.GetExt().Lower();

  // INO => format as C++
  if (ext == wxT("ino") || ext == wxT("pde"))
    return wxT("format.cpp");

  // headers keep header extension (helps language detection a bit)
  if (ext == wxT("h") || ext == wxT("hpp") || ext == wxT("hh") || ext == wxT("hxx"))
    return wxT("format.hpp");

  // everything else => cpp
  return wxT("format.cpp");
}

} // namespace

// -----------------------------------------------------------------------------
// ArduinoRefactoring
// -----------------------------------------------------------------------------

ArduinoRefactoring::ArduinoRefactoring(ArduinoEditor *editor)
    : m_ed(editor) {
}

ArduinoEditorFrame *ArduinoRefactoring::GetOwnerFrame() {
  if (!m_ed)
    return nullptr;
  return m_ed->GetOwnerFrame();
}

wxString ArduinoRefactoring::ConvertSourceHeaderExtension(const wxString &srcExt) {
  static const std::unordered_map<wxString, wxString> fwd = {
      {wxT("c"), wxT("h")},
      {wxT("cpp"), wxT("hpp")},
      {wxT("cxx"), wxT("hpp")},
      {wxT("cc"), wxT("hh")}};

  static const std::unordered_map<wxString, wxString> rev = {
      {wxT("h"), wxT("c")},
      {wxT("hpp"), wxT("cpp")},
      {wxT("hh"), wxT("cc")}};

  if (auto it = fwd.find(srcExt); it != fwd.end())
    return it->second;

  if (auto it = rev.find(srcExt); it != rev.end())
    return it->second;

  APP_DEBUG_LOG("EDIT: Unsupported extension %s!", wxToStd(srcExt).c_str());
  return wxString();
}

// -----------------------------------------------------------------------------
// Individual refactorings
// -----------------------------------------------------------------------------

void ArduinoRefactoring::RefactorRenameSymbolAtCursor() {
  if (!m_ed || !m_ed->m_editor)
    return;

  int line = 0;
  int column = 0;
  m_ed->GetCurrentCursor(line, column);

  const int stcLine = static_cast<int>(line) - 1;
  const int stcColumn = static_cast<int>(column) - 1;
  if (stcLine < 0 || stcColumn < 0) {
    return;
  }

  int pos = m_ed->m_editor->PositionFromLine(stcLine) + stcColumn;
  if (pos < 0 || pos > m_ed->m_editor->GetTextLength()) {
    return;
  }

  const int wordStart = m_ed->m_editor->WordStartPosition(pos, /*onlyWordChars=*/true);
  const int wordEnd = m_ed->m_editor->WordEndPosition(pos, /*onlyWordChars=*/true);

  if (wordStart >= wordEnd) {
    m_ed->ModalMsgDialog(_("No identifier under the cursor."),
                         _("Rename symbol"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  wxString oldNameWx = m_ed->m_editor->GetTextRange(wordStart, wordEnd);
  std::string oldName = wxToStd(oldNameWx);

  if (!LooksLikeIdentifier(oldName)) {
    m_ed->ModalMsgDialog(_("The text under the cursor is not a valid identifier."),
                         _("Rename symbol"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  if (auto *frame = GetOwnerFrame()) {
    frame->RefactorRenameSymbol(m_ed, line, column, oldNameWx);
  }
}

void ArduinoRefactoring::RefactorIntroduceVariable() {
  if (!m_ed || !m_ed->m_editor)
    return;

  auto *stc = m_ed->m_editor;

  if (stc->GetReadOnly()) {
    return;
  }

  int selStart = stc->GetSelectionStart();
  int selEnd = stc->GetSelectionEnd();

  if (selStart == selEnd) {
    m_ed->ModalMsgDialog(_("Please select an expression to extract into a variable."),
                         _("Introduce variable"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  if (selStart > selEnd) {
    std::swap(selStart, selEnd);
  }

  wxString selTextAll = stc->GetTextRange(selStart, selEnd);
  if (selTextAll.IsEmpty()) {
    m_ed->ModalMsgDialog(_("Please select an expression to extract into a variable."),
                         _("Introduce variable"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  int len = (int)selTextAll.Length();
  int leading = 0;
  while (leading < len && wxIsspace(selTextAll[leading])) {
    ++leading;
  }

  int trailing = 0;
  while (trailing < len - leading && wxIsspace(selTextAll[len - 1 - trailing])) {
    ++trailing;
  }

  if (leading + trailing >= len) {
    m_ed->ModalMsgDialog(_("The selection contains only whitespace. Please select a valid expression."),
                         _("Introduce variable"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  wxString expr = selTextAll.Mid(leading, len - leading - trailing);

  int exprStartPos = selStart + leading;
  int exprEndPos = selEnd - trailing;

  wxTextEntryDialog nameDlg(
      m_ed,
      _("Variable name:"),
      _("Introduce variable"),
      _("tmp"));

  if (nameDlg.ShowModal() != wxID_OK) {
    return;
  }

  wxString varNameWx = nameDlg.GetValue();
  varNameWx.Trim(true).Trim(false);

  if (varNameWx.IsEmpty()) {
    m_ed->ModalMsgDialog(_("Variable name cannot be empty."),
                         _("Introduce variable"),
                         wxOK | wxICON_WARNING);
    return;
  }

  std::string varName = wxToStd(varNameWx);
  if (!LooksLikeIdentifier(varName)) {
    m_ed->ModalMsgDialog(_("Variable name is not a valid identifier."),
                         _("Introduce variable"),
                         wxOK | wxICON_WARNING);
    return;
  }

  int exprLine = stc->LineFromPosition(exprStartPos);
  int lineStartPos = stc->PositionFromLine(exprLine);
  if (lineStartPos < 0)
    lineStartPos = 0;

  wxString lineText = stc->GetLine(exprLine);
  wxString indent;
  for (int i = 0; i < (int)lineText.Length(); ++i) {
    wxChar ch = lineText[i];
    if (ch == ' ' || ch == '\t') {
      indent += ch;
    } else {
      break;
    }
  }

  wxString decl = indent;
  decl += wxT("auto ");
  decl += varNameWx;
  decl += wxT(" = ");
  decl += expr;
  decl += wxT(";\n");

  stc->BeginUndoAction();

  stc->InsertText(lineStartPos, decl);
  int delta = (int)decl.Length();

  int newExprStart = exprStartPos + delta;
  int newExprEnd = exprEndPos + delta;

  wxString currentExpr = stc->GetTextRange(newExprStart, newExprEnd);
  currentExpr.Trim(true).Trim(false);

  if (currentExpr.Replace(wxT(" "), wxEmptyString) ==
      expr.Replace(wxT(" "), wxEmptyString)) {
    stc->SetTargetStart(newExprStart);
    stc->SetTargetEnd(newExprEnd);
    stc->ReplaceTarget(varNameWx);
  }

  int caretPos = lineStartPos + indent.Length() + 5; // "auto "
  stc->GotoPos(caretPos);
  stc->SetCurrentPos(caretPos);
  stc->SetSelection(caretPos, caretPos + (int)varNameWx.Length());

  stc->EndUndoAction();

  if (auto *frame = GetOwnerFrame()) {
    frame->RebuildProject();
  }
}

void ArduinoRefactoring::RefactorInlineVariable() {
  if (!m_ed || !m_ed->m_editor)
    return;

  auto *stc = m_ed->m_editor;
  if (stc->GetReadOnly())
    return;

  auto *completion = m_ed->completion;
  if (!completion || !completion->IsTranslationUnitValid()) {
    m_ed->ModalMsgDialog(_("Code completion is not initialized yet."),
                         _("Inline variable"),
                         wxOK | wxICON_WARNING);
    return;
  }

  completion->InvalidateTranslationUnit();
  completion->InitTranslationUnitForIno();

  int line = 0;
  int column = 0;
  m_ed->GetCurrentCursor(line, column);

  std::string code = wxToStd(stc->GetText());

  HoverInfo info;
  if (!completion->GetHoverInfo(m_ed->m_filename, code, line, column, info)) {
    m_ed->ModalMsgDialog(_("No symbol under the cursor."),
                         _("Inline variable"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  // It must be a VarDecl
  if (info.kind != "VarDecl") {
    m_ed->ModalMsgDialog(_("This refactoring currently supports only local variables declared inside a function."),
                         _("Inline variable"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  if (info.name.empty() || !LooksLikeIdentifier(info.name)) {
    m_ed->ModalMsgDialog(_("The symbol under the cursor is not a valid variable."),
                         _("Inline variable"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  std::string varName = info.name;

  std::vector<JumpTarget> occs;
  if (!completion->FindSymbolOccurrences(m_ed->GetFilePath(), code, line, column,
                                         /*onlyFromSketch=*/true, occs)) {
    m_ed->ModalMsgDialog(_("Unable to collect variable occurrences from the code model."),
                         _("Inline variable"),
                         wxOK | wxICON_WARNING);
    return;
  }

  if (occs.empty()) {
    m_ed->ModalMsgDialog(_("No usages of this variable were found."),
                         _("Inline variable"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  const std::string curFile = m_ed->GetFilePath();
  for (const auto &jt : occs) {
    if (jt.file != curFile) {
      m_ed->ModalMsgDialog(
          _("This variable is used in another file.\n"
            "Inline variable currently supports only variables used in a single file."),
          _("Inline variable"),
          wxOK | wxICON_INFORMATION);
      return;
    }
  }

  // --- We find the declaration: line with "<name> = ...;" ---
  struct DeclInfo {
    int lineIndex = -1;  // 0-based line index
    int eqPosRel = -1;   // position '=' on line
    int semiPosRel = -1; // position ';' on line
  };

  DeclInfo declInfo;

  for (const auto &jt : occs) {
    int l = jt.line - 1;   // 0-based
    int c = jt.column - 1; // 0-based
    if (l < 0 || c < 0)
      continue;
    if (l >= stc->GetLineCount())
      continue;

    int lineStartPos = stc->PositionFromLine(l);
    int pos = lineStartPos + c;
    if (pos < 0 || pos > stc->GetTextLength())
      continue;

    wxString token = stc->GetTextRange(pos, pos + (int)varName.size());
    if (wxToStd(token) != varName)
      continue;

    wxString lineText = stc->GetLine(l);

    int nameOffsetRel = pos - lineStartPos;
    int afterNameRel = nameOffsetRel + (int)varName.size();

    // We are looking for '=' after the name and ';' after '='
    int eqPosRel = lineText.Find(wxT('='), afterNameRel);
    if (eqPosRel == wxNOT_FOUND)
      continue;

    int semiPosRel = lineText.Find(wxT(';'), eqPosRel + 1);
    if (semiPosRel == wxNOT_FOUND)
      continue;

    // We have a candidate for the declaration; if there is one, we better quit
    if (declInfo.lineIndex != -1) {
      m_ed->ModalMsgDialog(
          _("Multiple assignments or initializations of this variable were found.\n"
            "Inline variable currently supports only a single initialized declaration."),
          _("Inline variable"),
          wxOK | wxICON_INFORMATION);
      return;
    }

    declInfo.lineIndex = l;
    declInfo.eqPosRel = eqPosRel;
    declInfo.semiPosRel = semiPosRel;
  }

  if (declInfo.lineIndex == -1) {
    m_ed->ModalMsgDialog(
        _("Unable to locate a simple initialized declaration for this variable.\n"
          "Expected something like \"auto name = expr;\"."),
        _("Inline variable"),
        wxOK | wxICON_INFORMATION);
    return;
  }

  int declLine = declInfo.lineIndex;
  int declLineStart = stc->PositionFromLine(declLine);
  int declLineEnd = stc->GetLineEndPosition(declLine);
  wxString declLineText = stc->GetTextRange(declLineStart, declLineEnd);

  // --- We parse the expression after '=' and after ';' ---
  int exprStartRel = declInfo.eqPosRel + 1;
  int exprEndRel = declInfo.semiPosRel;
  if (exprStartRel >= exprEndRel) {
    m_ed->ModalMsgDialog(_("The variable does not have an initializer."),
                         _("Inline variable"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  wxString expr = declLineText.Mid(exprStartRel, exprEndRel - exprStartRel);
  expr.Trim(true).Trim(false);

  if (expr.IsEmpty()) {
    m_ed->ModalMsgDialog(_("The variable does not have an initializer."),
                         _("Inline variable"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  wxString replacementWx;
  replacementWx << wxT("(") << expr << wxT(")");
  std::string replacement = wxToStd(replacementWx);

  // --- All uses except declaration ---
  std::vector<JumpTarget> uses;
  uses.reserve(occs.size());
  for (const auto &jt : occs) {
    int l = jt.line - 1;
    if (l == declLine)
      continue;
    uses.push_back(jt);
  }

  if (uses.empty()) {
    int rc = m_ed->ModalMsgDialog(
        _("No usages of this variable were found outside its declaration.\n"
          "Do you still want to remove the declaration?"),
        _("Inline variable"),
        wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);
    if (rc != wxID_YES)
      return;
  }

  std::sort(uses.begin(), uses.end(),
            [](const JumpTarget &a, const JumpTarget &b) {
              if (a.line != b.line)
                return a.line > b.line;
              return a.column > b.column;
            });

  stc->BeginUndoAction();

  for (const auto &u : uses) {
    int l = u.line - 1;
    int c = u.column - 1;
    if (l < 0 || c < 0)
      continue;
    if (l >= stc->GetLineCount())
      continue;

    int startPos = stc->PositionFromLine(l) + c;
    if (startPos < 0 || startPos > stc->GetTextLength())
      continue;

    wxString currentText = stc->GetTextRange(startPos, startPos + (int)varName.size());
    if (wxToStd(currentText) != varName) {
      continue;
    }

    char before = 0;
    char after = 0;

    if (startPos > 0) {
      before = stc->GetCharAt(startPos - 1);
    }
    if (startPos + (int)varName.size() < stc->GetTextLength()) {
      after = stc->GetCharAt(startPos + (int)varName.size());
    }

    bool surrounded = (before == '(' && after == ')');

    wxString replWx;
    if (surrounded) {
      replWx = expr;
    } else {
      replWx = wxT("(") + expr + wxT(")");
    }

    stc->SetTargetStart(startPos);
    stc->SetTargetEnd(startPos + (int)varName.size());
    stc->ReplaceTarget(replWx);
  }

  {
    int lineCount = stc->GetLineCount();
    int removeStart = stc->PositionFromLine(declLine);
    int removeEnd;
    if (declLine + 1 < lineCount) {
      removeEnd = stc->PositionFromLine(declLine + 1);
    } else {
      removeEnd = stc->GetTextLength();
    }

    stc->SetTargetStart(removeStart);
    stc->SetTargetEnd(removeEnd);
    stc->ReplaceTarget(wxEmptyString);
  }

  stc->EndUndoAction();

  if (auto *frame = GetOwnerFrame()) {
    frame->RebuildProject();
  }
}

void ArduinoRefactoring::RefactorGenerateFunctionFromCursor() {
  if (!m_ed || !m_ed->m_editor)
    return;

  if (!m_ed->completion || !m_ed->completion->IsTranslationUnitValid()) {
    m_ed->ModalMsgDialog(_("Code completion is not initialized yet."),
                         _("Generate function"),
                         wxOK | wxICON_WARNING);
    return;
  }

  ArduinoEditorFrame *frame = GetOwnerFrame();
  if (!frame)
    return;

  int line = 0;
  int column = 0;
  m_ed->GetCurrentCursor(line, column);

  std::string code = wxToStd(m_ed->m_editor->GetText());

  HoverInfo info;
  if (!m_ed->completion->GetHoverInfo(m_ed->m_filename, code, line, column, info)) {
    m_ed->ModalMsgDialog(_("No symbol under the cursor."),
                         _("Generate function"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  // We will build the signature base from HoverInfo + parameters
  std::string sig = BuildSignatureWithHoverParams(info.signature, info);

  if (sig.empty() || sig.find('(') == std::string::npos) {
    m_ed->ModalMsgDialog(_("The symbol under the cursor does not look like a function."),
                         _("Generate function"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  bool inHeader = isHeaderFile(m_ed->m_filename);
  bool isMemberInHeader =
      inHeader &&
      (info.kind == "CXXMethod" ||
       info.kind == "CXXConstructor" ||
       info.kind == "CXXDestructor");

  // If we are in the header and generating the implementation in .cpp, we add ClassName:: before the name
  if (isMemberInHeader && !info.name.empty()) {
    m_ed->completion->InvalidateTranslationUnit();
    AeContainerInfo containerInfo;
    if (m_ed->completion->FindEnclosingContainerInfo(m_ed->GetFilePath(), code, line, column, containerInfo)) {
      std::string clsName = containerInfo.name;
      if (!clsName.empty() &&
          (containerInfo.kind == "ClassDecl" || containerInfo.kind == "StructDecl")) {

        if (sig.find("::") == std::string::npos) {
          size_t namePos = sig.find(info.name);
          if (namePos != std::string::npos) {
            sig.insert(namePos, clsName + "::");
          }
        }
      }
    }
  }

  wxString wxSig = wxString::FromUTF8(sig.c_str());
  wxSig.Trim(true).Trim(false);

  if (wxSig.IsEmpty()) {
    m_ed->ModalMsgDialog(_("Unable to build function signature."),
                         _("Generate function"),
                         wxOK | wxICON_WARNING);
    return;
  }

  bool isDeclaration = (info.kind == "FunctionDecl" || info.kind == "CXXMethod");

  wxArrayString implExts;
  implExts.Add(wxT("cpp"));
  implExts.Add(wxT("cc"));
  implExts.Add(wxT("cxx"));
  implExts.Add(wxT("c"));

  ArduinoEditor *targetEd = m_ed;

  if (inHeader && isDeclaration) {
    if (ArduinoEditor *implEd = FindSiblingEditorForCurrentFile(implExts, /*createIfMissing=*/true)) {
      targetEd = implEd;

      JumpTarget out;
      if (m_ed->completion->FindSiblingFunctionDefinition(m_ed->GetFilePath(), code, line, column, out)) {
        if (out.file == targetEd->GetFilePath()) {
          std::string name = targetEd->GetFileName();

          int rc = m_ed->ModalMsgDialog(
              wxString::Format(_("An implementation of this function already exists in %s.\n"
                                 "Do you want to open it?"),
                               wxString::FromUTF8(name)),
              _("Generate function"),
              wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);
          if (rc == wxID_YES) {
            frame->PushNavLocation(m_ed->GetFilePath(), line, column);
            frame->HandleGoToLocation(out);
          }

          return;
        }
      }
    }
  }

  wxString targetName = wxString::FromUTF8(targetEd->GetFileName());

  wxTextEntryDialog dlg(
      m_ed,
      wxString::Format(_("Function signature (in %s):"), targetName),
      _("Generate function"),
      wxSig);

  if (dlg.ShowModal() != wxID_OK) {
    return;
  }

  wxSig = dlg.GetValue();
  wxSig.Trim(true).Trim(false);
  if (wxSig.IsEmpty()) {
    m_ed->ModalMsgDialog(_("Function signature cannot be empty."),
                         _("Generate function"),
                         wxOK | wxICON_WARNING);
    return;
  }

  frame->PushNavLocation(m_ed->GetFilePath(), line, column);

  auto *ctrl = targetEd->m_editor;

  wxString needle = wxT("// TODO: implement");

  wxString snippet;
  snippet << wxT("\n\n")
          << wxSig
          << wxT(" {\n");

  EditorSettings settings;
  settings.Load(m_ed->m_config);

  if (settings.useTabs) {
    snippet << wxT("\t");
  } else {
    for (int i = 0; i < settings.tabWidth; i++) {
      snippet << wxT(" ");
    }
  }

  snippet << needle
          << wxT("\n}\n");

  ctrl->BeginUndoAction();

  int insertPos = ctrl->GetTextLength();
  ctrl->InsertText(insertPos, snippet);

  wxString fullText = ctrl->GetText();

  int rel = fullText.find(needle, insertPos);
  int tgtLine, tgtColumn;
  if (rel != wxNOT_FOUND) {
    int todoPos = insertPos + rel;
    ctrl->SetCurrentPos(todoPos);
    ctrl->SetSelection(todoPos, todoPos + (int)needle.Length());
    tgtLine = ctrl->LineFromPosition(todoPos);
    tgtColumn = ctrl->GetColumn(todoPos);
  } else {
    tgtLine = ctrl->LineFromPosition(insertPos);
    tgtColumn = ctrl->GetColumn(insertPos);
  }

  ctrl->EndUndoAction();

  JumpTarget tgt{targetEd->GetFilePath(), tgtLine, tgtColumn};
  frame->HandleGoToLocation(tgt);

  frame->RebuildProject();
}

void ArduinoRefactoring::RefactorCreateDeclarationInHeader() {
  if (!m_ed || !m_ed->m_editor)
    return;

  if (!m_ed->completion || !m_ed->completion->IsTranslationUnitValid()) {
    m_ed->ModalMsgDialog(_("Code completion is not initialized yet."),
                         _("Create declaration in header"),
                         wxOK | wxICON_WARNING);
    return;
  }

  if (isHeaderFile(m_ed->m_filename) || isIno(m_ed->m_filename)) {
    m_ed->ModalMsgDialog(_("This refactoring can only be used in a C/C++ source file (.cpp, .c, ...)."),
                         _("Create declaration in header"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  ArduinoEditorFrame *frame = GetOwnerFrame();
  if (!frame)
    return;

  wxString wxPath = wxString::FromUTF8(m_ed->GetFilePath());
  if (wxPath.empty())
    return;

  wxArrayString hdrExts;
  hdrExts.Add(wxT("hpp"));
  hdrExts.Add(wxT("h"));
  hdrExts.Add(wxT("hh"));

  ArduinoEditor *hdrEd = FindSiblingEditorForCurrentFile(hdrExts, /*createIfMissing=*/true);
  if (!hdrEd || !hdrEd->m_editor) {
    m_ed->ModalMsgDialog(_("Unable to locate or create a header file for this source file."),
                         _("Create declaration in header"),
                         wxOK | wxICON_WARNING);
    return;
  }

  m_ed->completion->InvalidateTranslationUnit();
  m_ed->completion->InitTranslationUnitForIno();

  int line = 0;
  int column = 0;
  m_ed->GetCurrentCursor(line, column);

  std::string code = wxToStd(m_ed->m_editor->GetText());

  HoverInfo info;
  if (!m_ed->completion->GetHoverInfo(m_ed->m_filename, code, line, column, info)) {
    m_ed->ModalMsgDialog(_("No symbol under the cursor."),
                         _("Create declaration in header"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  std::string sig = BuildSignatureWithHoverParams(info.signature, info);

  if (sig.empty() || sig.find('(') == std::string::npos) {
    m_ed->ModalMsgDialog(_("The symbol under the cursor does not look like a function."),
                         _("Create declaration in header"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  AeContainerInfo containerInfo;
  bool hasContainer = m_ed->completion->FindEnclosingContainerInfo(
      m_ed->GetFilePath(), code, line, column, containerInfo);
  bool isClassContainer = hasContainer &&
                          (containerInfo.kind == "ClassDecl" || containerInfo.kind == "StructDecl");

  APP_DEBUG_LOG("EDIT: %s: hasContainer=%d, isClassContainer=%d",
                m_ed->GetFileName().c_str(), hasContainer, isClassContainer);

  std::string headerSig = sig;
  if (isClassContainer) {
    headerSig = StripQualificationFromFunctionSignature(headerSig);
  }

  wxString wxSig = wxString::FromUTF8(headerSig.c_str());
  wxSig.Trim(true).Trim(false);

  if (wxSig.IsEmpty()) {
    m_ed->ModalMsgDialog(_("Unable to build function signature."),
                         _("Create declaration in header"),
                         wxOK | wxICON_WARNING);
    return;
  }

  JumpTarget out;
  if (m_ed->completion->FindSiblingFunctionDefinition(m_ed->GetFilePath(), code, line, column, out)) {
    if (out.file == hdrEd->GetFilePath()) {
      std::string name = hdrEd->GetFileName();

      int rc = m_ed->ModalMsgDialog(
          wxString::Format(_("An declaration of this function already exists in %s.\n"
                             "Do you want to open it?"),
                           wxString::FromUTF8(name)),
          _("Create declaration in header"),
          wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);
      if (rc == wxID_YES) {
        frame->PushNavLocation(m_ed->GetFilePath(), line, column);
        frame->HandleGoToLocation(out);
      }

      return;
    }
  }

  auto *hCtrl = hdrEd->m_editor;
  wxString headerText = hCtrl->GetText();

  wxString headerName = wxString::FromUTF8(hdrEd->GetFileName());

  wxString defaultDecl = wxSig;
  if (!defaultDecl.EndsWith(wxT(";")))
    defaultDecl << wxT(";");

  wxTextEntryDialog dlg(
      m_ed,
      wxString::Format(_("Function declaration (in %s):"), headerName),
      _("Create declaration in header"),
      defaultDecl);

  if (dlg.ShowModal() != wxID_OK) {
    return;
  }

  wxString declLine = dlg.GetValue();
  declLine.Trim(true).Trim(false);
  if (declLine.IsEmpty()) {
    m_ed->ModalMsgDialog(_("Function declaration cannot be empty."),
                         _("Create declaration in header"),
                         wxOK | wxICON_WARNING);
    return;
  }
  if (!declLine.EndsWith(wxT(";")))
    declLine << wxT(";");

  int declLineNo = -1;
  int insertPos = -1;

  std::string hdrPathStd = hdrEd->GetFilePath();

  if (isClassContainer &&
      !containerInfo.filename.empty() &&
      containerInfo.filename == hdrPathStd) {

    const auto &members = containerInfo.members;

    std::vector<const SymbolInfo *> methodSyms;
    std::vector<const SymbolInfo *> anySyms;

    for (const auto &m : members) {
      if (m.file != hdrPathStd)
        continue;

      anySyms.push_back(&m);

      if (m.kind == CXCursor_CXXMethod ||
          m.kind == CXCursor_Constructor ||
          m.kind == CXCursor_Destructor) {
        methodSyms.push_back(&m);
        APP_DEBUG_LOG("EDIT: adding symbol %s", m.display.c_str());
      }
    }

    bool insertAfterRef = false;
    int refLine = -1;

    if (!methodSyms.empty()) {
      const SymbolInfo *lastMethod =
          *std::max_element(methodSyms.begin(), methodSyms.end(),
                            [](const SymbolInfo *a, const SymbolInfo *b) {
                              if (a->line != b->line)
                                return a->line < b->line;
                              return a->column < b->column;
                            });

      refLine = (lastMethod->line > 0) ? (lastMethod->line - 1) : 0;
      if (refLine < 0)
        refLine = 0;

      insertPos = hCtrl->GetLineEndPosition(refLine);
      insertAfterRef = true;
    } else if (!anySyms.empty()) {
      const SymbolInfo *firstSym =
          *std::min_element(anySyms.begin(), anySyms.end(),
                            [](const SymbolInfo *a, const SymbolInfo *b) {
                              if (a->line != b->line)
                                return a->line < b->line;
                              return a->column < b->column;
                            });

      refLine = (firstSym->line > 0) ? (firstSym->line - 1) : 0;
      if (refLine < 0)
        refLine = 0;

      insertPos = hCtrl->PositionFromLine(refLine);
      insertAfterRef = false;
    } else {
      auto lineColToOffset = [](const std::string &text, int line1, int col1) -> size_t {
        if (line1 <= 0)
          return 0;

        size_t offset = 0;
        int currentLine = 1;

        while (offset < text.size() && currentLine < line1) {
          if (text[offset] == '\n') {
            ++currentLine;
          }
          ++offset;
        }

        if (col1 > 1) {
          int toSkip = col1 - 1;
          while (offset < text.size() && toSkip > 0 && text[offset] != '\n') {
            ++offset;
            --toSkip;
          }
        }

        return offset;
      };

      std::string headerStd = wxToStd(headerText);

      APP_DEBUG_LOG("EDIT: lineColToOffset(); line=%d, column=%d, text=\n%s",
                    containerInfo.line, containerInfo.column, headerStd.c_str());

      size_t startOffset = lineColToOffset(
          headerStd,
          containerInfo.line,
          containerInfo.column);

      size_t bracePos = headerStd.find('{', startOffset);
      if (bracePos == std::string::npos) {
        insertPos = -1;
        refLine = -1;
      } else {
        int depth = 0;
        size_t i = bracePos;
        for (; i < headerStd.size(); ++i) {
          char c = headerStd[i];
          if (c == '{') {
            ++depth;
          } else if (c == '}') {
            --depth;
            if (depth == 0)
              break;
          }
        }

        if (i >= headerStd.size()) {
          insertPos = -1;
          refLine = -1;
        } else {
          int braceLine = hCtrl->LineFromPosition(static_cast<int>(i));
          if (braceLine < 0)
            braceLine = 0;

          refLine = braceLine - 1;
          insertPos = hCtrl->PositionFromLine(braceLine);
          insertAfterRef = false;
        }
      }
    }

    APP_DEBUG_LOG("EDIT: insertPos=%d, refLine=%d", insertPos, refLine);

    if (insertPos >= 0 && refLine >= 0) {
      wxString refLineText = hCtrl->GetLine(refLine);
      wxString indent = ExtractIndent(refLineText);

      frame->PushNavLocation(m_ed->GetFilePath(), line, column);

      hCtrl->BeginUndoAction();

      int declPos = -1;

      if (insertAfterRef) {
        wxString toInsert;
        toInsert << wxT("\n")
                 << indent
                 << declLine;

        hCtrl->InsertText(insertPos, toInsert);

        declPos = insertPos + 1 + (int)indent.Length();
      } else {
        wxString toInsert;
        toInsert << indent
                 << declLine
                 << wxT("\n");

        hCtrl->InsertText(insertPos, toInsert);

        declPos = insertPos + (int)indent.Length();
      }

      if (declPos < 0)
        declPos = insertPos;

      declLineNo = hCtrl->LineFromPosition(declPos);

      hCtrl->EndUndoAction();

      JumpTarget tgt;
      tgt.file = hdrEd->GetFilePath();
      tgt.line = declLineNo + 1;
      tgt.column = 1;
      frame->HandleGoToLocation(tgt);

      frame->RebuildProject();
      return;
    }
  }

  APP_DEBUG_LOG("EDIT: fallback");
  int textLen = hCtrl->GetTextLength();
  insertPos = textLen;

  wxString suffix = wxT("\n");
  if (!headerText.EndsWith(wxT("\n\n"))) {
    if (!headerText.EndsWith(wxT("\n")))
      suffix.Prepend(wxT("\n"));
  }

  frame->PushNavLocation(m_ed->GetFilePath(), line, column);

  hCtrl->BeginUndoAction();
  hCtrl->InsertText(insertPos, suffix + declLine + wxT("\n"));
  int declPos = insertPos + (int)suffix.Length();
  declLineNo = hCtrl->LineFromPosition(declPos);
  hCtrl->EndUndoAction();

  JumpTarget tgt;
  tgt.file = hdrEd->GetFilePath();
  tgt.line = declLineNo + 1;
  tgt.column = 1;
  frame->HandleGoToLocation(tgt);

  frame->RebuildProject();
}

bool ArduinoRefactoring::RunClangFormatOnEditor(bool selectionOnly, wxString *errorOut) {
  if (!m_ed) {
    if (errorOut)
      *errorOut = wxT("Editor is not initialized.");
    return false;
  }

  wxStyledTextCtrl *stc = m_ed->m_editor;

  if (stc->GetReadOnly()) {
    if (errorOut)
      *errorOut = _("Document is read-only.");
    return false;
  }

  const std::string sketchRootStd = m_ed->arduinoCli->GetSketchPath();
  wxString sketchRoot = wxString::FromUTF8(sketchRootStd);

  if (sketchRoot.IsEmpty()) {
    if (errorOut)
      *errorOut = wxT("Sketch root path is empty.");
    return false;
  }

  // Bundled clang-format path
  wxString clangFormatPath = GetBundledClangFormatPath();
  if (!wxFileExists(clangFormatPath)) {
    if (errorOut) {
      *errorOut = wxString::Format(wxT("clang-format not found: %s"), clangFormatPath);
    }
    return false;
  }

  // Prepare .ardedit directory
  wxString ardeditDir = EnsureArdeditDir(sketchRoot);
  if (ardeditDir.IsEmpty()) {
    if (errorOut)
      *errorOut = wxT("Failed to create .ardedit directory.");
    return false;
  }

  // Decide which config to use
  wxString configPath = FindProjectClangFormatFile(sketchRoot);
  if (configPath.IsEmpty()) {
    EditorSettings settings;
    settings.Load(m_ed->m_config);

    configPath = BuildTempClangFormatFile(ardeditDir, settings);
    if (configPath.IsEmpty()) {
      if (errorOut)
        *errorOut = wxT("Failed to create temporary clang-format config.");
      return false;
    }
  }

  // Save source to temp file
  wxString tempFileName = PickFormatTempFilename(wxString::FromUTF8(m_ed->GetFileName()));
  wxFileName srcFn(ardeditDir, tempFileName);
  wxString srcPath = srcFn.GetFullPath();

  std::string srcUtf8 = wxToStd(stc->GetText());
  if (!SaveFileFromString(wxToStd(srcPath), srcUtf8)) {
    if (errorOut)
      *errorOut = wxString::Format(wxT("Failed to write temp source file: %s"), srcPath);
    return false;
  }

  // Build command line
  int fromLine1 = 0, toLine1 = 0;
  bool hasSel = false;
  if (selectionOnly) {
    hasSel = ComputeSelectedLines1Based(stc, fromLine1, toLine1);
    if (!hasSel) {
      if (errorOut)
        *errorOut = wxT("No selection to format.");
      return false;
    }
  }

  // clang-format -i -style=file:<configPath> [--lines=a:b] <srcPath>
  std::string cmd;
  cmd += QuoteArgSimple(wxToStd(clangFormatPath));
  cmd += " -i";
  cmd += " ";
  cmd += QuoteArgSimple(std::string("-style=file:") + wxToStd(configPath));

  if (selectionOnly && hasSel) {
    cmd += " ";
    cmd += wxToStd(wxString::Format(wxT("--lines=%d:%d"), fromLine1, toLine1));
  }

  cmd += " ";
  cmd += QuoteArgSimple(wxToStd(srcPath));

  std::string output;
  int rc = ArduinoCli::ExecuteCommand(cmd, output);

  if (rc != 0) {
    if (errorOut) {
      wxString msg;
      msg << wxT("clang-format failed.\n\n")
          << wxString::Format(wxT("Command:\n%s\n\n"), wxString::FromUTF8(cmd))
          << wxT("Output:\n")
          << wxString::FromUTF8(output);
      *errorOut = msg;
    }
    return false;
  }

  // Load formatted result
  std::string formatted;
  if (!LoadFileToString(wxToStd(srcPath), formatted)) {
    if (errorOut)
      *errorOut = wxT("Failed to read formatted temp file.");
    return false;
  }

  // Preserve caret position (line/col)
  int oldPos = stc->GetCurrentPos();
  int oldLine = stc->LineFromPosition(oldPos);
  int oldCol = stc->GetColumn(oldPos);

  stc->BeginUndoAction();
  stc->SetText(wxString::FromUTF8(formatted));
  stc->EndUndoAction();

  // Restore caret approximately
  int lineCount = stc->GetLineCount();
  if (lineCount <= 0)
    lineCount = 1;

  int newLine = oldLine;
  if (newLine < 0)
    newLine = 0;
  if (newLine >= lineCount)
    newLine = lineCount - 1;

  int lineStart = stc->PositionFromLine(newLine);
  int lineEnd = stc->GetLineEndPosition(newLine);
  int maxCol = lineEnd - lineStart;
  int newCol = oldCol;
  if (newCol < 0)
    newCol = 0;
  if (newCol > maxCol)
    newCol = maxCol;

  int newPos = lineStart + newCol;
  stc->GotoPos(newPos);
  stc->SetSelection(newPos, newPos); // clear selection

  return true;
}

void ArduinoRefactoring::RefactorFormatSelection() {
  wxString err;
  if (!RunClangFormatOnEditor(/*selectionOnly=*/true, &err)) {
    m_ed->ModalMsgDialog(err, _("Format selection"), wxOK | wxICON_WARNING);
  }
}

void ArduinoRefactoring::RefactorFormatWholeFile() {
  wxString err;
  if (!RunClangFormatOnEditor(/*selectionOnly=*/false, &err)) {
    m_ed->ModalMsgDialog(err, _("Format file"), wxOK | wxICON_WARNING);
  }
}

ArduinoEditor *ArduinoRefactoring::FindSiblingEditorForCurrentFile(const wxArrayString &searchExts,
                                                                   bool createIfMissing) {
  ArduinoEditorFrame *frame = GetOwnerFrame();
  if (!frame || !m_ed)
    return nullptr;

  wxString wxPath = wxString::FromUTF8(m_ed->GetFilePath());
  if (wxPath.empty())
    return nullptr;

  wxFileName fn(wxPath);
  wxString baseName = fn.GetName();
  wxString dir = fn.GetPath();

  std::vector<SketchFileBuffer> files;
  frame->CollectEditorSources(files);

  for (auto ext : searchExts) {
    wxFileName cand(dir, baseName, ext);
    wxString full = cand.GetFullPath();
    std::string fullStd = wxToStd(full);

    for (const auto &f : files) {
      if (f.filename == fullStd) {
        if (ArduinoEditor *ed = frame->FindEditorWithFile(fullStd)) {
          return ed;
        }
      }
    }
  }

  if (!createIfMissing)
    return nullptr;

  wxString srcExt = fn.GetExt();

  wxString targetExt = ConvertSourceHeaderExtension(srcExt);
  if (targetExt.IsEmpty()) {
    return nullptr;
  }

  wxFileName newHdr(dir, baseName, targetExt);

  wxString targetFileType;
  if (isHeaderFile(m_ed->m_filename)) {
    targetFileType = _("implementation");
  } else if (isSourceFile(m_ed->m_filename)) {
    targetFileType = _("declaration");
  }

  int rc = m_ed->ModalMsgDialog(
      wxString::Format(_("The %s file %s does not exist.\nDo you want to create it?"),
                       targetFileType.c_str(), newHdr.GetFullName()),
      _("Refactor"),
      wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);

  if (rc != wxID_YES) {
    return nullptr;
  }

  std::string initSource;
  std::string ext = wxToStd(targetExt);

  if (isSourceExt(ext)) {
    initSource = "\n#include \"" + m_ed->GetFileName() /*relative to project root*/ + "\"\n\n";
  } else if (isHeaderExt(ext)) {
    initSource = "\n#pragma once\n\n";
    if (ext != "c") {
      std::string expectedClassName;

      int curLine = 0;
      int curCol = 0;
      m_ed->GetCurrentCursor(curLine, curCol);
      int stcLine = curLine > 0 ? curLine - 1 : 0;

      wxStyledTextCtrl *stc = m_ed->m_editor;
      int lineCount = stc->GetLineCount();

      auto tryLine = [&](int line) -> std::string {
        if (line < 0 || line >= lineCount)
          return std::string();

        wxString wxLineText = stc->GetLine(line);
        std::string s = wxToStd(wxLineText);
        if (s.empty())
          return std::string();

        std::size_t posScope = s.rfind("::");
        if (posScope == std::string::npos)
          return std::string();

        int i = static_cast<int>(posScope) - 1;
        while (i >= 0 && (s[i] == ':' || s[i] == ' ' || s[i] == '\t'))
          --i;
        if (i < 0)
          return std::string();

        int end = i;
        while (i >= 0) {
          unsigned char c = static_cast<unsigned char>(s[i]);
          if (!(std::isalnum(c) || c == '_'))
            break;
          --i;
        }
        int start = i + 1;
        if (start > end)
          return std::string();

        std::string ident = s.substr(start, end - start + 1);
        ident = TrimCopy(ident);
        return ident;
      };

      std::string className;
      for (int delta : {0, -1, 1}) {
        int line = stcLine + delta;
        className = tryLine(line);
        if (!className.empty())
          break;
      }

      if (!className.empty()) {
        expectedClassName = className;
      } else {
        expectedClassName = wxToStd(baseName);
      }

      EditorSettings settings;
      settings.Load(m_ed->m_config);

      std::string indent;
      if (settings.useTabs) {
        indent = "\t";
      } else {
        for (int i = 0; i < settings.tabWidth; i++) {
          indent += " ";
        }
      }

      initSource = "class " + expectedClassName + " {\n";
      initSource += "private:\n";
      initSource += indent + "\n";
      initSource += "};\n";
    }
  }

  wxString full = newHdr.GetFullPath();
  std::string fullStd = wxToStd(full);

  int line, column;
  m_ed->GetCurrentCursor(line, column);
  frame->PushNavLocation(m_ed->GetFilePath(), line, column);

  frame->CreateNewSketchFile(full);

  ArduinoEditor *ed = frame->FindEditorWithFile(fullStd);

  if (!ed) {
    return nullptr;
  }

  ed->m_editor->SetText(wxString::FromUTF8(initSource));
  ed->Save();

  JumpTarget openTgt;
  openTgt.file = fullStd;
  openTgt.line = 0;
  openTgt.column = 0;
  frame->HandleGoToLocation(openTgt);
  return frame->FindEditorWithFile(fullStd);
}

int ArduinoRefactoring::RefactorRenameSymbol(const std::vector<JumpTarget> &occs,
                                             const std::string &oldName,
                                             const std::string &newName) {
  if (!m_ed || !m_ed->m_editor)
    return 0;

  auto *stc = m_ed->m_editor;

  int replacements = 0;

  stc->BeginUndoAction();

  for (const auto &o : occs) {
    int l = static_cast<int>(o.line) - 1;
    int c = static_cast<int>(o.column) - 1;
    if (l < 0 || c < 0)
      continue;

    int startPos = stc->PositionFromLine(l) + c;
    if (startPos < 0 || startPos > stc->GetTextLength())
      continue;

    wxString currentText = stc->GetTextRange(startPos, startPos + (int)oldName.size());
    if (wxToStd(currentText) != oldName) {
      continue;
    }

    stc->SetTargetStart(startPos);
    stc->SetTargetEnd(startPos + (int)oldName.size());
    stc->ReplaceTarget(wxString::FromUTF8(newName));

    ++replacements;
  }

  stc->EndUndoAction();

  return replacements;
}

void ArduinoRefactoring::SortIncludesInCurrentFile() {
  if (!m_ed || !m_ed->m_editor)
    return;

  auto *stc = m_ed->m_editor;
  if (stc->GetReadOnly())
    return;

  wxString text = stc->GetText();
  if (text.IsEmpty())
    return;

  bool hadTrailingNewline = text.EndsWith(wxT("\n"));

  wxArrayString lines;
  {
    wxStringTokenizer tok(text, wxT("\n"), wxTOKEN_RET_EMPTY);
    while (tok.HasMoreTokens()) {
      lines.Add(tok.GetNextToken());
    }
  }

  wxString outText;
  outText.reserve(text.Length());

  const int lineCount = (int)lines.size();
  int i = 0;

  while (i < lineCount) {
    ParsedInclude pi;

    if (!ParseIncludeLine(lines[i], pi)) {
      if (!outText.IsEmpty())
        outText += wxT("\n");
      outText += lines[i];
      ++i;
      continue;
    }

    std::vector<ParsedInclude> block;
    block.push_back(pi);

    int j = i + 1;
    while (j < lineCount) {
      ParsedInclude pj;
      if (!ParseIncludeLine(lines[j], pj))
        break;
      block.push_back(pj);
      ++j;
    }

    std::vector<ParsedInclude> cleaned = block;

    std::sort(cleaned.begin(), cleaned.end(),
              [](const ParsedInclude &a, const ParsedInclude &b) {
                if (a.isSystem != b.isSystem) {
                  return a.isSystem && !b.isSystem; // system includes first
                }
                return a.header.Cmp(b.header) < 0;
              });

    // deduplication
    struct Key {
      bool isSystem;
      wxString header;
      bool operator<(const Key &o) const {
        if (isSystem != o.isSystem)
          return isSystem > o.isSystem;
        return header.Cmp(o.header) < 0;
      }
    };

    std::set<Key> seen;

    for (const auto &inc : cleaned) {
      Key k{inc.isSystem, inc.header};
      if (!seen.insert(k).second)
        continue;

      if (!outText.IsEmpty())
        outText += wxT("\n");

      wxString line;
      line << inc.leading
           << wxT("#include ")
           << (inc.isSystem ? wxT("<") : wxT("\""))
           << inc.header
           << (inc.isSystem ? wxT(">") : wxT("\""));

      wxString trailing = inc.trailing;
      trailing.Trim(true);
      if (!trailing.IsEmpty()) {
        line << wxT(" ") << trailing;
      }

      outText += line;
    }

    i = j;
  }

  if (hadTrailingNewline && !outText.IsEmpty() && !outText.EndsWith(wxT("\n"))) {
    outText += wxT("\n");
  }

  stc->BeginUndoAction();

  int curPos = stc->GetCurrentPos();
  int curLine = stc->LineFromPosition(curPos);
  int curCol = stc->GetColumn(curPos);

  stc->SetText(outText);

  int newLineCount = stc->GetLineCount();
  if (curLine >= newLineCount)
    curLine = newLineCount - 1;
  if (curLine < 0)
    curLine = 0;

  int newPos = stc->PositionFromLine(curLine) + curCol;
  if (newPos > stc->GetTextLength())
    newPos = stc->GetTextLength();

  stc->GotoPos(newPos);
  stc->SetCurrentPos(newPos);
  stc->SetSelection(newPos, newPos);

  stc->EndUndoAction();

  if (auto *frame = GetOwnerFrame()) {
    frame->RebuildProject();
  }
}

void ArduinoRefactoring::RefactorOrganizeIncludes() {
  if (!m_ed || !m_ed->m_editor)
    return;

  auto *stc = m_ed->m_editor;
  auto *completion = m_ed->completion;

  if (stc->GetReadOnly())
    return;

  if (!completion || !completion->IsTranslationUnitValid()) {
    m_ed->ModalMsgDialog(_("Code completion is not initialized yet."),
                         _("Organize includes"),
                         wxOK | wxICON_WARNING);
    return;
  }

  completion->InvalidateTranslationUnit(); // we need fresh TU

  std::string code = wxToStd(stc->GetText());

  std::vector<IncludeUsage> includes;
  if (!m_ed->completion->AnalyzeIncludes(m_ed->m_filename, code, includes)) {
    m_ed->ModalMsgDialog(_("Unable to analyze includes for this file."),
                         _("Organize includes"),
                         wxOK | wxICON_WARNING);
    return;
  }

  if (includes.empty()) {
    m_ed->ModalMsgDialog(_("No #include directives found in this file."),
                         _("Organize includes"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  // Prepare data for dialog
  std::vector<OrgIncludeItem> items;
  items.reserve(includes.size());

  for (const auto &u : includes) {
    OrgIncludeItem it;
    it.usage = u;

    int stcLine = (int)u.line - 1;
    if (stcLine >= 0 && stcLine < stc->GetLineCount()) {
      it.displayText = stc->GetLine(stcLine);
    } else {
      it.displayText = wxString::Format(wxT("#include (?)  // line %u"), u.line);
    }

    it.displayText.Trim(true).Trim(false);
    it.remove = !u.used;

    items.push_back(std::move(it));
  }

  ArduinoRefactoringOrgIncludes dlg(m_ed, items, m_ed->m_config);
  if (dlg.ShowModal() != wxID_OK) {
    return;
  }

  bool doSort = dlg.GetSortIncludes();

  std::vector<IncludeUsage> toRemove;
  dlg.GetIncludesToRemove(toRemove);

  if (toRemove.empty() && !doSort) {
    return;
  }

  // delete selected includes - from end
  std::sort(toRemove.begin(), toRemove.end(),
            [](const IncludeUsage &a, const IncludeUsage &b) {
              return a.line > b.line; // desc
            });

  stc->BeginUndoAction();

  for (const auto &u : toRemove) {
    int stcLine = (int)u.line - 1;
    if (stcLine < 0 || stcLine >= stc->GetLineCount())
      continue;

    int startPos = stc->PositionFromLine(stcLine);
    int endPos;
    if (stcLine + 1 < stc->GetLineCount()) {
      endPos = stc->PositionFromLine(stcLine + 1);
    } else {
      endPos = stc->GetTextLength();
    }

    stc->SetTargetStart(startPos);
    stc->SetTargetEnd(endPos);
    stc->ReplaceTarget(wxEmptyString);
  }

  if (doSort) {
    SortIncludesInCurrentFile();
  }

  stc->EndUndoAction();

  if (auto *frame = GetOwnerFrame()) {
    frame->RebuildProject();
  }
}

void ArduinoRefactoring::RefactorExtractFunction() {
  if (!m_ed || !m_ed->m_editor)
    return;

  auto *stc = m_ed->m_editor;
  auto *completion = m_ed->completion;

  if (stc->GetReadOnly())
    return;

  if (!completion || !completion->IsTranslationUnitValid()) {
    m_ed->ModalMsgDialog(_("Code completion is not initialized yet."),
                         _("Extract function"),
                         wxOK | wxICON_WARNING);
    return;
  }

  completion->InvalidateTranslationUnit(); // we need fresh TU

  int selStart = stc->GetSelectionStart();
  int selEnd = stc->GetSelectionEnd();
  if (selStart == selEnd) {
    m_ed->ModalMsgDialog(_("Please select a block of code to extract."),
                         _("Extract function"),
                         wxOK | wxICON_INFORMATION);
    return;
  }
  if (selStart > selEnd)
    std::swap(selStart, selEnd);

  wxString selText = stc->GetTextRange(selStart, selEnd);
  if (selText.Trim(true).Trim(false).IsEmpty()) {
    m_ed->ModalMsgDialog(_("The selection contains only whitespace. Please select a valid block of code."),
                         _("Extract function"),
                         wxOK | wxICON_INFORMATION);
    return;
  }

  int curLine = 0, curCol = 0;
  m_ed->GetCurrentCursor(curLine, curCol);

  int startLine = stc->LineFromPosition(selStart);
  int endLine = stc->LineFromPosition(selEnd);

  int lineStartPos = stc->PositionFromLine(startLine);
  int lineEndPos;
  if (endLine + 1 < stc->GetLineCount())
    lineEndPos = stc->PositionFromLine(endLine + 1);
  else
    lineEndPos = stc->GetTextLength();

  selStart = lineStartPos;
  selEnd = lineEndPos;
  selText = stc->GetTextRange(selStart, selEnd);

  wxString firstLineText = stc->GetLine(startLine);
  wxString blockIndent = ExtractIndent(firstLineText);

  wxTextEntryDialog nameDlg(
      m_ed,
      _("Function name:"),
      _("Extract function"),
      _("extractedFunction"));

  if (nameDlg.ShowModal() != wxID_OK) {
    return;
  }

  wxString funcNameWx = nameDlg.GetValue();
  funcNameWx.Trim(true).Trim(false);

  if (funcNameWx.IsEmpty()) {
    m_ed->ModalMsgDialog(_("Function name cannot be empty."),
                         _("Extract function"),
                         wxOK | wxICON_WARNING);
    return;
  }

  std::string funcNameStd = wxToStd(funcNameWx);
  if (!LooksLikeIdentifier(funcNameStd)) {
    m_ed->ModalMsgDialog(_("Function name is not a valid identifier."),
                         _("Extract function"),
                         wxOK | wxICON_WARNING);
    return;
  }

  int startLine1 = startLine + 1;
  int startCol1 = 1;

  int endPosExclusive = selEnd;
  if (endPosExclusive > 0)
    --endPosExclusive;

  int endLineIdx = stc->LineFromPosition(endPosExclusive);
  int endCol0 = stc->GetColumn(endPosExclusive);
  int endLine1 = endLineIdx + 1;
  int endCol1 = endCol0 + 1;

  std::string code = wxToStd(stc->GetText());

  ExtractFunctionAnalysis analysis;
  if (!completion->AnalyzeExtractFunction(m_ed->m_filename,
                                          code,
                                          startLine1, startCol1,
                                          endLine1, endCol1,
                                          analysis) ||
      !analysis.success) {
    m_ed->ModalMsgDialog(_("Unable to analyze the selected block."),
                         _("Extract function"),
                         wxOK | wxICON_WARNING);
    return;
  }

  wxString returnType = wxString::FromUTF8(analysis.returnType.c_str());
  if (returnType.IsEmpty())
    returnType = wxT("void");

  wxString paramList;
  wxString callArgs;

  for (size_t i = 0; i < analysis.parameters.size(); ++i) {
    const auto &p = analysis.parameters[i];

    wxString pType = wxString::FromUTF8(p.type.c_str());
    wxString pName = wxString::FromUTF8(p.name.c_str());

    if (!paramList.IsEmpty())
      paramList << wxT(", ");
    paramList << pType << wxT(" ") << pName;

    if (!callArgs.IsEmpty())
      callArgs << wxT(", ");
    callArgs << pName;
  }

  AeContainerInfo containerInfo;
  bool hasContainer = completion->FindEnclosingContainerInfo(
      m_ed->GetFilePath(),
      code,
      startLine1,
      startCol1,
      containerInfo);

  bool isClassContainer = hasContainer &&
                          (containerInfo.kind == "ClassDecl" ||
                           containerInfo.kind == "StructDecl");

  wxString qualifiedFuncName = funcNameWx;
  if (isClassContainer && !containerInfo.name.empty()) {
    qualifiedFuncName = wxString::FromUTF8(containerInfo.name.c_str()) +
                        wxT("::") +
                        funcNameWx;
  }

  int minIndentCols = -1;
  wxArrayString lines;
  {
    wxStringTokenizer tok(selText, wxT("\n"), wxTOKEN_RET_EMPTY);
    while (tok.HasMoreTokens()) {
      lines.Add(tok.GetNextToken());
    }
  }

  for (auto &line : lines) {
    wxString trimmed = line;
    trimmed.Trim(true).Trim(false);
    if (trimmed.IsEmpty())
      continue;

    int col = 0;
    for (int i = 0; i < (int)line.Length(); ++i) {
      wxChar ch = line[i];
      if (ch == ' ' || ch == '\t')
        ++col;
      else
        break;
    }

    if (minIndentCols < 0 || col < minIndentCols)
      minIndentCols = col;
  }

  if (minIndentCols < 0)
    minIndentCols = 0;

  wxString bodyText;
  for (size_t i = 0; i < lines.size(); ++i) {
    wxString line = lines[i];

    int col = 0;
    int idx = 0;
    while (idx < (int)line.Length() && col < minIndentCols) {
      wxChar ch = line[idx];
      if (ch == ' ' || ch == '\t') {
        ++col;
        ++idx;
      } else {
        break;
      }
    }

    wxString stripped = line.Mid(idx);
    bodyText += stripped;
    if (i + 1 < lines.size())
      bodyText += wxT("\n");
  }

  EditorSettings settings;
  settings.Load(m_ed->m_config);

  wxString funcIndent;
  wxString bodyIndent;
  if (settings.useTabs) {
    bodyIndent = wxT("\t");
  } else {
    for (int i = 0; i < settings.tabWidth; ++i)
      bodyIndent += wxT(" ");
  }

  // prepare function body
  wxArrayString bodyLines;
  {
    wxStringTokenizer tok(bodyText, wxT("\n"), wxTOKEN_RET_EMPTY);
    while (tok.HasMoreTokens())
      bodyLines.Add(tok.GetNextToken());
  }

  wxString finalBody;
  for (size_t i = 0; i < bodyLines.size(); ++i) {
    wxString line = bodyLines[i];
    if (!line.IsEmpty())
      finalBody << bodyIndent << line;
    if (i + 1 < bodyLines.size())
      finalBody << wxT("\n");
  }

  wxString newFunction;
  newFunction << wxT("\n")
              << funcIndent
              << returnType
              << wxT(" ")
              << qualifiedFuncName
              << wxT("(")
              << paramList
              << wxT(")")
              << wxT(" {\n")
              << finalBody
              << wxT("\n}\n\n");

  // replace block with function call
  wxString callLine;
  callLine << blockIndent
           << funcNameWx
           << wxT("(")
           << callArgs
           << wxT(");\n");

  ArduinoEditorFrame *frame = GetOwnerFrame();
  if (frame) {

    frame->PushNavLocation(m_ed->GetFilePath(), curLine, curCol);
  }

  const int newFuncLen = (int)newFunction.Length();

  stc->BeginUndoAction();

  int insertPos = -1;
  if (analysis.enclosingFuncLine > 0) {
    int funcLine0 = analysis.enclosingFuncLine - 1; // 0-based
    if (funcLine0 >= 0 && funcLine0 < stc->GetLineCount()) {
      insertPos = stc->PositionFromLine(funcLine0);
    }
  }
  if (insertPos < 0) {
    // fallback: file end
    insertPos = stc->GetTextLength();
  }

  if (insertPos <= selStart) {
    // first we insert a new function above the selection
    stc->InsertText(insertPos, newFunction);

    // we move the selection, because the text has moved
    selStart += newFuncLen;
    selEnd += newFuncLen;

    // we replace the block with a call
    stc->SetTargetStart(selStart);
    stc->SetTargetEnd(selEnd);
    stc->ReplaceTarget(callLine);
  } else {
    // first we replace the block, then we insert the function (if it is after it)
    stc->SetTargetStart(selStart);
    stc->SetTargetEnd(selEnd);
    stc->ReplaceTarget(callLine);

    stc->InsertText(insertPos, newFunction);
  }

  stc->EndUndoAction();

  // jump to embedded function
  int tgtPos = insertPos + (int)(funcIndent.Length() + returnType.Length() + 1);

  frame->PushNavLocation(m_ed->GetFilePath(), curLine, curCol);

  stc->GotoPos(tgtPos);
  stc->SetCurrentPos(tgtPos);
  stc->SetSelection(tgtPos + 1, tgtPos + (int)funcNameWx.Length() + 1);

  if (auto *frame2 = GetOwnerFrame()) {
    frame2->RebuildProject();
  }
}
