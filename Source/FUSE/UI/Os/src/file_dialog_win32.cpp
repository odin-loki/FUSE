// Win32 native file dialog: IFileOpenDialog / IFileSaveDialog (Vista+ common item dialog), in process via COM.
#if defined(_WIN32)
#include "fuse/ui/os.hpp"

#include <objbase.h>
#include <shobjidl.h>
#include <windows.h>

#include <string>
#include <vector>

namespace fuse::ui::os {
namespace {
std::wstring wide(std::string_view s) {
    std::wstring w(std::size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring backslashes(std::wstring p) {
    for (auto& c : p) if (c == L'/') c = L'\\';
    return p;
}
bool pathOf(IShellItem* it, std::vector<std::string>& out) {
    PWSTR p = nullptr;
    if (FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
    out.push_back(narrow(p));
    CoTaskMemFree(p);
    return true;
}
} // namespace

NativeDialogResult win32FileDialog(const NativeDialogSpec& s) {
    using K = NativeDialogSpec::Kind;
    NativeDialogResult res;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = SUCCEEDED(init);
    IFileDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(s.kind == K::Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, s.kind == K::Save ? IID_IFileSaveDialog : IID_IFileOpenDialog,
                                  reinterpret_cast<void**>(&dlg));
    if (FAILED(hr) || !dlg) {
        res.unavailable = true;
        if (uninit) CoUninitialize();
        return res;
    }
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (s.kind == K::Folder) opts |= FOS_PICKFOLDERS;
    if (s.kind == K::Open && s.multiple) opts |= FOS_ALLOWMULTISELECT;
    if (s.kind == K::Save) opts |= FOS_OVERWRITEPROMPT;
    if (s.kind != K::Save) opts |= FOS_PATHMUSTEXIST;
    dlg->SetOptions(opts);
    if (!s.title.empty()) dlg->SetTitle(wide(s.title).c_str());
    if (!s.fileName.empty()) dlg->SetFileName(wide(s.fileName).c_str());
    if (!s.directory.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(backslashes(wide(s.directory)).c_str(), nullptr, IID_IShellItem,
                                                  reinterpret_cast<void**>(&folder))) && folder) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    const auto spec = fileDialogFilterSpec(s);
    std::vector<std::wstring> names, pats;
    std::vector<COMDLG_FILTERSPEC> fs;
    for (const auto& [n, p] : spec) { names.push_back(wide(n)); pats.push_back(wide(p)); }
    for (std::size_t i = 0; i < spec.size(); ++i) fs.push_back({names[i].c_str(), pats[i].c_str()});
    if (!fs.empty() && s.kind != K::Folder) dlg->SetFileTypes(UINT(fs.size()), fs.data());

    hr = dlg->Show(GetActiveWindow());
    if (SUCCEEDED(hr)) {
        if (s.kind == K::Open && s.multiple) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(static_cast<IFileOpenDialog*>(dlg)->GetResults(&items)) && items) {
                DWORD n = 0;
                items->GetCount(&n);
                for (DWORD i = 0; i < n; ++i) {
                    IShellItem* it = nullptr;
                    if (SUCCEEDED(items->GetItemAt(i, &it)) && it) { pathOf(it, res.paths); it->Release(); }
                }
                items->Release();
            }
        } else {
            IShellItem* it = nullptr;
            if (SUCCEEDED(dlg->GetResult(&it)) && it) { pathOf(it, res.paths); it->Release(); }
        }
    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        res.unavailable = true; // no shell (server core) etc: let the caller fall back to the in-UI dialog
    }
    dlg->Release();
    if (uninit) CoUninitialize();
    return res;
}

} // namespace fuse::ui::os
#endif
