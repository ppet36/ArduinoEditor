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

#include "ard_cli.hpp"
#include "ard_ev.hpp"
#include "ard_setdlg.hpp"
#include "utils.hpp"
#include <atomic>
#include <chrono>
#include <clang-c/Index.h>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <wx/rawbmp.h>
#include <wx/stc/stc.h>
#include <wx/wx.h>

struct CompletionItem {
  std::string text;
  std::string type;
  std::string label;
  int priority;
  CXCursorKind kind;

  std::string file;
  bool fromSketch = false;

  std::vector<CompletionItem> overloads;
};

struct ArduinoParseError {
  std::string file;
  unsigned line;
  unsigned column;
  std::string message;
  CXDiagnosticSeverity severity;
  std::vector<ArduinoParseError> childs; // clang notes for main error/warning

  const char *SeverityString() const;
  std::string ToString() const;
};

struct ParameterInfo {
  std::string name; // can be empty (e.g. for anonymous parameters)
  std::string type; // text of type as returned by clang (including const &, etc.)
};

struct HoverInfo {
  std::string name;      // symbol name (Serial, digitalWrite, ...)
  std::string kind;      // type of symbol (function, variable, macro, ...)
  std::string type;      // type (int, HardwareSerial, ...)
  std::string signature; // nicer signature (e.g. "void digitalWrite(uint8_t pin, uint8_t val)")
  std::string usr;
  std::string briefComment; // short comment (doxygen @brief, ///)
  std::string fullComment;  // entire comment block (/** ... */)
  std::vector<ParameterInfo> parameters;

  std::string ToHoverString() const;
};

struct JumpTarget {
  std::string file;
  int line = 0;
  int column = 0;
};

struct SymbolInfo {
  std::string name;    // name (digitalWrite, dsTempObyvak, MyClass...)
  std::string display; // nicer text (signature, etc.)
  std::string file;    // full path to the file
  std::string usr;
  int line = 0;
  int column = 0;
  CXCursorKind kind; // for icons / filter
  std::vector<ParameterInfo> parameters;

  int bodyLineFrom = 0;
  int bodyColFrom = 0;
  int bodyLineTo = 0;
  int bodyColTo = 0;
};

struct ExtractFunctionAnalysis {
  bool success = false;
  std::string returnType;
  std::vector<ParameterInfo> parameters;
  // Enclosing function/method
  std::string enclosingFuncFile; // full path
  int enclosingFuncLine = 0;     // 1-based, in original file (after subtracting addedLines)
  int enclosingFuncColumn = 0;   // 1-based
};

struct CompletionMetadata {
  int m_lastWordStart;
  int m_lastLengthEntered;
  uint64_t m_pendingRequestId;
  std::vector<CompletionItem> m_lastCompletions;
};

struct ClangUnsavedFiles {
  CXUnsavedFile files[2]; // max. two files, .ino.cpp + .ino.hpp in worst case
  unsigned count = 0;
  int hppAddedLines = 0; // added lines due to includes manipulation

  std::string mainFilename;
  std::string mainCode;
  std::string hppFilename;
  std::string hppCode;
};

struct CachedTranslationUnit {
  std::string filename;     // normalized original filename (map key)
  std::string mainFilename; // the actual "clang" filename (.ino.cpp, .cpp, ...)
  std::size_t codeHash = 0; // FNV-1a hash of the original code
  int addedLines = 0;       // line shift due to inserted .hpp
  CXTranslationUnit tu = nullptr;
};

struct ProjectTuEntry {
  std::string key;          // abs original filename (.ino/.cpp)
  std::string mainFilename; // clang main filename (e.g. .ino.cpp)
  std::size_t codeHash = 0;
  std::size_t headersSigHash = 0; // hash of opened headers (unsaved)
  std::size_t argsHash = 0;       // hash clang args (+ file-specific extras)
  CXTranslationUnit tu = nullptr;

  // Diagnostics filtered/sorted (CollectDiagnosticsLocked)
  std::vector<ArduinoParseError> cachedErrors;
};

struct SymbolCacheEntry {
  std::string filename;     // absolute filename
  std::size_t codeHash = 0; // hash of the code when the symbols were counted
  std::vector<SymbolInfo> symbols;

  // when we last recalculated the symbols
  std::chrono::steady_clock::time_point lastUpdated{};
};

struct InoHeaderCacheEntry {
  std::size_t codeHash = 0;
  std::string hppCode;
};

struct CompletionSessionCache {
  bool valid = false;
  std::string filename;                  // absolute filename
  int wordStart = -1;                    // position of the start of the word in the editor
  std::string basePrefix;                // prefix at the moment when we first called clang
  std::vector<CompletionItem> baseItems; // completions after marking fromSketch, but before the prefix filter
};

struct AeContainerInfo {
  std::string filename; // where the container is declared

  // Container name - e.q. "MyClass", "Detail", "Impl".
  std::string name;

  // Container type clang cursor kind spelling:
  // "ClassDecl", "StructDecl", "UnionDecl", "Namespace", "EnumDecl", ...
  std::string kind;

  int line = 0;
  int column = 0;

  // All symbols (fields, methods, enums, typedefs, ...) directly within the container
  std::vector<SymbolInfo> members;
};

struct IncludeUsage {
  unsigned line = 0;
  std::string includedFile;
  bool used = false;
};

using CollectSketchFilesFn = std::function<void(std::vector<SketchFileBuffer> &)>;

class ArduinoCodeCompletion {
private:
  static constexpr size_t MAX_ITEMS = 20;

  bool m_ready = false;

  CXIndex index;
  ArduinoCli *arduinoCli;
  ClangSettings m_clangSettings;

  CompletionMetadata m_completionMetadata;
  // Cache TU according to the "main" clang filename (.ino.cpp, .cpp, ...)
  std::unordered_map<std::string, CachedTranslationUnit> m_tuCache;
  // .. and for whole project
  std::unordered_map<std::string, ProjectTuEntry> m_projectTuCache;
  // cache for sibling definitions
  std::unordered_map<std::string, CXTranslationUnit> m_siblingTuCache;

  std::unordered_map<std::string, SymbolCacheEntry> m_symbolCache;
  std::unordered_map<uint64_t, InoHeaderCacheEntry> m_inoHeaderCache;
  // cache: decls signature -> insertIdx for "#include <sketch>.hpp" replacement
  mutable std::unordered_map<uint64_t, std::size_t> m_inoInsertCache;

  // includes resolving caching
  mutable std::mutex m_resolvedIncludesCacheMutex;
  mutable std::unordered_map<uint64_t, std::vector<std::string>> m_resolvedIncludesCache;

  mutable std::mutex m_ccMutex;   // protection TU/libclang
  std::atomic<uint64_t> m_seq{0}; // sequential request counter

  // Completion session machinery
  mutable std::mutex m_completionSessionMutex;
  CompletionSessionCache m_completionSession;

  std::atomic<bool> m_cancelAsync{false};
  CollectSketchFilesFn m_collectSketchFilesFn;

  bool IsIno(const std::string &filename) const;
  std::string AbsoluteFilename(const std::string &filename) const;
  std::string GetClangFilename(const std::string &filename) const;
  std::string GetClangCode(const std::string &filename, const std::string &code, int *addedLines) const;
  std::string GenerateInoHpp(const std::string &inoFilename, const std::string &code) const;
  void CreateClangUnsavedFiles(const std::string &filename, const std::string &code, ClangUnsavedFiles& out);

  // FNV-1a hash of text (fast "CRC")
  static std::size_t HashCode(const std::string &code);

  // Internal helper - must be called under m_ccMutex!
  // Returns the TU for the given file + optionally the offset of added lines (.ino -> .ino.cpp)
  // and mainFilename under which the TU is in the cache.
  CXTranslationUnit GetTranslationUnit(const std::string &filename,
                                       const std::string &code,
                                       int *outAddedLines = nullptr,
                                       std::string *outMainFile = nullptr);
  // Returns an existing TU from the cache without reparsing.
  // If it does not exist, create it using the classic GetTranslationUnit().
  CXTranslationUnit GetTranslationUnitNoReparse(const std::string &filename,
                                                const std::string &code,
                                                int *outAddedLines = nullptr,
                                                std::string *outMainFile = nullptr);

  int GetCompletionPriority(CXCursorKind kind);
  static void buildCompletionTexts(CXCompletionString cs, std::string &text, std::string &label);

  std::vector<ArduinoParseError> ParseCode(const std::string &filename, const std::string &code);

  std::vector<CompletionItem> GetCompletions(const std::string &filename, const std::string &code, int line, int column);

  std::vector<ArduinoParseError> CollectDiagnosticsLocked(CXTranslationUnit tu) const;

  bool FindSiblingFunctionDefinition(CXCursor declCursor, JumpTarget &out);

  std::vector<ArduinoParseError> ComputeProjectDiagnosticsLocked(const std::vector<SketchFileBuffer> &files);

  void QueueUiEvent(const wxWeakRef<wxEvtHandler> &weak, wxEvent *event);

  void FilterAndSortCompletionsWithPrefix(const std::string &prefix, std::vector<CompletionItem> &inOutCompletions);

  std::vector<std::string> GetCompilerArgs(const std::vector<SketchFileBuffer> &files) const;
  std::vector<std::string> GetCompilerArgs() const;

  void AeGetBestDiagLocation(CXSourceLocation loc, CXFile *out_file, unsigned *out_line, unsigned *out_column, unsigned *out_offset) const;

public:
  ArduinoCodeCompletion(ArduinoCli *ardCli, const ClangSettings &clangSettings, CollectSketchFilesFn collectSketchFilesFn);
  ~ArduinoCodeCompletion();

  static std::size_t ComputeDiagHash(const std::vector<ArduinoParseError> &errs);

  ArduinoCli *GetCli() { return arduinoCli; }
  void CollectSketchFiles(std::vector<SketchFileBuffer> &outFiles) const;

  void ApplySettings(const ClangSettings &settings);

  static bool IsClangTargetSupported(const std::string &target);

  long AutoDetectSerialBaudRate(const std::vector<SketchFileBuffer> &files);
  long AutoDetectSerialBaudRate();

  bool IsReady() { return m_ready; }
  void SetReady(bool ready = true) { m_ready = ready; }

  bool IsTranslationUnitValid();

  static std::string GetKindSpelling(CXCursorKind kind);

  void ShowAutoCompletionAsync(wxStyledTextCtrl *editor, std::string filename, CompletionMetadata &metadata, wxEvtHandler *handler);

  bool GetHoverInfo(const std::string &filename, const std::string &code, int line, int column, HoverInfo &outInfo);
  bool GetHoverInfo(const std::string &filename, const std::string &code, int line, int column, const std::vector<SketchFileBuffer> files, HoverInfo &outInfo);

  bool GetSymbolInfo(const std::string &filename, const std::string &code, int line, int column, SymbolInfo &outInfo);
  bool FindDefinition(const std::string &filename, const std::string &code, int line, int column, JumpTarget &out);
  std::vector<SymbolInfo> GetAllSymbols(const std::string &filename, const std::string &code);
  std::vector<SymbolInfo> GetAllSymbols();

  bool FindSymbolOccurrences(const std::string &filename, const std::string &code, int line, int column, bool onlyFromSketch, std::vector<JumpTarget> &outTargets);
  void FindSymbolOccurrencesAsync(const std::string &filename,
                                  const std::string &code,
                                  int line,
                                  int column,
                                  bool onlyFromSketch,
                                  wxEvtHandler *handler,
                                  uint64_t requestId,
                                  wxEventType eventType = EVT_SYMBOL_OCCURRENCES_READY);

  bool FindSymbolOccurrencesProjectWide(
      const std::vector<SketchFileBuffer> &files,
      const std::string &filename,
      const std::string &code,
      int line,
      int column,
      bool onlyFromSketch,
      std::vector<JumpTarget> &outTargets);

  void FindSymbolOccurrencesProjectWideAsync(
      const std::vector<SketchFileBuffer> &files,
      const std::string &filename,
      const std::string &code,
      int line,
      int column,
      bool onlyFromSketch,
      wxEvtHandler *handler,
      uint64_t requestId,
      wxEventType eventType);

  bool FindEnclosingContainerInfo(const std::string &filename,
                                  const std::string &code,
                                  int line,
                                  int column,
                                  AeContainerInfo &out);

  // Finds implementation in sibling .cpp for the function under given location
  bool FindSiblingFunctionDefinition(const std::string &filename,
                                     const std::string &code,
                                     int line,
                                     int column,
                                     JumpTarget &out);

  // Parses includes in the given file and returns a list of includes + info,
  // if the header is used. Returns false if TU cannot be created.
  bool AnalyzeIncludes(const std::string &filename,
                       const std::string &code,
                       std::vector<IncludeUsage> &outIncludes);

  bool AnalyzeExtractFunction(const std::string &filename,
                              const std::string &code,
                              int selStartLine, int selStartColumn,
                              int selEndLine, int selEndColumn,
                              ExtractFunctionAnalysis &out);

  void RefreshDiagnosticsAsync(const std::string &filename, const std::string &code, wxEvtHandler *handler);
  std::vector<ArduinoParseError> GetErrorsFor(const std::string &filename) const;

  // From files extracts includes and via ArduinoCli::ResolveLibraries returns
  // -I list for libraries.
  std::vector<std::string> ResolveLibrariesIncludes(const std::vector<SketchFileBuffer> &files) const;

  // Asynchronously recalculates diagnoses for the *entire sketch* (more TU).
  // filename/code can still be used for "quick" diagnostics of the current editor,
  // this is a "deep scan".
  void RefreshProjectDiagnosticsAsync(const std::vector<SketchFileBuffer> &files, wxEvtHandler *handler);

  // Cancels the current translationUnit for hover and completing.
  void InvalidateTranslationUnit();
  // Initializes translation unit for main file ino.
  bool InitTranslationUnitForIno();

  void CancelAsyncOperations();
};
