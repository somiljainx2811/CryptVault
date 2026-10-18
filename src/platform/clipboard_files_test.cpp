#include "platform/clipboard_files.h"
#include <cstdio>
#include <cstdlib>
using namespace platform;
int fails=0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL line %d: %s\n",__LINE__,#c); ++fails;} }while(0)
int main() {
    CHECK(FileUriFromPath("/a b/ü%.txt") == "file:///a%20b/%C3%BC%25.txt");
    CHECK(PathFromFileUri("file:///a%20b/%C3%BC%25.txt\r\n") == "/a b/ü%.txt");
    CHECK(PathFromFileUri("file://localhost/x") == "/x" && PathFromFileUri("file://otherhost/x").empty() && PathFromFileUri("http://x/y").empty());
    bool cut = false;
    auto p = ParseClipboardPayload(BuildClipboardPayload({"/a", "/b c"}, true, true), &cut);
    CHECK(p.size() == 2 && p[1] == "/b c" && cut);
    p = ParseClipboardPayload(BuildClipboardPayload({"/a", "/b c"}, false, false), &cut);
    CHECK(p.size() == 2 && p[0] == "/a" && !cut);
    CHECK(BuildClipboardPayload({"/a"}, false, true) == "copy\nfile:///a");
    std::printf(fails ? "FAILED %d\n" : "clipboard_files: all checks passed\n", fails);
    return fails != 0;
}
