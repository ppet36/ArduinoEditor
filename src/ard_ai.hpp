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

#include "ai_client.hpp"
#include "ard_ai_seenint.hpp"
#include "ard_cc.hpp"
#include <cstdint>
#include <unordered_map>
#include <wx/event.h>

class ArduinoEditor;
class AiExplainPopup;
struct ArduinoParseError;
class wxStyledTextCtrl;
struct AiSettings;
struct SketchFileBuffer;

struct AiPatchHunk {
  wxString file;    // relative/absolute path within the sketch (.ino, .cpp, .hpp...)
  int fromLine = 0; // 1-based inclusive
  int toLine = 0;   // 1-based inclusive
  wxString replacement;
};

struct AiChatSessionInfo {
  std::string id; // filename without extension
  std::string createdUtc;
  int messageCount = 0; // user+assistant events
  std::string title;

  wxString GetCreatedDateByLocale() const;
  wxString GetTitle() const;
};

struct AiChatUiItem {
  std::string role; // "user" | "assistant" | "info" | "error"
  wxString text;

  std::string model;
  std::string tsUtc; // ISO8601 UTC from jsonl (e.q. "2025-12-15T19:12:34Z")
  int inputTokens = -1;
  int outputTokens = -1;
  int totalTokens = -1;

  wxString GetCreatedDateByLocale() const;
  wxString GetTokenInfo() const;
};

struct AiTokenTotals {
  long long input = 0;
  long long output = 0;
  long long total = 0;
  int calls = 0;

  void Reset() {
    input = 0;
    output = 0;
    total = 0;
    calls = 0;
  }

  void Add(int inTok, int outTok, int totTok) {
    if (inTok < 0 || outTok < 0 || totTok < 0)
      return;
    input += (long long)inTok;
    output += (long long)outTok;
    total += (long long)totTok;
    calls++;
  }

  bool HasAny() const { return calls > 0; }
};

class ArduinoAiActions : public wxEvtHandler {
public:
  enum class Action {
    None = 0,
    ExplainSelection,
    GenerateDocComment,
    SolveProjectError,
    InteractiveChat,
    GenerateDocCommentsInFile,
    GenerateDocCommentsInClass,
    OptimizeFunctionOrMethod
  };

  explicit ArduinoAiActions(ArduinoEditor *editor);
  ~ArduinoAiActions();

  void SetCurrentEditor(ArduinoEditor *editor);

  void ExplainSelection();
  void GenerateDocCommentForSymbol();
  void SolveProjectError(const ArduinoParseError &error);

  // interactive chat
  bool StartInteractiveChat(const wxString &userText, wxEvtHandler *origin);
  void ResetInteractiveChat();
  std::vector<AiChatSessionInfo> ListStoredChatSessions() const;
  bool LoadChatSessionUi(const std::string &sessionId, std::vector<AiChatUiItem> &outItems);
  bool LoadChatSession(const std::string &sessionId, wxString &outTranscript) const;
  void RestoreInteractiveChatFromTranscript(const wxString &transcript, const std::string &sessionIdToContinue);

  // When true, full INFO_RESPONSE content is injected directly into the model context.
  // This disables obfuscation of info requests and allows the model to build
  // a richer internal understanding for refactoring.
  void SetFullInfoRequest(bool enable) { m_fullInfoRequest = enable; }
  bool GetFullInfoRequest() const { return m_fullInfoRequest; }
  // When enabled, the interactive chat context is kept as a "floating window":
  // after each successfully applied PATCH we keep only the last user message
  // (and everything that followed it), older history is discarded from
  // m_chatTranscript. Persistent JSONL history remains untouched.
  void SetFloatingWindow(bool enable) { m_floatingWindow = enable; }
  bool GetFloatingWindow() const { return m_floatingWindow; }

  void GenerateDocCommentsForCurrentFile();
  void GenerateDocCommentsForCurrentClass();
  void OptimizeFunctionOrMethod();

  AiSettings GetSettings() const;
  AiClient *GetClient();

private:
  AiClient *m_client = nullptr;

  // *** AI structures ***
  struct SolveSession {
    Action action = Action::None;
    ArduinoParseError compilerError; // compile error, if any
    wxString basename;               // base filename
    wxString transcript;             // text, userPrompt (growing)
    int iteration = 0;
    int maxIterations = 5;
    bool finished = false;
    // For OptimizeFunctionOrMethod
    int bodyFromLine = 0; // 1-based
    int bodyToLine = 0;   // 1-based
    AiSeenIntervals seen;
    AiTokenTotals tokenTotals;
    std::vector<SketchFileBuffer> workingFiles;
    wxString assistantPatchExplanation;
  };

  struct AiInfoRequest {
    int id = 0;
    wxString type; // includes | symbol_declaration | search
    wxString file;
    wxString symbol;
    wxString query;
    wxString kind;
    int fromLine = 0;
    int toLine = 0;
    wxString rawBlock;
  };

  struct PendingDocComment {
    SymbolInfo symbol; // old symbol from GetAllSymbols / GetSymbolInfo
    wxString reply;    // plain AI response
  };

  enum class AssistantNoteKind {
    PatchApplied,
    PatchRejected,
    InfoResponsesProvided,
    RetryRequested
  };

  // shorthands
  void RebuildProject();
  wxStyledTextCtrl *GetStc();
  std::string GetSketchRoot() const;
  bool CheckNumberOfIterations();

  void SendDoneEventToOrigin();

  std::string GetCurrentCode();
  SketchFileBuffer *FindBufferWithFile(const std::string &filename, bool allowCreate = false);

  // Interactive chat machinery
  bool m_chatActive = false;
  wxString m_chatTranscript; // long time chat history, this model sees this all the time.
  // If true, the requested file contents remain in the transcript.
  bool m_fullInfoRequest = false;
  // If true, interactive chat transcript is trimmed aggressively after each
  // successfully applied PATCH so that only the last "user -> patch" window
  // stays in m_chatTranscript.
  bool m_floatingWindow = false;

  // chat fs persistence
  std::string m_chatSessionId;
  bool m_chatSessionOpen = false;
  void AppendAssistantNote(AssistantNoteKind kind, const wxString &detail = wxEmptyString,
                           const std::vector<AiPatchHunk> *patches = nullptr,
                           const std::vector<AiInfoRequest> *reqs = nullptr);
  void AppendAssistantPlaintextToTranscript(const wxString &text);
  // Helper used both for live transcript and when loading session from disk.
  void TrimTranscriptToLastPatchWindow(wxString &transcript) const;

  // batch documentation generation
  std::vector<SymbolInfo> m_docBatchSymbols;
  size_t m_docBatchIndex = 0;
  bool StartDocCommentForSymbol(const SymbolInfo &info);
  std::vector<PendingDocComment> m_pendingDocComments;
  void FinalizeBatchDocComments(Action action);

  // action management
  Action m_currentAction = Action::None;
  bool StartAction(Action action);
  void StopCurrentAction();

  ArduinoEditor *m_editor;
  wxEvtHandler *m_origin;
  wxString m_interactiveChatPayload;

  void OnAiSimpleChatSuccess(wxThreadEvent &event);
  void OnAiSimpleChatError(wxThreadEvent &event);
  void OnDiagnosticsUpdated(wxThreadEvent &evt);

  void InsertAiGeneratedDocComment(const wxString &reply);
  bool ApplyAiModelSolution(const wxString &reply);

  bool ParseAiInfoRequests(const wxString &raw, std::vector<AiInfoRequest> &out, wxString *payload = nullptr);
  bool ParseAiPatch(const wxString &rawPatch, std::vector<AiPatchHunk> &out, wxString *payload = nullptr);
  bool ApplyAiPatchToFiles(const std::vector<AiPatchHunk> &patches);

  wxString FormatSymbolInfoForAi(const SymbolInfo &s);

  wxString HandleInfoRequest(const AiInfoRequest &req);

  int m_docCommentTargetLine = -1;
  SolveSession m_solveSession;

  bool EnsureChatSessionStarted(const wxString &initialPromptForModel, const wxString &sessionTitle);
  void AppendChatEvent(const char *type, const wxString &text, int inputTokens = -1, int outputTokens = -1, int totalTokens = -1);
  bool CheckModelQueriedFile(const std::vector<AiPatchHunk> &patches);

  wxString GenerateSessionTitleIfRequested(const wxString &userText);
  void OnAiSummarizationUpdated(wxThreadEvent &event);

  // Prompt building (centralized)
  wxString GetPromptCurrentFile() const;
  wxString BuildStablePromptHeader() const;
  wxString BuildPromptForModel(const wxString &baseTranscript,
                               const wxString &extraEphemeral,
                               const wxString &outOfBandBlocks) const;
  wxString BuildAppliedPatchEvidence(const std::vector<AiPatchHunk> &patches, int extraContextLines, int maxTotalLines);
};
