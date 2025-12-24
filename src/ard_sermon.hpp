#pragma once

#include <wx/config.h>
#include <wx/wx.h>

class SerialMonitorWorker : public wxThread {
public:
  SerialMonitorWorker(wxEvtHandler *handler,
                      const wxString &port,
                      long baud);
  ~SerialMonitorWorker() override;

  void RequestStop();

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
                            const wxString &portName,
                            long baudRate);
  ~ArduinoSerialMonitorFrame() override;

  void Block();
  void Unblock();
  bool IsBlocked() const { return m_isBlocked; }
  void Close();

private:
  void CreateControls();
  void StartWorker();
  void StopWorker();

  void OnClose(wxCloseEvent &event);
  void OnSend(wxCommandEvent &event);
  void OnInputEnter(wxCommandEvent &event);
  void OnBaudChanged(wxCommandEvent &event);
  void OnData(wxThreadEvent &event);
  void OnError(wxThreadEvent &event);

  void OnLineEndingChanged(wxCommandEvent &event);
  void OnClear(wxCommandEvent &event);
  void OnPause(wxCommandEvent &event);

  wxConfigBase *m_config{nullptr};
  wxString m_portName;
  long m_baudRate{115200};

  wxTextCtrl *m_outputCtrl{nullptr};
  wxTextCtrl *m_inputCtrl{nullptr};
  wxComboBox *m_baudCombo{nullptr};
  wxComboBox *m_lineEndCombo{nullptr};
  wxBitmapButton *m_sendButton{nullptr};
  wxCheckBox *m_autoscrollCheck{nullptr};
  wxCheckBox *m_timestampCheck{nullptr};
  wxButton *m_pauseButton = nullptr;
  wxButton *m_clearButton{nullptr};

  SerialMonitorWorker *m_worker{nullptr};

  LineEndingMode m_lineEndingMode{LineEndingMode::LF};

  std::vector<wxString> m_pausedBuffer;
  bool m_isBlocked = false;
  bool m_paused = false;
};
