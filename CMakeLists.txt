# CryptVault - a GPU-accelerated, immediate-mode-UI native app shell for
# Windows and Linux, built on GLFW (windowing/input) + wgpu-native
# (WebGPU -> D3D12/Vulkan). No app framework, no Electron/Chromium, no
# GC - a small executable that talks to the GPU directly.
#
# One-time setup (both platforms): download the matching wgpu-native
# binary and drop it in third_party/wgpu_native/lib/ - see
# third_party/wgpu_native/README.md. GLFW is fetched from source
# automatically below.
#
# Build:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release

cmake_minimum_required(VERSION 3.20)
project(cryptvault CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- GLFW (windowing/input) -------------------------------------------------
# Fetched from source and built as part of this project so there's
# nothing to preinstall on either platform. Docs/examples/tests are
# switched off - only the library itself is needed.
include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# X11 alone covers native X11 sessions and XWayland (the common case);
# skip building GLFW's native Wayland backend so this doesn't require
# wayland-scanner on the build machine. Flip this on (and install
# wayland-protocols) if you specifically need a native Wayland surface.
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glfw)

# --- libsodium (AEAD encryption + Argon2id KDF for real vault encryption) --
# Vendored from source (same reasoning as GLFW above: nothing to
# preinstall) via a small community CMake wrapper around upstream
# libsodium, since libsodium itself ships autotools/MSVC-project build
# files rather than a CMakeLists.txt. See src/vault/vault_store.h/.cpp
# for what this is actually used for.
set(SODIUM_DISABLE_TESTS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    libsodium_cmake
    GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
    GIT_TAG        master
    GIT_SUBMODULES libsodium
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libsodium_cmake)

# --- cpp-httplib (local-only HTTP server for the third-party API) ----------
# Single header, vendored the same way as everything else above. Backs
# the localhost-only API described in src/ui/app_ui.cpp's ApiServer -
# see that class's comment for the actual security model (bearer
# token, 127.0.0.1-only, only vaults already unlocked in the app).
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cpp_httplib)

# --- Shared dependency directory (stb, wgpu-native, sqlite, fonts) ---------
# One variable, not five scattered relative paths - the exact bug that
# broke this build (a missing `/` and a wrong `..` count, in five
# different places) is much harder to reintroduce when every
# dependency path derives from a single, validated root instead of
# each spelling out its own "../../Project_Dependency/...".
#
# Override on the command line if your dependency folder lives
# somewhere else: cmake -B build -DPROJECT_DEPS_DIR=D:/deps
set(PROJECT_DEPS_DIR "${CMAKE_SOURCE_DIR}/../../Project_Dependency" CACHE PATH
    "Directory containing shared third-party dependencies (wgpu-native, sqlite, stb, fonts) reused across projects")
# Normalizes away any ../.. and makes the path absolute, so a typo'd
# dot-count fails loudly (a nonexistent absolute path) rather than
# silently resolving to the wrong directory.
get_filename_component(PROJECT_DEPS_DIR "${PROJECT_DEPS_DIR}" ABSOLUTE)
if(NOT EXISTS "${PROJECT_DEPS_DIR}")
    message(FATAL_ERROR "PROJECT_DEPS_DIR does not exist: ${PROJECT_DEPS_DIR}\n"
        "Set it explicitly: cmake -B build -DPROJECT_DEPS_DIR=/path/to/Project_Dependency")
endif()

set(WGPU_NATIVE_LIB_DIR "${PROJECT_DEPS_DIR}/wgpu-windows-x86_64-msvc-release/lib" CACHE PATH
    "Directory containing the downloaded wgpu-native binary for this platform")

find_package(Threads REQUIRED)  # std::thread (llm/llama_engine.cpp) needs -pthread on Linux/MinGW

if(WIN32)
    if(NOT EXISTS "${WGPU_NATIVE_LIB_DIR}/wgpu_native.dll.lib")
        message(FATAL_ERROR
            "Requires wgpu_native.dll.lib in ${WGPU_NATIVE_LIB_DIR}. "
            "Download wgpu-windows-x86_64-msvc-release.zip from the pinned wgpu-native v25.0.2.2 release "
            "and extract wgpu_native.dll and wgpu_native.dll.lib into C:/Users/jains/Downloads/Project/wgpu-windows-x86_64-msvc-release/lib.")
    endif()
    if(NOT EXISTS "${WGPU_NATIVE_LIB_DIR}/wgpu_native.dll")
        message(FATAL_ERROR
            "Requires wgpu_native.dll in ${WGPU_NATIVE_LIB_DIR}. "
            "Extract it from the same wgpu-native v25.0.2.2 Windows release.")
    endif()
endif()

# --- Native folder-picker dialog (Windows: IFileOpenDialog, Linux:
# zenity/kdialog subprocess - see src/platform/folder_picker.h), and
# "open a file with its OS default app" (Windows: ShellExecute, Linux:
# xdg-open - see src/platform/open_file.h) --------------------------------
if(WIN32)
    set(PLATFORM_SOURCES src/platform/folder_picker_win.cpp src/platform/open_file_win.cpp)
elseif(UNIX AND NOT APPLE)
    set(PLATFORM_SOURCES src/platform/folder_picker_linux.cpp src/platform/open_file_linux.cpp)
endif()

add_executable(cryptvault
    src/main.cpp
    src/window/window.cpp
    src/gpu/gpu_context.cpp
    src/ui/app_ui.cpp
    src/ui/ui_renderer.cpp
    src/vault/vault_store.cpp
    ${PLATFORM_SOURCES}
)

# TEMPORARY diagnostic harness for reproducing reported UI bugs with
# scripted synthetic input - see src/debug_repro.cpp's own comment.
# Links against app_ui.cpp/vault_store.cpp/ui_renderer.cpp directly,
# just like cryptvault itself, for a simpler, single-binary repro.
add_executable(debug_repro
    src/debug_repro.cpp
    src/window/window.cpp
    src/gpu/gpu_context.cpp
    src/ui/app_ui.cpp
    src/ui/ui_renderer.cpp
    src/vault/vault_store.cpp
    ${PLATFORM_SOURCES}
)
target_include_directories(debug_repro PRIVATE
    src
    "${PROJECT_DEPS_DIR}/wgpu-windows-x86_64-msvc-release/include/webgpu"
    "${PROJECT_DEPS_DIR}/stb"
)
# app_ui.cpp is compiled directly into this executable (statically) -
# so its exported functions (UiAppCreate etc., marked CRYPTVAULT_UI_API
# in app_ui.h) need to be compiled as dllexport, not the default
# dllimport, on Windows. Same flag cryptvault itself gets below;
# without it MSVC rejects *defining* a function declared dllimport
# ("definition of dllimport function not allowed").
target_compile_definitions(debug_repro PRIVATE CRYPTVAULT_UI_BUILD=1)
target_link_directories(debug_repro PRIVATE ${WGPU_NATIVE_LIB_DIR})
target_link_libraries(debug_repro PRIVATE glfw sodium httplib::httplib)
if(WIN32)
    target_link_libraries(debug_repro PRIVATE
        "${WGPU_NATIVE_LIB_DIR}/wgpu_native.dll.lib"
        userenv ws2_32 bcrypt advapi32 ntdll ole32 shell32
    )
elseif(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(debug_repro PRIVATE
        "${WGPU_NATIVE_LIB_DIR}/libwgpu_native.so"
        Threads::Threads dl X11
    )
    set_target_properties(debug_repro PROPERTIES BUILD_RPATH "$ORIGIN")
    add_custom_command(TARGET debug_repro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WGPU_NATIVE_LIB_DIR}/libwgpu_native.so"
            "$<TARGET_FILE_DIR:debug_repro>"
    )
endif()

# Standalone test/demo executable for the vault storage+encryption
# engine (src/vault/vault_store.*) - deliberately has no dependency on
# GLFW/wgpu/the UI at all, so it can be built and run in a plain
# headless environment to verify the crypto round-trips correctly
# before (or without) touching any GPU/windowing code.
add_executable(vault_store_test
    src/vault/vault_store.cpp
    src/vault/vault_store_test.cpp
)
target_include_directories(vault_store_test PRIVATE src)
target_link_libraries(vault_store_test PRIVATE sodium)

# app_ui.cpp is compiled directly into this executable (statically),
# so its exported functions (UiAppCreate etc., marked CRYPTVAULT_UI_API
# in app_ui.h) need to be compiled as dllexport, not the default
# dllimport, on Windows - same flag debug_repro gets above; without it
# MSVC rejects *defining* a function declared dllimport ("definition
# of dllimport function not allowed").
target_compile_definitions(cryptvault PRIVATE CRYPTVAULT_UI_BUILD=1)
target_include_directories(cryptvault PRIVATE
    src
    "${PROJECT_DEPS_DIR}/wgpu-windows-x86_64-msvc-release/include/webgpu"
    "${PROJECT_DEPS_DIR}/stb"
)
target_link_libraries(cryptvault PRIVATE sodium httplib::httplib)

# CryptVault icon PNGs (see src/ui/ui_renderer.h LoadIcon) are loaded
# at runtime by relative path from the executable's working directory
# - copy them next to the built binaries so `./cryptvault` from the
# build dir (or from wherever it ends up shipped) finds them without
# the user needing to pass an absolute path.
#
# A plain custom_target (ALL, no declared OUTPUT) rather than a
# POST_BUILD step tied to a specific target: POST_BUILD only reliably
# re-runs when that target itself needed rebuilding, so adding a new
# file under assets/ (e.g. a background.png dropped in later) without
# touching any source file could leave it missing from the build
# output even after a `cmake --build` that reports success - a real,
# reported bug, not a hypothetical one. A target with no declared
# OUTPUT re-runs its COMMAND unconditionally every time it's part of
# the requested build, which is exactly the "always fresh" behavior
# wanted here.
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:cryptvault>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:debug_repro>/assets"
)
add_dependencies(cryptvault copy_assets)
add_dependencies(debug_repro copy_assets)

target_link_directories(cryptvault PRIVATE ${WGPU_NATIVE_LIB_DIR})
target_link_libraries(cryptvault PRIVATE glfw)

if(WIN32)
    # v25.0.2.2, wgpu-windows-x86_64-msvc-release.zip -> wgpu_native.dll.lib
    # ole32/shell32 are for src/platform/folder_picker_win.cpp's
    # IFileOpenDialog/SHCreateItemFromParsingName calls.
    target_link_libraries(cryptvault PRIVATE
        "${WGPU_NATIVE_LIB_DIR}/wgpu_native.dll.lib"
        userenv ws2_32 bcrypt advapi32 ntdll ole32 shell32
    )
    add_custom_command(TARGET cryptvault POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WGPU_NATIVE_LIB_DIR}/wgpu_native.dll"
            "$<TARGET_FILE_DIR:cryptvault>"
    )
elseif(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(cryptvault PRIVATE
        "${WGPU_NATIVE_LIB_DIR}/libwgpu_native.a"
        Threads::Threads
        dl
        X11
    )
endif()