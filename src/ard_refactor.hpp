// ard_refactor.hpp
#pragma once

#include <string>
#include <vector>

#include <wx/arrstr.h>
#include <wx/string.h>

class ArduinoEditor;
class ArduinoEditorFrame;
struct JumpTarget;
struct AeContainerInfo;
struct SymbolInfo;

class ArduinoRefactoring {
public:
  explicit ArduinoRefactoring(ArduinoEditor *editor);

  // high-level refactorings
  void RefactorRenameSymbolAtCursor();
  void RefactorIntroduceVariable();
  void RefactorInlineVariable();
  void RefactorGenerateFunctionFromCursor();
  void RefactorCreateDeclarationInHeader();
  void RefactorFormatSelection();
  void RefactorFormatWholeFile();

  // low-level rename implementation (used from frame)
  int RefactorRenameSymbol(const std::vector<JumpTarget> &occs,
                           const std::string &oldName,
                           const std::string &newName);

  void RefactorOrganizeIncludes();
  void RefactorExtractFunction();

private:
  ArduinoEditor *m_ed;

  // helpers
  ArduinoEditorFrame *GetOwnerFrame();

  bool RunClangFormatOnEditor(bool selectionOnly, wxString *errorOut);

  ArduinoEditor *FindSiblingEditorForCurrentFile(const wxArrayString &searchExts,
                                                 bool createIfMissing);

  wxString ConvertSourceHeaderExtension(const wxString &srcExt);

  void SortIncludesInCurrentFile();
};
