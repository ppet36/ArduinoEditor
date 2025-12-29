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

#include "ard_ap.hpp"
#include "utils.hpp"

#include "material_xpm.h" // generated static const char* const ...

wxBitmapBundle ArduinoArtProvider::CreateBitmapBundle(const wxArtID &id, const wxArtClient &WXUNUSED(client), const wxSize &WXUNUSED(size)) {
  const bool dark = IsDarkMode();

  // 16/32 bundle (HiDPI)
  const char *const *x16 = nullptr;
  const char *const *x20 = nullptr;
  const char *const *x24 = nullptr;
  const char *const *x32 = nullptr;

  APP_DEBUG_LOG("ARTPROV: getting bitmap %s for %s mode", wxToStd(id).c_str(), dark ? "dark" : "light");

  if (id == wxAEArt::Refresh) {
    x16 = dark ? mdi_sync_dark_16 : mdi_sync_light_16;
    x20 = dark ? mdi_sync_dark_20 : mdi_sync_light_20;
    x24 = dark ? mdi_sync_dark_24 : mdi_sync_light_24;
    x32 = dark ? mdi_sync_dark_32 : mdi_sync_light_32;
  } else if (id == wxAEArt::Delete) {
    x16 = dark ? mdi_delete_dark_16 : mdi_delete_light_16;
    x20 = dark ? mdi_delete_dark_20 : mdi_delete_light_20;
    x24 = dark ? mdi_delete_dark_24 : mdi_delete_light_24;
    x32 = dark ? mdi_delete_dark_32 : mdi_delete_light_32;
  } else if (id == wxAEArt::New) {
    x16 = dark ? mdi_note_add_dark_16 : mdi_note_add_light_16;
    x20 = dark ? mdi_note_add_dark_20 : mdi_note_add_light_20;
    x24 = dark ? mdi_note_add_dark_24 : mdi_note_add_light_24;
    x32 = dark ? mdi_note_add_dark_32 : mdi_note_add_light_32;
  } else if (id == wxAEArt::FileOpen) {
    x16 = dark ? mdi_folder_open_dark_16 : mdi_folder_open_light_16;
    x20 = dark ? mdi_folder_open_dark_20 : mdi_folder_open_light_20;
    x24 = dark ? mdi_folder_open_dark_24 : mdi_folder_open_light_24;
    x32 = dark ? mdi_folder_open_dark_32 : mdi_folder_open_light_32;
  } else if (id == wxAEArt::FileSave) {
    x16 = dark ? mdi_save_dark_16 : mdi_save_light_16;
    x20 = dark ? mdi_save_dark_20 : mdi_save_light_20;
    x24 = dark ? mdi_save_dark_24 : mdi_save_light_24;
    x32 = dark ? mdi_save_dark_32 : mdi_save_light_32;
  } else if (id == wxAEArt::FileSaveAs) {
    x16 = dark ? mdi_save_as_dark_16 : mdi_save_as_light_16;
    x20 = dark ? mdi_save_as_dark_20 : mdi_save_as_light_20;
    x24 = dark ? mdi_save_as_dark_24 : mdi_save_as_light_24;
    x32 = dark ? mdi_save_as_dark_32 : mdi_save_as_light_32;
  } else if (id == wxAEArt::Quit) {
    x16 = dark ? mdi_logout_dark_16 : mdi_logout_light_16;
    x20 = dark ? mdi_logout_dark_20 : mdi_logout_light_20;
    x24 = dark ? mdi_logout_dark_24 : mdi_logout_light_24;
    x32 = dark ? mdi_logout_dark_32 : mdi_logout_light_32;
  } else if (id == wxAEArt::GoBack) {
    x16 = dark ? mdi_arrow_back_dark_16 : mdi_arrow_back_light_16;
    x20 = dark ? mdi_arrow_back_dark_20 : mdi_arrow_back_light_20;
    x24 = dark ? mdi_arrow_back_dark_24 : mdi_arrow_back_light_24;
    x32 = dark ? mdi_arrow_back_dark_32 : mdi_arrow_back_light_32;
  } else if (id == wxAEArt::GoForward) {
    x16 = dark ? mdi_arrow_forward_dark_16 : mdi_arrow_forward_light_16;
    x20 = dark ? mdi_arrow_forward_dark_20 : mdi_arrow_forward_light_20;
    x24 = dark ? mdi_arrow_forward_dark_24 : mdi_arrow_forward_light_24;
    x32 = dark ? mdi_arrow_forward_dark_32 : mdi_arrow_forward_light_32;
  } else if (id == wxAEArt::Find) {
    x16 = dark ? mdi_search_dark_16 : mdi_search_light_16;
    x20 = dark ? mdi_search_dark_20 : mdi_search_light_20;
    x24 = dark ? mdi_search_dark_24 : mdi_search_light_24;
    x32 = dark ? mdi_search_dark_32 : mdi_search_light_32;
  } else if (id == wxAEArt::FindReplace) {
    x16 = dark ? mdi_find_replace_dark_16 : mdi_find_replace_light_16;
    x20 = dark ? mdi_find_replace_dark_20 : mdi_find_replace_light_20;
    x24 = dark ? mdi_find_replace_dark_24 : mdi_find_replace_light_24;
    x32 = dark ? mdi_find_replace_dark_32 : mdi_find_replace_light_32;
  } else if (id == wxAEArt::FindAll) {
    x16 = dark ? mdi_find_in_page_dark_16 : mdi_find_in_page_light_16;
    x20 = dark ? mdi_find_in_page_dark_20 : mdi_find_in_page_light_20;
    x24 = dark ? mdi_find_in_page_dark_24 : mdi_find_in_page_light_24;
    x32 = dark ? mdi_find_in_page_dark_32 : mdi_find_in_page_light_32;
  } else if (id == wxAEArt::GoToDef) {
    x16 = dark ? mdi_adjust_dark_16 : mdi_adjust_light_16;
    x20 = dark ? mdi_adjust_dark_20 : mdi_adjust_light_20;
    x24 = dark ? mdi_adjust_dark_24 : mdi_adjust_light_24;
    x32 = dark ? mdi_adjust_dark_32 : mdi_adjust_light_32;
  } else if (id == wxAEArt::Undo) {
    x16 = dark ? mdi_undo_dark_16 : mdi_undo_light_16;
    x20 = dark ? mdi_undo_dark_20 : mdi_undo_light_20;
    x24 = dark ? mdi_undo_dark_24 : mdi_undo_light_24;
    x32 = dark ? mdi_undo_dark_32 : mdi_undo_light_32;
  } else if (id == wxAEArt::Redo) {
    x16 = dark ? mdi_redo_dark_16 : mdi_redo_light_16;
    x20 = dark ? mdi_redo_dark_20 : mdi_redo_light_20;
    x24 = dark ? mdi_redo_dark_24 : mdi_redo_light_24;
    x32 = dark ? mdi_redo_dark_32 : mdi_redo_light_32;
  } else if (id == wxAEArt::Cut) {
    x16 = dark ? mdi_content_cut_dark_16 : mdi_content_cut_light_16;
    x20 = dark ? mdi_content_cut_dark_20 : mdi_content_cut_light_20;
    x24 = dark ? mdi_content_cut_dark_24 : mdi_content_cut_light_24;
    x32 = dark ? mdi_content_cut_dark_32 : mdi_content_cut_light_32;
  } else if (id == wxAEArt::Copy) {
    x16 = dark ? mdi_content_copy_dark_16 : mdi_content_copy_light_16;
    x20 = dark ? mdi_content_copy_dark_20 : mdi_content_copy_light_20;
    x24 = dark ? mdi_content_copy_dark_24 : mdi_content_copy_light_24;
    x32 = dark ? mdi_content_copy_dark_32 : mdi_content_copy_light_32;
  } else if (id == wxAEArt::Paste) {
    x16 = dark ? mdi_content_paste_dark_16 : mdi_content_paste_light_16;
    x20 = dark ? mdi_content_paste_dark_20 : mdi_content_paste_light_20;
    x24 = dark ? mdi_content_paste_dark_24 : mdi_content_paste_light_24;
    x32 = dark ? mdi_content_paste_dark_32 : mdi_content_paste_light_32;
  } else if (id == wxAEArt::Print) {
    x16 = dark ? mdi_print_dark_16 : mdi_print_light_16;
    x20 = dark ? mdi_print_dark_20 : mdi_print_light_20;
    x24 = dark ? mdi_print_dark_24 : mdi_print_light_24;
    x32 = dark ? mdi_print_dark_32 : mdi_print_light_32;
  } else if (id == wxAEArt::Folder) {
    x16 = dark ? mdi_folder_dark_16 : mdi_folder_light_16;
    x20 = dark ? mdi_folder_dark_20 : mdi_folder_light_20;
    x24 = dark ? mdi_folder_dark_24 : mdi_folder_light_24;
    x32 = dark ? mdi_folder_dark_32 : mdi_folder_light_32;
  } else if (id == wxAEArt::FolderOpen) {
    x16 = dark ? mdi_folder_open_dark_16 : mdi_folder_open_light_16;
    x20 = dark ? mdi_folder_open_dark_20 : mdi_folder_open_light_20;
    x24 = dark ? mdi_folder_open_dark_24 : mdi_folder_open_light_24;
    x32 = dark ? mdi_folder_open_dark_32 : mdi_folder_open_light_32;
  } else if (id == wxAEArt::NormalFile) {
    x16 = dark ? mdi_description_dark_16 : mdi_description_light_16;
    x20 = dark ? mdi_description_dark_20 : mdi_description_light_20;
    x24 = dark ? mdi_description_dark_24 : mdi_description_light_24;
    x32 = dark ? mdi_description_dark_32 : mdi_description_light_32;
  } else if (id == wxAEArt::ExecutableFile) {
    x16 = dark ? mdi_terminal_dark_16 : mdi_terminal_light_16;
    x20 = dark ? mdi_terminal_dark_20 : mdi_terminal_light_20;
    x24 = dark ? mdi_terminal_dark_24 : mdi_terminal_light_24;
    x32 = dark ? mdi_terminal_dark_32 : mdi_terminal_light_32;
  } else if (id == wxAEArt::SysLibrary) {
    x16 = dark ? mdi_library_books_dark_16 : mdi_library_books_light_16;
    x20 = dark ? mdi_library_books_dark_20 : mdi_library_books_light_20;
    x24 = dark ? mdi_library_books_dark_24 : mdi_library_books_light_24;
    x32 = dark ? mdi_library_books_dark_32 : mdi_library_books_light_32;
  } else if (id == wxAEArt::UserLibrary) {
    x16 = dark ? mdi_library_add_dark_16 : mdi_library_add_light_16;
    x20 = dark ? mdi_library_add_dark_20 : mdi_library_add_light_20;
    x24 = dark ? mdi_library_add_dark_24 : mdi_library_add_light_24;
    x32 = dark ? mdi_library_add_dark_32 : mdi_library_add_light_32;
  } else if (id == wxAEArt::GoUp) {
    x16 = dark ? mdi_arrow_upward_dark_16 : mdi_arrow_upward_light_16;
    x20 = dark ? mdi_arrow_upward_dark_20 : mdi_arrow_upward_light_20;
    x24 = dark ? mdi_arrow_upward_dark_24 : mdi_arrow_upward_light_24;
    x32 = dark ? mdi_arrow_upward_dark_32 : mdi_arrow_upward_light_32;
  } else if (id == wxAEArt::GoDown) {
    x16 = dark ? mdi_arrow_downward_dark_16 : mdi_arrow_downward_light_16;
    x20 = dark ? mdi_arrow_downward_dark_20 : mdi_arrow_downward_light_20;
    x24 = dark ? mdi_arrow_downward_dark_24 : mdi_arrow_downward_light_24;
    x32 = dark ? mdi_arrow_downward_dark_32 : mdi_arrow_downward_light_32;
  } else if (id == wxAEArt::GoToParent) {
    x16 = dark ? mdi_drive_file_move_dark_16 : mdi_drive_file_move_light_16;
    x20 = dark ? mdi_drive_file_move_dark_20 : mdi_drive_file_move_light_20;
    x24 = dark ? mdi_drive_file_move_dark_24 : mdi_drive_file_move_light_24;
    x32 = dark ? mdi_drive_file_move_dark_32 : mdi_drive_file_move_light_32;
  } else if (id == wxAEArt::Plus) {
    x16 = dark ? mdi_add_dark_16 : mdi_add_light_16;
    x20 = dark ? mdi_add_dark_20 : mdi_add_light_20;
    x24 = dark ? mdi_add_dark_24 : mdi_add_light_24;
    x32 = dark ? mdi_add_dark_32 : mdi_add_light_32;
  } else if (id == wxAEArt::Minus) {
    x16 = dark ? mdi_remove_dark_16 : mdi_remove_light_16;
    x20 = dark ? mdi_remove_dark_20 : mdi_remove_light_20;
    x24 = dark ? mdi_remove_dark_24 : mdi_remove_light_24;
    x32 = dark ? mdi_remove_dark_32 : mdi_remove_light_32;
  } else if (id == wxAEArt::Edit) {
    x16 = dark ? mdi_edit_dark_16 : mdi_edit_light_16;
    x20 = dark ? mdi_edit_dark_20 : mdi_edit_light_20;
    x24 = dark ? mdi_edit_dark_24 : mdi_edit_light_24;
    x32 = dark ? mdi_edit_dark_32 : mdi_edit_light_32;
  } else if (id == wxAEArt::ListView) {
    x16 = dark ? mdi_view_list_dark_16 : mdi_view_list_light_16;
    x20 = dark ? mdi_view_list_dark_20 : mdi_view_list_light_20;
    x24 = dark ? mdi_view_list_dark_24 : mdi_view_list_light_24;
    x32 = dark ? mdi_view_list_dark_32 : mdi_view_list_light_32;
  } else if (id == wxAEArt::ReportView) {
    x16 = dark ? mdi_table_rows_dark_16 : mdi_table_rows_light_16;
    x20 = dark ? mdi_table_rows_dark_20 : mdi_table_rows_light_20;
    x24 = dark ? mdi_table_rows_dark_24 : mdi_table_rows_light_24;
    x32 = dark ? mdi_table_rows_dark_32 : mdi_table_rows_light_32;
  } else if (id == wxAEArt::Tip) {
    x16 = dark ? mdi_lightbulb_dark_16 : mdi_lightbulb_light_16;
    x20 = dark ? mdi_lightbulb_dark_20 : mdi_lightbulb_light_20;
    x24 = dark ? mdi_lightbulb_dark_24 : mdi_lightbulb_light_24;
    x32 = dark ? mdi_lightbulb_dark_32 : mdi_lightbulb_light_32;
  } else if (id == wxAEArt::Information) {
    x16 = dark ? mdi_info_dark_16 : mdi_info_light_16;
    x20 = dark ? mdi_info_dark_20 : mdi_info_light_20;
    x24 = dark ? mdi_info_dark_24 : mdi_info_light_24;
    x32 = dark ? mdi_info_dark_32 : mdi_info_light_32;
  } else if (id == wxAEArt::Question) {
    x16 = dark ? mdi_question_mark_dark_16 : mdi_question_mark_light_16;
    x20 = dark ? mdi_question_mark_dark_20 : mdi_question_mark_light_20;
    x24 = dark ? mdi_question_mark_dark_24 : mdi_question_mark_light_24;
    x32 = dark ? mdi_question_mark_dark_32 : mdi_question_mark_light_32;
  } else if (id == wxAEArt::DevBoard) {
    x16 = dark ? mdi_developer_board_dark_16 : mdi_developer_board_light_16;
    x20 = dark ? mdi_developer_board_dark_20 : mdi_developer_board_light_20;
    x24 = dark ? mdi_developer_board_dark_24 : mdi_developer_board_light_24;
    x32 = dark ? mdi_developer_board_dark_32 : mdi_developer_board_light_32;
  } else if (id == wxAEArt::Play) {
    x16 = dark ? mdi_play_arrow_dark_16 : mdi_play_arrow_light_16;
    x20 = dark ? mdi_play_arrow_dark_20 : mdi_play_arrow_light_20;
    x24 = dark ? mdi_play_arrow_dark_24 : mdi_play_arrow_light_24;
    x32 = dark ? mdi_play_arrow_dark_32 : mdi_play_arrow_light_32;
  } else if (id == wxAEArt::Check) {
    x16 = dark ? mdi_check_circle_dark_16 : mdi_check_circle_light_16;
    x20 = dark ? mdi_check_circle_dark_20 : mdi_check_circle_light_20;
    x24 = dark ? mdi_check_circle_dark_24 : mdi_check_circle_light_24;
    x32 = dark ? mdi_check_circle_dark_32 : mdi_check_circle_light_32;
  } else if (id == wxAEArt::SerMon) {
    x16 = dark ? mdi_monitor_heart_dark_16 : mdi_monitor_heart_light_16;
    x20 = dark ? mdi_monitor_heart_dark_20 : mdi_monitor_heart_light_20;
    x24 = dark ? mdi_monitor_heart_dark_24 : mdi_monitor_heart_light_24;
    x32 = dark ? mdi_monitor_heart_dark_32 : mdi_monitor_heart_light_32;
  } else if (id == wxAEArt::SourceFormat) {
    x16 = dark ? mdi_format_align_justify_dark_16 : mdi_format_align_justify_light_16;
    x20 = dark ? mdi_format_align_justify_dark_20 : mdi_format_align_justify_light_20;
    x24 = dark ? mdi_format_align_justify_dark_24 : mdi_format_align_justify_light_24;
    x32 = dark ? mdi_format_align_justify_dark_32 : mdi_format_align_justify_light_32;
  } else if (id == wxAEArt::Settings) {
    x16 = dark ? mdi_settings_dark_16 : mdi_settings_light_16;
    x20 = dark ? mdi_settings_dark_20 : mdi_settings_light_20;
    x24 = dark ? mdi_settings_dark_24 : mdi_settings_light_24;
    x32 = dark ? mdi_settings_dark_32 : mdi_settings_light_32;
  } else if (id == wxAEArt::SelectAll) {
    x16 = dark ? mdi_select_all_dark_16 : mdi_select_all_light_16;
    x20 = dark ? mdi_select_all_dark_20 : mdi_select_all_light_20;
    x24 = dark ? mdi_select_all_dark_24 : mdi_select_all_light_24;
    x32 = dark ? mdi_select_all_dark_32 : mdi_select_all_light_32;
  } else if (id == wxAEArt::CheckForUpdates) {
    x16 = dark ? mdi_update_dark_16 : mdi_update_light_16;
    x20 = dark ? mdi_update_dark_20 : mdi_update_light_20;
    x24 = dark ? mdi_update_dark_24 : mdi_update_light_24;
    x32 = dark ? mdi_update_dark_32 : mdi_update_light_32;
  }

  if (!x16 || !x32 || !x24 || !x20)
    return wxNullBitmap;

  wxBitmap b16(x16);
  wxBitmap b20(x20);
  wxBitmap b24(x24);
  wxBitmap b32(x32);
  wxVector<wxBitmap> bitmaps;
  bitmaps.push_back(b16);
  bitmaps.push_back(b20);
  bitmaps.push_back(b24);
  bitmaps.push_back(b32);

  return wxBitmapBundle::FromBitmaps(bitmaps);
}
