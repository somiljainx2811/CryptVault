# CryptVault - a native Linux/Fedora file explorer with encrypted vaults.
#
#   * unrestricted side: a normal file explorer (GPU-rendered window + terminal)
#   * restricted side:   password-locked, XChaCha20-Poly1305 encrypted vaults
#
# Targets
#   cryptvault        the app: opens the window, or runs terminal commands
#                     when given a subcommand (`cryptvault ls`, `cryptvault vault ...`)
#   cryptvault-cli    terminal-only build (libsodium + libstdc++ only: no
#                     GLFW/GPU/X11 - runs over SSH, in containers, anywhere)
#   vault_store_test, fs_ops_test   headless unit tests (ctest)
#
# Fedora quick start:
#   sudo dnf install gcc-c++ cmake ninja-build libsodium-devel glfw-devel \
#        cpp-httplib-devel vulkan-loader mesa-vulkan-drivers xdg-utils zenity
#   (scripts/install-deps.sh does this for Fedora, Debian/Ubuntu, Arch, openSUSE)
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#   sudo cmake --install build          # or: cmake --install build --prefix ~/.local
#
# Anything not found on the system (libsodium, GLFW >= 3.4, cpp-httplib) is
# fetched and built from source automatically; wgpu-native and stb_image are
# downloaded as pinned, checksum-verified files at configure time.
# Terminal-only build (no GPU stack needed at all):
#   cmake -B build -DCRYPTVAULT_BUILD_GUI=OFF

cmake_minimum_required(VERSION 3.20)
project(cryptvault VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CRYPTVAULT_BUILD_GUI        "Build the windowed explorer (needs wgpu-native + GLFW)" ON)
option(CRYPTVAULT_BUILD_TESTS      "Build the headless unit tests" ON)
option(CRYPTVAULT_BUILD_DEBUG_REPRO "Build the scripted-input UI debug harness" OFF)
option(CRYPTVAULT_ENABLE_HARFBUZZ "Shape complex scripts (Arabic joining, Indic, Thai...) with HarfBuzz" ON)
option(CRYPTVAULT_USE_SYSTEM_LIBS  "Prefer distro libsodium/GLFW/cpp-httplib when available" ON)
option(CRYPTVAULT_FETCH_WGPU       "Download the pinned wgpu-native release if it is not already present" ON)

include(FetchContent)
find_package(Threads REQUIRED)

# --- libsodium (always needed: it is the vault engine's crypto) --------------
set(CV_SODIUM_TARGET "")
if(CRYPTVAULT_USE_SYSTEM_LIBS)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SODIUM IMPORTED_TARGET libsodium)
        if(SODIUM_FOUND)
            set(CV_SODIUM_TARGET PkgConfig::SODIUM)
            message(STATUS "libsodium: system ${SODIUM_VERSION}")
        endif()
    endif()
endif()
if(NOT CV_SODIUM_TARGET)
    message(STATUS "libsodium: fetching from source")
    set(SODIUM_DISABLE_TESTS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(libsodium_cmake
        GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
        GIT_TAG        master
        GIT_SUBMODULES libsodium
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(libsodium_cmake)
    set(CV_SODIUM_TARGET sodium)
endif()

# --- core library: vault engine + explorer engine (no UI, no GPU) -------------
add_library(cryptvault_core STATIC
    src/vault/vault_store.cpp
    src/explorer/fs_ops.cpp)
target_include_directories(cryptvault_core PUBLIC src)
target_link_libraries(cryptvault_core PUBLIC ${CV_SODIUM_TARGET})
if(MSVC)
    target_compile_options(cryptvault_core PRIVATE /W3)
else()
    target_compile_options(cryptvault_core PRIVATE -Wall -Wextra)
endif()

# --- platform helpers (folder picker, "open with default app") ----------------
if(WIN32)
    set(PLATFORM_SOURCES src/platform/folder_picker_win.cpp src/platform/open_file_win.cpp src/platform/clipboard_files.cpp)
elseif(UNIX AND NOT APPLE)
    set(PLATFORM_SOURCES src/platform/folder_picker_linux.cpp src/platform/open_file_linux.cpp src/platform/clipboard_files.cpp)
endif()

# --- terminal front-end (POSIX only) -------------------------------------------
if(UNIX)
    add_library(cryptvault_cli_lib STATIC src/cli/cli.cpp src/platform/open_file_linux.cpp)
    target_link_libraries(cryptvault_cli_lib PUBLIC cryptvault_core)
    target_compile_options(cryptvault_cli_lib PRIVATE -Wall -Wextra)

    add_executable(cryptvault-cli src/cli/cli_main.cpp)
    target_link_libraries(cryptvault-cli PRIVATE cryptvault_cli_lib)
    install(TARGETS cryptvault-cli RUNTIME DESTINATION bin)
endif()

# --- tests ---------------------------------------------------------------------
if(CRYPTVAULT_BUILD_TESTS)
    enable_testing()
    add_executable(vault_store_test src/vault/vault_store_test.cpp)
    target_link_libraries(vault_store_test PRIVATE cryptvault_core)
    add_test(NAME vault_store COMMAND vault_store_test)

    if(UNIX)
        add_executable(fs_ops_test src/explorer/fs_ops_test.cpp)
        target_link_libraries(fs_ops_test PRIVATE cryptvault_core)
        add_test(NAME fs_ops COMMAND fs_ops_test)

        add_executable(bidi_test src/ui/bidi_test.cpp src/ui/bidi.cpp src/ui/utf8.cpp)
        target_include_directories(bidi_test PRIVATE src)
        add_test(NAME bidi COMMAND bidi_test)

        add_executable(clipboard_files_test src/platform/clipboard_files_test.cpp src/platform/clipboard_files.cpp)
        target_include_directories(clipboard_files_test PRIVATE src)
        add_test(NAME clipboard_files COMMAND clipboard_files_test)

        find_program(BASH_PROGRAM bash)
        if(BASH_PROGRAM)
            add_test(NAME cli_smoke
                     COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/cli_smoke.sh $<TARGET_FILE:cryptvault-cli>)
        endif()
    endif()
endif()

# --- the windowed app ------------------------------------------------------------
if(NOT CRYPTVAULT_BUILD_GUI)
    message(STATUS "GUI disabled (CRYPTVAULT_BUILD_GUI=OFF): building terminal tools and tests only")
else()
    # GLFW ----------------------------------------------------------------------
    # GLFW >= 3.4 chooses Wayland or X11 at runtime (see window.cpp), and
    # src/gpu/gpu_context.cpp builds the matching GPU surface. A distro GLFW
    # (Fedora, Arch, Debian 13+, Ubuntu 24.10+, openSUSE) normally has both
    # backends. When we have to build GLFW ourselves we enable Wayland too
    # if its build tools are present, otherwise X11 only (runs via XWayland).
    set(CV_GLFW_TARGET "")
    if(CRYPTVAULT_USE_SYSTEM_LIBS)
        find_package(glfw3 3.4 QUIET)
        if(glfw3_FOUND)
            set(CV_GLFW_TARGET glfw)
            message(STATUS "GLFW: system ${glfw3_VERSION}")
        endif()
    endif()
    if(NOT CV_GLFW_TARGET)
        message(STATUS "GLFW: fetching 3.4 from source")
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        if(UNIX AND NOT APPLE)
            set(CV_WAYLAND_OK OFF)
            find_program(CV_WAYLAND_SCANNER wayland-scanner)
            find_package(PkgConfig QUIET)
            if(CV_WAYLAND_SCANNER AND PKG_CONFIG_FOUND)
                pkg_check_modules(CV_WL QUIET wayland-client wayland-cursor wayland-egl xkbcommon wayland-protocols)
                if(CV_WL_FOUND)
                    set(CV_WAYLAND_OK ON)
                endif()
            endif()
            set(GLFW_BUILD_WAYLAND ${CV_WAYLAND_OK} CACHE BOOL "" FORCE)
            set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
            if(CV_WAYLAND_OK)
                message(STATUS "GLFW: building with native Wayland + X11")
            else()
                message(STATUS "GLFW: Wayland build tools not found (wayland-scanner, wayland-protocols, libxkbcommon, libwayland dev) - X11 only (runs through XWayland)")
            endif()
        endif()
        FetchContent_Declare(glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG        3.4
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(glfw)
        set(CV_GLFW_TARGET glfw)
    endif()

    # cpp-httplib (the opt-in localhost API) ------------------------------------------
    set(CV_HTTPLIB_TARGET "")
    if(CRYPTVAULT_USE_SYSTEM_LIBS)
        find_package(httplib 0.15 QUIET CONFIG)
        if(httplib_FOUND)
            set(CV_HTTPLIB_TARGET httplib::httplib)
            message(STATUS "cpp-httplib: system ${httplib_VERSION}")
        endif()
    endif()
    if(NOT CV_HTTPLIB_TARGET)
        message(STATUS "cpp-httplib: fetching v0.18.5")
        set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)  # a build-time dependency, not part of our install
        FetchContent_Declare(cpp_httplib
            GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
            GIT_TAG        v0.18.5
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(cpp_httplib)
        set(CV_HTTPLIB_TARGET httplib::httplib)
    endif()


    # HarfBuzz (text shaping for non-ASCII labels) -------------------------------------
    # Optional: without it, non-ASCII text is drawn one character at a time
    # (fine for Latin/Greek/Cyrillic/CJK; no Arabic joining or Indic layout).
    set(CV_HARFBUZZ_TARGET "")
    if(CRYPTVAULT_ENABLE_HARFBUZZ)
        if(CRYPTVAULT_USE_SYSTEM_LIBS AND PKG_CONFIG_FOUND)
            pkg_check_modules(HARFBUZZ IMPORTED_TARGET harfbuzz)
            if(HARFBUZZ_FOUND)
                set(CV_HARFBUZZ_TARGET PkgConfig::HARFBUZZ)
                message(STATUS "HarfBuzz: system ${HARFBUZZ_VERSION}")
            endif()
        endif()
        if(NOT CV_HARFBUZZ_TARGET)
            message(STATUS "HarfBuzz: fetching 10.1.0 and building its amalgamated source")
            FetchContent_Declare(harfbuzz_src
                GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
                GIT_TAG        10.1.0
                GIT_SHALLOW    TRUE)
            FetchContent_GetProperties(harfbuzz_src)
            if(NOT harfbuzz_src_POPULATED)
                FetchContent_Populate(harfbuzz_src)
            endif()
            add_library(cv_harfbuzz STATIC "${harfbuzz_src_SOURCE_DIR}/src/harfbuzz.cc")
            target_include_directories(cv_harfbuzz SYSTEM PUBLIC "${harfbuzz_src_SOURCE_DIR}/src")
            # Only core shaping + OpenType layout: no FreeType/ICU/GLib/Cairo needed.
            target_compile_definitions(cv_harfbuzz PRIVATE HB_NO_MT=0 HAVE_OT=1 HB_NO_PRAGMA_PUSH_POP=0)
            set_target_properties(cv_harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON)
            target_link_libraries(cv_harfbuzz PRIVATE Threads::Threads)
            set(CV_HARFBUZZ_TARGET cv_harfbuzz)
        endif()
    endif()

    # stb_image (icon/background decoding) -------------------------------------------
    # -DSTB_DIR=/dir/containing/stb_image.h to use your own copy; otherwise a
    # pinned revision is downloaded and verified.
    set(STB_DIR "" CACHE PATH "Directory containing stb_image.h and stb_truetype.h (downloaded if empty)")
    if(NOT STB_DIR OR NOT EXISTS "${STB_DIR}/stb_image.h" OR NOT EXISTS "${STB_DIR}/stb_truetype.h")
        set(STB_DIR "${CMAKE_BINARY_DIR}/_deps/stb")
        if(NOT EXISTS "${STB_DIR}/stb_truetype.h")
            message(STATUS "stb_truetype.h: downloading pinned revision")
            file(DOWNLOAD
                "https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h"
                "${STB_DIR}/stb_truetype.h"
                EXPECTED_HASH SHA256=ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab
                STATUS stbtt_status)
            list(GET stbtt_status 0 stbtt_code)
            if(NOT stbtt_code EQUAL 0)
                file(REMOVE "${STB_DIR}/stb_truetype.h")
                message(FATAL_ERROR "Could not download stb_truetype.h (${stbtt_status}). Download it manually into -DSTB_DIR=<dir>.")
            endif()
        endif()
        if(NOT EXISTS "${STB_DIR}/stb_image.h")
            message(STATUS "stb_image.h: downloading pinned revision")
            file(DOWNLOAD
                "https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h"
                "${STB_DIR}/stb_image.h"
                EXPECTED_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
                STATUS stb_status)
            list(GET stb_status 0 stb_code)
            if(NOT stb_code EQUAL 0)
                file(REMOVE "${STB_DIR}/stb_image.h")
                message(FATAL_ERROR "Could not download stb_image.h (${stb_status}). Download it manually and pass -DSTB_DIR=<dir>.")
            endif()
        endif()
    endif()

    # wgpu-native -----------------------------------------------------------------------
    # WGPU_NATIVE_DIR must contain include/webgpu/{webgpu,wgpu}.h and lib/.
    set(CV_WGPU_VERSION "v25.0.2.2")
    if(WIN32)
        set(CV_WGPU_ARCHIVE "wgpu-windows-x86_64-msvc-release")
        set(CV_WGPU_SHA256 "")
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            set(CV_WGPU_ARCHIVE "wgpu-linux-aarch64-release")
            set(CV_WGPU_SHA256 "da6d953ba4a4c1a69e466e3c6a73c9cda4a8f079c9aa33c2da0bf84c27354c1c")
        else()
            set(CV_WGPU_ARCHIVE "wgpu-linux-x86_64-release")
            set(CV_WGPU_SHA256 "78a2a4d90f3a0a67af2ab2634fe09873ced3baceac8822e890cdea34d8ba9834")
        endif()
    endif()
    set(WGPU_NATIVE_DIR "${CMAKE_SOURCE_DIR}/third_party/wgpu_native" CACHE PATH
        "wgpu-native release root (contains include/ and lib/)")
    if(NOT EXISTS "${WGPU_NATIVE_DIR}/include/webgpu/wgpu.h")
        if(CRYPTVAULT_FETCH_WGPU AND CV_WGPU_SHA256)
            message(STATUS "wgpu-native ${CV_WGPU_VERSION}: downloading (~15 MB)")
            set(zip "${CMAKE_BINARY_DIR}/_deps/${CV_WGPU_ARCHIVE}.zip")
            file(DOWNLOAD
                "https://github.com/gfx-rs/wgpu-native/releases/download/${CV_WGPU_VERSION}/${CV_WGPU_ARCHIVE}.zip"
                "${zip}" EXPECTED_HASH SHA256=${CV_WGPU_SHA256} SHOW_PROGRESS STATUS wgpu_status)
            list(GET wgpu_status 0 wgpu_code)
            if(NOT wgpu_code EQUAL 0)
                file(REMOVE "${zip}")
                message(FATAL_ERROR "wgpu-native download failed (${wgpu_status}).\n"
                    "Download ${CV_WGPU_ARCHIVE}.zip from the wgpu-native ${CV_WGPU_VERSION} release, extract it and pass "
                    "-DWGPU_NATIVE_DIR=<dir>, or build without the window: -DCRYPTVAULT_BUILD_GUI=OFF")
            endif()
            file(MAKE_DIRECTORY "${WGPU_NATIVE_DIR}")
            file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${WGPU_NATIVE_DIR}")
        else()
            message(FATAL_ERROR "wgpu-native not found in ${WGPU_NATIVE_DIR}.\n"
                "Extract the ${CV_WGPU_VERSION} ${CV_WGPU_ARCHIVE}.zip release there (or set -DWGPU_NATIVE_DIR=), "
                "enable -DCRYPTVAULT_FETCH_WGPU=ON, or build without the window: -DCRYPTVAULT_BUILD_GUI=OFF")
        endif()
    endif()
    if(WIN32)
        set(WGPU_LIBS "${WGPU_NATIVE_DIR}/lib/wgpu_native.dll.lib")
    else()
        set(WGPU_LIBS "${WGPU_NATIVE_DIR}/lib/libwgpu_native.a")
    endif()
    if(NOT EXISTS "${WGPU_LIBS}")
        message(FATAL_ERROR "Expected ${WGPU_LIBS} - check WGPU_NATIVE_DIR")
    endif()

    # app executable ---------------------------------------------------------------------
    set(CV_UI_SOURCES
        src/window/window.cpp
        src/gpu/gpu_context.cpp
        src/ui/app_ui.cpp
        src/ui/ui_renderer.cpp
        src/ui/unicode_font.cpp
        src/ui/utf8.cpp
        src/ui/bidi.cpp
        src/ui/media_tools.cpp
        ${PLATFORM_SOURCES})

    add_executable(cryptvault src/main.cpp ${CV_UI_SOURCES})
    # GLFW_INCLUDE_NONE: we use WebGPU, not OpenGL - without it glfw3.h wants GL/gl.h (mesa-libGL-devel).
    target_compile_definitions(cryptvault PRIVATE CRYPTVAULT_UI_BUILD=1 GLFW_INCLUDE_NONE)
    target_include_directories(cryptvault PRIVATE src "${WGPU_NATIVE_DIR}/include/webgpu" "${STB_DIR}")
    target_link_libraries(cryptvault PRIVATE cryptvault_core ${CV_GLFW_TARGET} ${CV_HTTPLIB_TARGET}
                                             ${WGPU_LIBS} Threads::Threads)
    if(CV_HARFBUZZ_TARGET)
        target_link_libraries(cryptvault PRIVATE ${CV_HARFBUZZ_TARGET})
        target_compile_definitions(cryptvault PRIVATE CRYPTVAULT_HAVE_HARFBUZZ=1)
    endif()
    if(UNIX)
        target_link_libraries(cryptvault PRIVATE cryptvault_cli_lib)
    endif()
    if(WIN32)
        target_link_libraries(cryptvault PRIVATE userenv ws2_32 bcrypt advapi32 ntdll ole32 shell32)
        add_custom_command(TARGET cryptvault POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WGPU_NATIVE_DIR}/lib/wgpu_native.dll" "$<TARGET_FILE_DIR:cryptvault>")
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(cryptvault PRIVATE dl)
    endif()

    # Icons are found next to the executable (build tree) or in
    # ../share/cryptvault/assets (installed) - see AssetsRoot() in app_ui.cpp.
    # A target with no declared OUTPUT re-runs every build, so a new file
    # dropped into assets/ is never silently missing from the build dir.
    add_custom_target(copy_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:cryptvault>/assets")
    add_dependencies(cryptvault copy_assets)

    if(CRYPTVAULT_BUILD_DEBUG_REPRO)
        add_executable(debug_repro src/debug_repro.cpp ${CV_UI_SOURCES})
        target_compile_definitions(debug_repro PRIVATE CRYPTVAULT_UI_BUILD=1 GLFW_INCLUDE_NONE)
        target_include_directories(debug_repro PRIVATE src "${WGPU_NATIVE_DIR}/include/webgpu" "${STB_DIR}")
        target_link_libraries(debug_repro PRIVATE cryptvault_core ${CV_GLFW_TARGET} ${CV_HTTPLIB_TARGET}
                                                  ${WGPU_LIBS} Threads::Threads)
        if(CV_HARFBUZZ_TARGET)
            target_link_libraries(debug_repro PRIVATE ${CV_HARFBUZZ_TARGET})
            target_compile_definitions(debug_repro PRIVATE CRYPTVAULT_HAVE_HARFBUZZ=1)
        endif()
        if(UNIX AND NOT APPLE)
            target_link_libraries(debug_repro PRIVATE dl)
        endif()
        add_custom_command(TARGET debug_repro POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:debug_repro>/assets")
    endif()

    install(TARGETS cryptvault RUNTIME DESTINATION bin)
endif()

# --- install: assets, desktop entry, icon, shell completions (Linux) -------------------
if(UNIX AND NOT APPLE)
    install(DIRECTORY assets/ DESTINATION share/cryptvault/assets)
    install(FILES packaging/cryptvault.desktop DESTINATION share/applications)
    install(FILES assets/icons/vault.png DESTINATION share/icons/hicolor/48x48/apps RENAME cryptvault.png)
    install(FILES packaging/completions/cryptvault.bash
            DESTINATION share/bash-completion/completions RENAME cryptvault)
    install(FILES packaging/completions/_cryptvault DESTINATION share/zsh/site-functions)
endif()
