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
#include "ard_setdlg.hpp"
#include "utils.hpp"
#include <errno.h>
#include <wx/artprov.h>
#include <wx/datetime.h>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

enum {
  ID_BaudCombo = wxID_HIGHEST + 100,
  ID_SendButton,
  ID_InputCtrl,
  ID_LineEndingCombo,
  ID_PauseButton,
  ID_ClearButton
};

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
  switch (m_baud) {
    case 9600:
      speed = B9600;
      break;
    case 19200:
      speed = B19200;
      break;
    case 38400:
      speed = B38400;
      break;
    case 57600:
      speed = B57600;
      break;
    case 115200:
      speed = B115200;
      break;
    default:
      speed = B115200;
      break;
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
    ssize_t n = ::read(m_fd, buf, BUF_SIZE);
    if (n > 0) {
      wxString chunk(buf, wxConvUTF8, (int)n);
      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_DATA);
      evt->SetString(chunk);
      wxQueueEvent(m_handler, evt);
    } else if (n == 0) {
      // EOF - port closed?
      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_ERROR);
      evt->SetString(_("Serial port closed"));
      wxQueueEvent(m_handler, evt);
      break;
    } else { // n < 0
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // nothing to read - wait a moment and check the stop flag
        wxThread::Sleep(10);
        continue;
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
      wxString chunk(buf, wxConvUTF8, (int)bytesRead);
      auto *evt = new wxThreadEvent(wxEVT_SERIAL_MONITOR_DATA);
      evt->SetString(chunk);
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

ArduinoSerialMonitorFrame::ArduinoSerialMonitorFrame(wxWindow *parent,
                                                     wxConfigBase *config,
                                                     const wxString &portName,
                                                     long baudRate)
    : wxFrame(parent,
              wxID_ANY,
              wxString::Format(_("Serial Monitor [%s]"), portName),
              wxDefaultPosition,
              wxSize(700, 500),
              wxDEFAULT_FRAME_STYLE | wxRESIZE_BORDER),
      m_config(config),
      m_portName(portName),
      m_baudRate(baudRate) {
  CreateControls();

  // ---- Bindings for GUI events ----
  Bind(wxEVT_CLOSE_WINDOW, &ArduinoSerialMonitorFrame::OnClose, this);

  m_sendButton->Bind(wxEVT_BUTTON, &ArduinoSerialMonitorFrame::OnSend, this);
  m_inputCtrl->Bind(wxEVT_TEXT_ENTER, &ArduinoSerialMonitorFrame::OnInputEnter, this);
  m_baudCombo->Bind(wxEVT_COMBOBOX, &ArduinoSerialMonitorFrame::OnBaudChanged, this);
  m_lineEndCombo->Bind(wxEVT_COMBOBOX, &ArduinoSerialMonitorFrame::OnLineEndingChanged, this);
  m_pauseButton->Bind(wxEVT_BUTTON, &ArduinoSerialMonitorFrame::OnPause, this);
  m_clearButton->Bind(wxEVT_BUTTON, &ArduinoSerialMonitorFrame::OnClear, this);

  // ---- Bindings for thread events ----
  Bind(wxEVT_SERIAL_MONITOR_DATA, &ArduinoSerialMonitorFrame::OnData, this);
  Bind(wxEVT_SERIAL_MONITOR_ERROR, &ArduinoSerialMonitorFrame::OnError, this);

  // default LF
  m_lineEndingMode = LineEndingMode::LF;

  StartWorker();
}

ArduinoSerialMonitorFrame::~ArduinoSerialMonitorFrame() {
  StopWorker();
}

void ArduinoSerialMonitorFrame::CreateControls() {
  if (m_config) {
    long savedBaud = 0;
    if (m_config->Read(wxT("SerialMonitorBaud"), &savedBaud) && savedBaud > 0) {
      m_baudRate = savedBaud;
    }
  }

  auto *topSizer = new wxBoxSizer(wxVERTICAL);

  // Top row - port + baud + line ending + autoscroll + timestamps + clear
  auto *headerSizer = new wxBoxSizer(wxHORIZONTAL);
  headerSizer->Add(new wxStaticText(this, wxID_ANY,
                                    wxString::Format(_("Port: %s"), m_portName)),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

  wxArrayString baudChoices;
  baudChoices.Add(wxT("9600"));
  baudChoices.Add(wxT("19200"));
  baudChoices.Add(wxT("38400"));
  baudChoices.Add(wxT("57600"));
  baudChoices.Add(wxT("115200"));
  baudChoices.Add(wxT("230400"));

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

  // Autoscroll + timestamps + Pause + Clear
  m_autoscrollCheck = new wxCheckBox(this, wxID_ANY, _("Autoscroll"));
  m_autoscrollCheck->SetValue(true);

  m_timestampCheck = new wxCheckBox(this, wxID_ANY, _("Show timestamps"));
  m_timestampCheck->SetValue(false);

  headerSizer->AddStretchSpacer(1);
  headerSizer->Add(m_autoscrollCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  headerSizer->Add(m_timestampCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

  // --- Pause / Resume ---
  m_pauseButton = new wxButton(this, ID_PauseButton, _("Pause"));
  headerSizer->Add(m_pauseButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

  m_clearButton = new wxButton(this, ID_ClearButton, _("Clear"));
  headerSizer->Add(m_clearButton, 0, wxALIGN_CENTER_VERTICAL);

  topSizer->Add(headerSizer, 0, wxEXPAND | wxALL, 8);

  // Text output
  m_outputCtrl = new wxTextCtrl(this,
                                wxID_ANY,
                                wxEmptyString,
                                wxDefaultPosition,
                                wxDefaultSize,
                                wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
  topSizer->Add(m_outputCtrl, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  EditorSettings settings;
  settings.Load(m_config);

  m_outputCtrl->SetFont(settings.GetFont());

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
                                    wxArtProvider::GetBitmapBundle(wxAEArt::GoForward, wxASCII_STR(wxART_BUTTON)));
  bottomSizer->Add(m_sendButton, 0, wxALIGN_CENTER_VERTICAL);

  topSizer->Add(bottomSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  SetSizer(topSizer);
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

    // wait until the thread terminates
    if (m_worker->IsRunning()) {
      m_worker->Wait(); // blocks until Entry() finishes
    }

    m_worker = nullptr;
  }
}

void ArduinoSerialMonitorFrame::Close() {
  StopWorker();

  SaveWindowSize(wxT("SerialMonitorFrame"), this, m_config);

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
  // toggle state
  m_paused = !m_paused;

  if (m_paused) {
    // transition to pause
    if (m_pauseButton) {
      m_pauseButton->SetLabel(_("Resume"));
      // soft red
      m_pauseButton->SetBackgroundColour(wxColour(255, 200, 200));
      m_pauseButton->Refresh();
    }
  } else {
    // return from pause -> flush buffer
    if (m_pauseButton) {
      m_pauseButton->SetLabel(_("Pause"));
      // return default
      m_pauseButton->SetBackgroundColour(wxNullColour);
      m_pauseButton->Refresh();
    }

    if (m_outputCtrl && !m_pausedBuffer.empty()) {
      for (const auto &s : m_pausedBuffer) {
        m_outputCtrl->AppendText(s);
      }
      m_pausedBuffer.clear();

      if (m_autoscrollCheck->GetValue()) {
        long lastPos = m_outputCtrl->GetLastPosition();
        m_outputCtrl->ShowPosition(lastPos);
      }
    }
  }
}

void ArduinoSerialMonitorFrame::OnClear(wxCommandEvent &WXUNUSED(event)) {
  if (m_outputCtrl) {
    m_outputCtrl->Clear();
  }
}

void ArduinoSerialMonitorFrame::OnInputEnter(wxCommandEvent &event) {
  (void)event;
  OnSend(event);
}

void ArduinoSerialMonitorFrame::OnBaudChanged(wxCommandEvent &WXUNUSED(event)) {
  wxString sel = m_baudCombo->GetStringSelection();
  long baud = 0;
  if (!sel.ToLong(&baud)) {
    return;
  }

  m_baudRate = baud;

  if (m_config) {
    m_config->Write(wxT("SerialMonitorBaud"), m_baudRate);
    m_config->Flush();
  }

  // simplest: restart the worker -> reconnect with a new baud rate
  StartWorker();
}

void ArduinoSerialMonitorFrame::OnData(wxThreadEvent &event) {
  wxString chunk = event.GetString();

  // --- Normalization of line endings ---
  // 1) CRLF -> LF
  chunk.Replace(wxT("\r\n"), wxT("\n"));
  // 2) the CR itself -> nothing (or "\n", depending on preference)
  chunk.Replace(wxT("\r"), wxEmptyString);

  wxString textToAppend;

  if (m_timestampCheck && m_timestampCheck->GetValue()) {
    wxDateTime now = wxDateTime::Now();
    wxString ts = now.Format(wxT("[%H:%M:%S] "));
    textToAppend += ts;
  }

  textToAppend += chunk;

  // If there is a pause, we just store it in the buffer and do not write anything
  if (m_paused) {
    m_pausedBuffer.push_back(textToAppend);
    return;
  }

  if (m_outputCtrl) {
    m_outputCtrl->AppendText(textToAppend);

    if (m_autoscrollCheck->GetValue()) {
      long lastPos = m_outputCtrl->GetLastPosition();
      m_outputCtrl->ShowPosition(lastPos);
    }
  }
}

void ArduinoSerialMonitorFrame::OnError(wxThreadEvent &event) {
  wxString msg = event.GetString();
  wxLogWarning(_("Serial monitor error: %s"), msg);
  m_outputCtrl->AppendText(wxString::Format(_("\n[ERROR] %s\n"), msg));
}

void ArduinoSerialMonitorFrame::Block() {
  if (m_isBlocked) {
    return;
  }

  m_isBlocked = true;

  m_paused = false;
  if (m_pauseButton) {
    m_pauseButton->SetLabel(_("Pause"));
    m_pauseButton->SetBackgroundColour(wxNullColour);
    m_pauseButton->Refresh();
  }
  m_pausedBuffer.clear();

  // disconnect worker -> release port
  StopWorker();

  // we gray out input/output and the send button
  if (m_inputCtrl)
    m_inputCtrl->Enable(false);
  if (m_outputCtrl)
    m_outputCtrl->Enable(false);
  if (m_sendButton)
    m_sendButton->Enable(false);

  if (m_outputCtrl) {
    m_outputCtrl->AppendText(_("\n[INFO] Serial monitor blocked - port released.\n"));
    long lastPos = m_outputCtrl->GetLastPosition();
    m_outputCtrl->ShowPosition(lastPos);
  }
}

void ArduinoSerialMonitorFrame::Unblock() {
  if (!m_isBlocked) {
    return;
  }

  m_isBlocked = false;

  // reconnect to the port
  StartWorker();

  // re-enable input/output and send button
  if (m_inputCtrl)
    m_inputCtrl->Enable(true);
  if (m_outputCtrl)
    m_outputCtrl->Enable(true);
  if (m_sendButton)
    m_sendButton->Enable(true);

  if (m_outputCtrl) {
    m_outputCtrl->AppendText(_("\n[INFO] Serial monitor unblocked - port re-opened.\n"));
    long lastPos = m_outputCtrl->GetLastPosition();
    m_outputCtrl->ShowPosition(lastPos);
  }
}
