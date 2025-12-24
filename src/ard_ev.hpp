#pragma once

#include <wx/event.h>

// Event that clang args are ready after the initialization compilation
wxDECLARE_EVENT(EVT_CLANG_ARGS_READY, wxThreadEvent);

// Event for autocompleting
wxDECLARE_EVENT(EVT_COMPLETION_READY, wxThreadEvent);

// Event for error notification
wxDECLARE_EVENT(EVT_DIAGNOSTICS_UPDATED, wxThreadEvent);

// Event for cmdline
wxDECLARE_EVENT(EVT_COMMANDLINE_OUTPUT_MSG, wxCommandEvent);

// Event for updating libraries
wxDECLARE_EVENT(EVT_LIBRARIES_UPDATED, wxThreadEvent);
wxDECLARE_EVENT(EVT_INSTALLED_LIBRARIES_UPDATED, wxThreadEvent);

// Event for updating CORE
wxDECLARE_EVENT(EVT_CORES_UPDATED, wxThreadEvent);
wxDECLARE_EVENT(EVT_CORES_LOADED, wxThreadEvent);

// Serial monitor
wxDECLARE_EVENT(wxEVT_SERIAL_MONITOR_DATA, wxThreadEvent);
wxDECLARE_EVENT(wxEVT_SERIAL_MONITOR_ERROR, wxThreadEvent);

// Asynchronous event on GetBoardOptions/Programmers
wxDECLARE_EVENT(EVT_BOARD_OPTIONS_READY, wxThreadEvent);
wxDECLARE_EVENT(EVT_PROGRAMMERS_READY, wxThreadEvent);

wxDECLARE_EVENT(EVT_AVAILABLE_BOARDS_UPDATED, wxThreadEvent);

wxDECLARE_EVENT(EVT_RESOLVED_LIBRARIES_READY, wxThreadEvent);

wxDECLARE_EVENT(EVT_SYMBOL_OCCURRENCES_READY, wxThreadEvent);
// Symbol reference searching
wxDECLARE_EVENT(EVT_SYMBOL_USAGES_READY, wxThreadEvent);

// AI
wxDECLARE_EVENT(wxEVT_AI_SIMPLE_CHAT_SUCCESS, wxThreadEvent);
wxDECLARE_EVENT(wxEVT_AI_SIMPLE_CHAT_ERROR, wxThreadEvent);
wxDECLARE_EVENT(wxEVT_AI_SIMPLE_CHAT_PROGRESS, wxThreadEvent);
wxDECLARE_EVENT(wxEVT_AI_SUMMARIZATION_UPDATED, wxThreadEvent);

// Async process stopping
wxDECLARE_EVENT(EVT_STOP_PROCESS, wxThreadEvent);
