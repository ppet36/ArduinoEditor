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

#include "ard_filestree.hpp"

#include "ard_ap.hpp"
#include "ard_cli.hpp"
#include <algorithm>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/imaglist.h>
#include <wx/menu.h>
#include <wx/sizer.h>

wxDEFINE_EVENT(EVT_SKETCH_TREE_OPEN_ITEM, wxCommandEvent);
wxDEFINE_EVENT(EVT_SKETCH_TREE_NEW_FILE, wxCommandEvent);
wxDEFINE_EVENT(EVT_SKETCH_TREE_NEW_FOLDER, wxCommandEvent);
wxDEFINE_EVENT(EVT_SKETCH_TREE_DELETE, wxCommandEvent);
wxDEFINE_EVENT(EVT_SKETCH_TREE_RENAME, wxCommandEvent);
wxDEFINE_EVENT(EVT_SKETCH_TREE_OPEN_EXTERNALLY, wxCommandEvent);

namespace {

// case-insensitive sort
int CmpNoCase(const wxString &a, const wxString &b) {
  return a.CmpNoCase(b);
}

enum {
  ID_TREE_NEW_FILE = wxID_HIGHEST + 2000,
  ID_TREE_NEW_FOLDER,
  ID_TREE_DELETE,
  ID_TREE_RENAME,
  ID_TREE_OPEN_EXTERNALLY
};

static bool ShouldShowDirName(const wxString &name) {
  return !name.StartsWith(wxT("."));
}

static bool ShouldShowFileName(const wxString &name) {
  if (name.StartsWith(wxT(".")))
    return false;
  if (name == wxT("sketch.yaml"))
    return false;
  return true;
}

static void CollectDirEntriesSorted(const wxString &dirPath,
                                    wxArrayString *subdirs,
                                    wxArrayString *files) {
  subdirs->Clear();
  files->Clear();

  wxDir dir(dirPath);
  if (!dir.IsOpened())
    return;

  wxString name;

  bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
  while (cont) {
    subdirs->Add(name);
    cont = dir.GetNext(&name);
  }

  cont = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES);
  while (cont) {
    files->Add(name);
    cont = dir.GetNext(&name);
  }

  subdirs->Sort(CmpNoCase);
  files->Sort(CmpNoCase);
}

} // namespace

SketchFilesPanel::SketchFilesPanel(wxWindow *parent, wxWindowID id)
    : wxPanel(parent, id) {
  m_tree = new wxTreeCtrl(
      this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
      wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);

  auto *sizer = new wxBoxSizer(wxVERTICAL);
  sizer->Add(m_tree, 1, wxEXPAND);
  SetSizer(sizer);

  Bind(wxEVT_SYS_COLOUR_CHANGED, &SketchFilesPanel::OnSysColourChanged, this);

  m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &SketchFilesPanel::OnItemActivated, this);
  m_tree->Bind(wxEVT_TREE_ITEM_MENU, &SketchFilesPanel::OnContextMenu, this);

  Bind(wxEVT_MENU, &SketchFilesPanel::OnMenuNewFile, this, ID_TREE_NEW_FILE);
  Bind(wxEVT_MENU, &SketchFilesPanel::OnMenuNewFolder, this, ID_TREE_NEW_FOLDER);
  Bind(wxEVT_MENU, &SketchFilesPanel::OnMenuDelete, this, ID_TREE_DELETE);
  Bind(wxEVT_MENU, &SketchFilesPanel::OnMenuRename, this, ID_TREE_RENAME);
  Bind(wxEVT_MENU, &SketchFilesPanel::OnMenuOpenExternally, this, ID_TREE_OPEN_EXTERNALLY);

  SetupIcons();

  RefreshTree();
}

void SketchFilesPanel::SetupIcons() {
  const int iconDip = 16;

  const int px = m_tree ? m_tree->FromDIP(iconDip) : FromDIP(iconDip);
  const wxSize iconSize(px, px);

  auto *newList = new wxImageList(iconSize.GetWidth(), iconSize.GetHeight(), true);

  auto addBundle = [&](const wxArtID &artId) -> int {
    wxBitmapBundle bb = AEGetArtBundle(artId);
    wxBitmap bmp = bb.GetBitmap(iconSize);

    if (bmp.IsOk() && (bmp.GetWidth() != iconSize.x || bmp.GetHeight() != iconSize.y)) {
      wxImage img = bmp.ConvertToImage();
      img.Rescale(iconSize.x, iconSize.y, wxIMAGE_QUALITY_HIGH);
      bmp = wxBitmap(img);
    }
    return newList->Add(bmp);
  };

  m_imgFolder = addBundle(wxAEArt::Folder);
  m_imgFolderOpen = addBundle(wxAEArt::FolderOpen);
  m_imgFile = addBundle(wxAEArt::NormalFile);
  m_imgExeFile = addBundle(wxAEArt::ExecutableFile);
  m_imgUserLibrary = addBundle(wxAEArt::UserLibrary);
  m_imgCoreLibrary = addBundle(wxAEArt::SysLibrary);

  m_tree->AssignImageList(newList);

  m_tree->Refresh();
  m_tree->Update();
}

void SketchFilesPanel::OnSysColourChanged(wxSysColourChangedEvent &event) {
  APP_DEBUG_LOG("FSTREE: OnSysColourChanged()");
  SetupIcons();
  event.Skip();
}

void SketchFilesPanel::SetRootPath(const wxString &rootPath) {
  m_rootPath = rootPath;

  if (!m_tree)
    return;

  m_tree->DeleteAllItems();
  BuildTree();
}

void SketchFilesPanel::UpdateResolvedLibraries(const std::vector<ResolvedLibraryInfo> &libs) {
  if (!m_tree)
    return;

  if (!m_rootId.IsOk()) {
    return;
  }

  if (!m_libsRootId.IsOk()) {
    m_libsRootId = m_tree->AppendItem(
        m_rootId,
        _("Libraries"),
        m_imgFolderOpen,
        m_imgFolder);
  }

  m_tree->DeleteChildren(m_libsRootId);

  // --- sort: core libs first (A-Z), then user libs (A-Z) ---
  std::vector<ResolvedLibraryInfo> sorted = libs;
  std::sort(sorted.begin(), sorted.end(),
            [](const ResolvedLibraryInfo &a, const ResolvedLibraryInfo &b) {
              if (a.isCoreLibrary != b.isCoreLibrary)
                return a.isCoreLibrary > b.isCoreLibrary; // core first

              // case-insensitive by name
              wxString an = wxString::FromUTF8(a.name.c_str());
              wxString bn = wxString::FromUTF8(b.name.c_str());
              int c = an.CmpNoCase(bn);
              if (c != 0)
                return c < 0;

              // deterministic tiebreakers
              if (a.name != b.name)
                return a.name < b.name;
              if (a.version != b.version)
                return a.version < b.version;
              return a.includePath < b.includePath;
            });

  for (const auto &lib : sorted) {
    wxString name = wxString::FromUTF8(lib.name.c_str());
    wxString ver = wxString::FromUTF8(lib.version.c_str());

    wxString label;
    if (!ver.empty()) {
      label = wxString::Format(wxT("%s (%s)"), name, ver);
    } else {
      label = name;
    }

    int ii = lib.isCoreLibrary ? m_imgCoreLibrary : m_imgUserLibrary;

    wxString includePath = wxString::FromUTF8(lib.includePath.c_str());

    m_tree->AppendItem(
        m_libsRootId,
        label, ii, ii, new SketchTreeItemData(includePath, /*isDir=*/true, /*isLibrary=*/true));
  }

  if (!libs.empty()) {
    m_tree->Expand(m_libsRootId);
  } else {
    m_tree->Collapse(m_libsRootId);
  }
}

void SketchFilesPanel::RefreshTree() {
  if (!m_tree)
    return;

  if (!m_rootId.IsOk() || !m_filesRootId.IsOk()) {
    m_tree->DeleteAllItems();
    BuildTree();
    return;
  }

  m_tree->DeleteChildren(m_filesRootId);

  AddDirRecursive(m_filesRootId, m_rootPath);
  m_tree->Expand(m_filesRootId);
}

void SketchFilesPanel::AddDirRecursive(const wxTreeItemId &parent, const wxString &dirPath) {
  wxArrayString subdirs;
  wxArrayString files;
  CollectDirEntriesSorted(dirPath, &subdirs, &files);

  for (auto &dName : subdirs) {
    if (!ShouldShowDirName(dName))
      continue;

    wxFileName fn(dirPath, dName);
    wxString full = fn.GetFullPath();

    wxTreeItemId id = m_tree->AppendItem(
        parent,
        dName,
        m_imgFolderOpen,
        m_imgFolder,
        new SketchTreeItemData(full, /*isDir=*/true));

    AddDirRecursive(id, full);
  }

  for (auto &fName : files) {
    if (!ShouldShowFileName(fName))
      continue;

    wxFileName fn(dirPath, fName);
    wxString full = fn.GetFullPath();

    int ii = fName.EndsWith(wxT(".ino")) ? m_imgExeFile : m_imgFile;

    m_tree->AppendItem(
        parent,
        fName,
        ii,
        ii,
        new SketchTreeItemData(full, /*isDir=*/false));
  }
}

void SketchFilesPanel::BuildTree() {
  m_rootId = wxTreeItemId();
  m_filesRootId = wxTreeItemId();
  m_libsRootId = wxTreeItemId();

  if (m_rootPath.empty() || !wxDirExists(m_rootPath)) {
    m_rootId = m_tree->AddRoot(_("No sketch opened"));
    return;
  }

  wxFileName rootFn;
  rootFn.AssignDir(m_rootPath);
  wxString rootName;
  const wxArrayString &dirs = rootFn.GetDirs();
  if (!dirs.IsEmpty()) {
    rootName = dirs.Last();
  } else {
    rootName = m_rootPath;
  }

  m_rootId = m_tree->AddRoot(
      rootName,
      m_imgFolderOpen,
      m_imgFolder,
      new SketchTreeItemData(m_rootPath, /*isDir=*/true));

  m_filesRootId = m_tree->AppendItem(
      m_rootId,
      _("Files"),
      m_imgFolderOpen,
      m_imgFolder);

  m_libsRootId = m_tree->AppendItem(
      m_rootId,
      _("Libraries"),
      m_imgFolderOpen,
      m_imgFolder);

  AddDirRecursive(m_filesRootId, m_rootPath);

  m_tree->Expand(m_rootId);
  m_tree->Expand(m_filesRootId);
}

void SketchFilesPanel::SelectPath(const wxString &fullPath) {
  if (!m_tree)
    return;

  wxTreeItemId root = m_tree->GetRootItem();
  if (!root.IsOk())
    return;

  wxTreeItemId found;

  std::function<void(const wxTreeItemId &)> find;
  find = [&](const wxTreeItemId &node) {
    if (!node.IsOk() || found.IsOk())
      return;

    auto *data = dynamic_cast<SketchTreeItemData *>(m_tree->GetItemData(node));
    if (data && !data->isDir && data->path == fullPath) {
      found = node;
      return;
    }

    wxTreeItemIdValue cookie;
    wxTreeItemId child = m_tree->GetFirstChild(node, cookie);
    while (child.IsOk() && !found.IsOk()) {
      find(child);
      child = m_tree->GetNextChild(node, cookie);
    }
  };

  find(root);

  if (found.IsOk()) {
    m_tree->SelectItem(found);
    m_tree->EnsureVisible(found);
  }
}

void SketchFilesPanel::OnItemActivated(wxTreeEvent &evt) {
  wxTreeItemId id = evt.GetItem();
  if (!id.IsOk())
    return;

  auto *data = dynamic_cast<SketchTreeItemData *>(m_tree->GetItemData(id));

  if (!data) {
    if (m_tree->IsExpanded(id))
      m_tree->Collapse(id);
    else
      m_tree->Expand(id);
    return;
  }

  if (data->isLibrary) {
    SendPathEvent(EVT_SKETCH_TREE_OPEN_EXTERNALLY, data->path, /*isDir=*/true);
    return;
  }

  if (data->isDir) {
    if (m_tree->IsExpanded(id))
      m_tree->Collapse(id);
    else
      m_tree->Expand(id);
    return;
  }

  SendPathEvent(EVT_SKETCH_TREE_OPEN_ITEM, data->path, data->isDir);
}

void SketchFilesPanel::OnContextMenu(wxTreeEvent &evt) {
  wxTreeItemId id = evt.GetItem();
  if (!id.IsOk())
    return;

  m_tree->SelectItem(id);

  auto *data = dynamic_cast<SketchTreeItemData *>(m_tree->GetItemData(id));
  if (!data)
    return;

  if (data->isLibrary)
    return;

  wxMenu menu;

  if (data->isDir) {
    menu.Append(ID_TREE_NEW_FILE, _("New file"));
    menu.Append(ID_TREE_NEW_FOLDER, _("New folder"));
    menu.AppendSeparator();
    menu.Append(ID_TREE_OPEN_EXTERNALLY, _("Open in file browser"));

    if (!m_rootPath.empty() && data->path != m_rootPath) {
      menu.AppendSeparator();
      menu.Append(ID_TREE_RENAME, _("Rename folder"));
      menu.Append(ID_TREE_DELETE, _("Delete folder"));
    }
  } else {
    wxFileName fn(data->path);
    menu.Append(ID_TREE_OPEN_EXTERNALLY, _("Open externally"));

    if (fn.GetExt().Lower() != wxT("ino")) {
      menu.AppendSeparator();
      menu.Append(ID_TREE_RENAME, _("Rename file"));
      menu.Append(ID_TREE_DELETE, _("Delete file"));
    }
  }

  PopupMenu(&menu);
}

void SketchFilesPanel::OnMenuNewFile(wxCommandEvent &) {
  wxTreeItemId id = m_tree->GetSelection();
  if (!id.IsOk())
    return;
  auto *data = dynamic_cast<SketchTreeItemData *>(m_tree->GetItemData(id));
  if (!data)
    return;

  SendPathEvent(EVT_SKETCH_TREE_NEW_FILE, data->path, data->isDir);
}

void SketchFilesPanel::OnMenuNewFolder(wxCommandEvent &) {
  wxTreeItemId id = m_tree->GetSelection();
  if (!id.IsOk())
    return;
  auto *data = dynamic_cast<SketchTreeItemData *>(m_tree->GetItemData(id));
  if (!data)
    return;

  SendPathEvent(EVT_SKETCH_TREE_NEW_FOLDER, data->path, data->isDir);
}

void SketchFilesPanel::OnMenuDelete(wxCommandEvent &) {
  wxTreeItemId id = m_tree->GetSelection();
  if (!id.IsOk())
    return;
  auto *data = dynamic_cast<SketchTreeItemData *>(m_tree->GetItemData(id));
  if (!data)
    return;

  SendPathEvent(EVT_SKETCH_TREE_DELETE, data->path, data->isDir);
}

void SketchFilesPanel::OnMenuRename(wxCommandEvent &) {
  wxTreeItemId id = m_tree->GetSelection();
  if (!id.IsOk())
    return;
  auto *data = dynamic_cast<SketchTreeItemData *>(m_tree->GetItemData(id));
  if (!data)
    return;

  SendPathEvent(EVT_SKETCH_TREE_RENAME, data->path, data->isDir);
}

void SketchFilesPanel::OnMenuOpenExternally(wxCommandEvent &) {
  wxTreeItemId id = m_tree->GetSelection();
  if (!id.IsOk())
    return;
  auto *data = dynamic_cast<SketchTreeItemData *>(m_tree->GetItemData(id));
  if (!data)
    return;

  SendPathEvent(EVT_SKETCH_TREE_OPEN_EXTERNALLY, data->path, data->isDir);
}

void SketchFilesPanel::SendPathEvent(wxEventType type,
                                     const wxString &path,
                                     bool isDir) const {
  wxCommandEvent evt(type, GetId());
  evt.SetEventObject(const_cast<SketchFilesPanel *>(this));
  evt.SetString(path);
  evt.SetInt(isDir ? 1 : 0);

  wxPostEvent(GetParent(), evt);
}
