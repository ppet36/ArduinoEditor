#pragma once

#include <vector>
#include <wx/panel.h>
#include <wx/treectrl.h>

class wxImageList;
class wxCommandEvent;
struct ResolvedLibraryInfo;
struct EditorSettings;

// Custom events
//  - event.GetString() = full path
//  - event.GetInt()    = isDir ? 1 : 0

wxDECLARE_EVENT(EVT_SKETCH_TREE_OPEN_ITEM, wxCommandEvent);
wxDECLARE_EVENT(EVT_SKETCH_TREE_NEW_FILE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SKETCH_TREE_NEW_FOLDER, wxCommandEvent);
wxDECLARE_EVENT(EVT_SKETCH_TREE_DELETE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SKETCH_TREE_RENAME, wxCommandEvent);
wxDECLARE_EVENT(EVT_SKETCH_TREE_OPEN_EXTERNALLY, wxCommandEvent);

// Tree view of sketch dir
class SketchFilesPanel : public wxPanel {
public:
  explicit SketchFilesPanel(wxWindow *parent, wxWindowID id = wxID_ANY);

  void SetRootPath(const wxString &rootPath);

  void UpdateResolvedLibraries(const std::vector<ResolvedLibraryInfo> &libs);

  void RefreshTree();

  void SelectPath(const wxString &fullPath);

  void OnSysColourChanged();

private:
  struct SketchTreeItemData : public wxTreeItemData {
    wxString path;
    bool isDir;
    bool isLibrary;
    SketchTreeItemData(const wxString &p, bool d, bool lib = false)
        : path(p), isDir(d), isLibrary(lib) {}
  };

  wxTreeCtrl *m_tree = nullptr;
  int m_imgFolder = -1;
  int m_imgFolderOpen = -1;
  int m_imgFile = -1;
  int m_imgExeFile = -1;
  int m_imgCoreLibrary = -1;
  int m_imgUserLibrary = -1;

  wxTreeItemId m_rootId;
  wxTreeItemId m_filesRootId;
  wxTreeItemId m_libsRootId;

  wxString m_rootPath;

  void BuildTree();
  void AddDirRecursive(const wxTreeItemId &parent, const wxString &dirPath);

  void OnItemActivated(wxTreeEvent &evt);
  void OnContextMenu(wxTreeEvent &evt);

  void OnMenuNewFile(wxCommandEvent &evt);
  void OnMenuNewFolder(wxCommandEvent &evt);
  void OnMenuDelete(wxCommandEvent &evt);
  void OnMenuRename(wxCommandEvent &evt);
  void OnMenuOpenExternally(wxCommandEvent &evt);

  void SendPathEvent(wxEventType type, const wxString &path, bool isDir) const;
};
