// cli: the terminal front-end for CryptVault. One binary, two faces:
//
//   cryptvault                 -> GPU file explorer (the GUI)
//   cryptvault ls ~/Downloads  -> prints to the terminal, never opens a window
//
// Subcommands cover both halves of the app:
//   unrestricted side:  ls, info, places, open, trash {put,list,restore,empty}
//   restricted side:    vault {list,create,register,forget,ls,tree,add,put,get,
//                              mkdir,rm,mv,passwd}
// plus `completion bash|zsh` and `gui`. Everything is stateless: each
// invocation unlocks what it needs, does its work, scrubs the keys and
// exits - there is no daemon and nothing stays unlocked between commands.
#pragma once

#include <string>

namespace cvcli {

// Set by `cryptvault [PATH]` / `cryptvault gui ...` when the caller wants
// the window rather than terminal output.
struct GuiLaunch {
    std::string start_path;     // empty = $HOME
    bool vault_side = false;    // start on the restricted side
};

// Returns the process exit code, or kLaunchGui when main() should
// start the GUI using `launch`. Never touches the GPU/window itself, so
// it works over SSH and in a headless build.
constexpr int kLaunchGui = -1;
int Run(int argc, char** argv, GuiLaunch& launch);

}  // namespace cvcli
