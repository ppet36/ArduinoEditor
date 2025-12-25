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
