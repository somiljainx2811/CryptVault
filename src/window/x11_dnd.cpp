// X11 drag source (the XDND protocol, version 5) - see drag_out.h. Compiled only
// when the Xlib headers were available at build time (CRYPTVAULT_HAVE_X11_DND);
// libX11 itself is opened with dlopen at run time, so the program never links
// against it and still starts on machines without X11.
//
// GLFW owns the X connection and its event loop, so the whole protocol runs on
// a PRIVATE connection with a tiny unmapped window of our own as the drag
// source. While the button that started the gesture is held, GLFW's window
// keeps its implicit pointer grab; we simply poll the pointer (XQueryPointer)
// and talk XDND to whichever window is under it.
#ifdef CRYPTVAULT_HAVE_X11_DND

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "window/drag_out.h"

extern "C" {
unsigned long glfwGetX11Window(GLFWwindow*) __attribute__((weak));
}

namespace cryptvault {
namespace {

struct Xlib {
    void* handle = nullptr;
    Display* (*XOpenDisplay)(const char*) = nullptr;
    int (*XCloseDisplay)(Display*) = nullptr;
    Atom (*XInternAtom)(Display*, const char*, Bool) = nullptr;
    Window (*XCreateSimpleWindow)(Display*, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long) = nullptr;
    int (*XDestroyWindow)(Display*, Window) = nullptr;
    int (*XSetSelectionOwner)(Display*, Atom, Window, Time) = nullptr;
    Status (*XSendEvent)(Display*, Window, Bool, long, XEvent*) = nullptr;
    int (*XFlush)(Display*) = nullptr;
    int (*XSync)(Display*, Bool) = nullptr;
    int (*XPending)(Display*) = nullptr;
    int (*XNextEvent)(Display*, XEvent*) = nullptr;
    Bool (*XQueryPointer)(Display*, Window, Window*, Window*, int*, int*, int*, int*, unsigned*) = nullptr;
    Window (*XDefaultRootWindow)(Display*) = nullptr;
    int (*XChangeProperty)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int) = nullptr;
    int (*XGetWindowProperty)(Display*, Window, Atom, long, long, Bool, Atom, Atom*, int*, unsigned long*, unsigned long*, unsigned char**) = nullptr;
    int (*XFree)(void*) = nullptr;
    Bool (*XTranslateCoordinates)(Display*, Window, Window, int, int, int*, int*, Window*) = nullptr;
    XErrorHandler (*XSetErrorHandler)(XErrorHandler) = nullptr;

    bool Load() {
        handle = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!handle) return false;
#define CV_LOAD(name) name = reinterpret_cast<decltype(name)>(dlsym(handle, #name)); if (!name) return false
        CV_LOAD(XOpenDisplay); CV_LOAD(XCloseDisplay); CV_LOAD(XInternAtom); CV_LOAD(XCreateSimpleWindow);
        CV_LOAD(XDestroyWindow); CV_LOAD(XSetSelectionOwner); CV_LOAD(XSendEvent); CV_LOAD(XFlush); CV_LOAD(XSync);
        CV_LOAD(XPending); CV_LOAD(XNextEvent); CV_LOAD(XQueryPointer); CV_LOAD(XDefaultRootWindow);
        CV_LOAD(XChangeProperty); CV_LOAD(XGetWindowProperty); CV_LOAD(XFree); CV_LOAD(XTranslateCoordinates);
        CV_LOAD(XSetErrorHandler);
#undef CV_LOAD
        return true;
    }
};

int IgnoreXErrors(Display*, XErrorEvent*) { return 0; }   // a target window vanishing mid-drag is not fatal

bool Debug() { return std::getenv("CRYPTVAULT_DEBUG") != nullptr; }

class X11Drag : public DragOut::Backend {
public:
    ~X11Drag() override {
        if (dpy_) {
            if (src_win_) x_.XDestroyWindow(dpy_, src_win_);
            x_.XCloseDisplay(dpy_);
        }
        if (x_.handle) dlclose(x_.handle);
    }

    bool Init(GLFWwindow* window) {
        if (!glfwGetX11Window || !x_.Load()) return false;
        own_window_ = glfwGetX11Window(window);
        dpy_ = x_.XOpenDisplay(nullptr);
        if (!dpy_) return false;
        root_ = x_.XDefaultRootWindow(dpy_);
        src_win_ = x_.XCreateSimpleWindow(dpy_, root_, -100, -100, 1, 1, 0, 0, 0);   // never mapped: only a message sender
        auto atom = [&](const char* n) { return x_.XInternAtom(dpy_, n, False); };
        a_aware_ = atom("XdndAware"); a_enter_ = atom("XdndEnter"); a_position_ = atom("XdndPosition");
        a_status_ = atom("XdndStatus"); a_leave_ = atom("XdndLeave"); a_drop_ = atom("XdndDrop");
        a_finished_ = atom("XdndFinished"); a_selection_ = atom("XdndSelection");
        a_copy_ = atom("XdndActionCopy"); a_move_ = atom("XdndActionMove");
        a_uri_ = atom("text/uri-list"); a_plain_ = atom("text/plain"); a_utf8_ = atom("UTF8_STRING");
        a_plain_utf8_ = atom("text/plain;charset=utf-8"); a_targets_ = atom("TARGETS");
        return true;
    }

    bool Begin(const std::vector<std::string>& paths) override {
        if (active_) return false;
        unsigned mask = 0;
        QueryPointer(&mask);
        if (!(mask & Button1Mask)) return false;             // no held button: nothing to drag with
        uri_list_ = DragOut::UriList(paths);
        plain_ = DragOut::PlainPaths(paths);
        x_.XSetSelectionOwner(dpy_, a_selection_, src_win_, CurrentTime);
        x_.XFlush(dpy_);
        active_ = true;
        finished_ = dropped_ = false;
        target_ = 0;
        accepted_ = awaiting_status_ = false;
        dropping_ = false;
        last_x_ = last_y_ = -1;
        started_ = std::chrono::steady_clock::now();
        return true;
    }

    void Pump() override {
        if (!dpy_) return;
        // Our private connection can raise asynchronous errors (e.g. BadWindow if the target
        // closes mid-drag); Xlib's default handler would exit the process.
        XErrorHandler previous = x_.XSetErrorHandler(&IgnoreXErrors);
        DrainEvents();
        if (active_) Drive();
        x_.XSync(dpy_, False);
        x_.XSetErrorHandler(previous);
    }

    bool Active() const override { return active_; }

    bool TakeFinished(bool* dropped) override {
        if (!finished_) return false;
        finished_ = false;
        if (dropped) *dropped = dropped_;
        return true;
    }

    const char* Name() const override { return "x11"; }

private:
    void QueryPointer(unsigned* mask) {
        Window root_ret, child_ret;
        int wx, wy;
        x_.XQueryPointer(dpy_, root_, &root_ret, &child_ret, &root_x_, &root_y_, &wx, &wy, mask);
        pointer_child_ = child_ret;
    }

    // Replies from the target and the target's requests for our data.
    void DrainEvents() {
        while (x_.XPending(dpy_) > 0) {
            XEvent ev;
            x_.XNextEvent(dpy_, &ev);
            if (ev.type == ClientMessage) {
                if (ev.xclient.message_type == a_status_ && active_) {
                    awaiting_status_ = false;
                    accepted_ = (ev.xclient.data.l[1] & 1) != 0;
                } else if (ev.xclient.message_type == a_finished_ && active_) {
                    dropped_ = (ev.xclient.data.l[1] & 1) != 0;
                    if (Debug()) std::fprintf(stderr, "[cryptvault] x11 drag finished (success=%d)\n", dropped_ ? 1 : 0);
                    EndDrag();
                }
            } else if (ev.type == SelectionRequest) {
                AnswerSelectionRequest(ev.xselectionrequest);
            }
        }
    }

    void AnswerSelectionRequest(const XSelectionRequestEvent& rq) {
        XEvent reply;
        std::memset(&reply, 0, sizeof reply);
        reply.xselection.type = SelectionNotify;
        reply.xselection.display = dpy_;
        reply.xselection.requestor = rq.requestor;
        reply.xselection.selection = rq.selection;
        reply.xselection.target = rq.target;
        reply.xselection.time = rq.time;
        reply.xselection.property = None;
        const Atom property = rq.property != None ? rq.property : rq.target;
        if (rq.selection == a_selection_) {
            if (rq.target == a_targets_) {
                Atom list[] = {a_targets_, a_uri_, a_plain_utf8_, a_plain_, a_utf8_};
                x_.XChangeProperty(dpy_, rq.requestor, property, XA_ATOM, 32, PropModeReplace,
                                   reinterpret_cast<unsigned char*>(list), 5);
                reply.xselection.property = property;
            } else if (rq.target == a_uri_) {
                x_.XChangeProperty(dpy_, rq.requestor, property, rq.target, 8, PropModeReplace,
                                   reinterpret_cast<const unsigned char*>(uri_list_.data()), static_cast<int>(uri_list_.size()));
                reply.xselection.property = property;
                if (Debug()) std::fprintf(stderr, "[cryptvault] x11 drag: target asked for text/uri-list\n");
            } else if (rq.target == a_plain_ || rq.target == a_utf8_ || rq.target == a_plain_utf8_) {
                x_.XChangeProperty(dpy_, rq.requestor, property, rq.target, 8, PropModeReplace,
                                   reinterpret_cast<const unsigned char*>(plain_.data()), static_cast<int>(plain_.size()));
                reply.xselection.property = property;
            }
        }
        x_.XSendEvent(dpy_, rq.requestor, False, NoEventMask, &reply);
        x_.XFlush(dpy_);
    }

    // The XdndAware window under the pointer (descending through child windows),
    // never our own window. Returns 0 if none; `version` gets min(5, its version).
    Window FindTarget(int rx, int ry, long* version) {
        Window w = pointer_child_;
        if (!w) return 0;
        for (int depth = 0; depth < 16 && w; ++depth) {
            Atom type;
            int format;
            unsigned long count, after;
            unsigned char* data = nullptr;
            if (x_.XGetWindowProperty(dpy_, w, a_aware_, 0, 1, False, XA_ATOM, &type, &format, &count, &after, &data) == Success
                && data && count > 0 && format == 32) {
                const long v = static_cast<long>(*reinterpret_cast<unsigned long*>(data));
                x_.XFree(data);
                if (v >= 3) {
                    *version = v < 5 ? v : 5;
                    return w == own_window_ ? 0 : w;
                }
            } else if (data) {
                x_.XFree(data);
            }
            int cx, cy;
            Window child = 0;
            if (!x_.XTranslateCoordinates(dpy_, root_, w, rx, ry, &cx, &cy, &child) || !child) break;
            w = child;
        }
        return 0;
    }

    void SendClient(Window to, Atom type, long d0, long d1, long d2, long d3, long d4) {
        XEvent ev;
        std::memset(&ev, 0, sizeof ev);
        ev.xclient.type = ClientMessage;
        ev.xclient.display = dpy_;
        ev.xclient.window = to;
        ev.xclient.message_type = type;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = d0; ev.xclient.data.l[1] = d1; ev.xclient.data.l[2] = d2;
        ev.xclient.data.l[3] = d3; ev.xclient.data.l[4] = d4;
        x_.XSendEvent(dpy_, to, False, NoEventMask, &ev);
    }

    void Drive() {
        const auto now = std::chrono::steady_clock::now();
        if (dropping_) {   // waiting for XdndFinished
            if (now - dropped_at_ > std::chrono::seconds(5)) { dropped_ = true; EndDrag(); }   // target never reported back
            return;
        }
        if (now - started_ > std::chrono::seconds(120)) { EndDrag(); return; }   // safety net
        unsigned mask = 0;
        QueryPointer(&mask);
        long version = 0;
        const Window found = FindTarget(root_x_, root_y_, &version);

        if (found != target_) {
            if (target_) SendClient(target_, a_leave_, static_cast<long>(src_win_), 0, 0, 0, 0);
            target_ = found;
            accepted_ = awaiting_status_ = false;
            last_x_ = last_y_ = -1;
            if (target_) {
                target_version_ = version;
                SendClient(target_, a_enter_, static_cast<long>(src_win_), (version << 24), static_cast<long>(a_uri_),
                           static_cast<long>(a_plain_), static_cast<long>(a_utf8_));
                if (Debug()) std::fprintf(stderr, "[cryptvault] x11 drag: entered window 0x%lx (XDND v%ld)\n", static_cast<unsigned long>(target_), version);
            }
        }
        const bool button_held = (mask & Button1Mask) != 0;
        if (button_held) {
            if (target_ && (root_x_ != last_x_ || root_y_ != last_y_) && !awaiting_status_) {
                const Atom action = (mask & ShiftMask) ? a_move_ : a_copy_;
                SendClient(target_, a_position_, static_cast<long>(src_win_), 0, (static_cast<long>(root_x_) << 16) | (root_y_ & 0xFFFF),
                           CurrentTime, static_cast<long>(action));
                last_x_ = root_x_;
                last_y_ = root_y_;
                awaiting_status_ = true;
                status_sent_ = now;
            } else if (awaiting_status_ && now - status_sent_ > std::chrono::milliseconds(300)) {
                awaiting_status_ = false;   // a slow target must not stall the drag forever
            }
        } else {                              // button released: drop or give up
            if (target_ && accepted_) {
                SendClient(target_, a_drop_, static_cast<long>(src_win_), 0, CurrentTime, 0, 0);
                dropping_ = true;
                dropped_at_ = now;
                if (Debug()) std::fprintf(stderr, "[cryptvault] x11 drag: dropped on 0x%lx\n", static_cast<unsigned long>(target_));
            } else {
                if (target_) SendClient(target_, a_leave_, static_cast<long>(src_win_), 0, 0, 0, 0);
                dropped_ = false;
                EndDrag();
            }
        }
        x_.XFlush(dpy_);
    }

    void EndDrag() {
        x_.XSetSelectionOwner(dpy_, a_selection_, None, CurrentTime);
        x_.XFlush(dpy_);
        active_ = false;
        dropping_ = false;
        finished_ = true;
        target_ = 0;
    }

    Xlib x_;
    Display* dpy_ = nullptr;
    Window root_ = 0, src_win_ = 0, own_window_ = 0, target_ = 0, pointer_child_ = 0;
    int root_x_ = 0, root_y_ = 0, last_x_ = -1, last_y_ = -1;
    long target_version_ = 5;
    Atom a_aware_ = 0, a_enter_ = 0, a_position_ = 0, a_status_ = 0, a_leave_ = 0, a_drop_ = 0, a_finished_ = 0, a_selection_ = 0;
    Atom a_copy_ = 0, a_move_ = 0, a_uri_ = 0, a_plain_ = 0, a_utf8_ = 0, a_plain_utf8_ = 0, a_targets_ = 0;
    std::string uri_list_, plain_;
    bool active_ = false, finished_ = false, dropped_ = false, accepted_ = false, awaiting_status_ = false, dropping_ = false;
    std::chrono::steady_clock::time_point started_, dropped_at_, status_sent_;
};

}  // namespace

std::unique_ptr<DragOut::Backend> CreateX11DragBackend(GLFWwindow* window) {
    auto backend = std::make_unique<X11Drag>();
    if (!backend->Init(window)) return nullptr;
    return backend;
}

}  // namespace cryptvault

#endif  // CRYPTVAULT_HAVE_X11_DND
