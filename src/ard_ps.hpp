#pragma once

#include <wx/string.h>

#include <string>
#include <unordered_map>
#include <vector>

struct AiPatchHunk;
struct SketchFileBuffer;

// AI Patch normalizer.
class ArduinoPatchStraightener {
public:
  struct Options {
    // Matching / anchoring
    int windowAroundOriginal; // candidate filtering window around original from/to, if provided
    int minStrongLineLen;     // heuristic for "strong" lines
    // Behavior
    bool dropNoopHunks; // if after straightening patch does nothing, remove it

    Options();
  };

  ArduinoPatchStraightener(const std::string &sketchPath, const std::vector<AiPatchHunk> &hunks,
                           const std::vector<SketchFileBuffer> &buffers,
                           Options opt);

  ArduinoPatchStraightener(const std::string &sketchPath, const std::vector<AiPatchHunk> &hunks,
                           const std::vector<SketchFileBuffer> &buffers);

  void Calculate();
  const std::vector<AiPatchHunk> &GetResult() const { return m_result; }

  // Optional: simple counters for debugging
  struct Stats {
    int hunksIn = 0;
    int hunksOut = 0;
    int hunksRemapped = 0;
    int hunksTrimmed = 0;
    int hunksDroppedNoop = 0;
    int hunksSkippedNoFile = 0;
    int hunksSkippedLowConfidence = 0;
  };
  const Stats &GetStats() const { return m_stats; }

private:
  struct FileLines {
    wxString text;
    std::vector<wxString> rawLines;                              // without trailing '\n'
    std::vector<std::string> normLines;                          // normalized for matching (UTF-8)
    std::unordered_map<std::string, std::vector<int>> positions; // normLine -> list of 0-based line indices
  };

  struct AlignResult {
    bool ok = false;
    int score = 0;

    // if replace:
    int from1 = 0; // 1-based
    int to1 = 0;   // 1-based inclusive

    // if pure insert:
    bool isInsert = false;
    int insertAt1 = 0; // 1-based
  };

  static void RebuildFileLines(FileLines &fl, const wxString &code);
  static void ApplyHunkToWorkingFile(FileLines &fl, const AiPatchHunk &h);
  static std::string NormalizeLineKeyLoose(const wxString &line);
  static int MatchScoreLine(const std::string &pStrict, const std::string &pLoose, const std::string &sStrict, const std::string &sLoose, int minStrongLen);
  static AlignResult FindBestReplaceIntervalByAlignment(const ArduinoPatchStraightener::FileLines &file,
                                                        const std::vector<wxString> &patchLines,
                                                        int hintFrom1,
                                                        int hintTo1,
                                                        const ArduinoPatchStraightener::Options &opt);
  std::string NormalizePathKey(const std::string &s);

  static wxString NormalizeNewlinesToLF(const wxString &s);
  static void SplitLinesLF(const wxString &text, std::vector<wxString> &outLines);

  static std::string NormalizeLineKey(const wxString &line);
  static bool IsWeakNormalizedLine(const std::string &norm);
  static bool IsStrongNormalizedLine(const std::string &norm, int minLen);
  static int LineWeight(const std::string &norm, int minLen);

  bool TryStraightenOne(AiPatchHunk &h, FileLines &file, const std::string &patchFile);

  static wxString JoinLinesLF(const std::vector<wxString> &lines);

  int BraceDeltaLines(const std::vector<wxString> &lines);

private:
  Options m_opt;
  std::vector<AiPatchHunk> m_hunks;
  std::vector<AiPatchHunk> m_result;
  std::string m_sketchPath;

  Stats m_stats;

  std::unordered_map<std::string, FileLines> m_files; // key = normalized path
};
