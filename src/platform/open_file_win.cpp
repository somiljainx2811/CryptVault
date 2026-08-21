// open_file_win.cpp - Windows implementation of
// platform::OpenFileWithDefaultApp, via ShellExecuteW - the same
// mechanism Explorer itself uses when you double-click a file.
#include "platform/open_file.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>

namespace platform {

namespace {
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
    return wide;
}
}  // namespace

bool OpenFileWithDefaultApp(const std::string& path, std::string* error) {
    std::wstring wide_path = Utf8ToWide(path);
    // ShellExecuteW returns a value > 32 on success (an HINSTANCE by
    // historical accident of the API, not something to actually use
    // as one) - anything <= 32 is an error code.
    HINSTANCE result = ShellExecuteW(nullptr, L"open", wide_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    auto code = reinterpret_cast<INT_PTR>(result);
    if (code > 32) {
        return true;
    }
    if (error) {
        if (code == static_cast<INT_PTR>(SE_ERR_NOASSOC)) {
            *error = "no application is associated with this file type";
        } else if (code == static_cast<INT_PTR>(SE_ERR_FNF)) {
            *error = "file not found";
        } else {
            *error = "couldn't open the file (error " + std::to_string(static_cast<long long>(code)) + ")";
        }
    }
    return false;
}

}  // namespace platform
