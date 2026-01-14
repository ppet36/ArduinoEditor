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

#include "ard_sermon.hpp"
#include "ard_ap.hpp"
#include "ard_ed_frm.hpp"
#include "ard_ev.hpp"
#include "ard_plotpars.hpp"
#include "ard_plotview.hpp"
#include "ard_setdlg.hpp"
#include "ard_valsview.hpp"
#include "utils.hpp"
#include <errno.h>
#include <memory>
#include <unordered_map>
#include <wx/datetime.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/notebook.h>
#include <wx/stc/stc.h>
#include <wx/wupdlock.h>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

// The devil knows why I wrote it in CamelCase but I'll leave it at that :)
enum {
  ID_BaudCombo = wxID_HIGHEST + 100,
  ID_SendButton,
  ID_InputCtrl,
  ID_LineEndingCombo,
  ID_OutputFormatChoice,
  ID_PauseButton,
  ID_ClearButton,
  ID_ResetButton,
  ID_OutputMenuCopy,
  ID_OutputMenuSaveSelection,
  ID_OutputMenuSaveAll,
  ID_OutputMenuDisplayValues,
  ID_OutputMenuAutoScroll,
  ID_OutputMenuTimestamps,
  ID_OutputMenuClear
};

// Limits to prevent uncontrollable growth.
namespace {
constexpr int kMaxOutputLines = 20000;              // safe number of rows
constexpr int kMaxOutputChars = 4 * 1024 * 1024;    // cca 4 MB inner stc buffer in bytes
constexpr size_t kMaxPausedTextChars = 1024 * 1024; // 1 MB paused text buffer (in "wxChar" length)
constexpr size_t kMaxPausedPlotLines = 50000;       // paused plot buffer limie

static inline bool IsLatin1Printable(unsigned char b) {
  // 0x20..0x7E = ASCII printable
  // 0xA0..0xFF = printable-ish in Latin-1 (0x80..0x9F are control codes)
  return (b >= 0x20 && b <= 0x7E) || (b >= 0xA0);
}

static inline wxString BytesToLatin1(const std::string &s) {
  wxString out;
  out.reserve(s.size());
  for (unsigned char b : s) {
    out += wxUniChar((wchar_t)b); // maps 0..255 to U+0000..U+00FF
  }
  return out;
}

} // namespace

// --- Telemetry sinks (Plot parser -> UI consumers) ---
class ArduinoFanoutSink final : public ArduinoPlotSink {
public:
  ArduinoFanoutSink(ArduinoPlotSink *a = nullptr, ArduinoPlotSink *b = nullptr)
      : m_a(a), m_b(b) {}

  void Set(ArduinoPlotSink *a, ArduinoPlotSink *b) {
    m_a = a;
    m_b = b;
  }

  void AddSampleAt(const std::string &name, double value, double t_ms, bool refresh) override {
    if (m_a)
      m_a->AddSampleAt(name, value, t_ms, refresh);
    if (m_b)
      m_b->AddSampleAt(name, value, t_ms, refresh);
  }

  void Refresh(bool eraseBackground) override {
    if (m_a)
      m_a->Refresh(eraseBackground);
    if (m_b)
      m_b->Refresh(eraseBackground);
  }

private:
  ArduinoPlotSink *m_a = nullptr;
  ArduinoPlotSink *m_b = nullptr;
};

class ArduinoPlotViewSink final : public ArduinoPlotSink {
public:
  explicit ArduinoPlotViewSink(ArduinoPlotView *view) : m_view(view) {}

  // Map external time base (serial monitor clock) to the plot view internal clock.
  void SetTimeOffset(double offsetMs) { m_timeOffsetMs = offsetMs; }

  void AddSampleAt(const std::string &name, double value, double t_ms, bool refresh) override {
    if (!m_view)
      return;
    const wxString &wxName = ToWxCached(name);
    m_view->AddSampleAt(wxName, value, t_ms + m_timeOffsetMs, refresh);
  }

  void Refresh(bool eraseBackground) override {
    if (m_view)
      m_view->Refresh(eraseBackground);
  }

private:
  const wxString &ToWxCached(const std::string &name) {
    auto it = m_cache.find(name);
    if (it != m_cache.end())
      return it->second;
    auto ins = m_cache.emplace(name, wxString::FromUTF8(name.c_str()));
    return ins.first->second;
  }

  ArduinoPlotView *m_view = nullptr;
  std::unordered_map<std::string, wxString> m_cache;
  double m_timeOffsetMs = 0.0;
};

class ArduinoValuesViewSink final : public ArduinoPlotSink {
public:
  explicit ArduinoValuesViewSink(ArduinoValuesView *view) : m_view(view) {}

  void AddSampleAt(const std::string &name, double value, double /*t_ms*/, bool refresh) override {
    if (!m_view)
      return;
    const wxString &wxName = ToWxCached(name);
    m_view->AddSample(wxName, value, refresh);
  }

  void Refresh(bool eraseBackground) override {
    if (m_view)
      m_view->Refresh(eraseBackground);
  }

private:
  const wxString &ToWxCached(const std::string &name) {
    auto it = m_cache.find(name);
    if (it != m_cache.end())
      return it->second;
    auto ins = m_cache.emplace(name, wxString::FromUTF8(name.c_str()));
    return ins.first->second;
  }

  ArduinoValuesView *m_view = nullptr;
  std::unordered_map<std::string, wxString> m_cache;
};

#if defined(__unix__) || defined(__APPLE__)

struct BaudMapItem {
  long baud;
  speed_t speed;
};

static const BaudMapItem kBaudMap[] = {
#ifdef B1200
    {1200, B1200},
#endif
#ifdef B2400
    {2400, B2400},
#endif
#ifdef B4800
    {4800, B4800},
#endif
#ifdef B9600
    {9600, B9600},
#endif
#ifdef B19200
    {19200, B19200},
#endif
#ifdef B38400
    {38400, B38400},
#endif
#ifdef B57600
    {57600, B57600},
#endif
#ifdef B115200
    {115200, B115200},
#endif
#ifdef B230400
    {230400, B230400},
#endif
#ifdef B460800
    {460800, B460800},
#endif
#ifdef B500000
    {500000, B500000},
#endif
#ifdef B576000
    {576000, B576000},
#endif
#ifdef B921600
    {921600, B921600},
#endif
#ifdef B1000000
    {1000000, B1000000},
#endif
#ifdef B1152000
    {1152000, B1152000},
#endif
#ifdef B1500000
    {1500000, B1500000},
#endif
#ifdef B2000000
    {2000000, B2000000},
#endif
#ifdef B2500000
    {2500000, B2500000},
#endif
#ifdef B3000000
    {3000000, B3000000},
#endif
#ifdef B3500000
    {3500000, B3500000},
#endif
#ifdef B4000000
    {4000000, B4000000},
#endif
};

static bool TryGetSpeedForBaud(long baud, speed_t &outSpeed) {
  for (const auto &it : kBaudMap) {
    if (it.baud == baud) {
      outSpeed = it.speed;
      return true;
    }
  }
  return false;
}

static wxArrayString BuildBaudChoicesPosix() {
  wxArrayString out;
  for (const auto &it : kBaudMap) {
    out.Add(wxString::Format(wxT("%ld"), it.baud));
  }
  return out;
}

#endif

SerialMonitorWorker::SerialMonitorWorker(wxEvtHandler *handler,
                                         const wxString &port,
                                         long baud)
    : wxThread(wxTHREAD_JOINABLE),
      m_handler(handler),
      m_port(port),
      m_baud(baud),
      m_stopRequested(false),
#if defined(__unix__) || defined(__APPLE__)
      m_fd(-1)
#else
      m_handle(nullptr)
#endif
{
}

SerialMonitorWorker::~SerialMonitorWorker() {
  // just to be sure
  RequestStop();
  ClosePort();
}

void SerialMonitorWorker::RequestStop() {
  wxMutexLocker lock(m_mutex);
  m_stopRequested = true;
}

bool SerialMonitorWorker::OpenPort() {
#if defined(__unix__) || defined(__APPLE__)
  m_fd = ::open(wxToStd(m_port).data(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (m_fd < 0) {
    return false;
  }

  struct termios tio{};
  if (tcgetattr(m_fd, &tio) != 0) {
    ClosePort();
    return false;
  }

  cfmakeraw(&tio);

  speed_t speed;
  if (!TryGetSpeedForBaud(m_baud, speed)) {
    // fallback
    speed = B115200;
  }

  cfsetispeed(&tio, speed);
  cfsetospeed(&tio, speed);

  tio.c_cflag |= (CLOCAL | CREAD);
  tio.c_cflag &= ~CSIZE;
  tio.c_cflag |= CS8;

  if (tcsetattr(m_fd, TCSANOW, &tio) != 0) {
    ClosePort();
    return false;
  }

  // we keep NONBLOCK - read then returns EAGAIN and the thread can check the stop flag
  return true;
#else
  // m_port me teoreticky obsahovat i "COM4 (Arduino Nano ...)" - usekni to na prvn mezeru
  wxString rawPort = m_port;
  int spacePos = rawPort.Find(' ');
  if (spacePos != wxNOT_FOUND) {
    rawPort = rawPort.Left(spacePos);
  }

  std::string portName = wxToStd(rawPort);

  if (!portName.empty()) {
    if (portName.rfind("\\\\.\\", 0) != 0) {
      if (portName.rfind("COM", 0) == 0 || portName.rfind("com", 0) == 0) {
        portName = "\\\\.\\" + portName;
      }
    }
  }

  wxLogDebug(wxT("SerialMonitorWorker::OpenPort() opening '%s'"), wxString::FromUTF8(portName));

  HANDLE h = CreateFileA(portName.c_str(),
                         GENERIC_READ | GENERIC_WRITE,
                         0, // exkluzivn pstup
                         nullptr,
                         OPEN_EXISTING,
                         0, // sync I/O
                         nullptr);

  if (h == INVALID_HANDLE_VALUE) {
    wxLogDebug(wxT("CreateFileA failed, GetLastError() = %lu"), GetLastError());
    m_handle = nullptr;
    return false;
  }

  // --- DCB nastaven ---
  DCB dcb;
  SecureZeroMemory(&dcb, sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);

  if (!GetCommState(h, &dcb)) {
    CloseHandle(h);
    m_handle = nullptr;
    return false;
  }

  dcb.BaudRate = static_cast<DWORD>(m_baud);
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;

  // Turn on DTR/RTS - on many Arduinos (especially USB-CDC)
  // Serial won't wake up or behaves strangely without it.
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;

  // Disable all HW/SW flow-control so that nothing interferes with us:
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  dcb.fNull = FALSE;

  if (!SetCommState(h, &dcb)) {
    CloseHandle(h);
    m_handle = nullptr;
    return false;
  }

  // --- Timeouts ---
  COMMTIMEOUTS timeouts{};
  // Simple mode: ReadFile immediately returns what is available to it
  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.ReadTotalTimeoutConstant = 0;
  timeouts.WriteTotalTimeoutMultiplier = 0;
  timeouts.WriteTotalTimeoutConstant = 0;

  if (!SetCommTimeouts(h, &timeouts)) {
    CloseHandle(h);
    m_handle = nullptr;
    return false;
  }

  // Clean buffers (mainly RX)
  PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

  m_handle = h;
  return true;
#endif
}

void SerialMonitorWorker::ClosePort() {
#if defined(__unix__) || defined(__APPLE__)
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
#else
  HANDLE h = static_cast<HANDLE>(m_handle);
  if (h) {
    CloseHandle(h);
    m_handle = nullptr;
  }
#endif
}

bool SerialMonitorWorker::Write(const std::string &data) {
#if defined(__unix__) || defined(__APPLE__)
  wxMutexLocker lock(m_mutex);
  if (m_fd < 0) {
    return false;
  }
  ssize_t written = ::write(m_fd, data.data(), data.size());
  return written == (ssize_t)data.size();
#else
  wxMutexLocker lock(m_mutex);
  HANDLE h = static_cast<HANDLE>(m_handle);
  if (!h) {
    return false;
  }

  DWORD bytesWritten = 0;
  BOOL ok = WriteFile(h,
                      data.data(),
                      static_cast<DWORD>(data.size()),
                      &bytesWritten,
                      nullptr);
  return ok && bytesWritten == data.size();
#endif
}

wxThread::ExitCode SerialMonitorWorker::Entry() {
  if (!OpenPort()) {
    auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_ERROR);
    evt->SetString(_("Failed to open serial port"));
    wxQueueEvent(m_handler, evt);
    return nullptr;
  }

  const size_t BUF_SIZE = 256;
  char buf[BUF_SIZE];

  while (true) {
    {
      wxMutexLocker lock(m_mutex);
      if (m_stopRequested) {
        break;
      }
    }

#if defined(__unix__) || defined(__APPLE__)
    // Copy fd under lock so InterruptIo()/ClosePort() can safely invalidate it.
    int fd = -1;
    bool stopping = false;
    {
      wxMutexLocker lock(m_mutex);
      fd = m_fd;
      stopping = m_stopRequested;
    }

    // If fd is already invalid, treat it as normal shutdown when stopping.
    if (fd < 0) {
      if (stopping) {
        break;
      }
      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_ERROR);
      evt->SetString(_("Serial port handle invalid"));
      wxQueueEvent(m_handler, evt);
      break;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    // Timeout so we periodically re-check stop flag.
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200 * 1000; // 200ms

    int rc = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (rc == 0) {
      // timeout
      continue;
    }
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }

      // If we're stopping and fd was closed from another thread, select() may fail with EBADF.
      {
        wxMutexLocker lock(m_mutex);
        stopping = m_stopRequested;
      }
      if (stopping && (errno == EBADF || errno == EIO)) {
        break;
      }

      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_ERROR);
      evt->SetString(wxString::Format(_("Serial select error: %d"), errno));
      wxQueueEvent(m_handler, evt);
      break;
    }

    if (!FD_ISSET(fd, &rfds)) {
      continue;
    }

    ssize_t n = ::read(fd, buf, BUF_SIZE);
    if (n > 0) {
      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_DATA);

      SerialChunkPayload p;
      p.sec = wxDateTime::Now().GetTicks();
      p.bytes.assign(buf, buf + n);

      evt->SetPayload<SerialChunkPayload>(p);
      wxQueueEvent(m_handler, evt);
    } else if (n == 0) {
      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_ERROR);
      evt->SetString(_("Serial port closed"));
      wxQueueEvent(m_handler, evt);
      break;
    } else { // n < 0
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Rare after select(), but harmless.
        continue;
      }

      // If we're stopping and InterruptIo()/ClosePort() closed fd, read() often returns EBADF/EIO.
      {
        wxMutexLocker lock(m_mutex);
        stopping = m_stopRequested;
      }
      if (stopping && (errno == EBADF || errno == EIO)) {
        break;
      }

      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_ERROR);
      evt->SetString(wxString::Format(_("Serial read error: %d"), errno));
      wxQueueEvent(m_handler, evt);
      break;
    }
#else
    HANDLE h = static_cast<HANDLE>(m_handle);
    if (!h) {
      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_ERROR);
      evt->SetString(_("Serial port handle invalid"));
      wxQueueEvent(m_handler, evt);
      break;
    }

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(h, buf, BUF_SIZE, &bytesRead, nullptr);
    if (ok && bytesRead > 0) {
      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_DATA);

      SerialChunkPayload p;
      p.sec = wxDateTime::Now().GetTicks();
      p.bytes.assign(buf, buf + bytesRead);

      evt->SetPayload<SerialChunkPayload>(p);
      wxQueueEvent(m_handler, evt);
    } else if (!ok) {
      DWORD err = GetLastError();
      if (err == ERROR_OPERATION_ABORTED) {
        // typical port closure -> we exit without much fuss
        break;
      }
      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_ERROR);
      evt->SetString(wxString::Format(_("Serial read error (Win): %lu"), err));
      wxQueueEvent(m_handler, evt);
      break;
    } else {
      // ok == TRUE, bytesRead == 0 -> time-out, just check the stop flag in the next iteration
      wxThread::Sleep(10);
    }
#endif
  }

  ClosePort();
  return nullptr;
}

// -----------------------------------------------------------------------------------------------

ArduinoSerialMonitorFrame::ArduinoSerialMonitorFrame(wxWindow *parent,
                                                     wxConfigBase *config,
                                                     wxConfigBase *sketchConfig,
                                                     const wxString &portName,
                                                     long baudRate)
    : wxFrame(parent,
              wxID_ANY,
              wxString::Format(_("Serial Monitor [%s]"), portName),
              wxDefaultPosition,
              wxSize(700, 500),
              wxDEFAULT_FRAME_STYLE | wxRESIZE_BORDER),
      m_config(config),
      m_sketchConfig(sketchConfig),
      m_portName(portName),
      m_baudRate(baudRate),
      m_textFlushTimer(this) {
  CreateControls();

  // ---- Bindings for GUI events ----
  Bind(wxEVT_CLOSE_WINDOW, &ArduinoSerialMonitorFrame::OnClose, this);
  Bind(wxEVT_SYS_COLOUR_CHANGED, &ArduinoSerialMonitorFrame::OnSysColourChanged, this);

  m_sendButton->Bind(wxEVT_BUTTON, &ArduinoSerialMonitorFrame::OnSend, this);
  m_inputCtrl->Bind(wxEVT_TEXT_ENTER, &ArduinoSerialMonitorFrame::OnInputEnter, this);
  m_baudCombo->Bind(wxEVT_COMBOBOX, &ArduinoSerialMonitorFrame::OnBaudChanged, this);
  m_lineEndCombo->Bind(wxEVT_COMBOBOX, &ArduinoSerialMonitorFrame::OnLineEndingChanged, this);
  m_pauseButton->Bind(wxEVT_BUTTON, &ArduinoSerialMonitorFrame::OnPause, this);
  m_resetButton->Bind(wxEVT_BUTTON, &ArduinoSerialMonitorFrame::OnReset, this);
  m_clearButton->Bind(wxEVT_BUTTON, &ArduinoSerialMonitorFrame::OnClear, this);

  // ---- Bindings for thread events ----
  Bind(wxEVT_SERIAL_MONITOR_DATA, &ArduinoSerialMonitorFrame::OnData, this);
  Bind(wxEVT_SERIAL_MONITOR_ERROR, &ArduinoSerialMonitorFrame::OnError, this);

  m_textFlushScheduled = false;
  Bind(wxEVT_TIMER, &ArduinoSerialMonitorFrame::OnTextFlushTimer, this);

  // ---- User scrolling ----
  m_outputCtrl->Bind(wxEVT_STC_UPDATEUI, &ArduinoSerialMonitorFrame::OnOutputUpdateUI, this);

  m_notebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, &ArduinoSerialMonitorFrame::OnNotebookPageChanged, this);

  // default LF
  m_lineEndingMode = LineEndingMode::LF;

  StartWorker();
}

ArduinoSerialMonitorFrame::~ArduinoSerialMonitorFrame() {
  StopWorker();
}

void ArduinoSerialMonitorFrame::SetupOutputCtrl(const EditorSettings &settings) {
  EditorColorScheme c = settings.GetColors();

  // Serial output behaves like a log: no wrapping and no undo stack.
  m_outputCtrl->SetWrapMode(wxSTC_WRAP_NONE);
  m_outputCtrl->SetUndoCollection(false);
  m_outputCtrl->SetReadOnly(true);

  // Hide all margins (line numbers, folding, etc.)
  for (int i = 0; i < 5; ++i) {
    m_outputCtrl->SetMarginWidth(i, 0);
  }

  // ---- Default style: fonts + colors ----
  m_outputCtrl->StyleSetFont(wxSTC_STYLE_DEFAULT, settings.GetFont());
  m_outputCtrl->StyleSetForeground(wxSTC_STYLE_DEFAULT, c.text);
  m_outputCtrl->StyleSetBackground(wxSTC_STYLE_DEFAULT, c.background);

  m_outputCtrl->StyleClearAll();

  m_outputCtrl->SetBackgroundColour(c.background);
  m_outputCtrl->SetCaretForeground(c.text);
}

void ArduinoSerialMonitorFrame::CreateControls() {
  if (m_sketchConfig) {
    long savedBaud = 0;
    if (m_sketchConfig->Read(wxT("SerialMonitorBaud"), &savedBaud) && (savedBaud > 0)) {
      m_baudRate = savedBaud;
    } else {
      if (m_baudRate == 0) {
        m_baudRate = 115200; // default
      }
    }

    long savedFmt = 0;
    if (m_sketchConfig->Read(wxT("SerialOutputFormat"), &savedFmt, 0)) {
      savedFmt = wxClip(savedFmt, 0L, 2L);
    }
    m_outputFormat = (SerialOutputFormat)savedFmt;

    m_sketchConfig->Read(wxT("SerialShowTimestamps"), &m_timestamps, false);
    m_sketchConfig->Read(wxT("SerialDisplayValues"), &m_displayValues, false);
  }

  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  // Top row - port + baud + line ending + autoscroll + timestamps + clear
  auto *headerSizer = new wxBoxSizer(wxHORIZONTAL);
  headerSizer->Add(new wxStaticText(this, wxID_ANY,
                                    wxString::Format(_("Port: %s"), m_portName)),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

  wxArrayString baudChoices;
#if defined(__unix__) || defined(__APPLE__)
  baudChoices = BuildBaudChoicesPosix();
#else
  // Windows
  baudChoices.Add(wxT("1200"));
  baudChoices.Add(wxT("2400"));
  baudChoices.Add(wxT("4800"));
  baudChoices.Add(wxT("9600"));
  baudChoices.Add(wxT("19200"));
  baudChoices.Add(wxT("38400"));
  baudChoices.Add(wxT("57600"));
  baudChoices.Add(wxT("115200"));
  baudChoices.Add(wxT("230400"));
  baudChoices.Add(wxT("460800"));
  baudChoices.Add(wxT("921600"));
  baudChoices.Add(wxT("1000000"));
  baudChoices.Add(wxT("2000000"));
#endif

  m_baudCombo = new wxComboBox(this,
                               ID_BaudCombo,
                               wxEmptyString,
                               wxDefaultPosition,
                               wxDefaultSize,
                               baudChoices,
                               wxCB_READONLY);

  // set the initial value
  wxString baudStr = wxString::Format(wxT("%ld"), m_baudRate);
  if (m_baudCombo->FindString(baudStr) == wxNOT_FOUND) {
    m_baudCombo->Append(baudStr);
  }
  m_baudCombo->SetStringSelection(baudStr);

  headerSizer->Add(new wxStaticText(this, wxID_ANY, _("Baud:")), 0,
                   wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  headerSizer->Add(m_baudCombo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

  // ---- line endings combobox ----
  wxArrayString leChoices;
  leChoices.Add(_("None"));
  leChoices.Add(_("<LF>"));
  leChoices.Add(_("<CR>"));
  leChoices.Add(_("<CR><LF>"));

  m_lineEndCombo = new wxComboBox(this,
                                  ID_LineEndingCombo,
                                  _("<LF>"),
                                  wxDefaultPosition,
                                  wxDefaultSize,
                                  leChoices,
                                  wxCB_READONLY);
  headerSizer->Add(new wxStaticText(this, wxID_ANY, _("Line ending:")), 0,
                   wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  headerSizer->Add(m_lineEndCombo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

  // ---- output format choice ----
  wxArrayString fmtChoices;
  fmtChoices.Add(_("Text"));
  fmtChoices.Add(_("Hex"));
  fmtChoices.Add(_("Hex + ASCII"));

  m_outputFormatChoice = new wxChoice(this,
                                      ID_OutputFormatChoice,
                                      wxDefaultPosition,
                                      wxDefaultSize,
                                      fmtChoices);

  if (m_outputFormatChoice) {
    m_outputFormatChoice->SetSelection((int)m_outputFormat);
  }

  headerSizer->Add(new wxStaticText(this, wxID_ANY, _("Output:")), 0,
                   wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  headerSizer->Add(m_outputFormatChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

  m_outputFormatChoice->Bind(wxEVT_CHOICE, &ArduinoSerialMonitorFrame::OnOutputFormatChanged, this);

  headerSizer->AddStretchSpacer(1);

  // --- Pause / Reset / Resume ---
  m_pauseButton = new wxButton(this, ID_PauseButton, _("Pause"));
  headerSizer->Add(m_pauseButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

  m_resetButton = new wxButton(this, ID_ResetButton, _("Reset (DTR/RTS)"));
  m_resetButton->SetToolTip(_("Resets the connected device (DTR/RTS pulse). Same behavior as opening the serial port."));
  headerSizer->Add(m_resetButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

  m_clearButton = new wxButton(this, ID_ClearButton, _("Clear"));
  headerSizer->Add(m_clearButton, 0, wxALIGN_CENTER_VERTICAL);

  topSizer->Add(headerSizer, 0, wxEXPAND | wxALL, 8);

  // Output notebook (Log + Plot)
  m_notebook = new wxNotebook(this, wxID_ANY);

  // --- Log page ---
  m_logPage = new wxPanel(m_notebook);
  auto *logSizer = new wxBoxSizer(wxVERTICAL);

  m_outputCtrl = new wxStyledTextCtrl(m_logPage, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, wxBORDER_NONE);

  EditorSettings settings;
  settings.Load(m_config);
  SetupOutputCtrl(settings);

  // --- Custom popup menu for the output control ---
  m_outputCtrl->UsePopUp(false); // disable the default STC popup menu

  m_outputCtrl->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent &e) {
    if (!m_outputCtrl)
      return;

    const bool hasSel = (m_outputCtrl->GetSelectionStart() != m_outputCtrl->GetSelectionEnd());

    wxMenu menu;
    AddMenuItemWithArt(&menu,
                       ID_OutputMenuCopy,
                       _("Copy\tCtrl+C"),
                       wxEmptyString,
                       wxAEArt::Copy);
    menu.AppendSeparator();

    menu.AppendCheckItem(ID_OutputMenuDisplayValues, _("Display values"));
    menu.Check(ID_OutputMenuDisplayValues, m_displayValues);
    menu.AppendCheckItem(ID_OutputMenuAutoScroll, _("Autoscroll"));
    menu.Check(ID_OutputMenuAutoScroll, m_autoScroll);
    menu.AppendCheckItem(ID_OutputMenuTimestamps, _("Show timestamps"));
    menu.Check(ID_OutputMenuTimestamps, m_timestamps);

    menu.AppendSeparator();
    AddMenuItemWithArt(&menu,
                       ID_OutputMenuSaveSelection,
                       _("Save selection..."),
                       wxEmptyString,
                       wxAEArt::FileSave);
    AddMenuItemWithArt(&menu,
                       ID_OutputMenuSaveAll,
                       _("Save all..."),
                       wxEmptyString,
                       wxAEArt::FileSaveAs);
    menu.AppendSeparator();
    AddMenuItemWithArt(&menu,
                       ID_OutputMenuClear,
                       _("Clear"),
                       wxEmptyString,
                       wxAEArt::Delete);

    menu.Enable(ID_OutputMenuCopy, hasSel);
    menu.Enable(ID_OutputMenuSaveSelection, hasSel);

    auto sanitizeFilePart = [](wxString s) {
      for (wxUniCharRef ch : s) {
        if (!wxIsalnum(ch))
          ch = wxChar('_');
      }
      if (s.length() > 32)
        s = s.Mid(0, 32);
      return s;
    };

    auto saveText = [this, &sanitizeFilePart](const wxString &text, bool selectionOnly) {
      if (text.empty()) {
        wxBell();
        return;
      }

      wxString base = sanitizeFilePart(m_portName);
      if (base.empty())
        base = wxT("serial");

      wxString defaultName = selectionOnly
                                 ? wxString::Format(wxT("%s_selection.txt"), base)
                                 : wxString::Format(wxT("%s.log"), base);

      wxFileDialog dlg(this,
                       selectionOnly ? _("Save selection...") : _("Save all..."),
                       wxEmptyString,
                       defaultName,
                       _("Text files (*.txt)|*.txt|Log files (*.log)|*.log|All files (*.*)|*.*"),
                       wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

      if (dlg.ShowModal() != wxID_OK)
        return;

      wxFile file;
      if (!file.Create(dlg.GetPath(), true)) {
        wxLogWarning(_("Failed to save file: %s"), dlg.GetPath());
        return;
      }

      const wxCharBuffer buf = text.ToUTF8();
      if (buf.data() && buf.length() > 0) {
        file.Write(buf.data(), buf.length());
      }
      file.Close();
    };

    // Handlers
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
      if (m_outputCtrl)
        m_outputCtrl->Copy(); }, ID_OutputMenuCopy);

    menu.Bind(wxEVT_MENU, [this, &saveText](wxCommandEvent &) {
      if (!m_outputCtrl)
        return;
      saveText(m_outputCtrl->GetSelectedText(), true); }, ID_OutputMenuSaveSelection);

    menu.Bind(wxEVT_MENU, [this, &saveText](wxCommandEvent &) {
      if (!m_outputCtrl)
        return;
      saveText(m_outputCtrl->GetText(), false); }, ID_OutputMenuSaveAll);

    menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
      wxCommandEvent dummy(wxEVT_BUTTON, ID_ClearButton);
      dummy.SetEventObject(this);
      OnClear(dummy); // same handler as the Clear button
    },
              ID_OutputMenuClear);

    menu.Bind(wxEVT_MENU, [this](wxCommandEvent &evt) {
      m_displayValues = evt.IsChecked();
      if (m_valuesView) {
        m_valuesView->Show(m_displayValues);
      }
      if (m_sketchConfig) {
        m_sketchConfig->Write(wxT("SerialDisplayValues"), m_displayValues);
        m_sketchConfig->Flush();
      }
      Layout(); }, ID_OutputMenuDisplayValues);

    menu.Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { m_autoScroll = evt.IsChecked(); }, ID_OutputMenuAutoScroll);

    menu.Bind(wxEVT_MENU, [this](wxCommandEvent &evt) {
      m_timestamps = evt.IsChecked();
      if (m_sketchConfig) {
        m_sketchConfig->Write(wxT("SerialShowTimestamps"), m_timestamps);
        m_sketchConfig->Flush();
      } }, ID_OutputMenuTimestamps);

    wxPoint pt = e.GetPosition();
    if (pt == wxDefaultPosition) {
      pt = wxGetMousePosition();
    }
    pt = m_outputCtrl->ScreenToClient(pt);
    m_outputCtrl->PopupMenu(&menu, pt);
  });

  // ------

  logSizer->Add(m_outputCtrl, 1, wxEXPAND);

  m_logPage->SetSizer(logSizer);

  m_notebook->AddPage(m_logPage, _("Log"), true);

  // --- Plot page (created lazily on first activation) ---
  m_plotPage = new wxPanel(m_notebook);
  auto *plotSizer = new wxBoxSizer(wxVERTICAL);
  plotSizer->Add(new wxStaticText(m_plotPage,
                                  wxID_ANY,
                                  _("Open this tab to start plotting serial values.")),
                 1, wxALIGN_CENTER | wxALL, 12);
  m_plotPage->SetSizer(plotSizer);

  m_notebook->AddPage(m_plotPage, _("Plot"), false);

  topSizer->Add(m_notebook, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  // Live values (outside notebook)
  m_valuesView = new ArduinoValuesView(this);
  m_valuesView->Show(m_displayValues);
  topSizer->Add(m_valuesView, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  // Bottom line - input + send
  auto *bottomSizer = new wxBoxSizer(wxHORIZONTAL);

  m_inputCtrl = new wxTextCtrl(this,
                               ID_InputCtrl,
                               wxEmptyString,
                               wxDefaultPosition,
                               wxDefaultSize,
                               wxTE_PROCESS_ENTER);

  bottomSizer->Add(m_inputCtrl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

  m_sendButton = new wxBitmapButton(this,
                                    ID_SendButton,
                                    AEGetArtBundle(wxAEArt::GoForward));
  bottomSizer->Add(m_sendButton, 0, wxALIGN_CENTER_VERTICAL);

  topSizer->Add(bottomSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  SetSizer(topSizer);

  // Telemetry parser (always-on) + sinks
  m_telemetryWatch.Start();
  m_valuesSink = std::make_unique<ArduinoValuesViewSink>(m_valuesView);
  m_telemetrySink = std::make_unique<ArduinoFanoutSink>(m_valuesSink.get(), nullptr);
  m_plotParser = std::make_unique<ArduinoPlotParser>(m_telemetrySink.get());

  topSizer->SetSizeHints(this);

  m_inputCtrl->SetFocus();

  LoadWindowSize(wxT("SerialMonitorFrame"), this, m_config);
}

void ArduinoSerialMonitorFrame::StartWorker() {
  StopWorker();

  m_worker = new SerialMonitorWorker(this, m_portName, m_baudRate);
  if (m_worker->Run() != wxTHREAD_NO_ERROR) {
    wxLogWarning(_("Failed to start serial monitor thread"));
    delete m_worker;
    m_worker = nullptr;
  }
}

void ArduinoSerialMonitorFrame::StopWorker() {
  if (m_worker) {
    m_worker->RequestStop();
    m_worker->InterruptIo();

    // wait until the thread terminates
    if (m_worker->IsRunning()) {
      m_worker->Wait(); // blocks until Entry() finishes
    }

    delete m_worker;
    m_worker = nullptr;
  }
}

void SerialMonitorWorker::InterruptIo() {
#ifndef __WXMSW__
  wxMutexLocker lock(m_mutex);
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
#else
  wxMutexLocker lock(m_mutex);
  HANDLE h = static_cast<HANDLE>(m_handle);
  if (h) {
    CloseHandle(h);
    m_handle = nullptr;
  }
#endif
}

bool SerialMonitorWorker::PulseResetLines(int pulseMs /*= 50*/, bool dtr /*= true*/, bool rts /*= true*/) {
#if defined(__unix__) || defined(__APPLE__)
  int fd = -1;
  {
    wxMutexLocker lock(m_mutex);
    fd = m_fd;
  }
  if (fd < 0)
    return false;

  int status = 0;
  if (ioctl(fd, TIOCMGET, &status) != 0)
    return false;

  auto pulseOne = [&](int bit) -> bool {
    const bool wasSet = (status & bit) != 0;
    int b = bit;

    // Toggle away from the current state, wait, then restore.
    if (wasSet) {
      if (ioctl(fd, TIOCMBIC, &b) != 0)
        return false; // clear
      ::usleep(pulseMs * 1000);
      if (ioctl(fd, TIOCMBIS, &b) != 0)
        return false; // set back
    } else {
      if (ioctl(fd, TIOCMBIS, &b) != 0)
        return false; // set
      ::usleep(pulseMs * 1000);
      if (ioctl(fd, TIOCMBIC, &b) != 0)
        return false; // clear back
    }
    return true;
  };

  bool ok = true;
  if (dtr)
    ok = ok && pulseOne(TIOCM_DTR);
  if (rts)
    ok = ok && pulseOne(TIOCM_RTS);
  return ok;
#else
  HANDLE h = nullptr;
  {
    wxMutexLocker lock(m_mutex);
    h = static_cast<HANDLE>(m_handle);
  }
  if (!h)
    return false;

  // On Windows, it's typically enough to do a short "drop" and return.
  if (dtr)
    EscapeCommFunction(h, CLRDTR);
  if (rts)
    EscapeCommFunction(h, CLRRTS);
  ::Sleep((DWORD)pulseMs);
  if (dtr)
    EscapeCommFunction(h, SETDTR);
  if (rts)
    EscapeCommFunction(h, SETRTS);
  return true;
#endif
}

void ArduinoSerialMonitorFrame::Close() {
  StopWorker();

  SaveWindowSize(wxT("SerialMonitorFrame"), this, m_config);

  if (m_sketchConfig) {
    if (m_plotView) {
      m_sketchConfig->Write(wxT("SerialPlotDuration"), m_plotView->GetTimeWindowMs());
      m_sketchConfig->Write(wxT("SerialPlotHiddenSignals"), JoinWxStrings(m_plotView->GetHiddenSignals(), wxChar('\t')));
      m_sketchConfig->Write(wxT("SerialFixedYRange"), m_plotView->HasFixedYRange());
      if (m_plotView->HasFixedYRange()) {
        double yMin, yMax;
        m_plotView->GetFixedYRange(&yMin, &yMax);
        m_sketchConfig->Write(wxT("SerialFixedYRangeMin"), yMin);
        m_sketchConfig->Write(wxT("SerialFixedYRangeMax"), yMax);
      }
    }
    m_sketchConfig->Write(wxT("SerialShowTimestamps"), m_timestamps);
    m_sketchConfig->Flush();
  }

  ArduinoEditorFrame *f = wxDynamicCast(GetParent(), ArduinoEditorFrame);
  if (f) {
    f->OnCloseSerialMonitor();
  }

  Destroy();
}

void ArduinoSerialMonitorFrame::OnClose(wxCloseEvent &WXUNUSED(event)) {
  Close();
}

void ArduinoSerialMonitorFrame::OnSend(wxCommandEvent &) {
  if (!m_worker) {
    return;
  }

  wxString text = m_inputCtrl->GetValue();
  if (text.IsEmpty()) {
    return;
  }

  std::string data = wxToStd(text);

  std::string eol;
  switch (m_lineEndingMode) {
    case LineEndingMode::None:
      eol = "";
      break;
    case LineEndingMode::LF:
      eol = "\n";
      break;
    case LineEndingMode::CR:
      eol = "\r";
      break;
    case LineEndingMode::CRLF:
      eol = "\r\n";
      break;
  }

  data += eol;

  if (!m_worker->Write(data)) {
    wxLogWarning(_("Failed to write to serial port"));
  } else {
    m_inputCtrl->Clear();
  }
}

void ArduinoSerialMonitorFrame::OnLineEndingChanged(wxCommandEvent &WXUNUSED(event)) {
  wxString sel = m_lineEndCombo->GetStringSelection();

  if (sel == _("None")) {
    m_lineEndingMode = LineEndingMode::None;
  } else if (sel == _("<LF>")) {
    m_lineEndingMode = LineEndingMode::LF;
  } else if (sel == _("<CR>")) {
    m_lineEndingMode = LineEndingMode::CR;
  } else if (sel == _("<CR><LF>")) {
    m_lineEndingMode = LineEndingMode::CRLF;
  } else {
    m_lineEndingMode = LineEndingMode::LF;
  }
}

void ArduinoSerialMonitorFrame::OnPause(wxCommandEvent &WXUNUSED(event)) {
  m_paused = !m_paused;

  if (m_paused) {
    if (m_pauseButton) {
      m_pauseButton->SetLabel(_("Resume"));
      m_pauseButton->SetBackgroundColour(wxColour(255, 200, 200));
      m_pauseButton->Refresh();
    }
    return;
  }

  // --- resume ---
  if (m_pauseButton) {
    m_pauseButton->SetLabel(_("Pause"));
    m_pauseButton->SetBackgroundColour(wxNullColour);
    m_pauseButton->Refresh();
  }

  // 1) Flush text in one shot
  bool didAppendText = false;
  if (m_outputCtrl && !m_pausedBuffer.empty()) {
    wxString all;
    for (const auto &s : m_pausedBuffer) {
      all += s;
    }

    QueueTextAppend(all);
    m_pausedBuffer.clear();
    m_pausedTextChars = 0;
    didAppendText = true;
  }

  // 2) Flush plot batch (independent of text)
  if (m_plotStarted && m_plotParser && m_plotView && !m_pausedPlotBuffer.empty()) {
    wxWindowUpdateLocker lock(m_plotView);
    m_plotParser->ApplyBatch(m_pausedPlotBuffer);
    m_pausedPlotBuffer.clear();
  }

  // 3) Autoscroll
  if (didAppendText && m_autoScroll) {
    ScrollOutputToEnd();
  }
}

void ArduinoSerialMonitorFrame::OnReset(wxCommandEvent &) {
  if (!m_worker) {
    return;
  }

  if (!m_worker->PulseResetLines(50, true, true)) {
    // Fallback: the crudest but functional option is "reconnect",
    // because board resets open ports as often.
    StopWorker();
    StartWorker();
  }

  ClearLog();
  ClearChart();
}

void ArduinoSerialMonitorFrame::ClearLog() {
  if (m_outputCtrl) {
    m_outputCtrl->SetReadOnly(false);
    m_outputCtrl->ClearAll();
    m_outputCtrl->SetReadOnly(true);
  }
  if (m_textFlushTimer.IsRunning()) {
    m_textFlushTimer.Stop();
  }
  m_textFlushScheduled = false;
  m_textPending.clear();
  m_pendingCR = false;
  m_pausedBuffer.clear();
  m_tsAtLineStart = true;
  m_tsPending = false;
  m_tsLastPrintedSec = (time_t)-1;
  m_tsPendingSec = (time_t)-1;

  if (m_valuesView) {
    m_valuesView->Clear();
  }
}

void ArduinoSerialMonitorFrame::ClearChart() {
  if (m_plotView) {
    m_plotView->Clear();
    AlignPlotTimeBase();
  }
  if (m_plotParser) {
    m_plotParser->Reset();
  }
  m_plotLineBuf.clear();
  m_pausedPlotBuffer.clear();
}

void ArduinoSerialMonitorFrame::OnClear(wxCommandEvent &WXUNUSED(event)) {
  switch (m_notebook->GetSelection()) {
    case 0:
      ClearLog();
      break;
    case 1:
      ClearChart();
      break;
    default:
      break;
  }
}

void ArduinoSerialMonitorFrame::OnInputEnter(wxCommandEvent &event) {
  OnSend(event);
}

void ArduinoSerialMonitorFrame::OnBaudChanged(wxCommandEvent &WXUNUSED(event)) {
  wxString sel = m_baudCombo->GetStringSelection();
  long baud = 0;
  if (!sel.ToLong(&baud)) {
    return;
  }

  m_baudRate = baud;

  if (m_sketchConfig) {
    m_sketchConfig->Write(wxT("SerialMonitorBaud"), m_baudRate);
    m_sketchConfig->Flush();
  }

  // simplest: restart the worker -> reconnect with a new baud rate
  StartWorker();
}

void ArduinoSerialMonitorFrame::OnOutputFormatChanged(wxCommandEvent &) {
  if (!m_outputFormatChoice)
    return;

  int sel = m_outputFormatChoice->GetSelection();
  if (sel < 0)
    sel = 0;
  if (sel > 2)
    sel = 2;

  m_outputFormat = (SerialOutputFormat)sel;

  // reset hex "column"
  m_hexCol = 0;
  m_hexAsciiBuf.clear();

  if (m_sketchConfig) {
    m_sketchConfig->Write(wxT("SerialOutputFormat"), (long)sel);
    m_sketchConfig->Flush();
  }
}

void ArduinoSerialMonitorFrame::OnNotebookPageChanged(wxBookCtrlEvent &event) {
  if (m_notebook && m_plotPage) {
    int sel = event.GetSelection();
    if (sel != wxNOT_FOUND && m_notebook->GetPage((size_t)sel) == m_plotPage) {
      EnsurePlotterStarted();
    }
  }
  event.Skip();
}

void ArduinoSerialMonitorFrame::EnsurePlotterStarted() {
  if (m_plotStarted) {
    return;
  }
  m_plotStarted = true;

  if (!m_plotPage) {
    return;
  }

  // Replace placeholder content with the actual plot view.
  m_plotPage->Freeze();

  m_plotPage->DestroyChildren();

  auto *sizer = new wxBoxSizer(wxVERTICAL);
  m_plotView = new ArduinoPlotView(m_plotPage);
  sizer->Add(m_plotView, 1, wxEXPAND);

  wxString hs;
  if (m_sketchConfig->Read(wxT("SerialPlotHiddenSignals"), &hs, wxEmptyString)) {
    m_plotView->SetHiddenSignals(SplitWxString(hs, wxChar('\t')));
  }
  double pd;
  if (m_sketchConfig->Read(wxT("SerialPlotDuration"), &pd, 10000.0)) {
    m_plotView->SetTimeWindowMs(pd);
  }

  bool fyr;
  if (m_sketchConfig->Read(wxT("SerialFixedYRange"), &fyr, false)) {
    if (fyr) {
      double yMin, yMax;
      if (m_sketchConfig->Read(wxT("SerialFixedYRangeMin"), &yMin, 0.0) && m_sketchConfig->Read(wxT("SerialFixedYRangeMax"), &yMax, 1023.0)) {
        m_plotView->SetFixedYRange(fyr, yMin, yMax);
      }
    }
  }

  m_plotPage->SetSizer(sizer);
  m_plotPage->Layout();

  if (m_notebook) {
    m_notebook->Layout();
  }

  m_plotSink = std::make_unique<ArduinoPlotViewSink>(m_plotView);
  AlignPlotTimeBase();

  // Attach plot sink to the always-on parser (fanout values + plot).
  m_telemetrySink = std::make_unique<ArduinoFanoutSink>(m_valuesSink.get(), m_plotSink.get());
  if (m_plotParser) {
    m_plotParser->SetSink(m_telemetrySink.get());
  }

  m_plotPage->Thaw();
}

void ArduinoSerialMonitorFrame::AlignPlotTimeBase() {
  if (!m_plotView || !m_plotSink)
    return;

  auto *vsink = dynamic_cast<ArduinoPlotViewSink *>(m_plotSink.get());
  if (!vsink)
    return;

  const double nowMonMs = (double)m_telemetryWatch.Time();
  const double nowViewMs = (double)m_plotView->GetCurrentTime();
  vsink->SetTimeOffset(nowViewMs - nowMonMs);
}

void ArduinoSerialMonitorFrame::FeedPlotChunkUtf8(const std::string &chunkUtf8, double time) {
  if (!m_plotParser) {
    return;
  }

  // Hard cap to avoid pathological growth on missing newlines.
  if (chunkUtf8.size() > 16 * 1024) {
    // If someone spams a megabyte without newlines, just drop it.
    return;
  }

  m_plotLineBuf += chunkUtf8;

  // Consume complete lines
  size_t pos = 0;
  while ((pos = m_plotLineBuf.find('\n')) != std::string::npos) {
    std::string line = m_plotLineBuf.substr(0, pos);
    // Normalize any stray CR (shouldn't happen after OnData normalization, but harmless)
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    m_plotParser->ApplyLine(line, time);
    m_plotLineBuf.erase(0, pos + 1);
  }

  // Keep tail bounded (in case line never ends)
  if (m_plotLineBuf.size() > kMaxPausedPlotLines) {
    m_plotLineBuf.erase(0, m_plotLineBuf.size() - kMaxPausedPlotLines);
  }
}

void ArduinoSerialMonitorFrame::BufferPlotChunkUtf8(const std::string &chunkUtf8, double time) {
  if (!m_plotParser)
    return;

  m_plotLineBuf += chunkUtf8;

  size_t pos = 0;
  while ((pos = m_plotLineBuf.find('\n')) != std::string::npos) {
    std::string line = m_plotLineBuf.substr(0, pos);

    // Normalize any stray CR
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    // Push ONLY complete lines
    BufferedPlotLine bpl;
    bpl.line = std::move(line);
    bpl.time = time;
    m_pausedPlotBuffer.push_back(std::move(bpl));

    m_plotLineBuf.erase(0, pos + 1);
  }

  // Keep tail bounded (same as FeedPlotChunkUtf8)
  if (m_plotLineBuf.size() > kMaxPausedPlotLines) {
    m_plotLineBuf.erase(0, m_plotLineBuf.size() - kMaxPausedPlotLines);
  }
}

void ArduinoSerialMonitorFrame::NormalizeLineEndings(wxString &chunk) {
  wxString out;
  out.reserve(chunk.length() + 1);

  size_t i = 0;

  if (m_pendingCR) {
    m_pendingCR = false;
    if (i < chunk.length() && chunk[i] == '\n') {
      i++;
    }
    out += '\n';
  }

  for (; i < chunk.length(); ++i) {
    wxUniChar c = chunk[i];

    if (c == '\r') {
      if (i + 1 < chunk.length()) {
        if (chunk[i + 1] == '\n') {
          out += '\n';
          i++; // skip '\n'
        } else {
          out += '\n';
        }
      } else {
        m_pendingCR = true;
      }
      continue;
    }

    out += c;
  }

  chunk.swap(out);
}

void ArduinoSerialMonitorFrame::OnTextFlushTimer(wxTimerEvent &) {
  m_textFlushScheduled = false;
  FlushPendingText(false);
}

static int TrimStyledTextCtrl(wxStyledTextCtrl *stc, int maxLines, int maxChars, bool keepAtEnd) {
  if (!stc)
    return 0;

  const int firstVisibleBefore = stc->GetFirstVisibleLine();

  bool wasRO = stc->GetReadOnly();
  if (wasRO)
    stc->SetReadOnly(false);

  int removedLines = 0;

  // --- 1) limit rows ---
  const int lineCount = stc->GetLineCount();
  if (maxLines > 0 && lineCount > maxLines) {
    removedLines = lineCount - maxLines;
    const int cutPos = stc->PositionFromLine(removedLines);
    stc->SetTargetStart(0);
    stc->SetTargetEnd(cutPos);
    stc->ReplaceTarget(wxEmptyString);
  }

  // --- 2) limit bytes ---
  const int len = stc->GetTextLength();
  if (maxChars > 0 && len > maxChars) {
    const int start = len - maxChars;
    const int cutLine = stc->LineFromPosition(start);
    const int cutPos = stc->PositionFromLine(cutLine);
    stc->SetTargetStart(0);
    stc->SetTargetEnd(cutPos);
    stc->ReplaceTarget(wxEmptyString);

    removedLines = wxMax(removedLines, cutLine);
  }

  if (wasRO)
    stc->SetReadOnly(true);

  if (keepAtEnd) {
    stc->GotoPos(stc->GetTextLength());
    stc->EnsureCaretVisible();
  } else if (removedLines > 0) {
    const int newFirst = wxMax(0, firstVisibleBefore - removedLines);
    stc->ScrollToLine(newFirst);
  }

  return removedLines;
}

void ArduinoSerialMonitorFrame::QueueTextAppend(const wxString &s) {
  if (!m_outputCtrl || s.empty())
    return;

  m_textPending += s;

  if (!m_textFlushScheduled) {
    m_textFlushScheduled = true;
    m_textFlushTimer.StartOnce(250);
  }

  if (m_textPending.length() > 256 * 1024) {
    FlushPendingText(true);
  }
}

void ArduinoSerialMonitorFrame::FlushPendingText(bool force) {
  if (!m_outputCtrl || m_textPending.empty())
    return;

  if (force) {
    if (m_textFlushTimer.IsRunning())
      m_textFlushTimer.Stop();
    m_textFlushScheduled = false;
  }

  const bool autoscroll = m_autoScroll;

  // Save view/caret state when autoscroll is OFF.
  int firstLine = 0;
  int xOffset = 0;
  int curPos = 0;
  int anchorPos = 0;
  int selStart = 0;
  int selEnd = 0;
  bool hadSel = false;

  if (!autoscroll) {
    firstLine = m_outputCtrl->GetFirstVisibleLine();
    xOffset = m_outputCtrl->GetXOffset();
    curPos = m_outputCtrl->GetCurrentPos();
    anchorPos = m_outputCtrl->GetAnchor();
    selStart = m_outputCtrl->GetSelectionStart();
    selEnd = m_outputCtrl->GetSelectionEnd();
    hadSel = (selStart != selEnd);
  }

  wxString toAppend;
  toAppend.swap(m_textPending);

  wxWindowUpdateLocker lock(m_outputCtrl);

  m_outputCtrl->SetReadOnly(false);
  m_programmaticScroll = true;

  // Insert at the end without touching the user's caret/viewport.
  const int insertPos = m_outputCtrl->GetTextLength();
  m_outputCtrl->InsertText(insertPos, toAppend);

  m_programmaticScroll = false;
  m_outputCtrl->SetReadOnly(true);

  if (autoscroll) {
    ScrollOutputToEnd();

    m_programmaticScroll = true;
    TrimStyledTextCtrl(m_outputCtrl, kMaxOutputLines, kMaxOutputChars, autoscroll /*keepAtEnd*/);
    m_programmaticScroll = false;
    return;
  }

  // ---- no autoscroll path ----
  // Restore caret/selection.
  m_outputCtrl->SetCurrentPos(curPos);
  m_outputCtrl->SetAnchor(anchorPos);
  if (hadSel) {
    m_outputCtrl->SetSelection(selStart, selEnd);
  } else {
    m_outputCtrl->SetSelection(curPos, curPos);
  }

  // Restore viewport (line & horizontal offset).
  const int newFirstLine = m_outputCtrl->GetFirstVisibleLine();
  if (newFirstLine != firstLine) {
    m_programmaticScroll = true;
    m_outputCtrl->LineScroll(0, firstLine - newFirstLine);
    m_programmaticScroll = false;
  }

  if (m_outputCtrl->GetXOffset() != xOffset) {
    m_outputCtrl->SetXOffset(xOffset);
  }

  // --- Safety trim (avoid unbounded growth) ---
  m_programmaticScroll = true;
  TrimStyledTextCtrl(m_outputCtrl, kMaxOutputLines, kMaxOutputChars, autoscroll /*keepAtEnd*/);
  m_programmaticScroll = false;
}

bool ArduinoSerialMonitorFrame::IsOutputAtBottom() const {
  if (!m_outputCtrl)
    return true;

  const int lastLine = m_outputCtrl->GetLineCount() - 1;
  if (lastLine < 0)
    return true;

  const int first = m_outputCtrl->GetFirstVisibleLine();
  const int onScreen = m_outputCtrl->LinesOnScreen();
  const int bottomVisible = first + onScreen;

  return bottomVisible >= lastLine;
}

void ArduinoSerialMonitorFrame::OnOutputUpdateUI(wxStyledTextEvent &event) {
  event.Skip();

  if (!m_outputCtrl)
    return;

  if ((event.GetUpdated() & wxSTC_UPDATE_V_SCROLL) == 0)
    return;

  if (m_programmaticScroll)
    return;

  const int first = m_outputCtrl->GetFirstVisibleLine();
  if (m_lastFirstVisibleLine < 0) {
    m_lastFirstVisibleLine = first;
    return;
  }

  const bool moved = (first != m_lastFirstVisibleLine);
  m_lastFirstVisibleLine = first;

  if (!moved) {
    return;
  }

  const bool atBottom = IsOutputAtBottom();
  const bool autoOn = m_autoScroll;

  if (autoOn && !atBottom) {
    m_autoScroll = false;
  } else if (!autoOn && atBottom) {
    m_autoScroll = true;
  }
}

void ArduinoSerialMonitorFrame::ScrollOutputToEnd() {
  m_programmaticScroll = true;

  const int endPos = m_outputCtrl->GetTextLength();
  m_outputCtrl->SetCurrentPos(endPos);
  m_outputCtrl->SetAnchor(endPos);

  // This keeps the view at the end without forcing horizontal repositioning.
  const int endLine = m_outputCtrl->LineFromPosition(endPos);
  m_outputCtrl->ScrollToLine(endLine);
  m_outputCtrl->EnsureCaretVisible();

  m_programmaticScroll = false;
}

void ArduinoSerialMonitorFrame::OnData(wxThreadEvent &event) {
  const SerialChunkPayload p = event.GetPayload<SerialChunkPayload>();

  m_hexWrapBytes = 16;

  // 0=Text, 1=Hex, 2=Hex+text
  int fmt = 0;
  if (m_outputFormatChoice) {
    fmt = m_outputFormatChoice->GetSelection();
    if (fmt < 0)
      fmt = 0;
    if (fmt > 2)
      fmt = 2;
  }

  wxString textToAppend;

  // ---------- TEXT MODE ----------
  if (fmt == 0) {
    // Decode UTF-8 "best effort". If it collapses, fallback to Latin-1 so nothing disappears.
    wxString chunk(p.bytes.data(), wxConvUTF8, (int)p.bytes.size());
    if (chunk.empty() && !p.bytes.empty()) {
      chunk = BytesToLatin1(p.bytes);
    }

    NormalizeLineEndings(chunk);

    // plot chunks only make sense in text mode
    const std::string plotChunkUtf8 = wxToStd(chunk);

    // timestamps: same behavior as before (only at line start)
    if (m_timestamps) {
      if (m_tsLastPrintedSec != p.sec) {
        m_tsPending = true;
        m_tsPendingSec = p.sec;
      }
    } else {
      m_tsPending = false;
    }

    auto flushTimestampIfNeeded = [&]() {
      if (!m_timestamps)
        return;
      if (!m_tsPending)
        return;
      if (!m_tsAtLineStart)
        return;

      wxDateTime t((time_t)m_tsPendingSec);
      textToAppend += t.Format(wxT("[%H:%M:%S]"));
      textToAppend += wxT("\n");

      m_tsLastPrintedSec = m_tsPendingSec;
      m_tsPending = false;
      m_tsAtLineStart = true;
    };

    flushTimestampIfNeeded();

    for (size_t i = 0; i < chunk.length(); ++i) {
      wxUniChar c = chunk[i];
      textToAppend += c;

      if (c == '\n') {
        m_tsAtLineStart = true;
        flushTimestampIfNeeded();
      } else {
        m_tsAtLineStart = false;
      }
    }

    // paused buffering / output / plot (text mode)
    if (m_paused) {
      if (textToAppend.length() > kMaxPausedTextChars) {
        textToAppend = textToAppend.Right(kMaxPausedTextChars);
      }

      const size_t add = (size_t)textToAppend.length();
      if (m_pausedTextChars + add > kMaxPausedTextChars) {
        const size_t need = (m_pausedTextChars + add) - kMaxPausedTextChars;

        size_t freed = 0;
        size_t count = 0;
        while (count < m_pausedBuffer.size() && freed < need) {
          freed += (size_t)m_pausedBuffer[count].length();
          ++count;
        }
        if (count > 0) {
          m_pausedBuffer.erase(m_pausedBuffer.begin(),
                               m_pausedBuffer.begin() + (ptrdiff_t)count);
          m_pausedTextChars -= freed;
        }
      }

      m_pausedBuffer.push_back(textToAppend);
      m_pausedTextChars += add;

      if (m_plotParser) {
        const double t_ms = (double)m_telemetryWatch.Time();
        BufferPlotChunkUtf8(plotChunkUtf8, t_ms);
      }
      return;
    }

    if (m_outputCtrl) {
      QueueTextAppend(textToAppend);
    }

    if (m_plotParser) {
      const double t_ms = (double)m_telemetryWatch.Time();
      FeedPlotChunkUtf8(plotChunkUtf8, t_ms);
    }
    return;
  }

  // ---------- HEX / HEX+LATIN1 MODE ----------
  int wrap = (m_hexWrapBytes > 0) ? m_hexWrapBytes : 16;
  if (wrap < 4)
    wrap = 16; // bezpečnostní pás
  if (wrap > 256)
    wrap = 256;

  // Timestamp v hex režimech: jen když jsme na začátku řádku a změnila se sekunda
  // (tzn. nebude ti to skákat doprostřed řádku)
  if (m_timestamps && m_hexCol == 0 && m_tsLastPrintedSec != p.sec) {
    wxDateTime t((time_t)p.sec);
    textToAppend += t.Format(wxT("[%H:%M:%S]"));
    textToAppend += wxT("\n");
    m_tsLastPrintedSec = p.sec;
  }
  m_tsPending = false;
  m_tsAtLineStart = true;

  auto flushHexLine = [&]() {
    if (m_hexCol == 0)
      return;

    if (fmt == 2) {
      // dorovnat hex část, aby ASCII sloupec seděl
      const int missing = wrap - m_hexCol;
      if (missing > 0) {
        textToAppend += wxString(' ', missing * 3);
      }
      textToAppend += wxT(" |");
      textToAppend += m_hexAsciiBuf;
      textToAppend += wxT("|");
      m_hexAsciiBuf.clear();
    }

    textToAppend += wxT("\n");
    m_hexCol = 0;
  };

  for (unsigned char b : p.bytes) {
    // zalomení po wrap bajtech
    if (m_hexCol == wrap) {
      flushHexLine();
    }

    textToAppend += wxString::Format(wxT("%02X "), (unsigned)b);

    if (fmt == 2) {
      // Latin-1 znak do pravého sloupce: ASCII + 0xA0..0xFF, jinak '.'
      wxChar ch = (IsLatin1Printable(b) ? (wxChar)b : (wxChar)'.');
      m_hexAsciiBuf += ch;
    }

    m_hexCol++;
  }

  // paused buffering / output (no plot in hex modes)
  if (m_paused) {
    if (textToAppend.length() > kMaxPausedTextChars) {
      textToAppend = textToAppend.Right(kMaxPausedTextChars);
    }

    const size_t add = (size_t)textToAppend.length();
    if (m_pausedTextChars + add > kMaxPausedTextChars) {
      const size_t need = (m_pausedTextChars + add) - kMaxPausedTextChars;

      size_t freed = 0;
      size_t count = 0;
      while (count < m_pausedBuffer.size() && freed < need) {
        freed += (size_t)m_pausedBuffer[count].length();
        ++count;
      }
      if (count > 0) {
        m_pausedBuffer.erase(m_pausedBuffer.begin(),
                             m_pausedBuffer.begin() + (ptrdiff_t)count);
        m_pausedTextChars -= freed;
      }
    }

    m_pausedBuffer.push_back(textToAppend);
    m_pausedTextChars += add;
    return;
  }

  if (m_outputCtrl) {
    QueueTextAppend(textToAppend);
  }
}

void ArduinoSerialMonitorFrame::OnError(wxThreadEvent &event) {
  wxString msg = event.GetString();
  wxLogWarning(_("Serial monitor error: %s"), msg);
  QueueTextAppend(wxString::Format(_("\n[ERROR] %s\n"), msg));
}

void ArduinoSerialMonitorFrame::Block() {
  if (m_isBlocked) {
    return;
  }

  ScopeTimer t("SERMON: Block()");

  m_isBlocked = true;

  m_paused = false;
  if (m_pauseButton) {
    m_pauseButton->SetLabel(_("Pause"));
    m_pauseButton->SetBackgroundColour(wxNullColour);
    m_pauseButton->Refresh();
  }
  m_pausedBuffer.clear();
  m_pausedTextChars = 0;

  // disconnect worker -> release port
  StopWorker();

  // we gray out input/output and the send button
  if (m_inputCtrl)
    m_inputCtrl->Enable(false);
  if (m_notebook)
    m_notebook->Enable(false);
  if (m_sendButton)
    m_sendButton->Enable(false);

  if (m_outputCtrl) {
    QueueTextAppend(_("\n[INFO] Serial monitor blocked - port released.\n"));
    if (m_autoScroll) {
      ScrollOutputToEnd();
    }
  }
}

void ArduinoSerialMonitorFrame::Unblock() {
  if (!m_isBlocked) {
    return;
  }

  ScopeTimer t("SERMON: Unblock()");

  m_isBlocked = false;

  // reconnect to the port
  StartWorker();

  // re-enable input/output and send button
  if (m_inputCtrl)
    m_inputCtrl->Enable(true);
  if (m_notebook)
    m_notebook->Enable(true);
  if (m_sendButton)
    m_sendButton->Enable(true);

  if (m_outputCtrl) {
    QueueTextAppend(_("\n[INFO] Serial monitor unblocked - port re-opened.\n"));
    if (m_autoScroll) {
      ScrollOutputToEnd();
    }
  }
}

void ArduinoSerialMonitorFrame::OnSysColourChanged(wxSysColourChangedEvent &event) {
  m_sendButton->SetBitmap(AEGetArtBundle(wxAEArt::GoForward));

  EditorSettings settings;
  settings.Load(m_config);
  SetupOutputCtrl(settings);

  Layout();
  event.Skip();
}
