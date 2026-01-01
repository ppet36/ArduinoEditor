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

#include <wx/artprov.h>
#include <wx/bmpbndl.h>
#include <wx/settings.h>

// Arduino Editor private art IDs (no overlap with wxART_*).
namespace wxAEArt {
inline const wxArtID Refresh = wxASCII_STR("wxAE_ART_REFRESH");
inline const wxArtID Delete = wxASCII_STR("wxAE_ART_DELETE");
inline const wxArtID New = wxASCII_STR("wxAE_ART_NEW");
inline const wxArtID FileOpen = wxASCII_STR("wxAE_ART_FILE_OPEN");
inline const wxArtID FileSave = wxASCII_STR("wxAE_ART_FILE_SAVE");
inline const wxArtID FileSaveAs = wxASCII_STR("wxAE_ART_FILE_SAVE_AS");
inline const wxArtID Quit = wxASCII_STR("wxAE_ART_QUIT");
inline const wxArtID GoBack = wxASCII_STR("wxAE_ART_GO_BACK");
inline const wxArtID GoForward = wxASCII_STR("wxAE_ART_GO_FORWARD");
inline const wxArtID Find = wxASCII_STR("wxAE_ART_FIND");
inline const wxArtID FindReplace = wxASCII_STR("wxAE_ART_FIND_AND_REPLACE");
inline const wxArtID FindAll = wxASCII_STR("wxAE_ART_FIND_ALL");
inline const wxArtID GoToDef = wxASCII_STR("wxAE_ART_GOTO_DEF");
inline const wxArtID Undo = wxASCII_STR("wxAE_ART_UNDO");
inline const wxArtID Redo = wxASCII_STR("wxAE_ART_REDO");
inline const wxArtID Cut = wxASCII_STR("wxAE_ART_CUT");
inline const wxArtID Copy = wxASCII_STR("wxAE_ART_COPY");
inline const wxArtID Paste = wxASCII_STR("wxAE_ART_PASTE");
inline const wxArtID Print = wxASCII_STR("wxAE_ART_PRINT");

inline const wxArtID Folder = wxASCII_STR("wxAE_ART_FOLDER");
inline const wxArtID FolderOpen = wxASCII_STR("wxAE_ART_FOLDER_OPEN");
inline const wxArtID NormalFile = wxASCII_STR("wxAE_ART_NORMAL_FILE");
inline const wxArtID ExecutableFile = wxASCII_STR("wxAE_ART_EXECUTABLE_FILE");

inline const wxArtID SysLibrary = wxASCII_STR("wxAE_ART_SYSLIBRARY");
inline const wxArtID UserLibrary = wxASCII_STR("wxAE_ART_USRLIBRARY");

inline const wxArtID GoUp = wxASCII_STR("wxAE_ART_GO_UP");
inline const wxArtID GoDown = wxASCII_STR("wxAE_ART_GO_DOWN");
inline const wxArtID GoToParent = wxASCII_STR("wxAE_ART_GO_TO_PARENT");
inline const wxArtID Plus = wxASCII_STR("wxAE_ART_PLUS");
inline const wxArtID Minus = wxASCII_STR("wxAE_ART_MINUS");
inline const wxArtID Edit = wxASCII_STR("wxAE_ART_EDIT");
inline const wxArtID ListView = wxASCII_STR("wxAE_ART_LIST_VIEW");
inline const wxArtID ReportView = wxASCII_STR("wxAE_ART_REPORT_VIEW");
inline const wxArtID Tip = wxASCII_STR("wxAE_ART_TIP");
inline const wxArtID Information = wxASCII_STR("wxAE_ART_INFORMATION");
inline const wxArtID Question = wxASCII_STR("wxAE_ART_QUESTION");
inline const wxArtID DevBoard = wxASCII_STR("wxAE_ART_DEVBOARD");
inline const wxArtID Play = wxASCII_STR("wxAE_ART_PLAY");
inline const wxArtID Check = wxASCII_STR("wxAE_ART_CHECK");
inline const wxArtID SerMon = wxASCII_STR("wxAE_ART_SERMON");
inline const wxArtID SourceFormat = wxASCII_STR("wxAE_ART_SOURCE_FORMAT");
inline const wxArtID Settings = wxASCII_STR("wxAE_ART_SETTINGS");
inline const wxArtID SelectAll = wxASCII_STR("wxAE_ART_SELECT_ALL");
inline const wxArtID CheckForUpdates = wxASCII_STR("wxAE_CHECK_FOR_UPDATES");
inline const wxArtID Close = wxASCII_STR("wxAE_ART_CLOSE");
inline const wxArtID GotoLine = wxASCII_STR("wxAE_ART_GOTO_LINE");
} // namespace wxAEArt

inline bool IsDarkMode() {
  return wxSystemSettings::GetAppearance().IsDark();
}

inline wxArtClient AEArtClient() {
  return IsDarkMode() ? wxASCII_STR("AE_DARK") : wxASCII_STR("AE_LIGHT");
}

class ArduinoArtProvider {
public:
  wxBitmapBundle CreateBitmapBundle(const wxArtID &id, const wxArtClient &client, const wxSize &size);
};

static ArduinoArtProvider gsArduinoArtProvider;

inline wxBitmapBundle AEGetArtBundle(const wxArtID &artId) {
  return gsArduinoArtProvider.CreateBitmapBundle(artId, AEArtClient(), wxDefaultSize);
}
