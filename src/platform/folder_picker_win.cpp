// folder_picker_win.cpp - Windows implementation of platform::PickFolder,
// via IFileOpenDialog (the modern Common Item Dialog) with the
// FOS_PICKFOLDERS option, which is what Explorer itself and most
// modern Windows apps use for "browse for a folder" - not the older
// SHBrowseForFolder, which looks and behaves noticeably dated by
// comparison.
#include "platform/folder_picker.h"

// Must come before shobjidl.h - defines the COM interfaces this file
// uses (IFileOpenDialog etc.) at a modern enough Windows version.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>  // Microsoft::WRL::ComPtr - RAII for COM interface pointers

#include <string>
#include <vector>

namespace platform {

namespace {

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return L"";
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
    return wide;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return "";
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                   nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), len, nullptr,
                         nullptr);
    return utf8;
}

}  // namespace

bool PickFolder(const std::string& title, const std::string& initial_path, std::string& out_path) {
    // COM may already be initialized (by GLFW, or by a previous call
    // to this function) - COINIT_APARTMENTTHREADED is what file
    // dialogs require, and RPC_E_CHANGED_MODE just means "already
    // initialized with a different concurrency model," which is still
    // fine to proceed with (we don't own that initialization either
    // way, so we also don't uninitialize it below).
    HRESULT com_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool we_initialized_com = SUCCEEDED(com_init);
    if (FAILED(com_init) && com_init != RPC_E_CHANGED_MODE) {
        return false;
    }

    bool picked = false;
    {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(hr)) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);

            std::wstring wide_title = Utf8ToWide(title);
            dialog->SetTitle(wide_title.c_str());

            // Best-effort: an initial_path that doesn't exist (very
            // common here - see DrawCreateVaultWizard, which pre-
            // fills a path that doesn't exist yet, since the vault
            // hasn't been created) just means the dialog opens
            // wherever Windows' own default is instead. Not treated
            // as an error.
            if (!initial_path.empty()) {
                Microsoft::WRL::ComPtr<IShellItem> initial_item;
                std::wstring wide_initial = Utf8ToWide(initial_path);
                if (SUCCEEDED(SHCreateItemFromParsingName(wide_initial.c_str(), nullptr,
                                                            IID_PPV_ARGS(&initial_item)))) {
                    dialog->SetFolder(initial_item.Get());
                }
            }

            // No owner HWND is passed (nullptr) - this app has no
            // stable, easily-reachable HWND at this call site (GLFW
            // owns window creation, and threading one through here
            // would mean a wider plumbing change for a dialog that's
            // already modal at the process level). The dialog still
            // works correctly without one; it just isn't visually
            // parented to CryptVault's window.
            hr = dialog->Show(nullptr);
            if (SUCCEEDED(hr)) {
                Microsoft::WRL::ComPtr<IShellItem> result;
                if (SUCCEEDED(dialog->GetResult(&result))) {
                    PWSTR wide_path = nullptr;
                    if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &wide_path))) {
                        out_path = WideToUtf8(wide_path);
                        CoTaskMemFree(wide_path);
                        picked = true;
                    }
                }
            }
            // HRESULT_FROM_WIN32(ERROR_CANCELLED) from Show() means
            // the user cancelled - not an error, `picked` just stays
            // false and the caller falls back to manual entry.
        }
    }  // dialog (a ComPtr) releases its COM reference here, before CoUninitialize below

    if (we_initialized_com) {
        CoUninitialize();
    }
    return picked;
}

bool PickFile(const std::string& title, const std::string& filter_label,
              const std::vector<std::string>& filter_extensions, std::string& out_path) {
    HRESULT com_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool we_initialized_com = SUCCEEDED(com_init);
    if (FAILED(com_init) && com_init != RPC_E_CHANGED_MODE) {
        return false;
    }

    bool picked = false;
    {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(hr)) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);

            std::wstring wide_title = Utf8ToWide(title);
            dialog->SetTitle(wide_title.c_str());

            // Builds "*.png;*.jpg;*.gif" from filter_extensions - the
            // wide strings need to outlive the SetFileTypes call
            // below, hence holding them here rather than as
            // temporaries.
            std::wstring wide_label = Utf8ToWide(filter_label);
            std::wstring wide_pattern;
            if (filter_extensions.empty()) {
                wide_pattern = L"*.*";
            } else {
                for (size_t i = 0; i < filter_extensions.size(); ++i) {
                    if (i) wide_pattern += L";";
                    wide_pattern += L"*." + Utf8ToWide(filter_extensions[i]);
                }
            }
            COMDLG_FILTERSPEC filter{wide_label.c_str(), wide_pattern.c_str()};
            dialog->SetFileTypes(1, &filter);
            dialog->SetFileTypeIndex(1);

            hr = dialog->Show(nullptr);
            if (SUCCEEDED(hr)) {
                Microsoft::WRL::ComPtr<IShellItem> result;
                if (SUCCEEDED(dialog->GetResult(&result))) {
                    PWSTR wide_path = nullptr;
                    if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &wide_path))) {
                        out_path = WideToUtf8(wide_path);
                        CoTaskMemFree(wide_path);
                        picked = true;
                    }
                }
            }
        }
    }

    if (we_initialized_com) {
        CoUninitialize();
    }
    return picked;
}

}  // namespace platform
