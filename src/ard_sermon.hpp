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

#include <memory>
#include <string>
#include <vector>
#include <wx/config.h>
#include <wx/stc/stc.h>
#include <wx/stopwatch.h>
#include <wx/wx.h>

class wxNotebook;
class wxPanel;
class wxBookCtrlEvent;

class ArduinoPlotView;
class ArduinoPlotSink;
class ArduinoValuesView;
class ArduinoPlotParser;
struct BufferedPlotLine;
struct EditorSettings;

enum class SerialOutputFormat {
  Text = 0,
  Hex = 1,
  HexAscii = 2,
};

struct SerialChunkPayload {
  time_t sec = 0;
  std::string bytes;
};

class SerialMonitorWorker : public wxThread {
public:
  SerialMonitorWorker(wxEvtHandler *handler,
                      const wxString &port,
                      long baud);
  ~SerialMonitorWorker() override;

  void RequestStop();
  void InterruptIo();
  bool PulseResetLines(int pulseMs = 50, bool dtr = true, bool rts = true);

  // simple sync write - parent is called from the GUI thread
  bool Write(const std::string &data);

protected:
  ExitCode Entry() override;

private:
  bool OpenPort();
  void ClosePort();

  wxEvtHandler *m_handler;
  wxString m_port;
  long m_baud;

  wxMutex m_mutex;
  bool m_stopRequested;

#if defined(__unix__) || defined(__APPLE__)
  int m_fd;
#else
  void *m_handle;
#endif
};

enum class LineEndingMode {
  None,
  LF,
  CR,
  CRLF
};

class ArduinoSerialMonitorFrame : public wxFrame {
public:
  ArduinoSerialMonitorFrame(wxWindow *parent,
                            wxConfigBase *config,
                            wxConfigBase *sketchConfig,
                            const wxString &portName,
                            long baudRate);
  ~ArduinoSerialMonitorFrame() override;

  void Block();
  void Unblock();
  bool IsBlocked() const { return m_isBlocked; }
  void Close();

private:
  void OnNotebookPageChanged(wxBookCtrlEvent &event);
  void EnsurePlotterStarted();
  void AlignPlotTimeBase();
  void FeedPlotChunkUtf8(const std::string &chunkUtf8, double time);
  void BufferPlotChunkUtf8(const std::string &chunkUtf8, double time);
  void NormalizeLineEndings(wxString &chunk);

  void CreateControls();
  void StartWorker();
  void StopWorker();

  void OnClose(wxCloseEvent &event);
  void OnSend(wxCommandEvent &event);
  void OnInputEnter(wxCommandEvent &event);
  void OnBaudChanged(wxCommandEvent &event);
  void OnData(wxThreadEvent &event);
  void OnError(wxThreadEvent &event);

  void ClearLog();
  void ClearChart();

  void OnLineEndingChanged(wxCommandEvent &event);
  void OnClear(wxCommandEvent &event);
  void OnReset(wxCommandEvent &event);
  void OnPause(wxCommandEvent &event);

  void SetupOutputCtrl(const EditorSettings &settings);

  void OnOutputUpdateUI(wxStyledTextEvent &event);

  void ScrollOutputToEnd();
  bool IsOutputAtBottom() const;
  bool m_programmaticScroll = false;
  int m_lastFirstVisibleLine = -1;

  void OnSysColourChanged(wxSysColourChangedEvent &event);

  wxConfigBase *m_config{nullptr};
  wxConfigBase *m_sketchConfig{nullptr};
  wxString m_portName;
  long m_baudRate{115200};

  wxStyledTextCtrl *m_outputCtrl = nullptr;
  wxTextCtrl *m_inputCtrl = nullptr;
  wxComboBox *m_baudCombo = nullptr;
  wxComboBox *m_lineEndCombo = nullptr;
  wxChoice *m_outputFormatChoice = nullptr;
  wxBitmapButton *m_sendButton = nullptr;
  wxButton *m_pauseButton = nullptr;
  wxButton *m_resetButton = nullptr;
  wxButton *m_clearButton = nullptr;
  wxNotebook *m_notebook = nullptr;
  wxPanel *m_logPage = nullptr;
  wxPanel *m_plotPage = nullptr;

  SerialMonitorWorker *m_worker{nullptr};

  LineEndingMode m_lineEndingMode{LineEndingMode::LF};

  std::vector<wxString> m_pausedBuffer;
  size_t m_pausedTextChars = 0;
  bool m_isBlocked = false;
  bool m_paused = false;
  bool m_pendingCR = false;
  bool m_autoScroll = true;
  bool m_timestamps = false;

  // --- text output throttling ---
  wxTimer m_textFlushTimer;
  bool m_textFlushScheduled = false;
  wxString m_textPending;
  void QueueTextAppend(const wxString &s);
  void FlushPendingText(bool force = false);
  void OnTextFlushTimer(wxTimerEvent &);

  // plot / telemetry
  ArduinoPlotView *m_plotView = nullptr;
  std::unique_ptr<ArduinoPlotSink> m_plotSink;

  ArduinoValuesView *m_valuesView = nullptr;
  std::unique_ptr<ArduinoPlotSink> m_valuesSink;

  std::unique_ptr<ArduinoPlotSink> m_telemetrySink;
  std::unique_ptr<ArduinoPlotParser> m_plotParser;

  bool m_plotStarted = false;
  bool m_displayValues = false;

  wxStopWatch m_telemetryWatch;

  // buffering for plot line parsing
  std::string m_plotLineBuf;
  std::vector<BufferedPlotLine> m_pausedPlotBuffer;

  // --- timestamps state ---
  bool m_tsAtLineStart = true;
  bool m_tsPending = false;
  time_t m_tsLastPrintedSec = (time_t)-1;
  time_t m_tsPendingSec = (time_t)-1;

  // --- output mode ---
  void OnOutputFormatChanged(wxCommandEvent &);
  SerialOutputFormat m_outputFormat = SerialOutputFormat::Text;

  // hex layout
  int m_hexWrapBytes = 16;
  wxString m_hexAsciiBuf;
  int m_hexCol = 0;
};
