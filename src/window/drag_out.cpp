#include "window/drag_out.h"

#include <cctype>

#include "platform/clipboard_files.h"

namespace cryptvault {

DragOut::DragOut() = default;
DragOut::~DragOut() = default;

std::string DragOut::UriList(const std::vector<std::string>& paths) {
    std::string out;
    for (const std::string& p : paths) out += platform::FileUriFromPath(p) + "\r\n";
    return out;
}

std::string DragOut::PlainPaths(const std::vector<std::string>& paths) {
    std::string out;
    for (const std::string& p : paths) out += p + "\n";
    return out;
}

bool DragOut::Init(GLFWwindow* window, bool wayland) {
    backend_ = wayland ? CreateWaylandDragBackend(window) : CreateX11DragBackend(window);
    return backend_ != nullptr;
}

void DragOut::Pump() { if (backend_) backend_->Pump(); }
bool DragOut::Begin(const std::vector<std::string>& paths) { return backend_ && !paths.empty() && backend_->Begin(paths); }
bool DragOut::Active() const { return backend_ && backend_->Active(); }
bool DragOut::TakeFinished(bool* dropped) { return backend_ && backend_->TakeFinished(dropped); }
const char* DragOut::BackendName() const { return backend_ ? backend_->Name() : "none"; }

#ifndef CRYPTVAULT_HAVE_WAYLAND_DND
std::unique_ptr<DragOut::Backend> CreateWaylandDragBackend(GLFWwindow*) { return nullptr; }
#endif
#ifndef CRYPTVAULT_HAVE_X11_DND
std::unique_ptr<DragOut::Backend> CreateX11DragBackend(GLFWwindow*) { return nullptr; }
#endif

}  // namespace cryptvault
