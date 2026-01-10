#include "ard_plotpars.hpp"
#include "ard_plotview.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

ArduinoPlotParser::ArduinoPlotParser(ArduinoPlotView *view)
    : m_view(view) {
  Reset();
}

void ArduinoPlotParser::Reset() {
  m_mode = Mode::Unknown;
  m_probe.clear();
  m_columnNames.clear();
  m_pendingHeaderNames.clear();
  m_lastColumnCount = 0;
  m_columnsDelimHint = '\0';
}

bool ArduinoPlotParser::HasAnyDigit(const std::string &s) {
  for (unsigned char c : s)
    if (std::isdigit(c))
      return true;
  return false;
}

bool ArduinoPlotParser::HasLetter(const std::string &s) {
  for (unsigned char c : s)
    if (std::isalpha(c))
      return true;
  return false;
}

bool ArduinoPlotParser::LooksLikeIdentifier(const std::string &tok) {
  if (tok.empty())
    return false;
  unsigned char c0 = (unsigned char)tok[0];
  if (!(std::isalpha(c0) || c0 == '_'))
    return false;

  for (unsigned char c : tok) {
    if (std::isalnum(c) || c == '_' || c == '.' || c == '/' || c == '-')
      continue;
    return false;
  }
  return true;
}

std::vector<std::string> ArduinoPlotParser::SplitTokensByDelims(const std::string &line, const char *delims) {
  std::vector<std::string> out;
  std::string cur;
  auto isDelim = [&](char c) -> bool {
    for (const char *d = delims; *d; ++d)
      if (c == *d)
        return true;
    return false;
  };

  for (char c : line) {
    if (isDelim(c)) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

// Loose tokenization for mixed logs: split on whitespace and common separators, but keep "a=1.2" intact.
std::vector<std::string> ArduinoPlotParser::SplitTokensLoose(const std::string &line) {
  // Split on whitespace, comma, semicolon, tabs.
  auto toks = SplitTokensByDelims(line, " \t\r\n,;");
  // Trim each token (paranoia)
  for (auto &t : toks)
    t = Trim(t);
  toks.erase(std::remove_if(toks.begin(), toks.end(),
                            [](const std::string &s) { return s.empty(); }),
             toks.end());
  return toks;
}

// Parses a numeric prefix of token, ignoring trailing units.
// Supports: [+/-]?\d+(\.\d+)?([eE][+/-]?\d+)? and also comma-decimals if enabled.
bool ArduinoPlotParser::ParseNumberPrefix(const std::string &tok, bool acceptCommaDecimal, double &out) {
  if (tok.empty())
    return false;

  // Scan a valid numeric prefix.
  size_t i = 0;
  auto peek = [&](size_t k) -> char { return (k < tok.size()) ? tok[k] : '\0'; };

  if (peek(i) == '+' || peek(i) == '-')
    i++;

  bool anyDigit = false;
  while (std::isdigit((unsigned char)peek(i))) {
    anyDigit = true;
    i++;
  }

  if (peek(i) == '.' || (acceptCommaDecimal && peek(i) == ',')) {
    i++;
    while (std::isdigit((unsigned char)peek(i))) {
      anyDigit = true;
      i++;
    }
  }

  // Exponent
  if (anyDigit && (peek(i) == 'e' || peek(i) == 'E')) {
    size_t j = i + 1;
    if (peek(j) == '+' || peek(j) == '-')
      j++;
    bool expDigit = false;
    while (std::isdigit((unsigned char)peek(j))) {
      expDigit = true;
      j++;
    }
    if (expDigit)
      i = j; // accept exponent only if it has digits
  }

  if (!anyDigit)
    return false;

  std::string num = tok.substr(0, i);
  if (acceptCommaDecimal) {
    // Convert comma decimal to dot for strtod
    std::replace(num.begin(), num.end(), ',', '.');
  }

  char *endp = nullptr;
  const double v = std::strtod(num.c_str(), &endp);
  if (endp == num.c_str())
    return false;

  if (!std::isfinite(v))
    return false; // in 1st phase ignore NaN/Inf
  out = v;
  return true;
}

bool ArduinoPlotParser::HeuristicAcceptCommaDecimal(const std::string &line) {
  // Safe-ish heuristic:
  // - If semicolon present AND there is digit ',' digit somewhere => comma is likely decimal.
  // - If commas are used as separators, we do NOT treat them as decimal here.
  if (line.find(';') == std::string::npos)
    return false;

  for (size_t i = 1; i + 1 < line.size(); i++) {
    if (line[i] == ',' &&
        std::isdigit((unsigned char)line[i - 1]) &&
        std::isdigit((unsigned char)line[i + 1])) {
      return true;
    }
  }
  return false;
}

// Returns number of parsed pairs from explicit "name=value" / "name:value" / "name = value" etc.
size_t ArduinoPlotParser::ParseTaggedPairs(const std::string &line,
                                           bool acceptCommaDecimal,
                                           std::vector<std::pair<std::string, double>> &out) {
  out.clear();

  // Tokenize loosely.
  std::vector<std::string> toks = SplitTokensLoose(line);
  if (toks.empty())
    return 0;

  // 1) inline operators inside token: name=value or name:value
  for (const std::string &t : toks) {
    size_t p = t.find('=');
    if (p == std::string::npos)
      p = t.find(':');
    if (p != std::string::npos && p > 0 && p + 1 < t.size()) {
      std::string name = Trim(t.substr(0, p));
      std::string valT = Trim(t.substr(p + 1));
      if (!LooksLikeIdentifier(name))
        continue;
      double v = 0.0;
      if (ParseNumberPrefix(valT, acceptCommaDecimal, v)) {
        out.emplace_back(name, v);
      }
    }
  }
  if (!out.empty())
    return out.size();

  // 2) 3-token patterns: name = value / name : value
  for (size_t i = 0; i + 2 < toks.size(); i++) {
    const std::string &a = toks[i];
    const std::string &op = toks[i + 1];
    const std::string &b = toks[i + 2];
    if (!LooksLikeIdentifier(a))
      continue;
    if (!(op == "=" || op == ":"))
      continue;
    double v = 0.0;
    if (ParseNumberPrefix(b, acceptCommaDecimal, v)) {
      out.emplace_back(a, v);
      i += 2;
    }
  }

  return out.size();
}

// Implicit pairs: name value name value (alternating), common in serial prints.
size_t ArduinoPlotParser::ParseImplicitPairs(const std::vector<std::string> &toks,
                                             bool acceptCommaDecimal,
                                             std::vector<std::pair<std::string, double>> &out) {
  out.clear();
  if (toks.size() < 2)
    return 0;
  if (toks.size() % 2 != 0)
    return 0;

  // Must be mostly alternating identifier + number
  for (size_t i = 0; i < toks.size(); i += 2) {
    if (!LooksLikeIdentifier(toks[i]))
      return 0;
    double v = 0.0;
    if (!ParseNumberPrefix(toks[i + 1], acceptCommaDecimal, v))
      return 0;
    out.emplace_back(toks[i], v);
  }
  return out.size();
}

// Columns: parse a list of numbers separated by common delimiters.
// Returns count of numbers. Also returns a delimiter hint (',' ';' or ' ').
size_t ArduinoPlotParser::ParseColumns(const std::string &line,
                                       bool acceptCommaDecimal,
                                       std::vector<double> &outValues,
                                       char &outDelimiterHint) {
  outValues.clear();
  outDelimiterHint = '\0';

  // Determine delimiter preference
  bool hasComma = (line.find(',') != std::string::npos);
  bool hasSemi = (line.find(';') != std::string::npos);

  const char *delims = nullptr;
  if (hasSemi) {
    delims = ";\t\r\n ";
    outDelimiterHint = ';';
  } else if (hasComma) {
    delims = ",\t\r\n ";
    outDelimiterHint = ',';
  } else {
    delims = " \t\r\n";
    outDelimiterHint = ' ';
  }

  auto toks = SplitTokensByDelims(line, delims);
  size_t parsed = 0;

  for (auto &t : toks) {
    t = Trim(t);
    if (t.empty())
      continue;
    double v = 0.0;
    if (ParseNumberPrefix(t, acceptCommaDecimal, v)) {
      outValues.push_back(v);
      parsed++;
    } else {
      // In columns mode we tolerate occasional junk, but not too much.
      // We'll decide later based on ratio.
    }
  }

  // Basic sanity: if nothing parsed, it's not columns.
  if (parsed == 0)
    return 0;

  // Ratio check: if line contains lots of letters, likely a log, not pure columns.
  // However: allow units suffix like "23.4C" (letters inside tokens) -> still ok.
  // We'll use a mild heuristic: if there are letters AND there is no obvious delimiter and only 1 value, reject.
  if (HasLetter(line) && !hasComma && !hasSemi && parsed < 2) {
    return 0;
  }

  return parsed;
}

// Header detection: line without numbers, looks like identifiers separated by delimiters.
bool ArduinoPlotParser::IsLikelyHeaderLine(const std::string &line, std::vector<std::string> &outNames) {
  outNames.clear();
  std::string s = Trim(line);
  if (s.empty())
    return false;
  if (HasAnyDigit(s))
    return false; // header should not have digits (simple rule)

  // Split by common delimiters
  bool hasComma = (s.find(',') != std::string::npos);
  bool hasSemi = (s.find(';') != std::string::npos);

  const char *delims = nullptr;
  if (hasSemi)
    delims = ";\t\r\n ";
  else if (hasComma)
    delims = ",\t\r\n ";
  else
    delims = " \t\r\n";

  auto toks = SplitTokensByDelims(s, delims);
  for (auto &t : toks) {
    t = Trim(t);
    if (t.empty())
      continue;
    if (!LooksLikeIdentifier(t))
      return false;
    outNames.push_back(t);
  }
  return outNames.size() >= 1;
}

void ArduinoPlotParser::EmitSample(const std::string &name, double value) {
  if (!m_view)
    return;
  m_view->AddSampleAt(wxString::FromUTF8(name.c_str()), value, m_time);
}

void ArduinoPlotParser::EmitColumns(const std::vector<double> &values) {
  if (!m_view)
    return;
  if (values.empty())
    return;

  // Ensure column names
  if ((int)m_columnNames.size() != (int)values.size()) {
    m_columnNames.clear();
    m_columnNames.reserve(values.size());
    for (size_t i = 0; i < values.size(); i++) {
      m_columnNames.push_back("v" + std::to_string(i));
    }
  }

  for (size_t i = 0; i < values.size(); i++) {
    EmitSample(m_columnNames[i], values[i]);
  }
}

void ArduinoPlotParser::ApplyLine(const std::string &lineRaw, double time) {
  m_time = time;

  std::string line = Trim(lineRaw);
  if (line.empty())
    return;

  switch (m_mode) {
    case Mode::Unknown:
      ProcessLine_Unknown(line);
      break;
    case Mode::Tagged:
      ProcessLine_Tagged(line);
      break;
    case Mode::Columns:
      ProcessLine_Columns(line);
      break;
  }
}

void ArduinoPlotParser::ApplyBatch(const std::vector<BufferedPlotLine> &lines) {
  if (!m_view || lines.empty())
    return;

  bool anyEmitted = false;

  // Cache for tagged names (avoid repeated UTF-8 conversions).
  // (Columns use a separate fast cache below.)
  std::unordered_map<std::string, wxString> wxNameCache;
  wxNameCache.reserve(64);

  auto toWxCached = [&](const std::string &name) -> const wxString & {
    auto it = wxNameCache.find(name);
    if (it != wxNameCache.end())
      return it->second;

    auto ins = wxNameCache.emplace(name, wxString::FromUTF8(name.c_str()));
    return ins.first->second;
  };

  auto emitSampleNR = [&](const std::string &name, double value, double t_ms) {
    m_view->AddSampleAt(toWxCached(name), value, t_ms, /*refresh=*/false);
    anyEmitted = true;
  };

  // Reused scratch buffers (reduce allocations).
  std::vector<std::pair<std::string, double>> pairs;
  pairs.reserve(16);

  std::vector<double> colVals;
  colVals.reserve(16);

  std::vector<std::string> header;
  header.reserve(16);

  // Fast cache for column names (no hashing, just parallel vector).
  std::vector<wxString> colWxNames;
  colWxNames.reserve(16);
  bool colCacheDirty = true;

  auto rebuildColCacheIfNeeded = [&]() {
    if (!colCacheDirty && (int)colWxNames.size() == (int)m_columnNames.size())
      return;

    colWxNames.clear();
    colWxNames.reserve(m_columnNames.size());
    for (const auto &n : m_columnNames)
      colWxNames.push_back(wxString::FromUTF8(n.c_str()));

    colCacheDirty = false;
  };

  auto processTaggedNR = [&](const std::string &line, double t_ms) {
    const bool acceptComma = m_acceptDecimalCommaUser || HeuristicAcceptCommaDecimal(line);

    if (ParseTaggedPairs(line, acceptComma, pairs) >= 1) {
      for (auto &kv : pairs)
        emitSampleNR(kv.first, kv.second, t_ms);
      return;
    }

    auto toks = SplitTokensLoose(line);
    if (ParseImplicitPairs(toks, acceptComma, pairs) >= 1) {
      for (auto &kv : pairs)
        emitSampleNR(kv.first, kv.second, t_ms);
      return;
    }

    // In tagged mode: if not parseable, ignore (same behavior as ProcessLine_Tagged).
  };

  auto processColumnsNR = [&](const std::string &line, double t_ms) {
    header.clear();
    if (IsLikelyHeaderLine(line, header)) {
      m_columnNames = header;
      m_lastColumnCount = (int)m_columnNames.size();
      colCacheDirty = true;
      return;
    }

    const bool acceptComma = m_acceptDecimalCommaUser || HeuristicAcceptCommaDecimal(line);

    char delim = '\0';
    const size_t n = ParseColumns(line, acceptComma, colVals, delim);
    if (n == 0)
      return;

    m_columnsDelimHint = delim;

    // Adopt pending header (from probing) if it matches.
    if (!m_pendingHeaderNames.empty() && (int)m_pendingHeaderNames.size() == (int)n) {
      m_columnNames = m_pendingHeaderNames;
      m_pendingHeaderNames.clear();
      colCacheDirty = true;
    }

    // Ensure names count matches.
    if ((int)m_columnNames.size() != (int)n) {
      m_columnNames.clear();
      m_columnNames.reserve(n);
      for (size_t i = 0; i < n; i++)
        m_columnNames.push_back("v" + std::to_string(i));
      colCacheDirty = true;
    }

    m_lastColumnCount = (int)n;

    rebuildColCacheIfNeeded();

    // Emit columns without refresh.
    for (size_t i = 0; i < n; i++) {
      m_view->AddSampleAt(colWxNames[i], colVals[i], t_ms, /*refresh=*/false);
      anyEmitted = true;
    }
  };

  auto processUnknownNR = [&](const std::string &line, double t_ms) {
    // Same logic as ProcessLine_Unknown, but flush uses NR processors.
    header.clear();
    if (IsLikelyHeaderLine(line, header)) {
      m_pendingHeaderNames = header;
      m_probe.push_back({line});
    } else {
      m_probe.push_back({line});
    }

    while (m_probe.size() > m_maxProbeLines)
      m_probe.pop_front();

    DecideModeFromProbe();

    if (m_mode != Mode::Unknown) {
      // Flush probe in decided mode.
      // NOTE: This matches the existing behavior: flushed probe lines get the current time.
      for (const auto &pl : m_probe) {
        if (m_mode == Mode::Tagged)
          processTaggedNR(pl.raw, t_ms);
        else if (m_mode == Mode::Columns)
          processColumnsNR(pl.raw, t_ms);
      }
      m_probe.clear();
    }
  };

  // Main loop
  for (const auto &bl : lines) {
    m_time = bl.time;

    std::string line = Trim(bl.line);
    if (line.empty())
      continue;

    switch (m_mode) {
      case Mode::Unknown:
        processUnknownNR(line, bl.time);
        break;
      case Mode::Tagged:
        processTaggedNR(line, bl.time);
        break;
      case Mode::Columns:
        processColumnsNR(line, bl.time);
        break;
    }
  }

  if (anyEmitted) {
    m_view->Refresh(false);
  }
}

void ArduinoPlotParser::ProcessLine_Unknown(const std::string &line) {
  // Collect header candidate if it looks like header
  std::vector<std::string> header;
  if (IsLikelyHeaderLine(line, header)) {
    m_pendingHeaderNames = header;
    // Still keep it in probe to allow flush if we decide Columns mode.
    m_probe.push_back({line});
  } else {
    m_probe.push_back({line});
  }

  // Keep probe bounded
  while (m_probe.size() > m_maxProbeLines)
    m_probe.pop_front();

  // Try decide mode
  DecideModeFromProbe();

  if (m_mode != Mode::Unknown) {
    FlushProbeAsCurrentMode();
    m_probe.clear();
  }
}

void ArduinoPlotParser::DecideModeFromProbe() {
  // Prefer Tagged if any probe line clearly contains tagged pairs.
  for (const auto &pl : m_probe) {
    bool acceptComma = m_acceptDecimalCommaUser || HeuristicAcceptCommaDecimal(pl.raw);

    std::vector<std::pair<std::string, double>> pairs;
    if (ParseTaggedPairs(pl.raw, acceptComma, pairs) >= 1) {
      m_mode = Mode::Tagged;
      return;
    }

    // implicit pairs? (name value name value)
    auto toks = SplitTokensLoose(pl.raw);
    if (ParseImplicitPairs(toks, acceptComma, pairs) >= 1) {
      m_mode = Mode::Tagged;
      return;
    }
  }

  // If no tagged detected, check if we have at least one strong columns line.
  int bestCount = 0;
  for (const auto &pl : m_probe) {
    bool acceptComma = m_acceptDecimalCommaUser || HeuristicAcceptCommaDecimal(pl.raw);

    std::vector<double> vals;
    char delim = '\0';
    size_t n = ParseColumns(pl.raw, acceptComma, vals, delim);
    if ((int)n > bestCount) {
      bestCount = (int)n;
    }
  }

  if (bestCount >= 2) {
    // Columns mode becomes likely if we saw at least one line with 2+ numbers.
    m_mode = Mode::Columns;
    return;
  }

  // Still unknown
  m_mode = Mode::Unknown;
}

void ArduinoPlotParser::FlushProbeAsCurrentMode() {
  // When switching from Unknown -> mode, interpret buffered lines in that mode.
  for (const auto &pl : m_probe) {
    if (m_mode == Mode::Tagged)
      ProcessLine_Tagged(pl.raw);
    else if (m_mode == Mode::Columns)
      ProcessLine_Columns(pl.raw);
  }
}

void ArduinoPlotParser::ProcessLine_Tagged(const std::string &line) {
  bool acceptComma = m_acceptDecimalCommaUser || HeuristicAcceptCommaDecimal(line);

  std::vector<std::pair<std::string, double>> pairs;

  // Try explicit tagged
  if (ParseTaggedPairs(line, acceptComma, pairs) >= 1) {
    for (auto &kv : pairs)
      EmitSample(kv.first, kv.second);
    return;
  }

  // Try implicit tagged: name value name value
  auto toks = SplitTokensLoose(line);
  if (ParseImplicitPairs(toks, acceptComma, pairs) >= 1) {
    for (auto &kv : pairs)
      EmitSample(kv.first, kv.second);
    return;
  }

  // If we're in tagged mode but this line isn't parseable as tagged, ignore it.
  // (Common when logs mix messages.)
}

void ArduinoPlotParser::ProcessLine_Columns(const std::string &line) {
  // Header line? (used to name channels)
  std::vector<std::string> header;
  if (IsLikelyHeaderLine(line, header)) {
    m_columnNames = header;
    m_lastColumnCount = (int)m_columnNames.size();
    return;
  }

  bool acceptComma = m_acceptDecimalCommaUser || HeuristicAcceptCommaDecimal(line);

  std::vector<double> vals;
  char delim = '\0';
  size_t n = ParseColumns(line, acceptComma, vals, delim);
  if (n == 0)
    return;

  m_columnsDelimHint = delim;

  // If we had a pending header from probing and it matches, adopt it now.
  if (!m_pendingHeaderNames.empty() && (int)m_pendingHeaderNames.size() == (int)n) {
    m_columnNames = m_pendingHeaderNames;
    m_pendingHeaderNames.clear();
  }

  // If current column name count doesn't match, regenerate (or shrink/expand)
  if ((int)m_columnNames.size() != (int)n) {
    // If existing names were from header but count changed, regenerate to avoid wrong mapping.
    m_columnNames.clear();
    m_columnNames.reserve(n);
    for (size_t i = 0; i < n; i++)
      m_columnNames.push_back("v" + std::to_string(i));
  }

  m_lastColumnCount = (int)n;
  EmitColumns(vals);
}
