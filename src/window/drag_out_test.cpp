// Headless checks for the drag-out wire formats (the live backends need a display server and are
// exercised by an end-to-end scripted test, see README "Testing").
#include "window/drag_out.h"

#include <cstdio>

using cryptvault::DragOut;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main() {
    // text/uri-list: one RFC 2483 line per file, CRLF-terminated, percent-encoded.
    CHECK(DragOut::UriList({"/home/me/a.txt"}) == "file:///home/me/a.txt\r\n");
    CHECK(DragOut::UriList({"/a b/\xC3\xBC.txt", "/x"}) == "file:///a%20b/%C3%BC.txt\r\nfile:///x\r\n");
    CHECK(DragOut::UriList({}).empty());
    // plain-text fallback: the paths, one per line
    CHECK(DragOut::PlainPaths({"/a b", "/c"}) == "/a b\n/c\n");
    // a DragOut with no backend (nothing Init'ed) refuses cleanly instead of crashing
    DragOut d;
    CHECK(!d.Begin({"/x"}) && !d.Active() && std::string(d.BackendName()) == "none");
    bool dropped = true;
    CHECK(!d.TakeFinished(&dropped));
    d.Pump();   // must be a no-op
    std::printf(g_fail ? "FAILED\n" : "drag_out: all checks passed\n");
    return g_fail != 0;
}
