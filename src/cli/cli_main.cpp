// cryptvault-cli: the terminal-only build of CryptVault. No GLFW, no
// GPU, no X11 - links just libsodium + the standard library, so it runs
// over SSH, in a container, or on a machine with no graphics stack.
#include <cstdio>

#include "cli/cli.h"

int main(int argc, char** argv) {
    cvcli::GuiLaunch launch;
    int rc = cvcli::Run(argc, argv, launch);
    if (rc == cvcli::kLaunchGui) {
        std::fprintf(stderr,
                     "cryptvault-cli: this is the terminal-only build; the window lives in the "
                     "'cryptvault' binary.\nRun 'cryptvault-cli --help' for the terminal commands.\n");
        return 2;
    }
    return rc;
}
