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

#include "ard_cc.hpp"
#include "ard_ed_frm.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

using Clock = std::chrono::steady_clock;

namespace {

// Thread-local pointer to the current snapshot sketch for the given thread
thread_local const std::vector<SketchFileBuffer> *g_ccFilesSnapshot = nullptr;

// RAII guard – sets snapshot at the start of the worker and returns the original value upon destruction
struct CcFilesSnapshotGuard {
  const std::vector<SketchFileBuffer> *prev = nullptr;

  explicit CcFilesSnapshotGuard(const std::vector<SketchFileBuffer> *files)
      : prev(g_ccFilesSnapshot) {
    g_ccFilesSnapshot = files;
  }

  ~CcFilesSnapshotGuard() {
    g_ccFilesSnapshot = prev;
  }
};

static std::string MakeBriefFromFull(const std::string &full) {
  // first non-empty line / paragraph as a brief
  std::string s = full;
  size_t pos = s.find('\n');
  std::string first = (pos == std::string::npos) ? s : s.substr(0, pos);
  TrimInPlace(first);
  return first;
}

static const char *ClangErrorToString(CXErrorCode err) {
  switch (err) {
    case CXError_Success:
      return "Success";
    case CXError_Failure:
      return "Failure";
    case CXError_Crashed:
      return "Crashed";
    case CXError_InvalidArguments:
      return "InvalidArguments";
    case CXError_ASTReadError:
      return "ASTReadError";
    default:
      return "Unknown";
  }
}

static const char *ClangSeverityToString(CXDiagnosticSeverity s) {
  switch (s) {
    case CXDiagnostic_Ignored:
      return "ignored";
    case CXDiagnostic_Note:
      return "note";
    case CXDiagnostic_Warning:
      return "warning";
    case CXDiagnostic_Error:
      return "error";
    case CXDiagnostic_Fatal:
      return "fatal";
    default:
      return "unknown";
  }
}

static bool HasUnknownTargetDiag(CXTranslationUnit tu) {
  if (!tu)
    return false;

  const unsigned n = clang_getNumDiagnostics(tu);
  const unsigned opts = clang_defaultDiagnosticDisplayOptions();

  for (unsigned i = 0; i < n; ++i) {
    CXDiagnostic d = clang_getDiagnostic(tu, i);
    CXString s = clang_formatDiagnostic(d, opts);
    const char *msg = clang_getCString(s);

    bool hit = false;
    if (msg) {
      // common clang texts
      if (strstr(msg, "unknown target triple") ||
          strstr(msg, "no targets are registered") ||
          strstr(msg, "could not create target")) {
        hit = true;
      }
    }

    clang_disposeString(s);
    clang_disposeDiagnostic(d);

    if (hit)
      return true;
  }
  return false;
}

static void LogClangArgsDebug(const std::vector<const char *> &args, int maxArgs = 1000) {
  const int n = (int)args.size();
  APP_DEBUG_LOG("CC: clang args (%d):", n);
  for (int i = 0; i < n && i < maxArgs; ++i) {
    const char *a = args[(size_t)i];
    APP_DEBUG_LOG("CC:   arg[%d] = %s", i, a ? a : "(null)");
  }
  if (n > maxArgs) {
    APP_DEBUG_LOG("CC:   ... (%d more args not shown)", n - maxArgs);
  }
}

static void LogUnsavedFilesDebug(const ClangUnsavedFiles &uf) {
  APP_DEBUG_LOG("CC: unsaved files: count=%u", (unsigned)uf.count);
  for (unsigned i = 0; i < (unsigned)uf.count; ++i) {
    const char *fn = uf.files[i].Filename ? uf.files[i].Filename : "(null)";
    const unsigned long long len = (unsigned long long)uf.files[i].Length;
    APP_DEBUG_LOG("CC:   unsaved[%u] = %s (%llu bytes)", i, fn, len);
  }
}

static void LogDiagnosticsDebug(CXTranslationUnit tu, unsigned maxDiags = 50) {
  if (!tu) {
    APP_DEBUG_LOG("CC: diagnostics unavailable (TU=null).");
    return;
  }

  const unsigned total = clang_getNumDiagnostics(tu);
  APP_DEBUG_LOG("CC: TU diagnostics: total=%u (showing up to %u)", total, maxDiags);

  unsigned shown = 0;
  const unsigned opts = clang_defaultDiagnosticDisplayOptions();

  for (unsigned i = 0; i < total && shown < maxDiags; ++i) {
    CXDiagnostic diag = clang_getDiagnostic(tu, i);
    const CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diag);

    // Notes tend to be noise; we don't want to spam them by default.
    if (sev == CXDiagnostic_Note) {
      clang_disposeDiagnostic(diag);
      continue;
    }

    CXString f = clang_formatDiagnostic(diag, opts);
    const char *msg = clang_getCString(f);
    APP_DEBUG_LOG("CC:   [%s] %s", ClangSeverityToString(sev), msg ? msg : "");
    clang_disposeString(f);
    clang_disposeDiagnostic(diag);
    ++shown;
  }

  if (total > maxDiags) {
    APP_DEBUG_LOG("CC:   ... (%u more diagnostics not shown)", total - maxDiags);
  }
}

} // namespace

// Helpers for symbol search
struct LocKey {
  std::string file;
  unsigned line;
  unsigned col;

  bool operator==(const LocKey &o) const {
    return line == o.line && col == o.col && file == o.file;
  }
};

struct FunctionKey {
  std::string name;
  int numArgs = -1;
  bool isVariadic = false;
  // optional - if empty, the container is not filtered
  std::string containerName;
};

struct LocKeyHash {
  std::size_t operator()(const LocKey &k) const {
    std::size_t h = 1469598103934665603ull;
    auto mix = [&h](unsigned char c) {
      h ^= c;
      h *= 1099511628211ull;
    };
    for (char c : k.file)
      mix((unsigned char)c);
    for (int i = 0; i < 4; ++i)
      mix((k.line >> (i * 8)) & 0xFF);
    for (int i = 0; i < 4; ++i)
      mix((k.col >> (i * 8)) & 0xFF);
    return h;
  }
};

static std::string cxStringToStd(CXString s) {
  const char *c = clang_getCString(s);
  std::string out = c ? c : "";
  clang_disposeString(s);
  return out;
}

// 0 = best, higher = worse
static int kindScore(CXCursorKind kind) {
  switch (kind) {
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_FunctionTemplate:
      return 0; // functions/methods

    case CXCursor_VarDecl:
    case CXCursor_FieldDecl:
    case CXCursor_ParmDecl:
    case CXCursor_NonTypeTemplateParameter:
      return 1; // variables / parameters

    case CXCursor_ClassDecl:
    case CXCursor_StructDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_EnumDecl:
      return 2; // types

    case CXCursor_EnumConstantDecl:
      return 3; // enum constants

    case CXCursor_MacroDefinition:
      return 4; // macros

    default:
      return 5; // others
  }
}

static std::string NormalizePathForClangCompare(std::string s) {
  // merge to forward slash
  std::replace(s.begin(), s.end(), '\\', '/');

#ifdef __WXMSW__
  // on Windows ignore case
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (unsigned char)std::tolower(c); });
#endif

  return s;
}

static void FillParameterInfoFromCursor(CXCursor cursor,
                                        std::vector<ParameterInfo> &outParams) {
  outParams.clear();

  int nArgs = clang_Cursor_getNumArguments(cursor);
  if (nArgs < 0)
    return;

  for (int i = 0; i < nArgs; ++i) {
    CXCursor argCur = clang_Cursor_getArgument(cursor, i);
    if (clang_Cursor_isNull(argCur))
      continue;

    ParameterInfo p;
    p.name = cxStringToStd(clang_getCursorSpelling(argCur));

    CXType argType = clang_getCursorType(argCur);
    if (argType.kind != CXType_Invalid) {
      p.type = cxStringToStd(clang_getTypeSpelling(argType));
    }

    outParams.push_back(std::move(p));
  }

  // variadic functions - add a synthetic "..." parameter at the end
  CXType fnType = clang_getCursorType(cursor);
  if (clang_isFunctionTypeVariadic(fnType)) {
    ParameterInfo p;
    p.name = "...";
    p.type.clear();
    outParams.push_back(std::move(p));
  }
}

static bool GetBodyRangeForCursor(CXCursor cursor,
                                  unsigned &fromLine,
                                  unsigned &fromCol,
                                  unsigned &toLine,
                                  unsigned &toCol,
                                  CXFile *outFile = nullptr) {
  fromLine = fromCol = toLine = toCol = 0;

  if (outFile) {
    *outFile = nullptr;
  }

  CXCursorKind kind = clang_getCursorKind(cursor);

  switch (kind) {
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_FunctionTemplate: {
      // We find CompoundStmt - function body
      CXCursor body = clang_getNullCursor();

      clang_visitChildren(
          cursor,
          [](CXCursor c, CXCursor WXUNUSED(parent), CXClientData client_data) {
            auto *bodyPtr = static_cast<CXCursor *>(client_data);
            if (!clang_Cursor_isNull(*bodyPtr)) {
              return CXChildVisit_Break;
            }

            if (clang_getCursorKind(c) == CXCursor_CompoundStmt) {
              *bodyPtr = c;
              return CXChildVisit_Break;
            }

            return CXChildVisit_Recurse;
          },
          &body);

      if (clang_Cursor_isNull(body)) {
        // Declaration without body
        return false;
      }

      CXSourceRange range = clang_getCursorExtent(body);
      CXSourceLocation startLoc = clang_getRangeStart(range);
      CXSourceLocation endLoc = clang_getRangeEnd(range);

      CXFile fStart = nullptr, fEnd = nullptr;
      unsigned sl = 0, sc = 0, so = 0;
      unsigned el = 0, ec = 0, eo = 0;

      clang_getSpellingLocation(startLoc, &fStart, &sl, &sc, &so);
      clang_getSpellingLocation(endLoc, &fEnd, &el, &ec, &eo);

      if (!fStart || !fEnd) {
        return false;
      }

      if (clang_File_isEqual(fStart, fEnd) == 0) {
        return false;
      }

      fromLine = sl;
      fromCol = sc;
      toLine = el;
      toCol = ec;
      if (outFile) {
        *outFile = fStart;
      }
      return true;
    }

    default:
      break;
  }

  // ---------------------------
  // class/struct/union/templ
  // ---------------------------
  if (kind == CXCursor_ClassDecl ||
      kind == CXCursor_StructDecl ||
      kind == CXCursor_UnionDecl ||
      kind == CXCursor_ClassTemplate) {

    // Only definitions, no forward declaration
    if (!clang_isCursorDefinition(cursor)) {
      return false;
    }

    CXSourceRange range = clang_getCursorExtent(cursor);
    CXSourceLocation startLoc = clang_getRangeStart(range);
    CXSourceLocation endLoc = clang_getRangeEnd(range);

    CXFile fStart = nullptr, fEnd = nullptr;
    unsigned sl = 0, sc = 0, so = 0;
    unsigned el = 0, ec = 0, eo = 0;

    clang_getSpellingLocation(startLoc, &fStart, &sl, &sc, &so);
    clang_getSpellingLocation(endLoc, &fEnd, &el, &ec, &eo);

    if (!fStart || !fEnd) {
      return false;
    }

    if (clang_File_isEqual(fStart, fEnd) == 0) {
      return false;
    }

    fromLine = sl;
    fromCol = sc;
    toLine = el;
    toCol = ec;
    if (outFile) {
      *outFile = fStart;
    }
    return true;
  }

  return false;
}

// Shared helper that collects symbols from a TU.
// If parentFilter is non-null cursor, only symbols whose semantic parent
// matches parentFilter are collected. Otherwise all top-level / nested
// symbols are collected (according to the filter in the visitor).
static void CollectSymbolsInTUForParent(CXTranslationUnit tu,
                                        const std::string &mainFile,
                                        int addedLines,
                                        std::vector<SymbolInfo> &symbols,
                                        CXCursor parentFilter) {
  if (!tu) {
    return;
  }

  CXCursor nullCursor = clang_getNullCursor();
  bool useParentFilter = !clang_equalCursors(parentFilter, nullCursor);
  if (useParentFilter) {
    parentFilter = clang_getCanonicalCursor(parentFilter);
  }

  struct VisitorData {
    std::vector<SymbolInfo> *symbols;
    std::string mainFile;
    int addedLines;
    CXCursor parentFilter;
    bool useParentFilter;
  } data{&symbols, mainFile, addedLines, parentFilter, useParentFilter};

  CXCursor tuCursor = clang_getTranslationUnitCursor(tu);

  clang_visitChildren(
      tuCursor,
      [](CXCursor cursor, CXCursor WXUNUSED(parent), CXClientData client_data) -> CXChildVisitResult {
        auto *data = static_cast<VisitorData *>(client_data);

        CXCursorKind kind = clang_getCursorKind(cursor);

        switch (kind) {
          case CXCursor_FunctionDecl:
          case CXCursor_CXXMethod:
          case CXCursor_Constructor:
          case CXCursor_Destructor:
          case CXCursor_FunctionTemplate:
          case CXCursor_VarDecl:
          case CXCursor_FieldDecl:
          case CXCursor_ParmDecl:
          case CXCursor_EnumConstantDecl:
          case CXCursor_StructDecl:
          case CXCursor_ClassDecl:
          case CXCursor_UnionDecl:
          case CXCursor_EnumDecl:
          case CXCursor_TypedefDecl:
          case CXCursor_MacroDefinition:
            break; // we take
          default:
            return CXChildVisit_Recurse;
        }

        std::string name = cxStringToStd(clang_getCursorSpelling(cursor));
        if (name.empty())
          return CXChildVisit_Recurse;

        // If we have a filter on the parent (container), we check the semantic parent
        if (data->useParentFilter) {
          CXCursor parent = clang_getCursorSemanticParent(cursor);
          if (clang_Cursor_isNull(parent))
            return CXChildVisit_Recurse;

          CXCursor canonParent = clang_getCanonicalCursor(parent);
          if (!clang_equalCursors(canonParent, data->parentFilter)) {
            return CXChildVisit_Recurse;
          }
        }

        CXSourceLocation loc = clang_getCursorLocation(cursor);
        CXFile file;
        unsigned line = 0, column = 0, offset = 0;
        clang_getSpellingLocation(loc, &file, &line, &column, &offset);
        if (!file)
          return CXChildVisit_Recurse;

        std::string fileName = cxStringToStd(clang_getFileName(file));

        SymbolInfo si;
        si.name = std::move(name);
        si.display = cxStringToStd(clang_getCursorDisplayName(cursor));
        if (si.display.empty())
          si.display = si.name;
        si.file = std::move(fileName);
        si.line = (int)line;
        si.column = (int)column;
        si.kind = kind;

        // If the symbol is a function/method/ctor/dtor/template -> fill in the parameters
        switch (kind) {
          case CXCursor_FunctionDecl:
          case CXCursor_CXXMethod:
          case CXCursor_Constructor:
          case CXCursor_Destructor:
          case CXCursor_FunctionTemplate:
            FillParameterInfoFromCursor(cursor, si.parameters);
            break;
          default:
            break;
        }

        // Strip bogus "void " for constructors/destructors.
        if ((kind == CXCursor_Constructor || kind == CXCursor_Destructor) &&
            hasPrefix(si.display, "void ")) {
          si.display.erase(0, 5); // drop "void "
          while (!si.display.empty() &&
                 std::isspace((unsigned char)si.display[0])) {
            si.display.erase(0, 1);
          }
        }

        // Body scope { ... } for functions/methods
        {
          unsigned blFrom = 0, bcFrom = 0, blTo = 0, bcTo = 0;
          if (GetBodyRangeForCursor(cursor, blFrom, bcFrom, blTo, bcTo)) {
            // correction .ino addedLines, same as si.line
            if (!data->mainFile.empty() &&
                si.file == data->mainFile &&
                data->addedLines > 0) {

              if (blFrom > (unsigned)data->addedLines &&
                  blTo > (unsigned)data->addedLines) {
                blFrom -= (unsigned)data->addedLines;
                blTo -= (unsigned)data->addedLines;
              } else {
                // the body is in the synthetic part (ino.hpp / include) -> we ignore
                blFrom = bcFrom = blTo = bcTo = 0;
              }
            }

            si.bodyLineFrom = (int)blFrom;
            si.bodyColFrom = (int)bcFrom;
            si.bodyLineTo = (int)blTo;
            si.bodyColTo = (int)bcTo;
          }
        }

        // Line correction for synthetic .ino.cpp
        if (!data->mainFile.empty() &&
            si.file == data->mainFile &&
            data->addedLines > 0) {
          si.line -= data->addedLines;
          if (si.line <= 0) {
            return CXChildVisit_Recurse;
          }
        }

        data->symbols->push_back(std::move(si));
        return CXChildVisit_Recurse;
      },
      &data);
}

static bool FindFunctionInTU(CXTranslationUnit tu,
                             const std::string &mainFile,
                             const FunctionKey &key,
                             bool requireDefinition,
                             JumpTarget &out) {
  if (!tu)
    return false;

  struct VisitorData {
    const FunctionKey *key;
    std::string mainFile;
    bool requireDefinition;
    JumpTarget *out;
    bool found = false;
  } data{&key, mainFile, requireDefinition, &out};

  CXCursor tuCursor = clang_getTranslationUnitCursor(tu);

  clang_visitChildren(
      tuCursor,
      [](CXCursor c, CXCursor WXUNUSED(parent), CXClientData client_data) -> CXChildVisitResult {
        auto *d = static_cast<VisitorData *>(client_data);

        CXCursorKind k = clang_getCursorKind(c);
        switch (k) {
          case CXCursor_FunctionDecl:
          case CXCursor_CXXMethod:
          case CXCursor_Constructor:
          case CXCursor_Destructor:
          case CXCursor_FunctionTemplate:
            break;
          default:
            return CXChildVisit_Recurse;
        }

        if (d->requireDefinition && !clang_isCursorDefinition(c))
          return CXChildVisit_Recurse;

        std::string name = cxStringToStd(clang_getCursorSpelling(c));
        if (name != d->key->name)
          return CXChildVisit_Recurse;

        // --- container (class/struct/namespace) - if filter ---
        if (!d->key->containerName.empty()) {
          std::string containerName;
          CXCursor p = clang_getCursorSemanticParent(c);
          while (!clang_Cursor_isNull(p)) {
            CXCursorKind pk = clang_getCursorKind(p);
            bool isContainer =
                pk == CXCursor_ClassDecl ||
                pk == CXCursor_StructDecl ||
                pk == CXCursor_UnionDecl ||
                pk == CXCursor_ClassTemplate ||
                pk == CXCursor_EnumDecl ||
                pk == CXCursor_Namespace;
            if (isContainer) {
              containerName = cxStringToStd(clang_getCursorSpelling(p));
              break;
            }
            CXCursor next = clang_getCursorSemanticParent(p);
            if (clang_equalCursors(next, p))
              break;
            p = next;
          }

          if (containerName != d->key->containerName) {
            return CXChildVisit_Recurse;
          }
        }

        // --- signature ---
        int nArgs = clang_Cursor_getNumArguments(c);
        CXType t = clang_getCursorType(c);
        bool variadic = clang_isFunctionTypeVariadic(t) != 0;

        if (nArgs != d->key->numArgs || variadic != d->key->isVariadic) {
          return CXChildVisit_Recurse;
        }

        // --- the file must be the main file TU --
        CXSourceLocation loc = clang_getCursorLocation(c);
        CXFile file;
        unsigned line = 0, col = 0, off = 0;
        clang_getSpellingLocation(loc, &file, &line, &col, &off);
        if (!file) {
          return CXChildVisit_Recurse;
        }

        std::string fileName = cxStringToStd(clang_getFileName(file));
        if (fileName != d->mainFile) {
          return CXChildVisit_Recurse;
        }

        d->out->file = fileName;
        d->out->line = (int)line;
        d->out->column = (int)col;
        d->found = true;

        return CXChildVisit_Break;
      },
      &data);

  return data.found;
}

// Auxiliary structure for data collection when traversing AST
struct ExtractVisitorData {
  CXTranslationUnit tu;
  CXFile file;
  CXSourceRange selRange;

  // name -> (type, alreadyMarkedAsParam)
  struct VarInfo {
    std::string type;

    bool usedInside = false;     // use inside selection
    bool declInside = false;     // declaration inside selection
    bool declInThisFile = false; // declaration in the same file as selection
    bool isParam = false;        // functional parameter surrounding functions
    bool isGlobal = false;       // file-scope global (parent == TranslationUnit)
  };

  std::map<std::string, VarInfo> vars;
};

static bool IsLocationInRange(CXSourceLocation loc, CXSourceRange range) {
  unsigned locOffset = 0;
  unsigned startOffset = 0;
  unsigned endOffset = 0;

  CXSourceLocation startLoc = clang_getRangeStart(range);
  CXSourceLocation endLoc = clang_getRangeEnd(range);

  clang_getSpellingLocation(loc, nullptr, nullptr, nullptr, &locOffset);
  clang_getSpellingLocation(startLoc, nullptr, nullptr, nullptr, &startOffset);
  clang_getSpellingLocation(endLoc, nullptr, nullptr, nullptr, &endOffset);

  return locOffset >= startOffset && locOffset <= endOffset;
}

static enum CXChildVisitResult ExtractVisitor(CXCursor cursor,
                                              CXCursor WXUNUSED(parent),
                                              CXClientData clientData) {
  auto *data = static_cast<ExtractVisitorData *>(clientData);

  CXSourceLocation loc = clang_getCursorLocation(cursor);
  CXFile file;
  unsigned line, col, offset;
  clang_getSpellingLocation(loc, &file, &line, &col, &offset);

  if (!file || file != data->file) {
    return CXChildVisit_Recurse;
  }

  CXCursorKind kind = clang_getCursorKind(cursor);

  // 1) VarDecl / ParmVarDecl
  if (kind == CXCursor_VarDecl || kind == CXCursor_ParmDecl) {
    CXString cxName = clang_getCursorSpelling(cursor);
    std::string name = clang_getCString(cxName);
    clang_disposeString(cxName);

    if (name.empty())
      return CXChildVisit_Recurse;

    auto &info = data->vars[name];

    info.declInThisFile = true;

    if (IsLocationInRange(loc, data->selRange)) {
      info.declInside = true;
    }

    // Differ parameters / globals
    CXCursor parent = clang_getCursorSemanticParent(cursor);
    CXCursorKind parentKind = clang_getCursorKind(parent);

    if (kind == CXCursor_ParmDecl) {
      info.isParam = true;
    } else if (kind == CXCursor_VarDecl) {
      // global = parent is TranslationUnit
      if (parentKind == CXCursor_TranslationUnit) {
        info.isGlobal = true;
      }
    }

    // We'll read the type right away (it'll come in handy later)
    CXType t = clang_getCursorType(cursor);
    CXString cxType = clang_getTypeSpelling(t);
    std::string typeStr = clang_getCString(cxType);
    clang_disposeString(cxType);

    if (!typeStr.empty())
      info.type = typeStr;

    return CXChildVisit_Recurse;
  }

  // 2) DeclRefExpr
  if (kind == CXCursor_DeclRefExpr) {
    if (!IsLocationInRange(loc, data->selRange)) {
      return CXChildVisit_Recurse;
    }

    CXCursor ref = clang_getCursorReferenced(cursor);
    CXCursorKind refKind = clang_getCursorKind(ref);
    if (refKind != CXCursor_VarDecl && refKind != CXCursor_ParmDecl) {
      return CXChildVisit_Recurse;
    }

    CXString cxName = clang_getCursorSpelling(ref);
    std::string name = clang_getCString(cxName);
    clang_disposeString(cxName);

    if (name.empty())
      return CXChildVisit_Recurse;

    auto &info = data->vars[name];
    info.usedInside = true;

    CXSourceLocation declLoc = clang_getCursorLocation(ref);
    CXFile declFile;
    unsigned dLine, dCol, dOff;
    clang_getSpellingLocation(declLoc, &declFile, &dLine, &dCol, &dOff);

    if (declFile && declFile == data->file) {
      info.declInThisFile = true;

      CXCursor declParent = clang_getCursorSemanticParent(ref);
      CXCursorKind declParentKind = clang_getCursorKind(declParent);
      if (refKind == CXCursor_VarDecl &&
          declParentKind == CXCursor_TranslationUnit) {
        info.isGlobal = true; // file-scope globl
      }
      if (refKind == CXCursor_ParmDecl) {
        info.isParam = true;
      }
    }

    if (info.type.empty()) {
      CXType t = clang_getCursorType(ref);
      CXString cxType = clang_getTypeSpelling(t);
      std::string typeStr = clang_getCString(cxType);
      clang_disposeString(cxType);

      info.type = typeStr;
    }

    return CXChildVisit_Recurse;
  }

  return CXChildVisit_Recurse;
}

// If we have ParameterInfo captured from clang, reconstruct the parameter list
// inside a "pretty" signature string and inject parameter names.
// Example: "void digitalWrite(uint8_t, uint8_t)" -> "void digitalWrite(uint8_t pin, uint8_t val)".
static std::string InjectParameterNamesIntoSignature(const std::string &sig, const std::vector<ParameterInfo> &params) {
  if (sig.empty() || params.empty()) {
    return sig;
  }

  const size_t open = sig.find('(');
  const size_t close = sig.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    return sig;
  }

  std::string out;
  out.reserve(sig.size() + params.size() * 8);

  // prefix including '('
  out.append(sig, 0, open + 1);

  for (size_t i = 0; i < params.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }

    std::string t = TrimCopy(params[i].type);
    std::string n = TrimCopy(params[i].name);

    // Variadic (best-effort).
    if (t == "..." || n == "...") {
      out += "...";
      continue;
    }

    if (!t.empty()) {
      out += t;
    }

    if (!n.empty()) {
      if (!t.empty()) {
        out += ' ';
      }
      out += n;
    }
  }

  // suffix including ')', plus any trailing qualifiers (const/noexcept/...)
  out.append(sig, close, std::string::npos);
  return out;
}

// -------------------------------------------------------------------------------

std::string ArduinoParseError::ToString() const {
  auto sevLabel = [](CXDiagnosticSeverity s) -> const char * {
    switch (s) {
      case CXDiagnostic_Fatal:
        return "Fatal";
      case CXDiagnostic_Error:
        return "Error";
      case CXDiagnostic_Warning:
        return "Warning";
      case CXDiagnostic_Note:
        return "Note";
      case CXDiagnostic_Ignored:
        return "Ignored";
      default:
        return "";
    }
  };

  std::ostringstream oss;

  auto dump = [&](auto &&self, const ArduinoParseError &e, int indent) -> void {
    if (indent > 0) {
      oss << std::string((size_t)indent, ' ');
    }

    const char *lbl = sevLabel(e.severity);
    if (lbl && *lbl) {
      oss << lbl << ": ";
    }

    if (!e.file.empty() && e.line > 0) {
      oss << e.file << ":" << e.line << ":" << e.column << ": ";
    }
    oss << e.message;

    for (const auto &ch : e.childs) {
      oss << "\n";
      self(self, ch, indent + 2);
    }
  };

  dump(dump, *this, 0);
  return oss.str();
}

std::string HoverInfo::ToHoverString() {
  std::string tooltip;

  bool sigIsSameAsName = !signature.empty() && signature == name;

  bool noUsefulSignature = signature.empty() || sigIsSameAsName;

  // does the type exist and is it different from the name?
  bool typeAddsInfo = !type.empty() && type != name;

  // "useful" symbol = has a comment, or a signature other than the name
  bool hasUsefulInfo =
      !kind.empty() ||
      !briefComment.empty() ||
      !fullComment.empty() ||
      (!noUsefulSignature) || // signature contains more than just the name
      typeAddsInfo;           // type makes sense to display

  // If we don't have useful info -> display nothing
  if (!hasUsefulInfo) {
    return tooltip;
  }

  // If we captured parameters with names, prefer them over unnamed params in the signature.
  std::string sig = signature;
  if (!parameters.empty()) {
    sig = InjectParameterNamesIntoSignature(sig, parameters);
  }

  // Compose the tooltip text
  if (!kind.empty()) {
    tooltip += kind + "\n\n";
  }

  if (!sig.empty() && !sigIsSameAsName) {
    // typical case of a function: "void digitalWrite(uint8_t pin, uint8_t val)"
    tooltip += sig;
    tooltip += "\n";
  } else if (!name.empty()) {
    // variable, typedef, enum value... -> name + type
    tooltip += name;
    if (!type.empty()) {
      tooltip += " : ";
      tooltip += type;
    }
    tooltip += "\n";
  }

  if (!fullComment.empty()) {
    tooltip += "\n";
    tooltip += fullComment;
  } else if (!briefComment.empty()) {
    tooltip += "\n";
    tooltip += briefComment;
  }

  return tooltip;
}

/**
 * Constructs the text that will be displayed in the completion dropdown.
 *
 * @param cs input completion string.
 * @param text output text.
 * @param label output label.
 */
void ArduinoCodeCompletion::buildCompletionTexts(CXCompletionString cs, std::string &text, std::string &label) {
  text.clear();
  label.clear();

  std::string resultType;

  unsigned numChunks = clang_getNumCompletionChunks(cs);
  for (unsigned i = 0; i < numChunks; ++i) {
    CXCompletionChunkKind ck = clang_getCompletionChunkKind(cs, i);
    std::string t = cxStringToStd(clang_getCompletionChunkText(cs, i));
    if (t.empty())
      continue;

    switch (ck) {
      case CXCompletionChunk_ResultType:
        // e.g. "void", "int", "size_t"
        resultType = t;
        break;

      case CXCompletionChunk_TypedText:
        // the name that we want to insert into the code
        text = t;
        [[fallthrough]];

      case CXCompletionChunk_Text:
      case CXCompletionChunk_Placeholder:
      case CXCompletionChunk_CurrentParameter:
      case CXCompletionChunk_LeftParen:
      case CXCompletionChunk_RightParen:
      case CXCompletionChunk_Comma:
      case CXCompletionChunk_Colon:
      case CXCompletionChunk_SemiColon:
      case CXCompletionChunk_Equal:
      case CXCompletionChunk_LeftAngle:
      case CXCompletionChunk_RightAngle:
      case CXCompletionChunk_LeftBracket:
      case CXCompletionChunk_RightBracket:
      case CXCompletionChunk_LeftBrace:
      case CXCompletionChunk_RightBrace:
        // from this we compose a "nice" label with a signature
        label += t;
        break;

      default:
        break;
    }
  }

  // We want the return type in the label as well.
  if (!resultType.empty() && !label.empty()) {
    label = label + " -> " + resultType;
  }

  // fallback - if the text somehow does not get created, we use the label
  if (text.empty())
    text = label;
}

/**
 * Returns a flag indicating whether the filename is an *.ino file.
 *
 * @param filename the name of the file.
 * @return bool is it ino?
 */
bool ArduinoCodeCompletion::IsIno(const std::string &filename) const {
  return (filename.size() >= 4 && filename.compare(filename.size() - 4, 4, ".ino") == 0);
}

/**
 * Normalizes filename to an absolute path.
 */
std::string ArduinoCodeCompletion::AbsoluteFilename(const std::string &filename) const {
  std::string result;

  fs::path p(filename);

  // If filename has no path, we add the path to the sketch
  if (p.has_parent_path()) {
    result = p.string();
  } else {
    fs::path sketchPath = arduinoCli->GetSketchPath();
    result = (sketchPath / p).string();
  }

  return result;
}

/**
 * Returns filename for CLang. Changes *.ino to *.ino.cpp.
 */
std::string ArduinoCodeCompletion::GetClangFilename(const std::string &filename) const {
  std::string clangFilename = AbsoluteFilename(filename);

  // If it ends with .ino -> add .cpp suffix
  if (IsIno(clangFilename)) {
    clangFilename += ".cpp";
  }

  return clangFilename;
}

std::string ArduinoCodeCompletion::GetClangCode(const std::string &filename,
                                                const std::string &code,
                                                int *addedLines) const {
  if (addedLines) {
    *addedLines = 0;
  }

  if (!IsIno(filename)) {
    return code;
  }

  std::vector<std::string> lines;
  {
    std::size_t pos = 0;
    while (true) {
      std::size_t lineEnd = code.find('\n', pos);
      if (lineEnd == std::string::npos) {
        lines.emplace_back(code.substr(pos));
        break;
      }
      lines.emplace_back(code.substr(pos, lineEnd - pos));
      pos = lineEnd + 1;
    }
  }

  if (lines.empty()) {
    return code;
  }

  auto isBlankLine = [](const std::string &s) {
    for (unsigned char c : s) {
      if (!std::isspace(c))
        return false;
    }
    return true;
  };

  // Fast-path: cache the insert line for the generated .ino.hpp include.
  // Typing inside function bodies keeps the same "declarations signature" -> we can avoid a clang scan.
  const uint64_t declsSig = CcSumDecls(std::string_view(filename), std::string_view(code));
  auto itCached = m_inoInsertCache.find(declsSig);
  if (itCached != m_inoInsertCache.end()) {
    const std::size_t cachedIdx = itCached->second;
    if (cachedIdx < lines.size() && isBlankLine(lines[cachedIdx])) {
      fs::path p(filename);
      std::string hppIncludeName = p.filename().string() + ".hpp";

      lines[cachedIdx] = "#include \"" + hppIncludeName + "\"";

      std::string result;
      result.reserve(code.size() + 32);
      for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0)
          result.push_back('\n');
        result += lines[i];
      }

      APP_DEBUG_LOG("CC: InoInsert cache hit for %s (idx=%zu)", filename.c_str(), cachedIdx);
      return result;
    }

    APP_DEBUG_LOG("CC: InoInsert cache stale for %s (idx=%zu) -> recompute", filename.c_str(), cachedIdx);
  }

  // --- 1) Using clang, we can find out where the top-level function definitions are ---
  struct FnInfo {
    unsigned line;
  };

  std::vector<FnInfo> functions;

  {
    std::string clangFilename = GetClangFilename(filename);

    const std::vector<std::string> &clangArgs = GetCompilerArgs();
    std::vector<const char *> args;
    args.reserve(clangArgs.size());
    for (const auto &a : clangArgs) {
      args.push_back(a.c_str());
    }

    CXUnsavedFile uf{};
    uf.Filename = clangFilename.c_str();
    uf.Contents = code.c_str();
    uf.Length = code.size();

    CXTranslationUnit tu = nullptr;
    CXErrorCode err = clang_parseTranslationUnit2(
        index,
        clangFilename.c_str(),
        args.data(),
        static_cast<int>(args.size()),
        &uf,
        1,
        CXTranslationUnit_KeepGoing,
        &tu);

    if (err == CXError_Success && tu) {
      struct VisitorData {
        std::string mainFile;
        std::vector<FnInfo> *outFns;
      } data{NormalizePathForClangCompare(clangFilename), &functions};

      CXCursor tuCursor = clang_getTranslationUnitCursor(tu);

      clang_visitChildren(
          tuCursor,
          [](CXCursor cursor, CXCursor parent, CXClientData client_data) -> CXChildVisitResult {
            auto *data = static_cast<VisitorData *>(client_data);

            CXCursorKind pk = clang_getCursorKind(parent);
            if (pk != CXCursor_TranslationUnit) {
              return CXChildVisit_Recurse; // only top-level
            }

            CXCursorKind kind = clang_getCursorKind(cursor);
            if (kind != CXCursor_FunctionDecl) {
              return CXChildVisit_Recurse;
            }

            // only definitions
            if (!clang_isCursorDefinition(cursor))
              return CXChildVisit_Recurse;

            CXSourceLocation loc = clang_getCursorLocation(cursor);
            CXFile file;
            unsigned line = 0, col = 0, offset = 0;
            clang_getSpellingLocation(loc, &file, &line, &col, &offset);
            if (!file)
              return CXChildVisit_Recurse;

            std::string fileName = cxStringToStd(clang_getFileName(file));
            fileName = NormalizePathForClangCompare(std::move(fileName));
            if (fileName != data->mainFile) {
              return CXChildVisit_Recurse;
            }

            if (line == 0)
              return CXChildVisit_Recurse;

            FnInfo fi;
            fi.line = line;
            data->outFns->push_back(std::move(fi));

            return CXChildVisit_Recurse;
          },
          &data);

      clang_disposeTranslationUnit(tu);
    } else {
      APP_DEBUG_LOG("CC: GetClangCode - failed to build TU for function scan: file=%s err=%d (%s)",
                    clangFilename.c_str(), (int)err, ClangErrorToString(err));
      // This parse is only used to locate function definitions for .ino preprocessing.
      // Dump args to make issues debuggable.
      LogClangArgsDebug(args);
    }
  }

  if (functions.empty()) {
    return code;
  }

  std::sort(functions.begin(), functions.end(),
            [](const FnInfo &a, const FnInfo &b) {
              return a.line < b.line;
            });

  // --- 2) We find an empty line before the first suitable function ---
  bool foundPlace = false;
  std::size_t insertIdx = 0;

  for (const auto &fn : functions) {
    if (fn.line <= 1 || fn.line > lines.size())
      continue;

    int idx = static_cast<int>(fn.line) - 2;
    for (int i = idx; i >= 0; --i) {
      if (isBlankLine(lines[static_cast<std::size_t>(i)])) {
        insertIdx = static_cast<std::size_t>(i);
        foundPlace = true;
        break;
      }
    }

    if (foundPlace)
      break;
  }

  if (!foundPlace) {
    return code;
  }

  // --- 3) On that empty line, replace include with the generated .ino.hpp ---

  fs::path p(filename);
  std::string hppIncludeName = p.filename().string() + ".hpp";

  // Cache this insert location (keyed by declarations/signatures only).
  m_inoInsertCache[declsSig] = insertIdx;

  lines[insertIdx] = "#include \"" + hppIncludeName + "\"";

  // --- 4) Let's put the code back into one string ---
  std::string result;
  result.reserve(code.size() + 32);

  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0)
      result.push_back('\n');
    result += lines[i];
  }

  APP_DEBUG_LOG("CC: clang code (with ino.hpp include): %d bytes", (int)result.size());
  APP_TRACE_LOG("CC: clang code:\n%s", result.c_str());

  return result;
}

std::string ArduinoCodeCompletion::GenerateInoHpp(const std::string &inoFilename, const std::string &code) const {
  ScopeTimer t("CC: GenerateInoHpp()");

  std::string hpp;
  hpp.reserve(code.size() / 2 + 256);

  APP_DEBUG_LOG("CC: GenerateInoHpp (filename=%s, code.length=%d)", inoFilename.c_str(), code.length());

  std::string clangFilename = GetClangFilename(inoFilename);

  hpp += "#pragma once\n";

  // --- Function prototypes ---
  struct FnProto {
    std::string ret;
    std::string name;
    std::string params;
    unsigned line; // definition line in .ino.cpp
  };

  std::vector<FnProto> functions;

  // --- We will build a mini TU from .ino and extract top-level functions ---
  {
    const std::vector<std::string> &clangArgs = GetCompilerArgs();

    std::vector<const char *> args;
    args.reserve(clangArgs.size());
    for (const auto &a : clangArgs) {
      args.push_back(a.c_str());
      APP_TRACE_LOG("CC: CLP: %s", a.c_str());
    }

    APP_DEBUG_LOG("CC: GetCompilerArgs(count=%u)", (unsigned)args.size());

    CXUnsavedFile uf;
    uf.Filename = clangFilename.c_str();
    uf.Contents = code.c_str();
    uf.Length = code.size();

    CXTranslationUnit tu = nullptr;

    CXErrorCode err = clang_parseTranslationUnit2(
        index,
        clangFilename.c_str(),
        args.data(),
        static_cast<int>(args.size()),
        &uf,
        1,
        CXTranslationUnit_KeepGoing,
        &tu);

    if (err != CXError_Success || !tu) {
      APP_DEBUG_LOG("CC: GenerateInoHpp - clang_parseTranslationUnit2 failed (%d)", (int)err);
      // .ino is so broken that we can't get anything out of it -> no prototypes
      return std::string();
    }

    APP_DEBUG_LOG("CC: GenerateInoHpp: %s parsed", clangFilename.c_str());

    struct VisitorData {
      std::string mainFile;
      std::vector<FnProto> *outFns;
    } data{NormalizePathForClangCompare(clangFilename), &functions};

    CXCursor tuCursor = clang_getTranslationUnitCursor(tu);

    clang_visitChildren(
        tuCursor,
        [](CXCursor cursor, CXCursor parent, CXClientData client_data) -> CXChildVisitResult {
          auto *data = static_cast<VisitorData *>(client_data);

          CXCursorKind kind = clang_getCursorKind(cursor);
          CXCursorKind pk = clang_getCursorKind(parent);

          // only top-level declarations from the main file
          if (pk != CXCursor_TranslationUnit) {
            return CXChildVisit_Recurse;
          }

          if (kind != CXCursor_FunctionDecl) {
            return CXChildVisit_Recurse;
          }

          if (!clang_isCursorDefinition(cursor))
            return CXChildVisit_Recurse;

          CXSourceLocation loc = clang_getCursorLocation(cursor);
          CXFile file;
          unsigned line = 0, col = 0, offset = 0;
          clang_getSpellingLocation(loc, &file, &line, &col, &offset);
          if (!file)
            return CXChildVisit_Recurse;

          std::string fileName = cxStringToStd(clang_getFileName(file));
          fileName = NormalizePathForClangCompare(std::move(fileName));
          std::string mainFileName = NormalizePathForClangCompare(data->mainFile);

          if (fileName != mainFileName) {
            return CXChildVisit_Recurse;
          }

          std::string name = cxStringToStd(clang_getCursorSpelling(cursor));
          if (name.empty())
            return CXChildVisit_Recurse;

          CXType funcType = clang_getCursorType(cursor);
          CXType retType = clang_getResultType(funcType);
          if (retType.kind == CXType_Invalid)
            return CXChildVisit_Recurse;

          std::string ret = cxStringToStd(clang_getTypeSpelling(retType));

          // storage class (static) - so that the prototype matches the definition
          CX_StorageClass sc = clang_Cursor_getStorageClass(cursor);
          if (sc == CX_SC_Static) {
            ret = "static " + ret;
          }

          // we compose the parameters "(type1 name1, type2 name2, ...)"
          std::string params = "(";
          int numArgs = clang_Cursor_getNumArguments(cursor);
          for (int i = 0; i < numArgs; ++i) {
            if (i > 0)
              params += ", ";

            CXCursor argCur = clang_Cursor_getArgument(cursor, i);
            CXType argType = clang_getCursorType(argCur);

            std::string argTypeStr = cxStringToStd(clang_getTypeSpelling(argType));
            std::string argNameStr = cxStringToStd(clang_getCursorSpelling(argCur));

            params += argTypeStr;
            if (!argNameStr.empty()) {
              params += " ";
              params += argNameStr;
            }
          }

          if (clang_isFunctionTypeVariadic(funcType)) {
            if (numArgs > 0)
              params += ", ";
            params += "...";
          }

          params += ")";

          FnProto fp;
          fp.ret = std::move(ret);
          fp.name = std::move(name);
          fp.params = std::move(params);
          fp.line = line;

          data->outFns->push_back(std::move(fp));
          return CXChildVisit_Recurse;
        },
        &data);

    clang_disposeTranslationUnit(tu);
  }

  // sort by definition line and deduplicate signatures
  std::sort(functions.begin(), functions.end(),
            [](const FnProto &a, const FnProto &b) {
              return a.line < b.line;
            });

  std::unordered_set<std::string> seenSignatures;

  for (auto &f : functions) {
    std::string ret = f.ret;
    std::string name = f.name;
    std::string params = f.params;

    TrimInPlace(ret);
    TrimInPlace(name);
    TrimInPlace(params);

    std::string signature = ret + " " + name + params;

    if (name.empty() || ret.empty())
      continue;

    if (!seenSignatures.insert(signature).second)
      continue;

    // to fuck up windows path separators
    std::string lineFilename = clangFilename;
    std::replace(lineFilename.begin(), lineFilename.end(), '\\', '/');

    // #line so that the prototype "lies" on the same line as the definition in .ino.cpp
    if (f.line > 0) {
      hpp += "#line ";
      hpp += std::to_string(f.line);
      hpp += " \"";
      hpp += lineFilename;
      hpp += "\"\n";
    }

    hpp += signature + ";\n";
    APP_DEBUG_LOG("CC: GenerateInoHpp: added method %s at logical line %u",
                  signature.c_str(), f.line);
  }

  APP_TRACE_LOG("CC: generated HPP:\n%s", hpp.c_str());
  APP_DEBUG_LOG("CC: generated HPP: %d bytes", (int)hpp.length());

  return hpp;
}

ClangUnsavedFiles ArduinoCodeCompletion::CreateClangUnsavedFiles(const std::string &filename,
                                                                 const std::string &code) {
  ClangUnsavedFiles uf;

  uf.mainFilename = GetClangFilename(filename);
  uf.mainCode = GetClangCode(filename, code, &uf.hppAddedLines);

  uf.files[0].Filename = uf.mainFilename.c_str();
  uf.files[0].Contents = uf.mainCode.c_str();
  uf.files[0].Length = uf.mainCode.size();
  uf.count = 1;

  if (IsIno(filename)) {
    const std::string absIno = AbsoluteFilename(filename);
    const std::size_t codeHash = HashCode(code);

    uint64_t sum = CcSumDecls(std::string_view(filename), std::string_view(code));

    std::string hppCode;
    auto it = m_inoHeaderCache.find(sum);
    if (it != m_inoHeaderCache.end()) {
      // cache hit
      hppCode = it->second.hppCode;
      APP_DEBUG_LOG("CC: InoHpp cache hit for %s", absIno.c_str());
    } else {
      // cache miss / code changed -> regenerate
      hppCode = GenerateInoHpp(filename, code);
      APP_DEBUG_LOG("CC: InoHpp cache miss for %s", absIno.c_str());

      InoHeaderCacheEntry entry;
      entry.codeHash = codeHash;
      entry.hppCode = hppCode;
      m_inoHeaderCache[sum] = std::move(entry);
    }

    uf.hppFilename = absIno + ".hpp";
    uf.hppCode = std::move(hppCode);

    if (!uf.hppCode.empty()) {
      uf.files[1].Filename = uf.hppFilename.c_str();
      uf.files[1].Contents = uf.hppCode.c_str();
      uf.files[1].Length = uf.hppCode.size();
      uf.count = 2;
    } else {
      // GenerateInoHpp may return an empty string if TU is not built at all
      APP_DEBUG_LOG("CC: InoHpp is empty for %s - skipping", absIno.c_str());
    }
  }

  return uf;
}

// They will try to get a translation unit at all costs.
// The result of a long struggle with parsing various Arduino sources.
CXTranslationUnit ArduinoCodeCompletion::GetTranslationUnit(const std::string &filename,
                                                            const std::string &code,
                                                            int *outAddedLines,
                                                            std::string *outMainFile) {
  // WARNING: expects that m_ccMutex is held!
  ScopeTimer t("CC: GetTranslationUnit()");

  APP_DEBUG_LOG("CC: GetTranslationUnit: filename=%s, code.length=%d", filename.c_str(), code.length());

  if (outAddedLines) {
    *outAddedLines = 0;
  }
  if (outMainFile) {
    *outMainFile = {};
  }

  // Normalized cache key - absolute original filename (.ino / .cpp / .h ...)
  const std::string key = AbsoluteFilename(filename);

  const std::size_t codeHash = HashCode(code);

  auto it = m_tuCache.find(key);
  if (it == m_tuCache.end()) {
    // ---------- DOES NOT EXIST HERE -> we will create ----------
    ClangUnsavedFiles uf = CreateClangUnsavedFiles(key, code);

    const auto &clangArgs = GetCompilerArgs();
    std::vector<const char *> args;
    args.reserve(clangArgs.size() + 20); // next optional flags + warning flags

    for (const auto &a : clangArgs) {
      args.push_back(a.c_str());
      APP_TRACE_LOG("CC: CLP: %s", a.c_str());
    }

    bool isHeader =
        hasSuffix(uf.mainFilename, ".h") ||
        hasSuffix(uf.mainFilename, ".hpp") ||
        hasSuffix(uf.mainFilename, ".hh");

    if (isHeader) {
      args.push_back("-x");
      args.push_back("c++-header");
      APP_TRACE_LOG("CC: CLP: -x");
      APP_TRACE_LOG("CC: CLP: c++-header");
    }

    bool isInoMain = hasSuffix(uf.mainFilename, ".ino.cpp");
    if (isInoMain) {
      args.push_back("-include");
      args.push_back("Arduino.h");
      APP_TRACE_LOG("CC: CLP: -include");
      APP_TRACE_LOG("CC: CLP: Arduino.h");
    }

    m_clangSettings.AppendWarningFlags(args);

    APP_DEBUG_LOG("CC: [TU NEW] %s (%d args)", uf.mainFilename.c_str(), args.size());

    const unsigned parseOptsFull =
        CXTranslationUnit_DetailedPreprocessingRecord |
        CXTranslationUnit_IncludeBriefCommentsInCodeCompletion |
        CXTranslationUnit_KeepGoing |
        CXTranslationUnit_PrecompiledPreamble |
        CXTranslationUnit_CreatePreambleOnFirstParse;

    const unsigned parseOptsNoPreamble =
        (parseOptsFull & ~CXTranslationUnit_LimitSkipFunctionBodiesToPreamble);

    CXTranslationUnit tu = nullptr;
    CXErrorCode err = clang_parseTranslationUnit2(
        index,
        uf.mainFilename.c_str(),
        args.data(),
        static_cast<int>(args.size()),
        uf.files,
        uf.count,
        parseOptsFull,
        &tu);

    if ((err != CXError_Success || !tu) && tu) {
      // Some libclang builds may return a TU even when err != Success (rare).
      APP_DEBUG_LOG("CC: parse returned TU but err=%d (%s) -> continuing",
                    (int)err, ClangErrorToString(err));
      err = CXError_Success;
    }

    if (err != CXError_Success || !tu) {
      APP_DEBUG_LOG("CC: could not parse %s (libclang err=%d / %s)",
                    uf.mainFilename.c_str(), (int)err, ClangErrorToString(err));

      // Context dump (helps a lot on RPi where sysroot/include paths differ)
      LogUnsavedFilesDebug(uf);
      LogClangArgsDebug(args);

      auto tryParse = [&](const char *label,
                          const std::vector<const char *> &tryArgs,
                          unsigned opts) -> CXTranslationUnit {
        CXTranslationUnit t = nullptr;
        CXErrorCode e = clang_parseTranslationUnit2(
            index,
            uf.mainFilename.c_str(),
            tryArgs.empty() ? nullptr : tryArgs.data(),
            static_cast<int>(tryArgs.size()),
            uf.files,
            uf.count,
            opts,
            &t);

        if (e == CXError_Success && t) {
          APP_DEBUG_LOG("CC: parse recovery succeeded (%s)", label);
          return t;
        }

        APP_DEBUG_LOG("CC: parse recovery failed (%s): err=%d (%s) tu=%p",
                      label, (int)e, ClangErrorToString(e), (void *)t);

        if (t) {
          // If we got a TU, show diagnostics - we may still learn something.
          LogDiagnosticsDebug(t, 40);
          clang_disposeTranslationUnit(t);
        }
        return nullptr;
      };

      // 1) Probe without "LimitSkipFunctionBodiesToPreamble" (some builds have issues here)
      if (!tu) {
        tu = tryParse("no-preamble-limit", args, parseOptsNoPreamble);
      }

      // 2) If this is the synthetic .ino.cpp, probe without the forced Arduino.h include.
      // (Often indicates missing Arduino core/toolchain include paths.)
      if (!tu && isInoMain) {
        if (args.size() >= 2 &&
            args[args.size() - 2] &&
            std::string(args[args.size() - 2]) == "-include") {
          std::vector<const char *> argsNoArduino(args.begin(), args.end() - 2);
          tu = tryParse("no-Arduino.h-force-include", argsNoArduino, parseOptsNoPreamble);
          if (!tu) {
            tu = tryParse("no-Arduino.h-force-include(full-opts)", argsNoArduino, parseOptsFull);
          }
        }
      }

      // 3) Last resort: try with *no* args to at least get a TU + diagnostics if possible.
      // This often reveals the first missing header or a broken include chain.
      if (!tu) {
        std::vector<const char *> emptyArgs;
        tu = tryParse("no-args", emptyArgs, parseOptsNoPreamble);
      }

      if (!tu) {
        return nullptr;
      }
    }

    CachedTranslationUnit entry;
    entry.filename = key;
    entry.mainFilename = uf.mainFilename;
    entry.codeHash = codeHash;
    entry.addedLines = uf.hppAddedLines;
    entry.tu = tu;

    auto [insertedIt, ok] = m_tuCache.emplace(key, std::move(entry));
    const CachedTranslationUnit &e = insertedIt->second;

    if (outAddedLines) {
      *outAddedLines = e.addedLines;
    }
    if (outMainFile) {
      *outMainFile = e.mainFilename;
    }

    return e.tu;
  } else {
    // ---------- HERE EXISTS -> possible reparse ----------

    CachedTranslationUnit &entry = it->second;

    if (entry.codeHash != codeHash) {
      // Code has changed -> we need to create unsaved files and reparse
      ClangUnsavedFiles uf = CreateClangUnsavedFiles(key, code);

      entry.codeHash = codeHash;
      entry.mainFilename = uf.mainFilename;
      entry.addedLines = uf.hppAddedLines;

      APP_DEBUG_LOG("CC: [TU REPARSE] %s (hash changed)",
                    uf.mainFilename.c_str());

      clang_reparseTranslationUnit(
          entry.tu,
          uf.count,
          uf.files,
          clang_defaultReparseOptions(entry.tu));
    } else {
      // Same hash -> code has not changed, no need to call CreateClangUnsavedFiles
      APP_DEBUG_LOG("CC: [TU CACHE-HIT] %s (no reparse)",
                    entry.mainFilename.c_str());
    }

    if (outAddedLines) {
      *outAddedLines = entry.addedLines;
    }
    if (outMainFile) {
      *outMainFile = entry.mainFilename;
    }

    return entry.tu;
  }
}

CXTranslationUnit ArduinoCodeCompletion::GetTranslationUnitNoReparse(
    const std::string &filename,
    const std::string &code,
    int *outAddedLines,
    std::string *outMainFile) {
  // expects m_ccMutex to be held, just like GetTranslationUnit

  if (outAddedLines)
    *outAddedLines = 0;
  if (outMainFile)
    *outMainFile = {};

  const std::string key = AbsoluteFilename(filename);
  auto it = m_tuCache.find(key);
  if (it != m_tuCache.end()) {
    const auto &entry = it->second;
    if (outAddedLines)
      *outAddedLines = entry.addedLines;
    if (outMainFile)
      *outMainFile = entry.mainFilename;
    return entry.tu;
  }

  // TU does not exist yet for this file -> we will create it
  return GetTranslationUnit(filename, code, outAddedLines, outMainFile);
}

std::vector<ArduinoParseError> ArduinoCodeCompletion::ParseCode(const std::string &filename,
                                                                const std::string &code) {
  // WARNING: called only under m_ccMutex!

  int addedLines = 0;
  std::string mainFile;

  CXTranslationUnit tu = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu) {
    ArduinoParseError e;
    e.file = filename;
    e.line = 0;
    e.column = 0;
    e.message = "Failed to parse translation unit";
    e.severity = CXDiagnostic_Error;
    return {e};
  }

  return CollectDiagnosticsLocked(tu);
}

std::size_t ArduinoCodeCompletion::DiagHashLocked(const std::vector<ArduinoParseError> &errs, const std::string &sketchDir) const {

  std::size_t h = 1469598103934665603ull; // FNV-1a

  for (const auto &e : errs) {
    // filter only files within the sketch dir:
    if (!e.file.empty() && e.file.rfind(sketchDir, 0) == 0) {
      // primitive hash - file+line+column+message
      for (char c : e.file)
        h ^= (unsigned char)c, h *= 1099511628211ull;
      h ^= (unsigned)e.line;
      h *= 1099511628211ull;
      h ^= (unsigned)e.column;
      h *= 1099511628211ull;
      for (char c : e.message)
        h ^= (unsigned char)c, h *= 1099511628211ull;
    }
  }

  return h;
}

std::size_t ArduinoCodeCompletion::HashCode(const std::string &code) {
  std::size_t h = 1469598103934665603ull; // FNV-1a offset

  for (unsigned char c : code) {
    h ^= c;
    h *= 1099511628211ull;
  }

  return h;
}

// Prefer expansion/presumed location if it points into sketch dir, otherwise keep spelling location.
// Drop-in replacement for clang_getSpellingLocation used only for diagnostics mapping.
// It may be a little confusing at first glance, but it can help the user.
void ArduinoCodeCompletion::AeGetBestDiagLocation(CXSourceLocation loc,
                                                  CXFile *out_file,
                                                  unsigned *out_line,
                                                  unsigned *out_column,
                                                  unsigned *out_offset) const {
  if (!out_file || !out_line || !out_column || !out_offset) {
    return;
  }

  auto cxFilePath = [](CXFile f) -> std::string {
    if (!f)
      return {};
    CXString s = clang_getFileName(f);
    return cxStringToStd(s);
  };

  // 1) Get spelling (fallback/default)
  CXFile spFile = nullptr;
  unsigned spLine = 0, spCol = 0, spOff = 0;
  clang_getSpellingLocation(loc, &spFile, &spLine, &spCol, &spOff);

  // 2) Get expansion
  CXFile exFile = nullptr;
  unsigned exLine = 0, exCol = 0, exOff = 0;
  clang_getExpansionLocation(loc, &exFile, &exLine, &exCol, &exOff);

  // 3) Get presumed (note: returns filename as CXString, not CXFile)
  CXString presName;
  unsigned prLine = 0, prCol = 0;
  clang_getPresumedLocation(loc, &presName, &prLine, &prCol);
  std::string presPath = cxStringToStd(presName);

  const std::string sketchDir = arduinoCli->GetSketchPath();

  auto pathOf = [&](CXFile f) -> std::string { return cxFilePath(f); };

  // Candidate validity
  const bool expOk = exFile && exLine > 0;
  const bool splOk = spFile && spLine > 0;
  const bool preOk = !presPath.empty() && prLine > 0;

  // Prefer a location inside sketch dir: Expansion > Spelling > Presumed.
  // If none are in sketch, keep original Spelling (fallback), otherwise Expansion, otherwise Presumed.
  enum Pick { PickSpelling,
              PickExpansion,
              PickPresumed };
  Pick pick = PickSpelling;

  if (expOk && IsInSketchDir(sketchDir, pathOf(exFile))) {
    pick = PickExpansion;
  } else if (splOk && IsInSketchDir(sketchDir, pathOf(spFile))) {
    pick = PickSpelling;
  } else if (preOk && IsInSketchDir(sketchDir, presPath)) {
    pick = PickPresumed;
  } else if (splOk) {
    pick = PickSpelling;
  } else if (expOk) {
    pick = PickExpansion;
  } else if (preOk) {
    pick = PickPresumed;
  }

  // Apply pick
  switch (pick) {
    case PickExpansion:
      *out_file = exFile;
      *out_line = exLine;
      *out_column = exCol;
      *out_offset = exOff;
      break;

    case PickPresumed:
      // We don't have CXFile for presumed reliably; keep best available file handle.
      *out_file = spFile ? spFile : exFile;
      *out_line = prLine;
      *out_column = prCol;
      *out_offset = 0;
      break;

    case PickSpelling:
    default:
      *out_file = spFile;
      *out_line = spLine;
      *out_column = spCol;
      *out_offset = spOff;
      break;
  }
}

std::vector<ArduinoParseError> ArduinoCodeCompletion::CollectDiagnosticsLocked(CXTranslationUnit tu) const {
  std::vector<ArduinoParseError> errors;

  if (!tu) {
    APP_DEBUG_LOG("CC: diagnostic unavailable; CXTranslationUnit empty.");
    return errors;
  }

  unsigned numDiags = clang_getNumDiagnostics(tu);
  errors.reserve(numDiags);

  APP_DEBUG_LOG("CC: available total %u diagnostic messages...", numDiags);

  std::string sketchDir = arduinoCli->GetSketchPath();

  auto makeParseErrorFromDiag = [&](CXDiagnostic d) -> ArduinoParseError {
    ArduinoParseError out;

    out.severity = clang_getDiagnosticSeverity(d);

    // message
    CXString spellingStr = clang_getDiagnosticSpelling(d);
    out.message = cxStringToStd(spellingStr);

    // location
    CXSourceLocation loc = clang_getDiagnosticLocation(d);
    CXFile f;
    unsigned l = 0, c = 0, off = 0;
    AeGetBestDiagLocation(loc, &f, &l, &c, &off);

    out.line = l;
    out.column = c;

    if (f) {
      CXString fnStr = clang_getFileName(f);
      out.file = cxStringToStd(fnStr);
    }

    return out;
  };

  auto noteKey = [](const ArduinoParseError &e) -> std::string {
    // key for deduplication of notes (enough for practical cases)
    std::ostringstream os;
    os << e.file << ":" << e.line << ":" << e.column << ":" << e.message;
    return os.str();
  };

  unsigned i = 0;
  while (i < numDiags) {
    CXDiagnostic diag = clang_getDiagnostic(tu, i);
    CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diag);

    // we skip the notes themselves (usually we paste them to the previous error/warning)
    if (severity == CXDiagnostic_Note || severity == CXDiagnostic_Ignored) {
      clang_disposeDiagnostic(diag);
      ++i;
      continue;
    }

    // parent
    ArduinoParseError e = makeParseErrorFromDiag(diag);

    // ---- NOTES: child diagnostics + sequential notes (dedup) ----
    std::unordered_set<std::string> seenNotes;
    seenNotes.reserve(16);

    // 1) child diagnostics (clang_getChildDiagnostics)
    {
      CXDiagnosticSet childSet = clang_getChildDiagnostics(diag);
      unsigned n = clang_getNumDiagnosticsInSet(childSet);
      for (unsigned k = 0; k < n; ++k) {
        CXDiagnostic child = clang_getDiagnosticInSet(childSet, k);
        if (clang_getDiagnosticSeverity(child) == CXDiagnostic_Note) {
          ArduinoParseError ch = makeParseErrorFromDiag(child);
          std::string key = noteKey(ch);
          if (seenNotes.insert(key).second) {
            e.childs.push_back(std::move(ch));
          }
        }
        clang_disposeDiagnostic(child);
      }
      clang_disposeDiagnosticSet(childSet);
    }

    // 2) sequential notes (lookahead)
    unsigned j = i + 1;
    while (j < numDiags) {
      CXDiagnostic next = clang_getDiagnostic(tu, j);
      CXDiagnosticSeverity ns = clang_getDiagnosticSeverity(next);

      if (ns != CXDiagnostic_Note) {
        clang_disposeDiagnostic(next);
        break;
      }

      ArduinoParseError ch = makeParseErrorFromDiag(next);
      std::string key = noteKey(ch);
      if (seenNotes.insert(key).second) {
        e.childs.push_back(std::move(ch));
      }

      clang_disposeDiagnostic(next);
      ++j;
    }

    clang_disposeDiagnostic(diag);

    // ---------- FILTERING (only parent) ----------
    if (e.file.empty()) {
      i = j;
      continue;
    }

    bool fromSketch = (e.file.rfind(sketchDir, 0) == 0);
    if (!fromSketch) {
      i = j;
      continue;
    }

    if (e.severity != CXDiagnostic_Warning &&
        e.severity != CXDiagnostic_Error &&
        e.severity != CXDiagnostic_Fatal) {
      i = j;
      continue;
    }
    // ---------- END OF FILTERING ----------

    APP_TRACE_LOG("CC: collected:  sev=%d %s:%u:%u: %s",
                  (int)e.severity,
                  e.file.c_str(),
                  (unsigned)e.line,
                  (unsigned)e.column,
                  e.message.c_str());

    if (!e.childs.empty()) {
      for (const auto &ch : e.childs) {
        APP_TRACE_LOG("CC: note: sev=%d %s:%u:%u: %s",
                      (int)ch.severity,
                      ch.file.c_str(),
                      (unsigned)ch.line,
                      (unsigned)ch.column,
                      ch.message.c_str());
      }
    }

    errors.push_back(std::move(e));
    i = j;
  }

  // ------ SORTING BY SEVERITY + FILE/LINE/COL ------
  auto severityRank = [](CXDiagnosticSeverity s) {
    switch (s) {
      case CXDiagnostic_Error:
      case CXDiagnostic_Fatal:
        return 0;
      case CXDiagnostic_Warning:
        return 1;
      default:
        return 2;
    }
  };

  std::sort(errors.begin(), errors.end(),
            [&](const ArduinoParseError &a, const ArduinoParseError &b) {
              int ra = severityRank(a.severity);
              int rb = severityRank(b.severity);
              if (ra != rb)
                return ra < rb;

              if (a.file != b.file)
                return a.file < b.file;
              if (a.line != b.line)
                return a.line < b.line;
              return a.column < b.column;
            });

  return errors;
}

std::size_t ArduinoCodeCompletion::ComputeDiagHash(const std::vector<ArduinoParseError> &errs) {
  // simple FNV-1a
  std::size_t h = 1469598103934665603ull;

  auto fnvMix = [&h](unsigned char c) {
    h ^= c;
    h *= 1099511628211ull;
  };

  for (const auto &e : errs) {
    for (char c : e.file)
      fnvMix((unsigned char)c);
    for (char c : e.message)
      fnvMix((unsigned char)c);

    // line/column
    for (int i = 0; i < 4; ++i)
      fnvMix((e.line >> (i * 8)) & 0xFF);
    for (int i = 0; i < 4; ++i)
      fnvMix((e.column >> (i * 8)) & 0xFF);
  }

  return h;
}

void ArduinoCodeCompletion::NotifyDiagnosticsChangedLocked(const std::vector<ArduinoParseError> &errs) {
  std::size_t newHash = ComputeDiagHash(errs);
  if ((m_lastDiagHash != 0) && (newHash == m_lastDiagHash)) {
    APP_DEBUG_LOG("No diagnostics errors changed; skipping event.");
    return;
  }

  m_lastDiagHash = newHash;

  wxThreadEvent evt(EVT_DIAGNOSTICS_UPDATED);
  QueueUiEvent(m_eventHandler, evt.Clone());
}

std::vector<ArduinoParseError> ArduinoCodeCompletion::GetErrorsFor(const std::string &filename) const {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  // The same normalization as in GetTranslationUnit - the key is the absolute filename
  std::string key = AbsoluteFilename(filename);

  auto it = m_tuCache.find(key);
  if (it == m_tuCache.end() || !it->second.tu) {
    return {};
  }

  return CollectDiagnosticsLocked(it->second.tu);
}

void ArduinoCodeCompletion::RefreshDiagnosticsAsync(const std::string &filename,
                                                    const std::string &code) {

  if (!m_ready)
    return;

  std::vector<SketchFileBuffer> filesSnapshot;
  CollectSketchFiles(filesSnapshot);

  std::thread([this, filename, code, filesSnapshot = std::move(filesSnapshot)]() {
    CcFilesSnapshotGuard guard(&filesSnapshot);

    std::lock_guard<std::mutex> lock(m_ccMutex);

    // ParseCode takes care of creating/updating the TU as well as calculating errors
    auto errors = ParseCode(filename, code);

    NotifyDiagnosticsChangedLocked(errors);
  }).detach();
}

std::vector<CompletionItem> ArduinoCodeCompletion::GetCompletions(const std::string &filename,
                                                                  const std::string &code,
                                                                  int line,
                                                                  int column) {
  std::lock_guard<std::mutex> lock(m_ccMutex);
  ScopeTimer t("CC: GetCompletions()");

  std::vector<CompletionItem> items;

  // 1) Ensure TU (cached variant due to speed)
  int dummyAddedLines = 0;
  CXTranslationUnit tu = GetTranslationUnitNoReparse(filename, code, &dummyAddedLines, nullptr);
  if (!tu) {
    return items;
  }

  // 2) For completion, we need the current unsaved files
  ClangUnsavedFiles uf = CreateClangUnsavedFiles(filename, code);
  int addedLines = uf.hppAddedLines;

  unsigned options = clang_defaultCodeCompleteOptions();
  options |= CXCodeComplete_IncludeMacros;

  auto start = Clock::now();

  CXCodeCompleteResults *results = clang_codeCompleteAt(
      tu,
      uf.mainFilename.c_str(),
      line + addedLines,
      column,
      uf.files,
      uf.count,
      options);

  if (!results) {
    return items;
  }

  auto end = Clock::now();
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  APP_DEBUG_LOG("CC: - clang_codeCompleteAt() %lldus", static_cast<long long>(us));

  start = Clock::now();

  // 3) Processing results - your original code, just leave it as it is
  for (unsigned i = 0; i < results->NumResults; i++) {
    CXCompletionResult result = results->Results[i];
    CXCompletionString completionString = result.CompletionString;

    //  Filtering of inaccessible symbols (protected/private, etc.)
    CXAvailabilityKind avail = clang_getCompletionAvailability(completionString);
    if (avail == CXAvailability_NotAccessible) {
      continue; // this cannot be legally used from the given location -> remove from the menu
    }

    std::string completionText;
    std::string displayText;

    unsigned numChunks = clang_getNumCompletionChunks(completionString);
    for (unsigned j = 0; j < numChunks; j++) {
      CXCompletionChunkKind kind = clang_getCompletionChunkKind(completionString, j);
      CXString text = clang_getCompletionChunkText(completionString, j);
      const char *chunkText = clang_getCString(text);

      if (kind == CXCompletionChunk_TypedText) {
        completionText = chunkText;
      }

      if (chunkText) {
        displayText += chunkText;
      }

      clang_disposeString(text);
    }

    if (!completionText.empty()) {
      CompletionItem item;
      buildCompletionTexts(completionString, item.text, item.label);
      item.kind = result.CursorKind;
      item.priority = kindScore(result.CursorKind);
      items.push_back(item);
    }
  }

  clang_disposeCodeCompleteResults(results);

  std::sort(items.begin(), items.end(),
            [](const CompletionItem &a, const CompletionItem &b) {
              return a.priority < b.priority;
            });

  end = Clock::now();
  us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  APP_DEBUG_LOG("CC: - extracting %d completions & sorting %lldus", items.size(), static_cast<long long>(us));
  return items;
}

void ArduinoCodeCompletion::FilterAndSortCompletionsWithPrefix(const std::string &prefix, std::vector<CompletionItem> &inOutCompletions) {

  ScopeTimer("CC: FilterAndSortCompletionsWithPrefix(%s)", prefix.c_str());

  inOutCompletions.erase(
      std::remove_if(inOutCompletions.begin(), inOutCompletions.end(),
                     [&](const CompletionItem &c) {
                       const std::string &t = c.text;
                       if (t.empty())
                         return true;

                       if (prefix.empty()) {
                         return !c.fromSketch;
                       } else {
                         if (!startsWithCaseSensitive(t, prefix) &&
                             !startsWithCaseInsensitive(t, prefix) &&
                             !containsCaseInsensitive(t, prefix)) {
                           return true;
                         }
                       }
                       return false;
                     }),
      inOutCompletions.end());

  std::sort(inOutCompletions.begin(), inOutCompletions.end(),
            [&](const CompletionItem &a, const CompletionItem &b) {
              const std::string &ta = a.text;
              const std::string &tb = b.text;

              //  1) Exact match with the prefix has absolute priority
              bool a_exact = (ta == prefix);
              bool b_exact = (tb == prefix);
              if (a_exact != b_exact)
                return a_exact; // true < false -> exact upwards

              //  2) Starts with prefix (case-sensitive)
              bool a_cs = startsWithCaseSensitive(ta, prefix);
              bool b_cs = startsWithCaseSensitive(tb, prefix);
              if (a_cs != b_cs)
                return a_cs;

              //  3) Starts with the prefix (case-insensitive)
              bool a_ci = startsWithCaseInsensitive(ta, prefix);
              bool b_ci = startsWithCaseInsensitive(tb, prefix);
              if (a_ci != b_ci)
                return a_ci;

              //  4) fromSketch up
              if (a.fromSketch != b.fromSketch)
                return a.fromSketch;

              //  5) kindScore
              int ak = kindScore(a.kind);
              int bk = kindScore(b.kind);
              if (ak != bk)
                return ak < bk;

              //  6) priority (from libclang)
              if (a.priority != b.priority)
                return a.priority < b.priority;

              //  7) Macro-like downwards
              bool a_macroLike = isAllUpperAlpha(ta) && countUnderscores(ta) > 0;
              bool b_macroLike = isAllUpperAlpha(tb) && countUnderscores(tb) > 0;
              if (a_macroLike != b_macroLike)
                return !a_macroLike;

              //  8) Fewer underscores / shorter / lexicographically
              int a_us = countUnderscores(ta);
              int b_us = countUnderscores(tb);
              if (a_us != b_us)
                return a_us < b_us;

              if (ta.size() != tb.size())
                return ta.size() < tb.size();

              return ta < tb;
            });

  // --- Collapse overloads by inserted text, keep them prepared in CompletionItem::overloads ---
  {
    std::unordered_set<std::string> seen;
    seen.reserve(inOutCompletions.size());

    auto outIt = inOutCompletions.begin();

    for (auto it = inOutCompletions.begin(); it != inOutCompletions.end(); ++it) {
      if (it->text.empty())
        continue;

      const std::string &key = it->text;

      if (seen.insert(key).second) {
        // First occurrence => representative item
        if (outIt != it)
          *outIt = std::move(*it);
        ++outIt;
      } else {
        // Another overload => append to representative's overloads
        // Representative is guaranteed to be the first one we kept for this key.
        // It's somewhere in [begin, outIt).
        for (auto rep = inOutCompletions.begin(); rep != outIt; ++rep) {
          if (rep->text == key) {
            rep->overloads.push_back(std::move(*it));
            break;
          }
        }
      }
    }

    // Now annotate labels (only for those that actually have overloads)
    for (auto it = inOutCompletions.begin(); it != outIt; ++it) {
      if (!it->overloads.empty()) {
        const int total = 1 + static_cast<int>(it->overloads.size());
        // keep label short; avoid long signatures drowning UI
        it->label = it->text + "(...) - " + std::to_string(total) + " overloads";
      }
    }

    inOutCompletions.erase(outIt, inOutCompletions.end());
  }

  constexpr std::size_t MAX_ITEMS = 256;
  if (inOutCompletions.size() > MAX_ITEMS) {
    inOutCompletions.resize(MAX_ITEMS);
  }
}

void ArduinoCodeCompletion::CollectSketchFiles(std::vector<SketchFileBuffer> &outFiles) const {
  if (!wxIsMainThread()) {
    AE_TRAP_MSG("CC: CollectSketchFiles(&); must run on the main (GUI) thread");
  }
  m_collectSketchFilesFn(outFiles);
}

std::vector<std::string> ArduinoCodeCompletion::GetCompilerArgs() const {
  // If we are running in a worker thread where a snapshot is set, we will use it.
  if (g_ccFilesSnapshot && !g_ccFilesSnapshot->empty()) {
    return GetCompilerArgs(*g_ccFilesSnapshot);
  }

  if (wxIsMainThread()) {
    // If we are on the main thread, we can still check out
    // the files directly from the editors.
    std::vector<SketchFileBuffer> files;
    CollectSketchFiles(files);
    return GetCompilerArgs(files);
  }

  AE_TRAP_MSG("CC: No files snapshot available!");

  return {};
}

std::vector<std::string> ArduinoCodeCompletion::GetCompilerArgs(const std::vector<SketchFileBuffer> &files) const {
  if (!arduinoCli) {
    return {};
  }

  ScopeTimer t("CC: GetCompilerArgs()");

  APP_DEBUG_LOG("CC: GetCompilerArgs(%zu files)", files.size());

  std::vector<std::string> result;
  std::vector<std::string> compArgs = arduinoCli->GetCompilerArgs();

  if (arduinoCli->IsInitializedFromCompileCommands()) {
    // If it is evaluated via compile_command, the library includes
    // are already in the arguments from ArduinoCli.

    result = compArgs;
  } else {
    std::vector<std::string> libsIncludes = ResolveLibrariesIncludes(files);

    if (!libsIncludes.empty()) {
      std::string platformPath = arduinoCli->GetPlatformPath();
      std::vector<std::string> merged;
      merged.reserve(compArgs.size() + libsIncludes.size());
      bool injected = false;

      for (const auto &a : compArgs) {
        merged.push_back(a);
        if (!injected && a.size() >= 2 && a[0] == '-' && a[1] == 'I') {
          for (const auto &dir : libsIncludes) {
            if (!platformPath.empty() && (dir.find(platformPath, 0) == 0)) {
              merged.push_back("-isystem");
              merged.push_back(dir);
              continue;
            }

            merged.push_back("-I" + dir);
          }
          injected = true;
        }
      }

      if (!injected) {
        for (const auto &dir : libsIncludes) {
          merged.push_back("-I" + dir);
        }
      }

      result = merged;
    } else {
      result = compArgs;
    }
  }

  return result;
}

void ArduinoCodeCompletion::ShowAutoCompletionAsync(wxStyledTextCtrl *editor, std::string filename, CompletionMetadata &metadata, wxEvtHandler *handler) {

  if (!m_ready)
    return;

  // --- GUI thread: read the editor state ---
  int currentPos = editor->GetCurrentPos();
  int line = editor->LineFromPosition(currentPos) + 1;
  int column = editor->GetColumn(currentPos) + 1;

  std::string code = wxToStd(editor->GetText());

  int wordStart = editor->WordStartPosition(currentPos, true);
  int lengthEntered = currentPos - wordStart;

  std::string prefix;
  if (lengthEntered > 0) {
    prefix = wxToStd(editor->GetTextRange(wordStart, currentPos));
  }

  APP_DEBUG_LOG("CC: prefix: '%s'", prefix.c_str());

  // new request ID
  uint64_t seq = ++m_seq;
  metadata.m_pendingRequestId = seq;

  // --- 0) I will try to use the session cache (without calling libclang) ---
  std::vector<CompletionItem> cachedItems;
  bool canUseCache = false;

  std::vector<SketchFileBuffer> filesSnapshot;
  CollectSketchFiles(filesSnapshot);

  {
    std::lock_guard<std::mutex> lock(m_completionSessionMutex);
    const auto &sess = m_completionSession;

    // conditions for "same session":
    // - the same file
    // - we are standing in the same word (same wordStart)
    // - we have some basic items
    // - the new prefix is an extension of basePrefix (or basePrefix is empty)
    if (sess.valid &&
        sess.filename == AbsoluteFilename(filename) &&
        sess.wordStart == wordStart &&
        !sess.baseItems.empty() &&
        (sess.basePrefix.empty() ||
         prefix.rfind(sess.basePrefix, 0) == 0)) { // prefix starts with basePrefix
      cachedItems = sess.baseItems;                // copy - we will filter it
      canUseCache = true;
    }
  }

  if (canUseCache) {
    // Pure memory filter - no thread, no clang.
    auto start = Clock::now();
    auto completions = std::move(cachedItems);

    if (!prefix.empty()) {
      FilterAndSortCompletionsWithPrefix(prefix, completions);
    }

    auto end = Clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    APP_DEBUG_LOG("Completion from cache: filtered %d items in %lld s.",
                  (int)completions.size(), (long long)us);

    // Event to GUI
    wxThreadEvent evt(EVT_COMPLETION_READY);
    evt.SetInt((int)seq);
    evt.SetExtraLong(wordStart);
    evt.SetPayload(completions);
    evt.SetString(wxString::Format(wxT("%d"), lengthEntered));

    QueueUiEvent(handler, evt.Clone());
    return;
  }

  // --- worker thread ---
  std::thread([this,
               handler,
               seq,
               filename = std::move(filename),
               code = std::move(code),
               line,
               column,
               prefix,
               wordStart,
               lengthEntered,
               filesSnapshot = std::move(filesSnapshot)]() {
    // Guard sets a thread-local snapshot for the entire thread run
    CcFilesSnapshotGuard guard(&filesSnapshot);

    // 1) Get completions from libclang
    auto start = Clock::now();

    auto completions = GetCompletions(filename, code, line, column);

    auto end = Clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    APP_DEBUG_LOG("Total %d completions in %lld s.", completions.size(), static_cast<long long>(us));

    start = Clock::now();

    // 2) Build a map "name -> file" for symbols from the current sketch dir
    std::unordered_map<std::string, std::string> localSymbols;
    std::string sketchDir;
    if (arduinoCli) {
      sketchDir = arduinoCli->GetSketchPath();
    }

    if (!sketchDir.empty()) {
      auto symbols = GetAllSymbols(filename, code);
      for (const auto &s : symbols) {
        if (s.file.empty())
          continue;

        // only files within the sketch directory
        if (s.file.rfind(sketchDir, 0) != 0)
          continue;

        // save only the first occurrence of the given name
        if (localSymbols.find(s.name) == localSymbols.end()) {
          localSymbols.emplace(s.name, s.file);
        }
      }
    }

    // 3) Mark completions that come from the current sketch dir
    if (!localSymbols.empty()) {
      for (auto &c : completions) {
        auto it = localSymbols.find(c.text);
        if (it != localSymbols.end()) {
          c.fromSketch = true;
          c.file = it->second;
        }
      }
    }

    // >>> 3b) Save session cache (before prefix filter) <<<
    {
      std::lock_guard<std::mutex> lock(m_completionSessionMutex);
      m_completionSession.valid = true;
      m_completionSession.filename = AbsoluteFilename(filename);
      m_completionSession.wordStart = wordStart;
      m_completionSession.basePrefix = prefix;     // prefix at the moment of heavy completion
      m_completionSession.baseItems = completions; // a copy that we do not modify further
    }

    end = Clock::now();
    us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    APP_DEBUG_LOG("Fetching symbols from sketch %lld s", static_cast<long long>(us));

    // 4) Prefix filter - keep only what the prefix CONTAINS (case-insensitive),
    //    or it starts with it (which is a subset).
    if (!prefix.empty()) {
      FilterAndSortCompletionsWithPrefix(prefix, completions);
    }

    // preparing event for GUI thread
    wxThreadEvent evt(EVT_COMPLETION_READY);
    evt.SetInt((int)seq);                                      // Request ID
    evt.SetExtraLong(wordStart);                               // save wordStart
    evt.SetPayload(completions);                               // vector<CompletionItem>
    evt.SetString(wxString::Format(wxT("%d"), lengthEntered)); // entered prefix length

    QueueUiEvent(handler, evt.Clone());
  }).detach();
}

/** Returns hover info for symbol at cursor location. */
bool ArduinoCodeCompletion::GetHoverInfo(const std::string &filename, const std::string &code, int line, int column, HoverInfo &outInfo) {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  if (!m_ready)
    return false;

  std::vector<SketchFileBuffer> filesSnapshot;
  CollectSketchFiles(filesSnapshot);
  CcFilesSnapshotGuard guard(&filesSnapshot);

  outInfo = HoverInfo{};

  int addedLines = 0;
  std::string mainFile;
  CXTranslationUnit tu = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu) {
    return false;
  }

  std::string clangFilename = GetClangFilename(filename);
  CXFile cxFile = clang_getFile(tu, clangFilename.c_str());
  if (!cxFile) {
    return false;
  }

  CXSourceLocation loc =
      clang_getLocation(tu, cxFile, line + addedLines, column);
  if (clang_equalLocations(loc, clang_getNullLocation())) {
    return false;
  }

  CXCursor cursor = clang_getCursor(tu, loc);
  if (clang_Cursor_isNull(cursor)) {
    return false;
  }

  CXCursor target = clang_getCursorReferenced(cursor);
  if (clang_Cursor_isNull(target)) {
    target = clang_getCursorDefinition(cursor);
  }
  if (!clang_Cursor_isNull(target)) {
    cursor = target;
  }

  if (clang_Cursor_isNull(cursor)) {
    return false;
  }

  // Filling HoverInfo
  outInfo.name = cxStringToStd(clang_getCursorSpelling(cursor));

  // kind -> string
  CXCursorKind ckind = clang_getCursorKind(cursor);
  outInfo.kind = cxStringToStd(clang_getCursorKindSpelling(ckind));

  // type of symbol
  CXType ctype = clang_getCursorType(cursor);
  if (ctype.kind != CXType_Invalid) {
    outInfo.type = cxStringToStd(clang_getTypeSpelling(ctype));
  }

  // signature - from displayName (often already nice)
  outInfo.signature = cxStringToStd(clang_getCursorDisplayName(cursor));

  bool isCtorOrDtor =
      (ckind == CXCursor_Constructor || ckind == CXCursor_Destructor);

  if (!isCtorOrDtor &&
      (ckind == CXCursor_FunctionDecl ||
       ckind == CXCursor_CXXMethod ||
       ckind == CXCursor_FunctionTemplate)) {
    // Add the return type before the signature (but NOT for ctors/dtors)
    CXType retType = clang_getResultType(ctype);
    if (retType.kind != CXType_Invalid) {
      std::string ret = cxStringToStd(clang_getTypeSpelling(retType));
      if (!ret.empty() && !outInfo.signature.empty()) {
        outInfo.signature = ret + " " + outInfo.signature;
      }
    }
  }

  // Comments (Doxygen, ///, /** */ etc.)
  outInfo.briefComment = cxStringToStd(clang_Cursor_getBriefCommentText(cursor));
  outInfo.fullComment = cxStringToStd(clang_Cursor_getRawCommentText(cursor));

  // Non Doxygen comments
  // Fallback: if clang doesn't return a doc comment (typically Arduino // comments),
  // try to pull the comment block just above the declaration/definition.
  if (outInfo.briefComment.empty() && outInfo.fullComment.empty()) {
    ScopeTimer t("CC: Harvest non Doxygen comment...");

    JumpTarget siblingDef;
    bool siblingDefFound = false;

    std::string codeBlock;

    // 1) comment block above this cursor (in the file where the cursor lives)
    {
      CXSourceLocation cloc = clang_getCursorLocation(cursor);
      CXFile cfile;
      unsigned cline = 0, ccol = 0, coff = 0;
      clang_getSpellingLocation(cloc, &cfile, &cline, &ccol, &coff);

      if (cfile && cline > 0) {
        std::string path = NormalizeFilename(arduinoCli->GetSketchPath(), cxStringToStd(clang_getFileName(cfile)));
        if (!path.empty()) {
          std::string txt;
          if (LoadFileToString(path, txt)) {
            std::string extracted = ExtractCommentBlockAboveLine(txt, (int)cline);
            if (!extracted.empty()) {
              outInfo.fullComment = extracted;
              outInfo.briefComment = MakeBriefFromFull(extracted);
            } else {
              unsigned blFrom = 0, bcFrom = 0, blTo = 0, bcTo = 0;
              if (GetBodyRangeForCursor(target, blFrom, bcFrom, blTo, bcTo)) {
                codeBlock = ExtractBodySnippetFromText(txt, blFrom, blTo);
              } else {
                codeBlock = ExtractBodySnippetFromText(txt, cline, cline);
              }
            }
          }
        }
      }
    }

    // 2) If still nothing and cursor is declared in the header, try sibling .cpp definition
    if (outInfo.fullComment.empty()) {
      siblingDefFound = FindSiblingFunctionDefinition(cursor, siblingDef);

      if (siblingDefFound) {
        if (!siblingDef.file.empty() && siblingDef.line > 0) {
          std::string txt;
          if (LoadFileToString(NormalizeFilename(arduinoCli->GetSketchPath(), siblingDef.file), txt)) {
            std::string extracted = ExtractCommentBlockAboveLine(txt, siblingDef.line);
            if (!extracted.empty()) {
              outInfo.fullComment = extracted;
              outInfo.briefComment = MakeBriefFromFull(extracted);
            } else if (codeBlock.empty()) {
              codeBlock = ExtractBodySnippetFromText(txt, siblingDef.line, siblingDef.line + 3);
            }
          }
        }
      }

      if (!codeBlock.empty()) {
        outInfo.fullComment = wxToStd(_("Declaration:")) + "\n" + NormalizeIndent(codeBlock, 2);
        outInfo.briefComment.clear();
      }
    }
  }

  // Function/method parameters (where it makes sense)
  if (ckind == CXCursor_FunctionDecl ||
      ckind == CXCursor_CXXMethod ||
      ckind == CXCursor_Constructor ||
      ckind == CXCursor_Destructor ||
      ckind == CXCursor_FunctionTemplate) {
    FillParameterInfoFromCursor(cursor, outInfo.parameters);
  }

  // When we have neither a name nor a signature, it doesn't make much sense to display it
  if (outInfo.name.empty() && outInfo.signature.empty() &&
      outInfo.briefComment.empty() && outInfo.fullComment.empty()) {
    return false;
  }

  return true;
}

bool ArduinoCodeCompletion::GetSymbolInfo(const std::string &filename,
                                          const std::string &code,
                                          int line,
                                          int column,
                                          SymbolInfo &outInfo) {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  outInfo = SymbolInfo{};

  if (!m_ready)
    return false;

  int addedLines = 0;
  std::string mainFile;

  CXTranslationUnit tu = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu) {
    return false;
  }

  std::string clangFilename = GetClangFilename(filename);

  CXFile cxFile = clang_getFile(tu, clangFilename.c_str());
  if (!cxFile) {
    return false;
  }

  CXSourceLocation loc =
      clang_getLocation(tu, cxFile, line + addedLines, column);
  if (clang_equalLocations(loc, clang_getNullLocation())) {
    return false;
  }

  CXCursor cursor = clang_getCursor(tu, loc);
  if (clang_Cursor_isNull(cursor)) {
    return false;
  }

  // prefer referenced, definition, and at end cursor
  CXCursor ref = clang_getCursorReferenced(cursor);
  CXCursor def = clang_getCursorDefinition(cursor);
  CXCursor target = clang_getNullCursor();

  if (!clang_Cursor_isNull(ref))
    target = ref;
  if (!clang_Cursor_isNull(def))
    target = def;
  if (clang_Cursor_isNull(target))
    target = cursor;
  if (clang_Cursor_isNull(target)) {
    return false;
  }

  // Get declaration of target symbol.
  CXSourceLocation tloc = clang_getCursorLocation(target);
  CXFile tfile;
  unsigned tline = 0, tcol = 0, toff = 0;
  clang_getSpellingLocation(tloc, &tfile, &tline, &tcol, &toff);
  if (!tfile) {
    return false;
  }

  std::string fileName = cxStringToStd(clang_getFileName(tfile));

  // Fill SymbolInfo
  outInfo.name = cxStringToStd(clang_getCursorSpelling(target));
  outInfo.display = cxStringToStd(clang_getCursorDisplayName(target));
  if (outInfo.display.empty())
    outInfo.display = outInfo.name;

  outInfo.file = fileName;
  outInfo.line = (int)tline;
  outInfo.column = (int)tcol;
  outInfo.kind = clang_getCursorKind(target);

  // - body range
  outInfo.bodyLineFrom = 0;
  outInfo.bodyColFrom = 0;
  outInfo.bodyLineTo = 0;
  outInfo.bodyColTo = 0;

  // Parameters funcs/metod/constructor/destructor
  switch (outInfo.kind) {
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_FunctionTemplate:
      FillParameterInfoFromCursor(target, outInfo.parameters);
      break;
    default:
      break;
  }

  // Strip "void " ctor/dtor
  if ((outInfo.kind == CXCursor_Constructor ||
       outInfo.kind == CXCursor_Destructor) &&
      hasPrefix(outInfo.display, "void ")) {
    outInfo.display.erase(0, 5); // "void "
    while (!outInfo.display.empty() &&
           std::isspace((unsigned char)outInfo.display[0])) {
      outInfo.display.erase(0, 1);
    }
  }

  // Body { ... } for funcs/methods
  {
    unsigned blFrom = 0, bcFrom = 0, blTo = 0, bcTo = 0;
    if (GetBodyRangeForCursor(target, blFrom, bcFrom, blTo, bcTo)) {
      // Korekce .ino addedLines pro hlavn soubor
      if (!mainFile.empty() &&
          fileName == mainFile &&
          addedLines > 0) {

        if (blFrom > (unsigned)addedLines &&
            blTo > (unsigned)addedLines) {
          blFrom -= (unsigned)addedLines;
          blTo -= (unsigned)addedLines;
        } else {
          // tlo le v syntetick sti (ino.hpp/include) -> ignorujeme
          blFrom = bcFrom = blTo = bcTo = 0;
        }
      }

      outInfo.bodyLineFrom = (int)blFrom;
      outInfo.bodyColFrom = (int)bcFrom;
      outInfo.bodyLineTo = (int)blTo;
      outInfo.bodyColTo = (int)bcTo;
    }
  }

  // Fix synthetic .ino.cpp
  if (!mainFile.empty() &&
      fileName == mainFile &&
      addedLines > 0) {
    if (tline <= (unsigned)addedLines) {
      // cursor in v synthetic header
      return false;
    }
    tline -= (unsigned)addedLines;
    outInfo.line = (int)tline;
  }

  // Mapping back to ino.hpp.
  if (IsIno(filename) && !mainFile.empty() && fileName == mainFile) {
    outInfo.file = AbsoluteFilename(filename);
  }

  if (hasSuffix(outInfo.file, ".ino.hpp")) {
    // /path/sketch.ino.hpp -> /path/sketch.ino
    outInfo.file.resize(outInfo.file.size() - 4);
  }

  return true;
}

/** Finds the definition of a function declaration in a sibling .cpp file.
 *  Expects a cursor that lives in a header file.
 */
bool ArduinoCodeCompletion::FindSiblingFunctionDefinition(CXCursor declCursor, JumpTarget &out) {
  CXCursorKind kind = clang_getCursorKind(declCursor);
  if (kind != CXCursor_FunctionDecl &&
      kind != CXCursor_CXXMethod &&
      kind != CXCursor_Constructor &&
      kind != CXCursor_Destructor &&
      kind != CXCursor_FunctionTemplate) {
    return false;
  }

  // File where the cursor currently is (header)
  CXSourceLocation loc = clang_getCursorLocation(declCursor);
  CXFile file;
  unsigned line = 0, col = 0, offset = 0;
  clang_getSpellingLocation(loc, &file, &line, &col, &offset);
  if (!file)
    return false;

  std::string headerPath = cxStringToStd(clang_getFileName(file));
  fs::path hp(headerPath);

  std::string ext = hp.extension().string();
  bool isHeader = (ext == ".h" || ext == ".hpp" || ext == ".hh");
  if (!isHeader) {
    APP_DEBUG_LOG("CC: FindSiblingFunctionDefinition() ... expected header!");
    return false;
  }

  std::string stem = hp.stem().string();
  APP_DEBUG_LOG("CC: FindSiblingFunctionDefinition(header=%s, stem=%s)", headerPath.c_str(), stem.c_str());

  if (stem.empty())
    return false;

  std::string funcName = cxStringToStd(clang_getCursorSpelling(declCursor));
  if (funcName.empty())
    return false;

  int wantedArgs = clang_Cursor_getNumArguments(declCursor);
  CXType declType = clang_getCursorType(declCursor);
  bool wantedVariadic = clang_isFunctionTypeVariadic(declType) != 0;

  // Candidates: header -> header.cpp / header.cc / header.cxx
  static const char *exts[] = {".cpp", ".cc", ".cxx"};

  for (const char *sext : exts) {
    fs::path cppPath = hp.parent_path() / (stem + sext);
    if (!fs::exists(cppPath))
      continue;

    const auto &clangArgs = GetCompilerArgs();

    std::vector<const char *> args;
    args.reserve(clangArgs.size() + 2);
    for (const auto &a : clangArgs) {
      args.push_back(a.c_str());
    }

    args.push_back("-x");
    args.push_back("c++-header");

    CXTranslationUnit tu = nullptr;
    CXErrorCode err = clang_parseTranslationUnit2(
        index,
        cppPath.string().c_str(),
        args.data(),
        (int)args.size(),
        nullptr,
        0,
        CXTranslationUnit_KeepGoing |
            CXTranslationUnit_IncludeBriefCommentsInCodeCompletion,
        &tu);

    if (err != CXError_Success || !tu)
      continue;

    FunctionKey key;
    key.name = funcName;
    key.numArgs = wantedArgs;
    key.isVariadic = wantedVariadic;
    // containerName nechme przdn -> stejn chovn jako dv

    bool found = FindFunctionInTU(tu, cppPath.string(), key,
                                  /*requireDefinition=*/true,
                                  out);

    clang_disposeTranslationUnit(tu);

    if (found)
      return true;
  }

  return false;
}

bool ArduinoCodeCompletion::FindSiblingFunctionDefinition(const std::string &filename,
                                                          const std::string &code,
                                                          int line,
                                                          int column,
                                                          JumpTarget &out) {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  if (!m_ready)
    return false;

  APP_DEBUG_LOG("CC: FindSiblingFunctionDefinition(fn=%s, line=%d, column=%d)", filename.c_str(), line, column);

  int addedLines = 0;
  std::string mainFile;
  CXTranslationUnit tu = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu) {
    APP_DEBUG_LOG("CC: No translation unit");
    return false;
  }

  APP_DEBUG_LOG("CC: mainFile=%s, addedLines=%d", mainFile.c_str(), addedLines);

  std::string clangFilename = GetClangFilename(filename);
  APP_DEBUG_LOG("clangFilename=%s", clangFilename.c_str());

  CXFile cxFile = clang_getFile(tu, clangFilename.c_str());
  if (!cxFile) {
    APP_DEBUG_LOG("CC: no cxFile");
    return false;
  }

  CXSourceLocation loc = clang_getLocation(tu, cxFile, line + addedLines, column);
  if (clang_equalLocations(loc, clang_getNullLocation())) {
    APP_DEBUG_LOG("CC: clang_equalLocations returns true");
    return false;
  }

  CXCursor cursor = clang_getCursor(tu, loc);
  if (clang_Cursor_isNull(cursor)) {
    APP_DEBUG_LOG("CC: no CXCursor");
    return false;
  }

  // Active file from the editor's perspective (not from the clang location)
  std::string activeAbs = AbsoluteFilename(filename);
  fs::path activePath(activeAbs);
  std::string activeExt = activePath.extension().string();

  bool activeIsHeader =
      (activeExt == ".h" || activeExt == ".hpp" || activeExt == ".hh");
  bool activeIsSource =
      (activeExt == ".cpp" || activeExt == ".cc" || activeExt == ".cxx" || activeExt == ".c");

  APP_DEBUG_LOG("CC: activeIsHeader=%d, activeIsSource=%d", activeIsHeader, activeIsSource);

  auto getLoc = [](CXCursor c, std::string &fileName, int &ln, int &col) -> bool {
    if (clang_Cursor_isNull(c))
      return false;
    CXSourceLocation l = clang_getCursorLocation(c);
    CXFile f;
    unsigned line = 0, column = 0, off = 0;
    clang_getSpellingLocation(l, &f, &line, &column, &off);
    if (!f)
      return false;
    fileName = cxStringToStd(clang_getFileName(f));
    ln = (int)line;
    col = (int)column;
    return true;
  };

  CXCursor ref = clang_getCursorReferenced(cursor);
  CXCursor def = clang_getCursorDefinition(cursor);

  // ---- CASE A: editor stays at .cpp/.cc/... -> we go to the declaration in the header ----
  if (activeIsSource) {
    APP_DEBUG_LOG("CC: activeIsSource");

    CXCursorKind ck = clang_getCursorKind(cursor);
    switch (ck) {
      case CXCursor_FunctionDecl:
      case CXCursor_CXXMethod:
      case CXCursor_Constructor:
      case CXCursor_Destructor:
      case CXCursor_FunctionTemplate:
        break;
      default:
        APP_DEBUG_LOG("CC: cursor is not a function-like symbol");
        return false;
    }

    CXCursor base = clang_Cursor_isNull(def) ? cursor : def;
    if (!clang_Cursor_isNull(ref)) {
      base = ref;
    }

    std::string funcName = cxStringToStd(clang_getCursorSpelling(base));
    if (funcName.empty()) {
      APP_DEBUG_LOG("CC: empty funcName");
      return false;
    }

    int wantedArgs = clang_Cursor_getNumArguments(base);
    CXType funcType = clang_getCursorType(base);
    bool wantedVariadic = clang_isFunctionTypeVariadic(funcType) != 0;

    // Find the container name (class/struct/namespace), if there is one
    std::string wantedContainerName;
    {
      CXCursor parent = clang_getCursorSemanticParent(base);
      while (!clang_Cursor_isNull(parent)) {
        CXCursorKind pk = clang_getCursorKind(parent);
        bool isContainer =
            pk == CXCursor_ClassDecl ||
            pk == CXCursor_StructDecl ||
            pk == CXCursor_UnionDecl ||
            pk == CXCursor_ClassTemplate ||
            pk == CXCursor_EnumDecl ||
            pk == CXCursor_Namespace;
        if (isContainer) {
          wantedContainerName = cxStringToStd(clang_getCursorSpelling(parent));
          break;
        }

        CXCursor next = clang_getCursorSemanticParent(parent);
        if (clang_equalCursors(next, parent))
          break;
        parent = next;
      }
    }

    APP_DEBUG_LOG("CC: looking for declaration of '%s' (args=%d, variadic=%d, container='%s')",
                  funcName.c_str(), wantedArgs, (int)wantedVariadic,
                  wantedContainerName.c_str());

    // 2) We find the sibling headers: new_file.h / new_file.hpp / new_file.hh
    fs::path ap(activeAbs);
    std::string stem = ap.stem().string();
    if (stem.empty()) {
      APP_DEBUG_LOG("CC: empty stem for active file");
      return false;
    }

    static const char *headerExts[] = {".h", ".hpp", ".hh"};

    FunctionKey key;
    key.name = funcName;
    key.numArgs = wantedArgs;
    key.isVariadic = wantedVariadic;
    key.containerName = wantedContainerName;

    for (const char *hext : headerExts) {
      fs::path hdrPath = ap.parent_path() / (stem + hext);
      if (!fs::exists(hdrPath)) {
        continue;
      }

      APP_DEBUG_LOG("CC: trying header %s", hdrPath.string().c_str());

      const auto &clangArgs = GetCompilerArgs();
      std::vector<const char *> args;
      args.reserve(clangArgs.size());
      for (const auto &a : clangArgs) {
        args.push_back(a.c_str());
      }

      CXTranslationUnit htu = nullptr;
      CXErrorCode err = clang_parseTranslationUnit2(
          index,
          hdrPath.string().c_str(),
          args.data(),
          (int)args.size(),
          nullptr,
          0,
          CXTranslationUnit_KeepGoing |
              CXTranslationUnit_IncludeBriefCommentsInCodeCompletion,
          &htu);

      if (err != CXError_Success || !htu) {
        APP_DEBUG_LOG("CC: failed to parse header TU (err=%d)", (int)err);
        continue;
      }

      bool found = FindFunctionInTU(htu,
                                    hdrPath.string(),
                                    key,
                                    /*requireDefinition=*/false,
                                    out);

      clang_disposeTranslationUnit(htu);

      if (found) {
        APP_DEBUG_LOG("CC: header declaration found in %s:%d",
                      out.file.c_str(), out.line);
        return true;
      }
    }

    APP_DEBUG_LOG("CC: no matching declaration in sibling headers");
    return false;
  }

  // ---- CASE B: editor stays at .h/.hpp/... -> we go to implementation in .cpp ----
  if (activeIsHeader) {
    std::string defFile;
    int dline = 0, dcol = 0;
    if (getLoc(def, defFile, dline, dcol)) {
      std::string activeNorm = NormalizePathForClangCompare(activeAbs);
      std::string defNorm = NormalizePathForClangCompare(defFile);

      if (defNorm != activeNorm) {
        out.file = defFile;
        out.line = dline;
        out.column = dcol;
        return true;
      }
    }

    // Fallback to original logic: scan sibling .cpp
    CXCursor symbol = clang_Cursor_isNull(ref) ? def : ref;
    if (clang_Cursor_isNull(symbol))
      symbol = cursor;

    if (!clang_Cursor_isNull(symbol)) {
      return FindSiblingFunctionDefinition(symbol, out);
    }

    return false;
  }

  return false;
}

bool ArduinoCodeCompletion::FindDefinition(const std::string &filename, const std::string &code, int line, int column, JumpTarget &out) {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  if (!m_ready)
    return false;

  int addedLines = 0;
  std::string mainFile;
  CXTranslationUnit tu = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu) {
    return false;
  }

  std::string clangFilename = GetClangFilename(filename);

  CXFile cxFile = clang_getFile(tu, clangFilename.c_str());
  if (!cxFile)
    return false;

  CXSourceLocation loc =
      clang_getLocation(tu, cxFile, line + addedLines, column);

  CXCursor cursor = clang_getCursor(tu, loc);
  if (clang_Cursor_isNull(cursor))
    return false;

  // 1) Let's find out where we stand.
  CXSourceLocation curLoc = clang_getCursorLocation(cursor);
  CXFile curFile;
  unsigned curLine = 0, curCol = 0, curOff = 0;
  clang_getSpellingLocation(curLoc, &curFile, &curLine, &curCol, &curOff);

  std::string curPath;
  if (curFile) {
    curPath = cxStringToStd(clang_getFileName(curFile));
  }

  fs::path curFs(curPath);
  std::string curExt = curFs.extension().string();
  bool cursorInHeader =
      (curExt == ".h" || curExt == ".hpp" || curExt == ".hh");

  // 2) Let's find out the reference and definition
  CXCursor targetRef = clang_getCursorReferenced(cursor);
  CXCursor targetDef = clang_getCursorDefinition(cursor);

  // 2a) Special case: declaration in .ino.hpp -> we want DEFINITION
  bool refInInoHpp = false;
  if (!clang_Cursor_isNull(targetRef)) {
    CXSourceLocation rloc = clang_getCursorLocation(targetRef);
    CXFile rf;
    unsigned rl = 0, rc = 0, ro = 0;
    clang_getSpellingLocation(rloc, &rf, &rl, &rc, &ro);
    if (rf) {
      std::string rfile = cxStringToStd(clang_getFileName(rf));
      if (hasSuffix(rfile, ".ino.hpp")) {
        refInInoHpp = true;
      }
    }
  }

  CXCursor target = clang_getNullCursor();

  if (cursorInHeader) {
    // 3) We are standing in the classic header -> let's try to find the implementation in the sibling .cpp
    JumpTarget implTarget;
    if (FindSiblingFunctionDefinition(
            clang_Cursor_isNull(targetRef) ? targetDef : targetRef, implTarget)) {
      out = implTarget;
      return true;
    }
  }

  // 4) Normal target selection:
  // - if declaration is in .ino.hpp -> we prefer definition
  // - otherwise we prefer "referenced" (declaration in header)
  if (refInInoHpp && !clang_Cursor_isNull(targetDef)) {
    target = targetDef;
  } else {
    target = clang_Cursor_isNull(targetRef) ? targetDef : targetRef;
  }

  if (clang_Cursor_isNull(target))
    return false;

  // 5) Convert target to file/line/column
  CXSourceLocation tloc = clang_getCursorLocation(target);

  CXFile f;
  unsigned tgtLine = 0, tgtCol = 0;
  clang_getSpellingLocation(tloc, &f, &tgtLine, &tgtCol, nullptr);

  if (!f)
    return false;

  std::string fileName = cxStringToStd(clang_getFileName(f));

  // 6) Correction for synthetic .ino.cpp:
  // if libclang points to mainFile (i.e. .ino.cpp),
  // remove addedLines and remap the file to the original .ino.
  if (!mainFile.empty() && fileName == mainFile && addedLines > 0) {
    if (tgtLine <= (unsigned)addedLines) {
      return false;
    }
    tgtLine -= (unsigned)addedLines;
  }

  // If the original filename was .ino, we want the target file as .ino, not .ino.cpp
  if (IsIno(filename) && !mainFile.empty() && fileName == mainFile) {
    fileName = AbsoluteFilename(filename);
  }

  // 7) Still protection against .ino.hpp - we don't have it in the editor anyway
  if (hasSuffix(fileName, ".ino.hpp")) {
    // /path/sketch.ino.hpp -> /path/sketch.ino
    fileName.resize(fileName.size() - 4); // strip ".hpp"
  }

  out.file = std::move(fileName);
  out.line = (int)tgtLine;
  out.column = (int)tgtCol;

  return true;
}

static void CollectSymbolOccurrencesInTU(CXTranslationUnit tu,
                                         CXCursor canonicalTarget,
                                         const std::string &targetUSR,
                                         const std::string &mainFile,
                                         int addedLines,
                                         const std::string &sketchDir,
                                         bool onlyFromSketch,
                                         std::unordered_set<LocKey, LocKeyHash> &seen,
                                         std::vector<JumpTarget> &outTargets) {
  CXCursor tuCursor = clang_getTranslationUnitCursor(tu);

  struct VisitorData {
    CXCursor canonicalTarget;
    std::string targetUSR;
    std::string mainFile;
    int addedLines;
    std::string sketchDir;
    bool onlyFromSketch;
    std::unordered_set<LocKey, LocKeyHash> *seen;
    std::vector<JumpTarget> *out;
  } data{canonicalTarget,
         targetUSR,
         mainFile,
         addedLines,
         sketchDir,
         onlyFromSketch,
         &seen,
         &outTargets};

  clang_visitChildren(
      tuCursor,
      [](CXCursor cur, CXCursor, CXClientData client_data) -> CXChildVisitResult {
        auto *d = static_cast<VisitorData *>(client_data);

        CXCursor ref = clang_getCursorReferenced(cur);
        CXCursor candidate = clang_Cursor_isNull(ref) ? cur : ref;
        candidate = clang_getCanonicalCursor(candidate);

        bool matches = false;
        if (!d->targetUSR.empty()) {
          std::string usr = cxStringToStd(clang_getCursorUSR(candidate));
          if (!usr.empty() && usr == d->targetUSR) {
            matches = true;
          }
        }
        if (!matches) {
          matches = clang_equalCursors(candidate, d->canonicalTarget) != 0;
        }
        if (!matches) {
          return CXChildVisit_Recurse;
        }

        CXSourceLocation loc = clang_getCursorLocation(cur);
        CXFile file;
        unsigned line = 0, col = 0, off = 0;
        clang_getSpellingLocation(loc, &file, &line, &col, &off);
        if (!file) {
          return CXChildVisit_Recurse;
        }

        std::string fileName = cxStringToStd(clang_getFileName(file));

        if (hasSuffix(fileName, ".ino.hpp")) {
          return CXChildVisit_Recurse;
        }

        std::string normFn = NormalizeFilename(d->sketchDir, fileName);
        if (d->onlyFromSketch && (normFn.rfind(d->sketchDir, 0) != 0)) {
          return CXChildVisit_Recurse;
        }

        if (!d->mainFile.empty() && fileName == d->mainFile && d->addedLines > 0) {
          if (line <= (unsigned)d->addedLines) {
            return CXChildVisit_Recurse;
          }
          line -= (unsigned)d->addedLines;
        }

        if (!d->mainFile.empty() && fileName == d->mainFile) {
          if (hasSuffix(fileName, ".ino.cpp")) {
            fileName.resize(fileName.size() - 4);
          }
        }

        LocKey key{fileName, line, col};
        if (!d->seen->insert(key).second) {
          return CXChildVisit_Recurse;
        }

        JumpTarget jt;
        jt.file = std::move(fileName);
        jt.line = (int)line;
        jt.column = (int)col;
        d->out->push_back(std::move(jt));

        return CXChildVisit_Recurse;
      },
      &data);
}

bool ArduinoCodeCompletion::FindSymbolOccurrences(const std::string &filename,
                                                  const std::string &code,
                                                  int line,
                                                  int column,
                                                  bool onlyFromSketch,
                                                  std::vector<JumpTarget> &outTargets) {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  if (!m_ready) {
    return false;
  }

  APP_DEBUG_LOG("CC: FindSymbolOccurrences(filename=%s, onlyFromSketch=%d)",
                filename.c_str(), onlyFromSketch);

  ScopeTimer t("CC: FindSymbolOccurrences()");
  outTargets.clear();

  int addedLines = 0;
  std::string mainFile;
  CXTranslationUnit tu = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu) {
    return false;
  }

  std::string clangFilename = GetClangFilename(filename);
  CXFile cxFile = clang_getFile(tu, clangFilename.c_str());
  if (!cxFile)
    return false;

  CXSourceLocation loc =
      clang_getLocation(tu, cxFile, line + addedLines, column);
  if (clang_equalLocations(loc, clang_getNullLocation())) {
    return false;
  }

  CXCursor cursor = clang_getCursor(tu, loc);
  if (clang_Cursor_isNull(cursor)) {
    return false;
  }

  CXCursor ref = clang_getCursorReferenced(cursor);
  CXCursor def = clang_getCursorDefinition(cursor);
  CXCursor target = clang_getNullCursor();

  if (!clang_Cursor_isNull(ref))
    target = ref;
  if (!clang_Cursor_isNull(def))
    target = def;
  if (clang_Cursor_isNull(target))
    target = cursor;
  if (clang_Cursor_isNull(target)) {
    return false;
  }

  // Canonical cursor + USR of target symbol
  CXCursor canonicalTarget = clang_getCanonicalCursor(target);
  std::string targetUSR = cxStringToStd(clang_getCursorUSR(canonicalTarget));

  std::string sketchDir;
  if (arduinoCli) {
    sketchDir = arduinoCli->GetSketchPath();
  }

  // Dedup by (file,line,col) - shared with project-wide variant
  std::unordered_set<LocKey, LocKeyHash> seen;

  CollectSymbolOccurrencesInTU(tu,
                               canonicalTarget,
                               targetUSR,
                               mainFile,
                               addedLines,
                               sketchDir,
                               onlyFromSketch,
                               seen,
                               outTargets);

  return !outTargets.empty();
}

void ArduinoCodeCompletion::FindSymbolOccurrencesAsync(const std::string &filename,
                                                       const std::string &code,
                                                       int line,
                                                       int column,
                                                       bool onlyFromSketch,
                                                       wxEvtHandler *handler,
                                                       uint64_t requestId,
                                                       wxEventType eventType) {
  if (!handler || !m_ready) {
    return;
  }

  std::vector<SketchFileBuffer> filesSnapshot;
  CollectSketchFiles(filesSnapshot);

  std::thread([this,
               handler,
               filename,
               code,
               line,
               column,
               onlyFromSketch,
               requestId,
               eventType,
               filesSnapshot = std::move(filesSnapshot)]() {
    CcFilesSnapshotGuard guard(&filesSnapshot);

    std::vector<JumpTarget> occurrences;
    bool ok = FindSymbolOccurrences(filename, code, line, column, onlyFromSketch, occurrences);

    if (!ok) {
      APP_DEBUG_LOG("CC: FindSymbolOccurrences failed!");
    }

    wxThreadEvent evt(eventType);
    evt.SetInt((int)requestId);
    evt.SetPayload(occurrences); // vector<JumpTarget>

    QueueUiEvent(handler, evt.Clone());
  }).detach();
}

bool ArduinoCodeCompletion::FindSymbolOccurrencesProjectWide(
    const std::vector<SketchFileBuffer> &files,
    const std::string &filename,
    const std::string &code,
    int line,
    int column,
    bool onlyFromSketch,
    std::vector<JumpTarget> &outTargets) {

  std::lock_guard<std::mutex> lock(m_ccMutex);

  if (!m_ready)
    return false;

  ScopeTimer t("CC: FindSymbolOccurrencesProjectWide()");

  outTargets.clear();

  // 1) From the first TU we find out the "identity" of the symbol (canonical + USR)
  int addedLines = 0;
  std::string mainFile;
  CXTranslationUnit tu0 = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu0) {
    return false;
  }

  std::string clangFilename = GetClangFilename(filename);
  CXFile cxFile = clang_getFile(tu0, clangFilename.c_str());
  if (!cxFile)
    return false;

  CXSourceLocation loc =
      clang_getLocation(tu0, cxFile, line + addedLines, column);
  if (clang_equalLocations(loc, clang_getNullLocation())) {
    return false;
  }

  CXCursor cursor = clang_getCursor(tu0, loc);
  if (clang_Cursor_isNull(cursor)) {
    return false;
  }

  CXCursor ref = clang_getCursorReferenced(cursor);
  CXCursor def = clang_getCursorDefinition(cursor);
  CXCursor target = clang_getNullCursor();

  if (!clang_Cursor_isNull(ref))
    target = ref;
  if (!clang_Cursor_isNull(def))
    target = def;
  if (clang_Cursor_isNull(target))
    target = cursor;
  if (clang_Cursor_isNull(target))
    return false;

  CXCursor canonicalTarget = clang_getCanonicalCursor(target);
  std::string targetUSR = cxStringToStd(clang_getCursorUSR(canonicalTarget));

  std::string sketchDir;
  if (arduinoCli) {
    sketchDir = arduinoCli->GetSketchPath();
  }

  std::unordered_set<LocKey, LocKeyHash> seen;

  // 2) First collect occurrences in the first TU (current editor)
  CollectSymbolOccurrencesInTU(tu0,
                               canonicalTarget,
                               targetUSR,
                               mainFile,
                               addedLines,
                               sketchDir,
                               onlyFromSketch,
                               seen,
                               outTargets);

  // 3) Now go through the other files from the vector files
  if (files.empty()) {
    return !outTargets.empty();
  }

  const std::string currentAbs = AbsoluteFilename(filename);

  for (const auto &f : files) {
    std::string abs = AbsoluteFilename(f.filename);

    if (abs == currentAbs)
      continue;

    int addedLines2 = 0;
    std::string mainFile2;
    CXTranslationUnit tu =
        GetTranslationUnit(f.filename, f.code, &addedLines2, &mainFile2);
    if (!tu) {
      continue;
    }

    CollectSymbolOccurrencesInTU(tu,
                                 canonicalTarget,
                                 targetUSR,
                                 mainFile2,
                                 addedLines2,
                                 sketchDir,
                                 onlyFromSketch,
                                 seen,
                                 outTargets);
  }

  return !outTargets.empty();
}

void ArduinoCodeCompletion::FindSymbolOccurrencesProjectWideAsync(
    const std::vector<SketchFileBuffer> &files,
    const std::string &filename,
    const std::string &code,
    int line,
    int column,
    bool onlyFromSketch,
    wxEvtHandler *handler,
    uint64_t requestId,
    wxEventType eventType) {

  if (!handler || !m_ready) {
    return;
  }

  auto filesCopy = files;

  std::thread([this,
               handler,
               filesCopy = std::move(filesCopy),
               filename,
               code,
               line,
               column,
               onlyFromSketch,
               requestId,
               eventType]() {
    CcFilesSnapshotGuard guard(&filesCopy);

    std::vector<JumpTarget> occurrences;
    bool ok = FindSymbolOccurrencesProjectWide(
        filesCopy,
        filename,
        code,
        line,
        column,
        onlyFromSketch,
        occurrences);

    if (!ok) {
      APP_DEBUG_LOG("CC: FindSymbolOccurrencesProjectWide failed!");
    }

    wxThreadEvent evt(eventType);
    evt.SetInt((int)requestId);
    evt.SetPayload(occurrences); // vector<JumpTarget>

    QueueUiEvent(handler, evt.Clone());
  }).detach();
}

bool ArduinoCodeCompletion::FindEnclosingContainerInfo(const std::string &filename,
                                                       const std::string &code,
                                                       int line,
                                                       int column,
                                                       AeContainerInfo &out) {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  ScopeTimer t("CC: FindEnclosingContainerInfo()");

  if (!m_ready)
    return false;

  out = AeContainerInfo{};

  int addedLines = 0;
  std::string mainFile;
  CXTranslationUnit tu = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu) {
    return false;
  }

  std::string clangFilename = GetClangFilename(filename);
  CXFile cxFile = clang_getFile(tu, clangFilename.c_str());
  if (!cxFile) {
    return false;
  }

  CXSourceLocation loc =
      clang_getLocation(tu, cxFile, line + addedLines, column);
  if (clang_equalLocations(loc, clang_getNullLocation())) {
    return false;
  }

  CXCursor cursor = clang_getCursor(tu, loc);
  if (clang_Cursor_isNull(cursor)) {
    return false;
  }

  // We find the "functional" cursor (function/method/ctor/dtor) in which we stand
  CXCursor fnCur = cursor;
  while (!clang_Cursor_isNull(fnCur)) {
    CXCursorKind k = clang_getCursorKind(fnCur);
    if (k == CXCursor_FunctionDecl ||
        k == CXCursor_CXXMethod ||
        k == CXCursor_Constructor ||
        k == CXCursor_Destructor ||
        k == CXCursor_FunctionTemplate) {
      break;
    }

    CXCursor parent = clang_getCursorSemanticParent(fnCur);
    if (clang_equalCursors(parent, fnCur)) {
      // we reached the top (TU) - nothing
      fnCur = clang_getNullCursor();
      break;
    }
    fnCur = parent;
  }

  if (clang_Cursor_isNull(fnCur)) {
    return false;
  }

  // From the function/method we climb to the wrapping container
  CXCursor container = clang_getCursorSemanticParent(fnCur);
  while (!clang_Cursor_isNull(container)) {
    CXCursorKind ck = clang_getCursorKind(container);

    bool isContainer =
        ck == CXCursor_ClassDecl ||
        ck == CXCursor_StructDecl ||
        ck == CXCursor_UnionDecl ||
        ck == CXCursor_ClassTemplate ||
        ck == CXCursor_EnumDecl ||
        ck == CXCursor_Namespace;

    if (isContainer) {
      out.name = cxStringToStd(clang_getCursorSpelling(container));
      out.kind = cxStringToStd(clang_getCursorKindSpelling(ck));

      {
        CXSourceLocation cloc = clang_getCursorLocation(container);
        CXFile cfile;
        unsigned cl = 0, cc = 0, co = 0;
        clang_getSpellingLocation(cloc, &cfile, &cl, &cc, &co);
        if (cfile) {
          std::string fileName = cxStringToStd(clang_getFileName(cfile));

          // Correction for synthetic .ino.cpp (same as elsewhere)
          if (!mainFile.empty() && fileName == mainFile && addedLines > 0) {
            if (cl > (unsigned)addedLines) {
              cl -= (unsigned)addedLines;
            }
          }

          // If it is .ino.cpp and the source file is .ino, we remap
          if (IsIno(filename) && !mainFile.empty() && fileName == mainFile) {
            fileName = AbsoluteFilename(filename);
          }

          // Protection against .ino.hpp - same as FindDefinition
          if (hasSuffix(fileName, ".ino.hpp")) {
            fileName.resize(fileName.size() - 4); // cut ".hpp"
          }

          out.filename = std::move(fileName);
          out.line = (int)cl;
          out.column = (int)cc;
        }
      }

      // We will load all symbols whose semantic parent is this container
      out.members.clear();
      std::string useMainFile = !mainFile.empty() ? mainFile : clangFilename;

      CXCursor parentFilter = container;
      CollectSymbolsInTUForParent(tu,
                                  useMainFile,
                                  addedLines,
                                  out.members,
                                  parentFilter);

      return true;
    }

    CXCursor parent = clang_getCursorSemanticParent(container);
    if (clang_equalCursors(parent, container)) {
      break;
    }
    container = parent;
  }

  // The function exists, but is not in any container (free function)
  return false;
}

bool ArduinoCodeCompletion::AnalyzeIncludes(const std::string &filename,
                                            const std::string &code,
                                            std::vector<IncludeUsage> &outIncludes) {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  ScopeTimer t("CC: AnalyzeIncludes()");

  APP_DEBUG_LOG("CC: AnalyzeIncludes (filename=%s)", filename.c_str());

  outIncludes.clear();

  if (!m_ready)
    return false;

  int addedLines = 0;
  std::string mainFile;

  CXTranslationUnit tu = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu) {
    return false;
  }

  std::string mainFileNorm = NormalizePathForClangCompare(
      !mainFile.empty() ? mainFile : GetClangFilename(filename));

  // --- 1) We collect all #includes from the main file using clang_getInclusions ---

  struct IncData {
    std::string mainFileNorm;
    std::vector<IncludeUsage> *includes;
    std::string generatedInoHpp; // abychom mohli peskoit syntetick .ino.hpp
  } data;

  data.mainFileNorm = mainFileNorm;
  data.includes = &outIncludes;

  std::string absIno = AbsoluteFilename(filename);
  data.generatedInoHpp.clear();
  if (IsIno(filename)) {
    std::string hpp = absIno + ".hpp";
    data.generatedInoHpp = NormalizePathForClangCompare(hpp);
  }

  clang_getInclusions(
      tu,
      [](CXFile includedFile, CXSourceLocation *inclusionStack,
         unsigned includeLen, CXClientData client_data) {
        auto *d = static_cast<IncData *>(client_data);

        if (!includedFile || includeLen == 0)
          return;

        // The file that was included
        std::string incFile = cxStringToStd(clang_getFileName(includedFile));
        if (incFile.empty())
          return;

        std::string incFileNorm = NormalizePathForClangCompare(incFile);

        // We skip the synthetic .ino.hpp
        if (!d->generatedInoHpp.empty() &&
            incFileNorm == d->generatedInoHpp) {
          return;
        }

        // first element of the stack = the place where the include is located
        CXSourceLocation loc = inclusionStack[0];
        CXFile srcFile;
        unsigned line = 0, col = 0, off = 0;
        clang_getSpellingLocation(loc, &srcFile, &line, &col, &off);
        if (!srcFile)
          return;

        std::string srcName = cxStringToStd(clang_getFileName(srcFile));
        if (srcName.empty())
          return;

        std::string srcNorm = NormalizePathForClangCompare(srcName);

        if (srcNorm != d->mainFileNorm)
          return;

        IncludeUsage u;
        u.line = line; // 1-based
        u.includedFile = incFileNorm;
        u.used = false;

        d->includes->push_back(std::move(u));
      },
      &data);

  if (outIncludes.empty()) {
    return true;
  }

  // --- 2) We specify the usage: only if clang finds a symbol,
  // its declaration comes from the PMO from the given includedFile
  // and a reference to that symbol is STORED in the main file.
  std::unordered_map<std::string, bool> fileUsed;
  fileUsed.reserve(outIncludes.size());

  for (auto &inc : outIncludes) {
    fileUsed[inc.includedFile] = false;
  }

  struct UsedData {
    std::string mainFileNorm;
    std::unordered_map<std::string, bool> *fileUsed;
  } udata;

  udata.mainFileNorm = mainFileNorm;
  udata.fileUsed = &fileUsed;

  CXCursor tuCursor = clang_getTranslationUnitCursor(tu);

  clang_visitChildren(
      tuCursor,
      [](CXCursor cur, CXCursor, CXClientData client_data) -> CXChildVisitResult {
        auto *d = static_cast<UsedData *>(client_data);

        // 1) We are only interested in cursors that are WRITTEN in the main file
        CXSourceLocation curLoc = clang_getCursorLocation(cur);
        CXFile curFile;
        unsigned curLine = 0, curCol = 0, curOff = 0;
        clang_getSpellingLocation(curLoc, &curFile, &curLine, &curCol, &curOff);
        if (!curFile)
          return CXChildVisit_Recurse;

        std::string curName = cxStringToStd(clang_getFileName(curFile));
        if (curName.empty())
          return CXChildVisit_Recurse;

        std::string curNorm = NormalizePathForClangCompare(curName);
        if (curNorm != d->mainFileNorm)
          return CXChildVisit_Recurse;

        // 2) We find the "referenced" declaration (function, type, macro...)
        CXCursor ref = clang_getCursorReferenced(cur);
        if (clang_Cursor_isNull(ref))
          return CXChildVisit_Recurse;

        CXSourceLocation loc = clang_getCursorLocation(ref);
        CXFile file;
        unsigned line = 0, col = 0, off = 0;
        clang_getSpellingLocation(loc, &file, &line, &col, &off);
        if (!file)
          return CXChildVisit_Recurse;

        std::string fn = cxStringToStd(clang_getFileName(file));
        if (fn.empty())
          return CXChildVisit_Recurse;

        std::string fnNorm = NormalizePathForClangCompare(fn);

        // 3) We are not interested in the declaration in the main file for the include
        if (fnNorm == d->mainFileNorm)
          return CXChildVisit_Recurse;

        // 4) If it is exactly one of the included headers -> we mark it as used
        auto it = d->fileUsed->find(fnNorm);
        if (it != d->fileUsed->end()) {
          it->second = true;
        }

        return CXChildVisit_Recurse;
      },
      &udata);

  // --- 3) We copy the results back into IncludeUsage
  for (auto &inc : outIncludes) {
    inc.used = fileUsed[inc.includedFile];
  }

  return true;
}

bool ArduinoCodeCompletion::AnalyzeExtractFunction(const std::string &filename,
                                                   const std::string &code,
                                                   int selStartLine, int selStartColumn,
                                                   int selEndLine, int selEndColumn,
                                                   ExtractFunctionAnalysis &out) {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  out = ExtractFunctionAnalysis{};
  out.returnType = "void";

  if (!m_ready) {
    return false;
  }

  int addedLines = 0;
  std::string mainFile;

  CXTranslationUnit tu = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu) {
    return false;
  }

  std::string clangFilename =
      !mainFile.empty() ? mainFile : GetClangFilename(filename);

  CXFile file = clang_getFile(tu, clangFilename.c_str());
  if (!file) {
    return false;
  }

  CXSourceLocation startLoc =
      clang_getLocation(tu, file, selStartLine + addedLines, selStartColumn);
  CXSourceLocation endLoc =
      clang_getLocation(tu, file, selEndLine + addedLines, selEndColumn);
  CXSourceRange selRange = clang_getRange(startLoc, endLoc);

  {
    CXCursor cur = clang_getCursor(tu, startLoc);

    CXCursor funcCur = cur;
    while (!clang_Cursor_isNull(funcCur)) {
      CXCursorKind k = clang_getCursorKind(funcCur);
      if (k == CXCursor_FunctionDecl ||
          k == CXCursor_CXXMethod ||
          k == CXCursor_Constructor ||
          k == CXCursor_Destructor ||
          k == CXCursor_FunctionTemplate) {

        CXSourceLocation floc = clang_getCursorLocation(funcCur);
        CXFile ffile;
        unsigned line = 0, col = 0, off = 0;
        clang_getSpellingLocation(floc, &ffile, &line, &col, &off);
        if (ffile) {
          CXString fn = clang_getFileName(ffile);
          std::string fpath = cxStringToStd(fn);

          out.enclosingFuncFile = fpath;

          int origLine = static_cast<int>(line) - addedLines;
          if (origLine < 1)
            origLine = 1;

          out.enclosingFuncLine = origLine;
          out.enclosingFuncColumn = static_cast<int>(col);
        }
        break;
      }

      CXCursor parent = clang_getCursorSemanticParent(funcCur);
      if (clang_equalCursors(parent, funcCur))
        break;
      funcCur = parent;
    }
  }

  ExtractVisitorData data;
  data.tu = tu;
  data.file = file;
  data.selRange = selRange;

  CXCursor root = clang_getTranslationUnitCursor(tu);
  clang_visitChildren(root, &ExtractVisitor, &data);

  std::vector<std::string> paramNames;
  paramNames.reserve(data.vars.size());

  out.parameters.clear();

  for (auto &kv : data.vars) {
    const std::string &name = kv.first;
    const auto &vi = kv.second;

    ParameterInfo param;
    param.name = name;

    if (!vi.usedInside)
      continue;

    if (vi.declInside)
      continue;

    if (!vi.declInThisFile)
      continue;

    if (vi.isGlobal)
      continue;

    if (name.empty())
      continue;

    std::string type = vi.type;

    param.type = type + " &";

    out.parameters.push_back(std::move(param));
  }

  out.success = true;
  return true;
}

std::vector<SymbolInfo> ArduinoCodeCompletion::GetAllSymbols(const std::string &filename,
                                                             const std::string &code) {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  std::vector<SymbolInfo> symbols;
  if (!m_ready)
    return symbols;

  ScopeTimer t("CC: GetAllSymbols from %s", filename.c_str());

  const std::string key = AbsoluteFilename(filename);
  const std::size_t codeHash = HashCode(code);
  const auto now = std::chrono::steady_clock::now();

  // --- Cache ---
  auto it = m_symbolCache.find(key);
  if (it != m_symbolCache.end()) {
    SymbolCacheEntry &entry = it->second;

    if (entry.codeHash == codeHash) {
      APP_DEBUG_LOG("CC: GetAllSymbols cache hit (%s, exact)", key.c_str());
      return entry.symbols;
    }

    constexpr auto MIN_REBUILD_INTERVAL = std::chrono::seconds(10);
    if (now - entry.lastUpdated < MIN_REBUILD_INTERVAL) {
      APP_DEBUG_LOG("CC: GetAllSymbols cache stale but recent (%s) - using without rebuild", key.c_str());
      return entry.symbols;
    }

    APP_DEBUG_LOG("CC: GetAllSymbols cache stale & old (%s) - rebuilding", key.c_str());
  }

  // --- Normal calculation ---
  int addedLines = 0;
  std::string mainFile;
  CXTranslationUnit tu = GetTranslationUnit(filename, code, &addedLines, &mainFile);
  if (!tu) {
    return symbols; // empty
  }

  std::string useMainFile = !mainFile.empty() ? mainFile : GetClangFilename(filename);

  symbols.clear();
  CXCursor nullParent = clang_getNullCursor();
  CollectSymbolsInTUForParent(tu,
                              useMainFile,
                              addedLines,
                              symbols,
                              nullParent);

  std::sort(symbols.begin(), symbols.end(),
            [](const SymbolInfo &a, const SymbolInfo &b) {
              if (a.name != b.name)
                return a.name < b.name;
              if (a.file != b.file)
                return a.file < b.file;
              return a.line < b.line;
            });

  // --- Cache update ---
  SymbolCacheEntry entry;
  entry.filename = key;
  entry.codeHash = codeHash;
  entry.symbols = symbols;
  entry.lastUpdated = now;

  m_symbolCache[key] = std::move(entry);

  return symbols;
}

void ArduinoCodeCompletion::InvalidateTranslationUnit() {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  for (auto &kv : m_tuCache) {
    if (kv.second.tu) {
      clang_disposeTranslationUnit(kv.second.tu);
      kv.second.tu = nullptr;
    }
  }
  m_tuCache.clear();

  for (auto &kv : m_projectTuCache) {
    if (kv.second.tu) {
      clang_disposeTranslationUnit(kv.second.tu);
      kv.second.tu = nullptr;
    }
  }
  m_projectTuCache.clear();

  m_symbolCache.clear();
  m_inoHeaderCache.clear();
  m_inoInsertCache.clear();

  m_completionSession.valid = false;
  m_completionSession.baseItems.clear();

  m_lastDiagHash = 0;
  m_lastProjectErrors.clear();

  m_resolvedIncludesCache.clear();
}

bool ArduinoCodeCompletion::InitTranslationUnitForIno() {
  ScopeTimer t("CC: InitTranslationUnitForIno()");
  APP_DEBUG_LOG("CC: InitTranslationUnitForIno()");

  std::string sketchPath = arduinoCli->GetSketchPath();
  if (sketchPath.empty()) {
    return false;
  }

  fs::path sketchDir(sketchPath);
  if (!fs::exists(sketchDir) || !fs::is_directory(sketchDir)) {
    return false;
  }

  // mainstream Arduino convention: <dir>/<dir>.ino
  std::string inoFilename;
  fs::path mainCandidate = sketchDir / (sketchDir.filename().string() + ".ino");
  if (fs::exists(mainCandidate) && fs::is_regular_file(mainCandidate)) {
    inoFilename = mainCandidate.string();
  } else {
    return false;
  }

  std::ifstream in(inoFilename);
  if (!in) {
    return false;
  }

  std::string code((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());

  std::lock_guard<std::mutex> lock(m_ccMutex);
  APP_DEBUG_LOG("CC: InitTranslationUnitForIno using '%s'", inoFilename.c_str());

  int addedLines = 0;
  std::string mainFile;
  CXTranslationUnit tu = GetTranslationUnit(inoFilename, code, &addedLines, &mainFile);
  if (!tu) {
    return false;
  }

  return true;
}

bool ArduinoCodeCompletion::IsTranslationUnitValid() {
  std::lock_guard<std::mutex> lock(m_ccMutex);
  return !m_tuCache.empty();
}

std::string ArduinoCodeCompletion::GetKindSpelling(CXCursorKind kind) {
  return cxStringToStd(clang_getCursorKindSpelling(kind));
}

void ArduinoCodeCompletion::RefreshProjectDiagnosticsAsync(const std::vector<SketchFileBuffer> &files) {
  if (!m_ready)
    return;

  APP_DEBUG_LOG("RefreshProjectDiagnosticsAsync(%d files)", files.size());

  auto filesCopy = files;

  std::thread([this, filesCopy = std::move(filesCopy)]() {
    CcFilesSnapshotGuard guard(&filesCopy);

    std::lock_guard<std::mutex> lock(m_ccMutex);

    // calculate multi-TU errors
    auto errors = ComputeProjectDiagnosticsLocked(filesCopy);

    // save to cache
    m_lastProjectErrors = errors;

    // we emit the event only when something has actually changed
    NotifyProjectDiagnosticsChangedLocked(m_lastProjectErrors);
  }).detach();
}

std::vector<ArduinoParseError> ArduinoCodeCompletion::ComputeProjectDiagnosticsLocked(const std::vector<SketchFileBuffer> &files) {
  ScopeTimer t("CC: ComputeProjectDiagnosticsLocked(%zu files)", files.size());
  std::vector<ArduinoParseError> allErrors;

  APP_DEBUG_LOG("CC: ComputeProjectDiagnosticsLocked(%zu files)", files.size());

  if (!arduinoCli || !index) {
    return allErrors;
  }

  const std::string sketchDir = arduinoCli->GetSketchPath();

  // -----------------------------
  // Base compiler args (common for all files in this snapshot)
  // -----------------------------
  const std::vector<std::string> clangArgs = GetCompilerArgs(files);

  std::vector<const char *> baseArgs;
  baseArgs.reserve(clangArgs.size() + 4);
  for (const auto &a : clangArgs) {
    baseArgs.push_back(a.c_str());
  }

  // -----------------------------
  // Unsaved headers (open headers only) + stable signature hash
  // -----------------------------
  struct HeaderItem {
    std::string abs;
    const SketchFileBuffer *f = nullptr;
  };

  std::vector<HeaderItem> headers;
  headers.reserve(files.size());

  for (const auto &hf : files) {
    if (!isHeaderFile(hf.filename))
      continue;

    HeaderItem it;
    it.abs = AbsoluteFilename(hf.filename);
    it.f = &hf;
    headers.push_back(std::move(it));
  }

  std::sort(headers.begin(), headers.end(),
            [](const HeaderItem &a, const HeaderItem &b) {
              return a.abs < b.abs;
            });

  // FNV-1a hash of (headerAbsPath + '\n' + HashCode(headerText)) for all headers, in stable order.
  std::size_t headersSigHash = 1469598103934665603ull;
  auto fnvMix = [&headersSigHash](unsigned char c) {
    headersSigHash ^= c;
    headersSigHash *= 1099511628211ull;
  };

  std::vector<std::string> headerAbsPaths;
  std::vector<CXUnsavedFile> headerUnsaved;

  headerAbsPaths.reserve(headers.size());
  headerUnsaved.reserve(headers.size());

  for (const auto &h : headers) {
    headerAbsPaths.push_back(h.abs);

    const std::size_t codeHash = HashCode(h.f->code);

    for (unsigned char c : headerAbsPaths.back())
      fnvMix(c);
    fnvMix((unsigned char)'\n');

    // mix codeHash bytes (portable enough for our local cache)
    for (std::size_t i = 0; i < sizeof(std::size_t); ++i) {
      fnvMix((unsigned char)((codeHash >> (i * 8)) & 0xFF));
    }

    CXUnsavedFile uf{};
    uf.Filename = headerAbsPaths.back().c_str();
    uf.Contents = h.f->code.c_str(); // backed by files snapshot in the worker thread
    uf.Length = h.f->code.size();
    headerUnsaved.push_back(uf);
  }

  // -----------------------------
  // Arg hash helper: base args + file-specific extras
  // Note: clang_reparseTranslationUnit() cannot change command line args,
  // so if args change we must recreate the TU.
  // -----------------------------
  auto HashArgsForTU = [&](bool isHeaderTU, bool isInoMain) -> std::size_t {
    std::size_t h = 1469598103934665603ull;
    auto mix = [&h](unsigned char c) {
      h ^= c;
      h *= 1099511628211ull;
    };

    auto mixStr = [&](const char *s) {
      if (!s)
        return;
      for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        mix(*p);
      mix(0); // delimiter
    };

    for (const auto &a : clangArgs) {
      mixStr(a.c_str());
    }

    if (isHeaderTU) {
      mixStr("-x");
      mixStr("c++-header");
    }

    if (isInoMain) {
      mixStr("-include");
      mixStr("Arduino.h");
    }

    return h;
  };

  // Keep track of files in this snapshot -> evict removed entries from cache.
  std::unordered_set<std::string> keepKeys;
  keepKeys.reserve(files.size());

  // -----------------------------
  // Build / update per-file TUs
  // -----------------------------
  for (const auto &f : files) {
    if (!isSourceFile(f.filename))
      continue;

    const std::string key = AbsoluteFilename(f.filename);
    keepKeys.insert(key);

    const std::size_t codeHash = HashCode(f.code);

    const std::string mainFilename = GetClangFilename(f.filename);

    // command line extras for header TU / ino TU
    const bool isHeaderTU = isHeaderFile(mainFilename);
    const bool isInoMain = hasSuffix(mainFilename, ".ino.cpp");

    std::vector<const char *> localArgs = baseArgs;

    if (isHeaderTU) {
      localArgs.push_back("-x");
      localArgs.push_back("c++-header");
    }

    if (isInoMain) {
      localArgs.push_back("-include");
      localArgs.push_back("Arduino.h");
    }

    m_clangSettings.AppendWarningFlags(localArgs);

    const std::size_t argsHash = HashArgsForTU(isHeaderTU, isInoMain);

    // Find or create cache entry
    auto it = m_projectTuCache.find(key);
    if (it == m_projectTuCache.end()) {
      ProjectTuEntry e;
      e.key = key;
      e.mainFilename = mainFilename;
      e.codeHash = 0;
      e.headersSigHash = 0;
      e.argsHash = 0;
      e.tu = nullptr;
      it = m_projectTuCache.emplace(key, std::move(e)).first;
    }

    ProjectTuEntry &entry = it->second;

    // Decide what to do:
    // - recreate TU if it doesn't exist or args/main file changed (reparse can't change args)
    // - otherwise, reparse if code/header signature changed
    const bool needRecreate =
        (!entry.tu) ||
        (entry.argsHash != argsHash) ||
        (entry.mainFilename != mainFilename);

    const bool needReparse =
        (!needRecreate) &&
        ((entry.codeHash != codeHash) ||
         (entry.headersSigHash != headersSigHash));

    if (needRecreate) {
      ClangUnsavedFiles uf = CreateClangUnsavedFiles(f.filename, f.code);

      if (entry.tu) {
        clang_disposeTranslationUnit(entry.tu);
        entry.tu = nullptr;
      }

      // unsaved = main file (+ .ino.hpp) + all open headers
      std::vector<CXUnsavedFile> unsaved;
      unsaved.reserve(uf.count + headerUnsaved.size());
      for (unsigned i = 0; i < uf.count; ++i) {
        unsaved.push_back(uf.files[i]);
      }
      for (const auto &hu : headerUnsaved) {
        unsaved.push_back(hu);
      }

      APP_DEBUG_LOG("CC: [PROJ TU NEW] %s (%d args, %d unsaved, hdrSig=%zu)",
                    uf.mainFilename.c_str(),
                    (int)localArgs.size(),
                    (int)unsaved.size(),
                    headersSigHash);

      CXTranslationUnit tu = nullptr;
      CXErrorCode err = clang_parseTranslationUnit2(
          index,
          uf.mainFilename.c_str(),
          localArgs.empty() ? nullptr : localArgs.data(),
          (int)localArgs.size(),
          unsaved.empty() ? nullptr : unsaved.data(),
          (int)unsaved.size(),
          CXTranslationUnit_KeepGoing | CXTranslationUnit_PrecompiledPreamble | CXTranslationUnit_CreatePreambleOnFirstParse,
          &tu);

      if ((err != CXError_Success || !tu) && tu) {
        // Some libclang builds may return a TU even when err != Success.
        APP_DEBUG_LOG("CC: project parse returned TU but err=%d (%s) -> continuing",
                      (int)err, ClangErrorToString(err));
        err = CXError_Success;
      }

      if (err != CXError_Success || !tu) {
        ArduinoParseError e;
        e.file = key;
        e.line = 0;
        e.column = 0;
        e.message =
            "Failed to parse translation unit (libclang error " +
            std::to_string(static_cast<int>(err)) + " / " + ClangErrorToString(err) + ")";
        e.severity = CXDiagnostic_Error;
        allErrors.push_back(std::move(e));

        APP_DEBUG_LOG("CC: [PROJ TU FAIL] %s: err=%d (%s) tu=%p",
                      uf.mainFilename.c_str(), (int)err, ClangErrorToString(err), (void *)tu);

        // keep entry fields updated for future attempts, but leave tu=null
        entry.mainFilename = uf.mainFilename;
        entry.codeHash = codeHash;
        entry.headersSigHash = headersSigHash;
        entry.argsHash = argsHash;
        entry.cachedErrors.clear();
        continue;
      }

      entry.tu = tu;
      entry.mainFilename = uf.mainFilename;
      entry.codeHash = codeHash;
      entry.headersSigHash = headersSigHash;
      entry.argsHash = argsHash;

      // refresh cached diagnostics
      entry.cachedErrors = CollectDiagnosticsLocked(entry.tu);
    } else if (needReparse) {
      ClangUnsavedFiles uf = CreateClangUnsavedFiles(f.filename, f.code);

      // unsaved = main file (+ .ino.hpp) + all open headers
      std::vector<CXUnsavedFile> unsaved;
      unsaved.reserve(uf.count + headerUnsaved.size());
      for (unsigned i = 0; i < uf.count; ++i) {
        unsaved.push_back(uf.files[i]);
      }
      for (const auto &hu : headerUnsaved) {
        unsaved.push_back(hu);
      }

      APP_DEBUG_LOG("CC: [PROJ TU REPARSE] %s (code/header changed, %d unsaved)",
                    entry.mainFilename.c_str(),
                    (int)unsaved.size());

      clang_reparseTranslationUnit(
          entry.tu,
          (unsigned)unsaved.size(),
          unsaved.empty() ? nullptr : unsaved.data(),
          clang_defaultReparseOptions(entry.tu));

      entry.codeHash = codeHash;
      entry.headersSigHash = headersSigHash;

      // refresh cached diagnostics
      entry.cachedErrors = CollectDiagnosticsLocked(entry.tu);
    } else {
      APP_DEBUG_LOG("CC: [PROJ TU CACHE-HIT] %s", entry.mainFilename.c_str());
    }

    // Append cached errors (already filtered to sketch dir by CollectDiagnosticsLocked)
    for (const auto &e : entry.cachedErrors) {
      allErrors.push_back(e);
    }
  }

  // -----------------------------
  // Evict cached TUs that no longer exist in this snapshot
  // -----------------------------
  for (auto it = m_projectTuCache.begin(); it != m_projectTuCache.end();) {
    if (keepKeys.find(it->first) == keepKeys.end()) {
      if (it->second.tu) {
        clang_disposeTranslationUnit(it->second.tu);
        it->second.tu = nullptr;
      }
      it = m_projectTuCache.erase(it);
    } else {
      ++it;
    }
  }

  // -----------------------------
  // Final sort + dedup (keeps hash stable and avoids header duplicates)
  // -----------------------------
  auto severityRank = [](CXDiagnosticSeverity s) {
    switch (s) {
      case CXDiagnostic_Error:
      case CXDiagnostic_Fatal:
        return 0;
      case CXDiagnostic_Warning:
        return 1;
      default:
        return 2;
    }
  };

  std::sort(allErrors.begin(), allErrors.end(),
            [&](const ArduinoParseError &a, const ArduinoParseError &b) {
              int ra = severityRank(a.severity);
              int rb = severityRank(b.severity);
              if (ra != rb)
                return ra < rb;

              if (a.file != b.file)
                return a.file < b.file;
              if (a.line != b.line)
                return a.line < b.line;
              if (a.column != b.column)
                return a.column < b.column;
              if (a.message != b.message)
                return a.message < b.message;
              return (int)a.severity < (int)b.severity;
            });

  allErrors.erase(std::unique(allErrors.begin(), allErrors.end(),
                              [](const ArduinoParseError &a, const ArduinoParseError &b) {
                                return a.severity == b.severity &&
                                       a.file == b.file &&
                                       a.line == b.line &&
                                       a.column == b.column &&
                                       a.message == b.message;
                              }),
                  allErrors.end());

  return allErrors;
}

std::vector<std::string> ArduinoCodeCompletion::ResolveLibrariesIncludes(const std::vector<SketchFileBuffer> &files) const {
  std::vector<std::string> result;

  if (!arduinoCli) {
    return result;
  }

  std::string sketchDirStr = arduinoCli->GetSketchPath();
  if (sketchDirStr.empty()) {
    return result;
  }

  uint64_t sum = CcSumIncludes(files);

  {
    std::lock_guard<std::mutex> lk(m_resolvedIncludesCacheMutex);
    auto it = m_resolvedIncludesCache.find(sum);
    if (it != m_resolvedIncludesCache.end()) {
      return it->second;
    }
  }

  // a unique list of header names to be resolved through libraries
  std::unordered_set<std::string> headerNames = SearchCodeIncludes(files, sketchDirStr);

  if (headerNames.empty()) {
    return result;
  }

  std::vector<std::string> includes;
  includes.reserve(headerNames.size());
  for (const auto &h : headerNames) {
    includes.push_back(h);
  }

  result = arduinoCli->ResolveLibraries(includes);

  {
    std::lock_guard<std::mutex> lk(m_resolvedIncludesCacheMutex);
    m_resolvedIncludesCache[sum] = result;
    return m_resolvedIncludesCache[sum];
  }
}

std::vector<ArduinoParseError> ArduinoCodeCompletion::GetLastProjectErrors() const {
  std::lock_guard<std::mutex> lock(m_ccMutex);
  return m_lastProjectErrors;
}

void ArduinoCodeCompletion::NotifyProjectDiagnosticsChangedLocked(const std::vector<ArduinoParseError> &errs) {
  // It only sends an event if something changes, so we need to terminate the process from here.
  ArduinoEditorFrame *frame = wxDynamicCast(m_eventHandler, ArduinoEditorFrame);
  if (frame) {
    wxThreadEvent ev(EVT_STOP_PROCESS);
    ev.SetInt(ID_PROCESS_DIAG_EVAL);
    QueueUiEvent(frame, ev.Clone());
  }

  std::size_t newHash = ComputeDiagHash(errs);
  if (newHash == m_lastDiagHash) {
    APP_DEBUG_LOG("No diagnostics errors changed. Skipping event.");
    return;
  }

  m_lastDiagHash = newHash;

  wxThreadEvent evt(EVT_DIAGNOSTICS_UPDATED);
  QueueUiEvent(m_eventHandler, evt.Clone());
}

void ArduinoCodeCompletion::QueueUiEvent(wxEvtHandler *handler, wxEvent *event) {
  if (m_cancelAsync.load(std::memory_order_relaxed)) {
    return;
  }

  wxQueueEvent(handler, event);
}

void ArduinoCodeCompletion::CancelAsyncOperations() {
  m_cancelAsync.store(true, std::memory_order_relaxed);
}

bool ArduinoCodeCompletion::IsClangTargetSupported(const std::string &target) {
  if (target.empty())
    return false;

  static std::unordered_map<std::string, bool> cache;
  auto it = cache.find(target);
  if (it != cache.end())
    return it->second;

  ScopeTimer t("CC: IsClangTargetSupported()");

  static const char kCode[] = "int main(int, char**) { return 0; }\n";

  CXUnsavedFile uf{};
  uf.Filename = "arduino_editor_target_probe.cpp";
  uf.Contents = kCode;
  uf.Length = (unsigned long)strlen(kCode);

  std::vector<const char *> args;
  args.push_back("-x");
  args.push_back("c++");
  args.push_back("-std=gnu++17");
  args.push_back("-target");
  args.push_back(target.c_str());

  CXIndex index = clang_createIndex(/*excludeDeclsFromPCH*/ 0, /*displayDiagnostics*/ 0);

  CXTranslationUnit tu = nullptr;
  CXErrorCode err = clang_parseTranslationUnit2(
      index,
      uf.Filename,
      args.data(),
      (int)args.size(),
      &uf,
      1,
      CXTranslationUnit_None,
      &tu);

  bool ok = false;

  if (err == CXError_Success && tu) {
    ok = !HasUnknownTargetDiag(tu);
  } else {
    ok = false;
  }

  if (tu) {
    clang_disposeTranslationUnit(tu);
  }

  clang_disposeIndex(index);

  cache[target] = ok;

  APP_DEBUG_LOG("CC: IsClangTargetSupported (%s) -> %d", target.c_str(), ok);

  return ok;
}

void ArduinoCodeCompletion::ApplySettings(const ClangSettings &settings) {
  m_clangSettings = settings;
}

ArduinoCodeCompletion::ArduinoCodeCompletion(ArduinoCli *ardCli, const ClangSettings &clangSettings, CollectSketchFilesFn collectSketchFilesFn, wxEvtHandler *eventHandler)
    : arduinoCli(ardCli), m_clangSettings(clangSettings), m_collectSketchFilesFn(std::move(collectSketchFilesFn)), m_eventHandler(eventHandler) {
  index = clang_createIndex(0, 0);

  CXString v = clang_getClangVersion();
  APP_DEBUG_LOG("CC: libclang version: %s", clang_getCString(v));
  clang_disposeString(v);
}

ArduinoCodeCompletion::~ArduinoCodeCompletion() {
  std::lock_guard<std::mutex> lock(m_ccMutex);

  for (auto &kv : m_tuCache) {
    if (kv.second.tu) {
      clang_disposeTranslationUnit(kv.second.tu);
      kv.second.tu = nullptr;
    }
  }
  m_tuCache.clear();

  for (auto &kv : m_projectTuCache) {
    if (kv.second.tu) {
      clang_disposeTranslationUnit(kv.second.tu);
      kv.second.tu = nullptr;
    }
  }
  m_projectTuCache.clear();

  clang_disposeIndex(index);
}
