// app_ui.cpp - CryptVault UI implementation.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <sodium.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

// httplib.h pulls in <windows.h> on Windows (for Winsock), which
// #defines LoadIcon as LoadIconA (the classic Win32 ANSI/Unicode
// macro-redirection trick, same family as CreateWindow/SendMessage/
// GetObject). Left alone, every ui.LoadIcon(...) call further down
// this file silently becomes ui.LoadIconA(...) - a method that
// doesn't exist - which compiles fine (it's just a member-call
// expression) but fails at *link* time, and only in this translation
// unit: ui_renderer.cpp, which actually defines UiRenderer::LoadIcon,
// never includes httplib.h and so never sees this macro, meaning its
// exported symbol is the real (unmangled) LoadIcon. That mismatch is
// exactly what produces an "unresolved external symbol ...LoadIconA"
// error despite the method obviously existing. #undef immediately
// after the include that introduces it (and the explicit <windows.h>
// just above, needed for ExecutableDir() below - same macro either
// way), rather than e.g. renaming the method, so this can't quietly
// reappear if another Windows-header-pulling dependency gets added
// later without anyone remembering why.
#ifdef LoadIcon
#undef LoadIcon
#endif

#include "ui/app_ui.h"
#include "ui/animation.h"
#include "ui/ui_renderer.h"
#include "platform/folder_picker.h"
#include "platform/open_file.h"
#include "vault/vault_store.h"

namespace {

namespace anim = appshell::anim;  // so anim::MoveTowards/Ease/duration::* resolve below



// Two fixed window sizes the top-left resize-toggle button (see
// DrawResizeToggle) switches between. 400x300 is the startup
// default; clicking the button flips to 900x600 and back.
//
// NOTE: main.cpp keeps its own copy of these same four numbers (it
// can't depend on this UI DLL's headers, and this file can't depend
// on main.cpp's) - keep them in sync by hand.
constexpr int kSmallWindowWidth = 400;
constexpr int kSmallWindowHeight = 300;
constexpr int kLargeWindowWidth = 900;
constexpr int kLargeWindowHeight = 600;

// Thick decorative frame replacing the OS title bar (see WindowConfig
// below - decorated=false). Kept as a fixed pixel thickness (not
// scaled with the window) per the original reference mockup.
//
// NOTE: main.cpp keeps its own copy of kBorder too - same
// out-of-sync risk as above.
constexpr float kBorder = 17.0f;
constexpr appshell::UiColor kBorderColor{0.200f, 0.247f, 0.302f, 1.0f};  // ~rgb(77, 51, 62)

// Everything drawn by the screen/toolbar/tab/grid code below lives in
// "content space": (0,0) is the top-left corner *inside* the border,
// and AppState::content_width/content_height (set once per frame in
// UiAppBeginFrame, from the *actual* window size - not a compile-time
// constant, now that the window can be resized at runtime) is its
// size, not the OS window's full size. See ContentRenderer.

constexpr float kToolbarHeight = 35.0f;
constexpr float kTabBarHeight = 30.0f;
constexpr float kStatusBarHeight = 20.0f;
constexpr float kAddressBarHeight = 20.0f;

// Thin coordinate-offsetting wrapper around UiRenderer so all the
// screen-drawing code can be written in content-space (see kBorder)
// without every call site adding the offset by hand. The border
// itself is drawn straight against the real UiRenderer, in window
// space, before this wrapper ever gets used - see DrawFrame in
// main().
class ContentRenderer {
public:
    ContentRenderer(appshell::UiRenderer& ui, float offset_x, float offset_y)
        : ui_(ui), offset_x_(offset_x), offset_y_(offset_y) {}

    void DrawRect(float x, float y, float w, float h, appshell::UiColor color) {
        ui_.DrawRect(x + offset_x_, y + offset_y_, w, h, color);
    }

    void DrawRoundedRect(float x, float y, float w, float h, float radius, appshell::UiColor color) {
        ui_.DrawRoundedRect(x + offset_x_, y + offset_y_, w, h, radius, color);
    }

    void DrawImage(float x, float y, float w, float h, int icon,
                   appshell::UiColor tint = appshell::UiColor{2.0f, 2.0f, 2.0f, 2.0f}) {
        ui_.DrawImage(x + offset_x_, y + offset_y_, w, h, icon, tint);
    }

    void DrawLabel(float x, float y, const std::string& text, appshell::UiColor color,
                    float pixel_size = 1.0f) {
        ui_.DrawLabel(x + offset_x_, y + offset_y_, text, color, pixel_size);
    }

    float MeasureText(const std::string& text, float pixel_size = 1.0f) {
        return ui_.MeasureText(text, pixel_size);
    }

    void PushClipRect(float x, float y, float w, float h) {
        ui_.PushClipRect(x + offset_x_, y + offset_y_, w, h);
    }

    void PopClipRect() {
        ui_.PopClipRect();
    }

    // Escape hatch to the underlying UiRenderer for the rare
    // operations ContentRenderer's own wrapper API doesn't cover
    // (right now: loading a new icon/background image picked at
    // runtime - see DrawSettingsPanel's background picker grid).
    // Everything else should go through the methods above, which
    // apply this ContentRenderer's offset automatically; this doesn't.
    appshell::UiRenderer& Raw() { return ui_; }

private:
    appshell::UiRenderer& ui_;
    float offset_x_;
    float offset_y_;
};

// --- palette -----------------------------------------------------------
// Approximate, hand-picked to match the reference mockup's dark,
// slightly desaturated toolbar/tab/content bands - no background
// wallpaper image is baked in (see LoadBackgroundIfPresent below), so
// the content band is a flat near-black instead of the mockup's
// purple art.
namespace palette {
constexpr appshell::UiColor kToolbar{0.14f, 0.16f, 0.13f, 1.0f};
constexpr appshell::UiColor kAddressBar{0.08f, 0.09f, 0.08f, 1.0f};
constexpr appshell::UiColor kTabBarBg{0.30f, 0.33f, 0.38f, 1.0f};
constexpr appshell::UiColor kTabActive{0.47f, 0.52f, 0.60f, 1.0f};
constexpr appshell::UiColor kTabInactive{0.34f, 0.37f, 0.43f, 1.0f};
constexpr appshell::UiColor kContentBg{0.07f, 0.07f, 0.09f, 1.0f};
constexpr appshell::UiColor kStatusBar{0.32f, 0.32f, 0.32f, 1.0f};
constexpr appshell::UiColor kCellHover{1.0f, 1.0f, 1.0f, 0.06f};
constexpr appshell::UiColor kCellSelected{1.0f, 1.0f, 1.0f, 0.10f};
constexpr appshell::UiColor kTextPrimary{0.90f, 0.91f, 0.93f, 1.0f};
constexpr appshell::UiColor kTextMuted{0.62f, 0.64f, 0.68f, 1.0f};
constexpr appshell::UiColor kIconTint{0.72f, 0.72f, 0.74f, 1.0f};
constexpr appshell::UiColor kIconTintHover{0.88f, 0.88f, 0.90f, 1.0f};
constexpr appshell::UiColor kAccent{0.35f, 0.78f, 0.85f, 1.0f};  // sliding tab indicator, toasts
constexpr appshell::UiColor kDanger{0.75f, 0.30f, 0.28f, 1.0f};  // destructive confirm buttons
constexpr appshell::UiColor kDangerHover{0.85f, 0.36f, 0.34f, 1.0f};
}  // namespace palette

// --- icon handles --------------------------------------------------------
// Filled in once at startup by LoadIcons(); -1 means "failed to load"
// (DrawImage silently no-ops on a -1 handle, so a missing icon just
// leaves a blank space rather than crashing).
struct Icons {
    int folder = -1;
    int vault = -1;
    int arrow_back = -1;
    int arrow_up = -1;
    int home = -1;
    int settings = -1;
    int search = -1;
    int refresh = -1;
    int plus = -1;
    int new_tab = -1;
    int background = -1;  // optional; see LoadBackgroundIfPresent
};

// Where the running executable actually lives, so assets can be found
// by a path relative to THAT rather than to the process's current
// working directory - which varies with how the app happens to be
// launched (double-clicked in Explorer vs. run from a terminal
// sitting in some other folder vs. a shortcut with its own "Start
// in") and isn't something this app controls or should have to rely
// on getting right. A user reporting "the background image I added
// still isn't showing" after confirming the file itself decodes fine
// is exactly the symptom of CWD not being what was assumed - resolving
// against the executable's own directory removes that variable
// entirely. Falls back to the current working directory (the old
// behavior) if the OS query itself fails for some reason, rather than
// producing an unusable empty/invalid path.
std::filesystem::path ExecutableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(buf).parent_path();
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        return std::filesystem::current_path();
    }
    buf[static_cast<size_t>(len)] = '\0';
    return std::filesystem::path(buf).parent_path();
#endif
}

bool LoadIcons(appshell::UiRenderer& ui, Icons& icons) {
    std::filesystem::path assets = ExecutableDir() / "assets";
    icons.folder = ui.LoadIcon((assets / "icons" / "folder.png").string());
    icons.vault = ui.LoadIcon((assets / "icons" / "vault.png").string());
    icons.arrow_back = ui.LoadIcon((assets / "icons" / "arrow_back.png").string());
    icons.arrow_up = ui.LoadIcon((assets / "icons" / "arrow_up.png").string());
    icons.home = ui.LoadIcon((assets / "icons" / "home.png").string());
    icons.settings = ui.LoadIcon((assets / "icons" / "settings.png").string());
    icons.search = ui.LoadIcon((assets / "icons" / "search.png").string());
    icons.refresh = ui.LoadIcon((assets / "icons" / "refresh.png").string());
    icons.plus = ui.LoadIcon((assets / "icons" / "plus.png").string());
    icons.new_tab = ui.LoadIcon((assets / "icons" / "new_tab.png").string());

    // Every one of these is required for the UI to look right - bail
    // out with a clear message rather than silently drawing gaps.
    bool ok = icons.folder >= 0 && icons.vault >= 0 && icons.arrow_back >= 0
              && icons.arrow_up >= 0 && icons.home >= 0 && icons.settings >= 0
              && icons.search >= 0 && icons.refresh >= 0 && icons.plus >= 0
              && icons.new_tab >= 0;
    if (!ok) {
        std::fprintf(stderr, "[cryptvault] one or more icons failed to load: %s\n",
                     ui.last_error().c_str());
    }
    return ok;
}

// --- app state -----------------------------------------------------------

enum class Screen { VaultList, VaultFolder };

// A vault as CryptVault's own UI-level bookkeeping sees it: a display
// name plus which real directory on disk it lives in (see
// src/vault/vault_store.h - that's where the actual encrypted content
// lives and where all the crypto happens). This is deliberately NOT
// the vault's decrypted contents - listing vaults on VaultList never
// requires a password, only opening one does (see
// AppState::OpenSession and RequestOpenVault below). The folder grid
// inside an opened vault uses vaultstore::TreeNode directly instead
// of a struct like this one - see that type's own comment for why it
// carries a UI-only hover_amount field despite living in the
// otherwise UI-agnostic vault_store module.
struct Vault {
    std::string name;
    std::string path;
    float hover_amount = 0.0f;  // see vaultstore::TreeNode::hover_amount's comment
};

struct AppState {
    appshell::UiInput input;
    Screen screen = Screen::VaultList;

    // Current frame's clock, copied from UiAppInput each
    // UiAppBeginFrame (see app_ui.h) - the single source of "now"
    // every anim::Timeline in this state should be measured against.
    // Not touched anywhere else; screens/widgets read it, they don't
    // set it.
    double now_seconds = 0.0;
    float dt_seconds = 0.0f;

    // Content-space size for this frame, derived from the *actual*
    // window size (see UiAppBeginFrame) now that the window can be
    // resized at runtime via the resize-toggle button - everything
    // below that used to read a compile-time kContentWidth/Height
    // constant now reads this instead. Defaulted to the small/startup
    // size purely so nothing divides-by-zero if a screen somehow got
    // drawn before the first UiAppBeginFrame (shouldn't happen).
    float content_width = kSmallWindowWidth - 2.0f * kBorder;
    float content_height = kSmallWindowHeight - 2.0f * kBorder;

    // Set by DrawResizeToggle on click; consumed (and cleared) by
    // UiAppConsumeResizeRequest, which main.cpp polls once per frame
    // - see app_ui.h. big_mode is the *current* toggle state (false =
    // small/400x300, true = large/900x600), kept here (not derived
    // from the live window size) so the button's own label reflects
    // what it's about to do even on the same frame it's clicked.
    bool big_mode = false;
    bool resize_requested = false;

    // VaultList screen: every vault the user has added, flat - no
    // directory-tab grouping above it (there used to be one, "DirTab";
    // removed so opening a vault is a single click straight from this
    // list rather than picking a source folder first). No seeded
    // vaults - starts completely empty; see DrawVaultListScreen's
    // "ADD VAULT" tile and the toolbar's "ADD VAULT" button. Loaded
    // from and saved to a small local list of "vaults I know about"
    // (see LoadKnownVaults/SaveKnownVaults) so real vaults - which
    // exist on disk independent of this app running at all - aren't
    // orphaned from the UI the moment the app restarts.
    std::vector<Vault> vaults;
    int selected_vault = -1;

    // VaultFolder screen: currently-open tabs, each just an index into
    // `vaults` above - a plain vector<int> (not a wrapper struct) is
    // enough now that there's no DirTab layer to also record. More
    // than one tab can point at the same vault index at once (see the
    // toolbar's "+" in DrawVaultTabs, which duplicates the active
    // tab's vault rather than dedup-ing against existing tabs). A tab
    // pointing at a given vault_index only means "this vault is
    // showing in a tab" - whether it's actually *unlocked* is a
    // separate question, answered by open_sessions below.
    std::vector<int> vault_tabs;
    int active_vault_tab = 0;

    // Parallel to vault_tabs (same index for the same tab) - which
    // folder each tab is currently browsing, as a path of folder
    // names walked from the vault's root (e.g. {"Photos", "2025"}).
    // Empty means "at the vault's root". Deliberately per-TAB, not
    // per-vault: two tabs open on the same vault (see DrawVaultTabs'
    // "+") should browse independently of each other - that's the
    // actual point of having more than one tab on one vault, and a
    // single shared path would undermine it (navigating into a
    // subfolder in one tab would - confusingly - also relocate every
    // other tab on that same vault). See CurrentFolderPath()/
    // CurrentFolder() for how this gets resolved to an actual
    // vaultstore::TreeNode. Kept in sync (same size, same index
    // correspondence) with vault_tabs at every site that
    // pushes/erases a tab - see PushVaultTab/EraseVaultTab, the only
    // two places allowed to touch vault_tabs directly.
    std::vector<std::vector<std::string>> tab_folder_paths;

    // Real, decrypted temp-file copies written by OpenFileFromVault
    // (see its own comment for the full "why does a decrypted copy
    // exist on disk at all" explanation) so a double-clicked file can
    // be opened with the OS's default application for its type.
    // Cleaned up best-effort in UiAppDestroy - not immediately after
    // opening, since whatever application is showing the file may
    // still be reading it.
    std::vector<std::string> temp_files_to_cleanup;

    // A vault that's actually been unlocked: the real, decrypted
    // vaultstore::Vault handle (holding the master key and the
    // decrypted folder tree in memory - see vault_store.h). Kept
    // separate from vault_tabs above so that opening the same vault
    // in a second tab (DrawVaultTabs' "+") reuses this same session
    // instead of prompting for the password a second time or ending
    // up with two independently-decrypted (and possibly diverging)
    // copies of the same vault's tree. An entry is removed - dropping
    // its vaultstore::Vault, which scrubs the master key from memory
    // (see Vault::Impl's destructor) - the moment no tab references
    // its vault_index anymore; see CloseVaultTab.
    struct OpenSession {
        int vault_index = -1;
        std::unique_ptr<vaultstore::Vault> vault;
    };
    std::vector<OpenSession> open_sessions;
    int selected_folder = -1;


    // Hover-amount state for each screen's synthetic "Add" tile (see
    // DrawTileGrid) - it has no backing Vault/Tile to store its own
    // hover_amount on, so each screen gets one persistent float here
    // instead.
    float add_vault_hover = 0.0f;
    float add_folder_hover = 0.0f;

    // Vertical scroll offset for each screen's tile grid (see
    // DrawTileGrid) - one per screen so scrolling the vault grid
    // doesn't affect a folder grid's scroll position or vice versa.
    // Note this is shared across *all* dir tabs/vaults on a given
    // screen (there's one vault_grid_scroll total, not one per dir
    // tab) - switching tabs keeps whatever scroll position you had,
    // which DrawTileGrid's own clamp-to-max_scroll then corrects if
    // the new tab has fewer rows. Flagging this since "reset scroll on
    // tab switch" is an equally reasonable choice if that reads
    // better in practice.
    float vault_grid_scroll = 0.0f;
    float folder_grid_scroll = 0.0f;

    // A pending "are you sure?" delete confirmation (see
    // DrawDeleteModal), opened by clicking a tile's hover-revealed "x"
    // (see DrawTileGrid). Only one at a time - while `active`, the
    // underlying screen keeps drawing normally (so vaults/tabs are
    // still visible) but UiAppBeginFrame masks out clicks/double-
    // clicks to it, so nothing else can be triggered underneath the
    // modal.
    struct PendingDelete {
        bool active = false;
        bool is_vault = true;  // true: a Vault in state.vaults; false: a Tile in the open Vault's folders
        int index = -1;  // index into state.vaults, or the open vault's folders
        std::string item_name;  // captured at open time, so a rename mid-confirm can't desync the label
        anim::Timeline timeline;  // fade/scale-in, started the first time this is drawn
    };
    PendingDelete pending_delete;

    // Settings toggles (see DrawSettingsPanel) - actually functional,
    // not decorative. `animations_enabled` gates the eased hover/
    // press/tab-indicator micro-interactions (see EffectiveDt below):
    // off makes them snap to their target in one frame instead of
    // easing. `glass_effect_enabled` gates translucency for the
    // panels introduced alongside this setting (command palette,
    // settings panel, delete modal) - off trades a flatter, more
    // opaque look for less see-through layering. There's no real GPU
    // blur behind either panel (see the roadmap notes on
    // glassmorphism), so "glass effect" here just means alpha, not a
    // blurred backdrop; and neither toggle yet reaches older surfaces
    // (toasts, tile hover glow) - flagging that as a known gap rather
    // than quietly claiming full coverage.
    struct Settings {
        bool animations_enabled = true;
        bool glass_effect_enabled = true;
    };
    Settings settings;
    bool settings_panel_open = false;
    // Eased 0..1 slide-in amount, separate from settings_panel_open's
    // instant on/off so the panel can animate closed instead of
    // vanishing the instant the gear icon is clicked again.
    float settings_panel_progress = 0.0f;

    // The active background's decoded frames (see LoadBackgroundChoice)
    // - a real animated GIF has more than one; anything else is a
    // single "frame" with a delay long enough it never advances.
    // Empty means no background is active (the plain flat panel
    // colors - see BackdropColor - the default, unchanged look).
    struct Background {
        std::vector<int> frame_icons;
        std::vector<int> frame_delays_ms;  // parallel to frame_icons
        int current_frame = 0;
        double frame_started_at = 0.0;  // now_seconds when current_frame began showing
    };
    Background background;

    // Every background choice available in Settings' picker grid -
    // real files under assets/backgrounds/ (plus the legacy single
    // assets/background.png, if present - see ScanBackgroundChoices'
    // comment). background_thumbnails is parallel to
    // background_choices: one small static-preview icon handle per
    // choice (a GIF's thumbnail is just its first frame - see
    // LoadBackgroundChoice's comment on why a plain LoadIcon call
    // already does the right thing there). background_selected_index
    // is which one is currently active, or -1 for "none."
    std::vector<std::string> background_choices;
    std::vector<int> background_thumbnails;
    int background_selected_index = -1;

    // How far the settings panel's content is scrolled down (0 = top)
    // - the panel's content (toggles, security, the third-party API
    // status, every known vault's location, data controls, quit) can
    // easily run taller than the window, especially in the small
    // default window size or with several vaults listed. See
    // DrawSettingsPanel's scroll handling.
    float settings_scroll = 0.0f;

    // Command palette (see DrawCommandPalette) - opened via the
    // toolbar's search icon (previously a dead button - see #7/#8 in
    // the original roadmap notes, "search becomes a real component"
    // and "command palette", satisfied together by the same panel
    // rather than as two separate features).
    struct CommandPalette {
        bool open = false;
        std::string query;  // typed filter text - see the text_input plumbing in UiInput
        int selected = 0;   // index into the *filtered* command list
        anim::Timeline timeline;  // opacity+scale open animation
    };
    CommandPalette command_palette;

    // Which single tile (vault or folder) a right-click most recently
    // targeted (see DrawTileGrid::right_clicked and DrawContextMenu).
    // Reuses the same is_vault/index shape as PendingDelete above
    // since both need to name "one Vault in state.vaults, or one Tile
    // in the currently-open Vault's folders".
    struct ContextMenu {
        bool open = false;
        bool is_vault = true;
        int index = -1;
        float x = 0.0f, y = 0.0f;  // content-space position to draw at (captured at open time)
        anim::Timeline timeline;
    };
    ContextMenu context_menu;

    // An in-progress rename (see DrawRenamePanel), started from a
    // context menu's "RENAME" entry. `buffer` is seeded with the
    // current name when opened and edited via the same text_input/
    // key_backspace plumbing the command palette's search field uses.
    struct RenameState {
        bool active = false;
        bool is_vault = true;
        int index = -1;
        std::string buffer;
    };
    RenameState rename;

    // Whole-app lock (see DrawLockToggle/DrawLockScreen) - engaging it
    // hides the vault list/vault contents entirely behind a centered
    // padlock, independent of any per-vault "encryption" (there isn't
    // one yet - see the open questions about real on-disk encryption).
    // This is a UI-level lock: a convenience/privacy screen, not a
    // cryptographic one. `password` empty means none has been set yet
    // (see DrawSettingsPanel's "CHANGE PASSWORD" row / DrawChangePasswordPanel) -
    // locking still works with no password set, it just doesn't
    // prompt for one to unlock again (see DrawLockScreen's comment).
    struct Lock {
        bool locked = false;
        std::string password;
        bool prompt_open = false;  // the "enter password to unlock" panel
        std::string buffer;        // typed attempt, masked as dots in the panel
        std::string error;         // "WRONG PASSWORD", cleared on reopen/retry
    };
    Lock lock;

    // Whether the local third-party API (see ApiServer, near the
    // bottom of this file) should be running - toggled from
    // Settings' SECURITY section. Deliberately always starts false on
    // every launch, never persisted across restarts the way
    // state.vaults is: exposing *any* local HTTP surface is a
    // meaningful attack-surface increase even though it's
    // 127.0.0.1-only (another process running as the same user could
    // still reach it), so re-enabling it each session is a small,
    // deliberate friction against "turned it on once for testing and
    // forgot it was still running."
    bool api_enabled = false;
    // Read-only status ApiServer fills in once it's actually
    // listening (0/empty while off or still starting up) - shown in
    // Settings so the person can see it's genuinely running and copy
    // the connection info a third-party app would need.
    uint16_t api_port = 0;
    std::string api_token;

    // One-shot: set by Settings' QUIT button (or the border right-
    // click menu's CLOSE), consumed by main.cpp via
    // UiAppConsumeQuitRequest to actually call Window::RequestClose -
    // only main.cpp owns the real GLFW window, so it's the one that
    // has to act on this, the same "request here, act there" split
    // already used for window resizing (see AppState::resize_requested).
    bool quit_requested = false;

    // NOT one-shot, unlike quit_requested above - a persistent toggle
    // (see the border right-click menu's "ALWAYS ON TOP" row) that
    // main.cpp polls every frame via UiAppGetAlwaysOnTop and applies
    // via Window::SetAlwaysOnTop whenever it changes, the same idea
    // as the API server's enabled/running sync in UiAppBeginFrame.
    bool always_on_top = false;

    // Right-click-on-the-window-border context menu (see
    // DrawBorderMenu) - CLOSE and ALWAYS ON TOP. Deliberately
    // separate from AppState::ContextMenu (which targets a specific
    // vault/folder tile and needs an index) since this one has fixed,
    // always-the-same options and no target to resolve.
    struct BorderMenu {
        bool open = false;
        float x = 0.0f, y = 0.0f;
        anim::Timeline timeline;
    };
    BorderMenu border_menu;

    // In-progress "forget every vault" confirmation (see Settings'
    // "CLEAR ALL DATA" row and DrawClearAllDataModal) - separate from
    // PendingDelete (which targets one vault/folder) since this one
    // has no target index and a different, more emphatic message.
    bool clear_all_data_pending = false;

    // In-progress password change (see DrawChangePasswordPanel),
    // opened from Settings' "CHANGE PASSWORD" row. A little 3-step
    // wizard rather than one form with three fields at once, since
    // there's no multi-field-focus model built for text input yet
    // (see the command palette/rename panel - both are exactly one
    // field, always implicitly focused) - stepping through one field
    // at a time sidesteps needing that.
    struct ChangePassword {
        bool open = false;
        // 0 = verify the current password (skipped - starts at 1 -
        // if lock.password is empty, i.e. there's nothing to verify
        // yet), 1 = enter the new password, 2 = confirm it.
        int stage = 0;
        std::string buffer;        // current step's typed text
        std::string new_password;  // captured at the end of stage 1, checked against stage 2
        std::string error;
    };
    ChangePassword change_password;

    // Per-vault unlock prompt (see RequestOpenVault) - shown when
    // opening a vault that isn't already unlocked in another tab.
    // Distinct from Lock's own prompt above: that one gates the whole
    // app behind one UI-level password compared against a stored
    // string; this one is a real vaultstore::OpenVault() call against
    // that specific vault's real password.
    struct UnlockPrompt {
        bool open = false;
        int vault_index = -1;
        std::string buffer;
        std::string error;
    };
    UnlockPrompt unlock_prompt;

    // The "create a new vault" wizard (see DrawCreateVaultWizard),
    // opened from VaultList's "ADD VAULT" tile/button/command. Steps
    // through name -> location (a real directory on disk - there's no
    // native OS folder-picker dialog wired up yet, see that panel's
    // comment) -> password -> confirm password, one field at a time
    // for the same reason ChangePassword above does.
    struct CreateVaultWizard {
        bool open = false;
        int stage = 0;  // 0 = name, 1 = location, 2 = password, 3 = confirm password
        std::string name;
        std::string location;
        std::string password;
        std::string buffer;  // current stage's typed text
        std::string error;
    };
    CreateVaultWizard create_vault_wizard;

    // In-progress drag-to-reorder within a tile grid (see
    // DrawTileGrid) - press and hold a tile, then move the mouse past
    // a small threshold to actually "pick it up" (a plain click still
    // selects/opens normally, same as before); the tile follows the
    // cursor as a ghost, the slot under the cursor gets an outline
    // showing where it'll land, and releasing there reorders the
    // underlying vector. Shared by the vault grid and the folder grid
    // rather than one instance each: only one of those two grids is
    // ever visible/interactable in a given frame (VaultList xor
    // VaultFolder), so there's never a moment where both could be
    // mid-drag at once.
    struct TileDrag {
        bool mouse_was_down = false;  // edge-detects a fresh press vs. an already-held button
        bool pressed = false;         // true from press until release, even under the threshold
        bool active = false;          // true once past the threshold - actually "picked up"
        int source_index = -1;
        float press_x = 0.0f, press_y = 0.0f;
        int hover_slot = -1;          // recomputed every frame while active
    };
    TileDrag grid_drag;

    // Brief accent-colored flash over the VaultFolder screen right
    // after a vault opens (see OpenVaultTab and DrawVaultFolderScreen)
    // - a deliberately scoped-down version of the original roadmap's
    // "vault opening" animation (#3/#4): not a multi-second state
    // machine with the tile animating from the grid into a new
    // screen, just a quick "something happened" flourish on the
    // screen that's about to show its contents. See the reply text
    // for why the fuller version wasn't attempted this pass.
    anim::Timeline vault_open_flourish;

    // Sliding active-tab indicator (see DrawSlidingTabIndicator),
    // shared by DrawDirTabs/DrawVaultTabs since only one is ever drawn
    // per frame. tab_indicator_screen records which screen it was
    // last drawn for, so switching screens snaps to the new position
    // instead of sliding in from wherever the *other* tab bar's
    // indicator happened to be.
    float tab_indicator_x = 0.0f;
    float tab_indicator_w = 0.0f;
    bool tab_indicator_valid = false;
    Screen tab_indicator_screen = Screen::VaultList;

    // Press-scale animation state for each toolbar/tab-bar icon
    // button (see IconButton's optional scale_state param) - one
    // persistent float per button, since IconButton itself is
    // stateless and these are a fixed set of chrome buttons, not
    // dynamic per-item ones (contrast with Tile::hover_amount above).
    float scale_back = 1.0f;
    float scale_home = 1.0f;
    float scale_settings = 1.0f;
    float scale_search = 1.0f;
    float scale_refresh = 1.0f;
    float scale_plus = 1.0f;
    float scale_vault_tab_add = 1.0f;
    float scale_lock_toggle = 1.0f;

    // Toast notifications (see DrawToasts) - "Vault created", "Folder
    // created", etc., triggered wherever those actions actually
    // happen. Newest first; DrawToasts drops expired ones.
    struct Toast {
        std::string text;
        double start_seconds = 0.0;
    };
    std::vector<Toast> toasts;

    // Window-level double-click detection (position + timing), since
    // UiRenderer only knows about single clicks - see on_mouse_button
    // in main().
    double last_release_time = -1.0;
    float last_release_x = -1.0f;
    float last_release_y = -1.0f;
};

// Where CryptVault remembers which real, on-disk vaults it knows
// about (just name+path pairs - never anything from inside a vault,
// which stays exactly as encrypted as vault_store.h promises) across
// restarts. Real vaults exist independently of whether this app is
// running, so without this, closing and reopening CryptVault would
// "lose" every vault from the list even though the actual encrypted
// data sits untouched on disk - there'd be no way to get a vault back
// into the list short of remembering its path and recreating the
// entry by hand.
std::filesystem::path KnownVaultsPath() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    std::filesystem::path base = home ? std::filesystem::path(home) : std::filesystem::path(".");
    return base / ".cryptvault" / "known_vaults.txt";
}

// A sensible default location to pre-fill in DrawCreateVaultWizard -
// not otherwise used for anything; the user can always type a
// different path (there's no native OS folder-picker dialog wired up
// yet - see that panel's own comment).
std::string DefaultVaultsRoot() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    std::string base = home ? home : ".";
    return base + "/CryptVaultData";
}

// Simple line-based format - "name\tpath\n" per vault - rather than
// anything more structured (JSON etc.), since there's no such
// dependency already in this project; see vault_store.cpp's own
// hand-rolled binary manifest format for the same reasoning. Not
// itself encrypted or treated as sensitive: a vault's name and where
// it lives on disk don't reveal anything about its contents, which is
// the whole point of vault_store.h's design. Known limitation: a
// vault name containing a literal tab or newline would corrupt this
// file's line structure - not sanitized against on write, since
// there's no realistic way to type either into the create-vault
// wizard's text field in practice.
void LoadKnownVaults(AppState& state) {
    std::ifstream f(KnownVaultsPath());
    if (!f) {
        return;  // no file yet - first run, or nothing's ever been saved
    }
    std::string line;
    while (std::getline(f, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        Vault vault;
        vault.name = line.substr(0, tab);
        vault.path = line.substr(tab + 1);
        if (!vault.name.empty() && !vault.path.empty()) {
            state.vaults.push_back(std::move(vault));
        }
    }
}

void SaveKnownVaults(const AppState& state) {
    std::filesystem::path path = KnownVaultsPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        return;  // best-effort - a failed save here shouldn't crash the app or block whatever triggered it
    }
    for (const Vault& vault : state.vaults) {
        f << vault.name << '\t' << vault.path << '\n';
    }
}

// Resolves a vault_tabs entry (a plain index into state.vaults) to
// its open, decrypted session (see AppState::OpenSession), or nullptr
// if that vault isn't currently unlocked (or vault_index is stale -
// e.g. the vault was removed out from under this tab). Screens fall
// back to VaultList/prompt-for-password on nullptr rather than
// dereferencing nothing.
vaultstore::Vault* ResolveOpenVault(AppState& state, int vault_index) {
    for (AppState::OpenSession& session : state.open_sessions) {
        if (session.vault_index == vault_index) {
            return session.vault.get();
        }
    }
    return nullptr;
}

// Convenience for the common case: resolve state.vault_tabs[state.active_vault_tab].
vaultstore::Vault* ActiveOpenVault(AppState& state) {
    if (state.active_vault_tab < 0
        || state.active_vault_tab >= static_cast<int>(state.vault_tabs.size())) {
        return nullptr;
    }
    return ResolveOpenVault(state, state.vault_tabs[state.active_vault_tab]);
}

// The active tab's current folder-browsing path (see
// AppState::tab_folder_paths' comment) - a mutable reference so
// double-clicking a folder/Back can push/pop it in place. Returns
// nullptr if there's no active tab to have a path for at all (mirrors
// ActiveOpenVault's own nullptr contract).
std::vector<std::string>* ActiveFolderPath(AppState& state) {
    if (state.active_vault_tab < 0
        || state.active_vault_tab >= static_cast<int>(state.tab_folder_paths.size())) {
        return nullptr;
    }
    return &state.tab_folder_paths[state.active_vault_tab];
}

// Walks `path` (a list of folder names, root-to-leaf) down from
// `vault`'s root, returning the folder currently being browsed. Falls
// back to the root - and, if `path` is non-null, clears it - if any
// segment along the way no longer exists (that folder got renamed or
// deleted out from under this tab by another tab on the same vault,
// or by the third-party API) or turned out not to be a folder at all,
// rather than silently showing stale/wrong contents or dereferencing
// nothing.
vaultstore::TreeNode& CurrentFolder(vaultstore::Vault& vault, std::vector<std::string>* path) {
    vaultstore::TreeNode* node = &vault.root();
    if (!path) {
        return *node;
    }
    for (size_t i = 0; i < path->size(); ++i) {
        auto it = std::find_if(node->children.begin(), node->children.end(),
                                [&](vaultstore::TreeNode& n) { return n.name == (*path)[i]; });
        if (it == node->children.end() || !it->is_folder) {
            path->clear();
            return vault.root();
        }
        node = &*it;
    }
    return *node;
}

// Resolves the same is_vault/index shape used by
// PendingDelete/ContextMenu/RenameState to a mutable pointer at the
// name to display/edit, or nullptr if it's gone stale (deleted from
// under an open menu/rename, or - for a folder - its vault is no
// longer open).
std::string* ResolveItemName(AppState& state, bool is_vault, int index) {
    if (is_vault) {
        if (index < 0 || index >= static_cast<int>(state.vaults.size())) {
            return nullptr;
        }
        return &state.vaults[index].name;
    }
    vaultstore::Vault* vault = ActiveOpenVault(state);
    if (!vault) {
        return nullptr;
    }
    std::vector<vaultstore::TreeNode>& children =
        CurrentFolder(*vault, ActiveFolderPath(state)).children;
    if (index < 0 || index >= static_cast<int>(children.size())) {
        return nullptr;
    }
    return &children[index].name;
}

// The only two places allowed to mutate state.vault_tabs directly -
// every other site goes through these so state.tab_folder_paths (see
// its own comment) can never drift out of sync with it (different
// size, or a path sitting at the wrong index for its tab).

// Appends a new tab for vault_index, its folder-browsing position
// starting at `initial_path` (empty = the vault's root). Returns the
// new tab's index.
int PushVaultTab(AppState& state, int vault_index, std::vector<std::string> initial_path = {}) {
    state.vault_tabs.push_back(vault_index);
    state.tab_folder_paths.push_back(std::move(initial_path));
    return static_cast<int>(state.vault_tabs.size()) - 1;
}

// Removes tab_index from both vault_tabs and tab_folder_paths
// together, and fixes up active_vault_tab - the same bookkeeping
// three separate call sites used to each do by hand (a good way for
// the two vectors to quietly drift out of sync if only one of them
// remembered to update).
void EraseVaultTab(AppState& state, int tab_index) {
    state.vault_tabs.erase(state.vault_tabs.begin() + tab_index);
    state.tab_folder_paths.erase(state.tab_folder_paths.begin() + tab_index);
    if (state.active_vault_tab >= static_cast<int>(state.vault_tabs.size())) {
        state.active_vault_tab = static_cast<int>(state.vault_tabs.size()) - 1;
    }
}

// Switches to vault_index's existing tab if it's already open in one
// (see the dedup-by-vault find() below), or creates a fresh tab if
// not - the right semantics for "open this vault", used when
// double-clicking a vault tile, the context menu's "OPEN", and
// unlocking a vault for the first time: re-opening an already-open
// vault should take you to its existing tab (and wherever you'd
// navigated to in it), not spawn a second one at the root. NOT used
// by the vault tab bar's "+" (see DuplicateVaultTab below, right
// after this) - that button's whole purpose is creating an actual
// duplicate, which this function's dedup would silently prevent.
void SwitchOrOpenVaultTab(AppState& state, int vault_index) {
    auto it = std::find(state.vault_tabs.begin(), state.vault_tabs.end(), vault_index);
    if (it == state.vault_tabs.end()) {
        state.active_vault_tab = PushVaultTab(state, vault_index);
    } else {
        state.active_vault_tab = static_cast<int>(it - state.vault_tabs.begin());
    }
    state.screen = Screen::VaultFolder;
    state.selected_folder = -1;

    state.vault_open_flourish.duration_seconds =
        state.settings.animations_enabled ? anim::duration::kMajor : 0.001f;

    state.vault_open_flourish.Start(state.now_seconds);
}

// Always pushes a brand-new tab for vault_index, even if it's already
// open in another tab - what the vault tab bar's "+" (see
// DrawVaultTabs) actually needs, since duplicating the active tab is
// its entire purpose. The new tab starts browsing at the SAME folder
// the tab it's duplicated from is currently showing (a "duplicate
// this view" feel, not "jump back to the vault's root") - the two
// tabs are independent from that point on, free to navigate
// separately (see state.tab_folder_paths' comment for why that
// independence is the actual point of this button). This used to
// just call SwitchOrOpenVaultTab (then named OpenVaultTabForSession),
// whose dedup-by-vault find() meant "+" was silently a no-op whenever
// the vault it was duplicating was (trivially, always) already open
// in the very tab you clicked "+" from - a real, shipped bug ("New
// Tab button doesn't do anything"), not a hypothetical one.
void DuplicateVaultTab(AppState& state, int vault_index) {
    std::vector<std::string> path_to_copy;
    if (state.active_vault_tab >= 0
        && state.active_vault_tab < static_cast<int>(state.tab_folder_paths.size())) {
        path_to_copy = state.tab_folder_paths[state.active_vault_tab];
    }
    state.active_vault_tab = PushVaultTab(state, vault_index, path_to_copy);
    state.screen = Screen::VaultFolder;
    state.selected_folder = -1;

    state.vault_open_flourish.duration_seconds =
        state.settings.animations_enabled ? anim::duration::kMajor : 0.001f;

    state.vault_open_flourish.Start(state.now_seconds);
}

// Entry point for "open this vault" - double-clicking a vault tile,
// or the context menu's "OPEN" entry. If it's already unlocked in
// another tab, this is just SwitchOrOpenVaultTab (no reason to ask
// for the password twice). Otherwise it opens the unlock-password
// prompt (see AppState::UnlockPrompt/DrawUnlockPrompt) and defers
// actually creating a tab until that succeeds - unlike the old
// simulated vaults, there's a real decrypt (and a real chance of a
// wrong password) between "click a vault" and "see its contents" now.
void RequestOpenVault(AppState& state, int vault_index) {
    if (ResolveOpenVault(state, vault_index)) {
        SwitchOrOpenVaultTab(state, vault_index);
        return;
    }
    AppState::UnlockPrompt prompt;
    prompt.open = true;
    prompt.vault_index = vault_index;
    state.unlock_prompt = prompt;
}

// Closes one tab. If that was the last tab pointing at its vault
// (i.e. no other open tab shares the vault - see SwitchOrOpenVaultTab's
// dedup-by-vault), also drops the underlying OpenSession, which
// destroys the vaultstore::Vault and scrubs its master key from
// memory (see Vault::Impl's destructor in vault_store.cpp) - closing
// every tab on a vault is the closest thing this app has to "lock
// just this one vault again" (see AppState::Lock's comment for the
// whole-app version).
void CloseVaultTab(AppState& state, int tab_index) {
    if (tab_index < 0 || tab_index >= static_cast<int>(state.vault_tabs.size())) {
        return;
    }
    int vault_index = state.vault_tabs[tab_index];
    EraseVaultTab(state, tab_index);

    bool still_referenced = std::find(state.vault_tabs.begin(), state.vault_tabs.end(), vault_index)
                             != state.vault_tabs.end();
    if (!still_referenced) {
        state.open_sessions.erase(
            std::remove_if(state.open_sessions.begin(), state.open_sessions.end(),
                            [&](const AppState::OpenSession& s) { return s.vault_index == vault_index; }),
            state.open_sessions.end());
    }

    if (state.vault_tabs.empty()) {
        state.screen = Screen::VaultList;
    }
}

// Forward declaration - defined further down, near where toasts are
// actually drawn - but needed here since a couple of helpers just
// below (AddFolderToActiveVault, OpenCreateVaultWizard) use it and
// are grouped with the other vault-session helpers above rather than
// off on their own next to the toast-drawing code.
void PushToast(AppState& state, const std::string& text);

// Picks a name not already used by any item in `existing` (which must
// have a `.name` member): "New Vault", then "New Vault (2)", "New
// Vault (3)", etc. Used everywhere a button creates a new dir tab/
// vault/folder, since there's no text-input widget yet to let the
// user type a name themselves (see the questions at the end of this
// change).
template <typename T>
std::string NextUniqueName(const std::string& base, const std::vector<T>& existing) {
    auto name_taken = [&](const std::string& candidate) {
        return std::any_of(existing.begin(), existing.end(),
                            [&](const T& item) { return item.name == candidate; });
    };
    if (!name_taken(base)) {
        return base;
    }
    for (int n = 2; n < 100000; ++n) {
        std::string candidate = base + " (" + std::to_string(n) + ")";
        if (!name_taken(candidate)) {
            return candidate;
        }
    }
    return base;  // unreachable in practice
}

// Opens the "create a new vault" wizard (see
// AppState::CreateVaultWizard/DrawCreateVaultWizard) pre-filled with a
// reasonable default name - shared by every place that used to
// instantly create a vault (the "ADD VAULT" tile, the toolbar's "+",
// the command palette). Instant creation stopped making sense once
// vaults became real, on-disk, encrypted things: a name, a location,
// and a password all need to actually be decided, not auto-generated.
void OpenCreateVaultWizard(AppState& state) {
    AppState::CreateVaultWizard wizard;
    wizard.open = true;
    wizard.stage = 0;
    wizard.name = NextUniqueName("New Vault", state.vaults);
    wizard.buffer = wizard.name;
    state.create_vault_wizard = wizard;
}

// Adds a new, empty folder to the currently-open vault - shared by
// the "ADD FOLDER" tile, the toolbar's "+", and the command palette.
// Reports failure via toast rather than a blocking dialog since the
// only realistic failure here is a name collision, which
// NextUniqueName already avoids by construction (an actual disk
// error - e.g. the vault's directory got deleted out from under the
// app - is the only other realistic case, and a toast is still an
// honest way to surface that without derailing whatever the user was
// doing).
void AddFolderToActiveVault(AppState& state) {
    vaultstore::Vault* vault = ActiveOpenVault(state);
    if (!vault) {
        return;
    }
    vaultstore::TreeNode& folder = CurrentFolder(*vault, ActiveFolderPath(state));
    std::string name = NextUniqueName("New Folder", folder.children);
    std::string error;
    if (vault->AddFolder(folder, name, error)) {
        PushToast(state, "FOLDER CREATED");
    } else {
        PushToast(state, "COULDN'T CREATE FOLDER: " + error);
    }
}

// Decrypts `file` to a real, temporary location on disk and asks the
// OS to open it with whatever application is associated with its type
// (see platform::OpenFileWithDefaultApp) - the closest thing to
// "double-click a file to open it" this app can do without embedding
// a viewer for every possible file type itself.
//
// Security trade-off, stated plainly rather than glossed over: this
// necessarily puts a DECRYPTED copy of the file on disk, outside
// CryptVault's control, for as long as whatever application opens it
// keeps it open - this app has no reliable way to know when that's
// done (short of watching for that process to exit, which is real
// platform-specific work of its own), so it doesn't try to delete the
// copy immediately; see AppState::temp_files_to_cleanup, which is
// cleaned up best-effort when the app itself closes instead. This is
// inherent to "let some other program show this file," not a gap
// specific to this implementation - wherever it's opened, it has to
// be a real file on a real filesystem for the other program to read,
// not bytes only CryptVault's own process holds. Written under the
// OS's own temp directory (std::filesystem::temp_directory_path() -
// %TEMP% on Windows, $TMPDIR or /tmp on Linux), which on both
// platforms is already scoped to the current user by default.
void OpenFileFromVault(AppState& state, vaultstore::Vault& vault, vaultstore::TreeNode& file) {
    std::vector<uint8_t> content;
    std::string read_error;
    if (!vault.ReadFile(file, content, read_error)) {
        PushToast(state, "COULDN'T OPEN FILE: " + read_error);
        return;
    }

    std::error_code ec;
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path(ec) / "cryptvault_open";
    if (ec) {
        PushToast(state, "COULDN'T FIND A TEMP DIRECTORY TO OPEN THE FILE FROM");
        return;
    }
    std::filesystem::create_directories(temp_dir, ec);

    // A random prefix (not the vault's own blob_id, which is already
    // random but would let two different vaults' files collide if
    // they happened to share one - vanishingly unlikely, but free to
    // avoid) keeps repeated opens of files with the same name from
    // colliding with each other in the shared temp directory.
    unsigned char rand_bytes[8];
    randombytes_buf(rand_bytes, sizeof(rand_bytes));
    std::string prefix;
    for (unsigned char b : rand_bytes) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", b);
        prefix += buf;
    }
    std::filesystem::path temp_path = temp_dir / (prefix + "_" + file.name);

    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out
            || (!content.empty()
                && !out.write(reinterpret_cast<const char*>(content.data()), content.size()))) {
            PushToast(state, "COULDN'T WRITE A TEMPORARY COPY TO OPEN");
            return;
        }
    }

    std::string launch_error;
    if (platform::OpenFileWithDefaultApp(temp_path.string(), &launch_error)) {
        state.temp_files_to_cleanup.push_back(temp_path.string());
    } else {
        PushToast(state, "COULDN'T OPEN: " + launch_error);
        std::filesystem::remove(temp_path, ec);  // nothing's using it if the launch itself failed
    }
}

// Reads a plain (unencrypted, real) file from disk - the OS-file-
// system side of "drag a file from Explorer into a vault to encrypt
// it" (see DropFilesIntoVault below). Deliberately separate from
// vault_store.cpp's own ReadWholeFile, which reads *encrypted* blobs
// and lives inside that module's anonymous namespace - the two have
// nothing in common besides both calling ifstream, and this one has
// no reason to know anything about vault internals.
bool ReadPlainFileFromDisk(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return false;
    }
    std::streamsize size = f.tellg();
    if (size < 0) {
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(out.data()), size)) {
        return false;
    }
    return true;
}

// Recursively encrypts one real, on-disk file or directory into
// `target` (a folder already inside the vault - either an existing
// one the drop landed on, or the vault's root). A file becomes one
// encrypted file entry; a directory becomes a same-named encrypted
// folder, walked and imported all the way down, so dropping a whole
// folder from Explorer preserves its structure instead of failing
// outright (see the file header's note on where this gap used to be:
// trying to open a directory path as a file - std::ifstream can't -
// silently counted as a failed file and produced a generic "couldn't
// encrypt" toast with no indication a folder was even involved).
// Returns {succeeded, failed} counts of individual files (a folder
// itself isn't counted, only what's inside it) so the caller can
// summarize across every top-level dropped path in one toast.
std::pair<int, int> ImportPathIntoVault(vaultstore::Vault& vault, vaultstore::TreeNode& target,
                                         const std::filesystem::path& disk_path) {
    std::error_code ec;
    std::string name = disk_path.filename().string();
    if (name.empty()) {
        return {0, 1};
    }

    if (std::filesystem::is_directory(disk_path, ec)) {
        std::string unique_name = NextUniqueName(name, target.children);
        std::string error;
        if (!vault.AddFolder(target, unique_name, error)) {
            return {0, 1};
        }
        // AddFolder appended to target.children - find it by name
        // (just added, so NextUniqueName guarantees it's unique)
        // rather than assuming it's at .back(), since target.children
        // is whatever order the vault already had it in.
        auto it = std::find_if(target.children.begin(), target.children.end(),
                                [&](vaultstore::TreeNode& n) { return n.name == unique_name; });
        if (it == target.children.end()) {
            return {0, 1};  // shouldn't happen - AddFolder just said it succeeded
        }
        vaultstore::TreeNode& new_folder = *it;

        int succeeded = 0, failed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(disk_path, ec)) {
            auto [s, f] = ImportPathIntoVault(vault, new_folder, entry.path());
            succeeded += s;
            failed += f;
        }
        return {succeeded, failed};
    }

    std::vector<uint8_t> content;
    if (!ReadPlainFileFromDisk(disk_path.string(), content)) {
        return {0, 1};
    }
    std::string unique_name = NextUniqueName(name, target.children);
    std::string error;
    if (vault.AddFile(target, unique_name, content, error)) {
        return {1, 0};
    }
    return {0, 1};
}

// Encrypts every path in state.input.dropped_paths into the currently
// open vault - the "drag files from Explorer to encrypt them" half of
// cross-app drag-and-drop (see main.cpp's on_files_dropped for the
// other half: GLFW's drop callback is already cross-platform - OLE on
// Windows, XDND on Linux - so nothing OS-specific was needed just to
// *receive* the drop). Each dropped file or folder lands inside
// whichever folder tile the cursor was over at drop time (see
// TileGridResult::hovered_index), or loose in whichever folder is
// currently being browsed otherwise - matching what the drop looked
// like to the user. A dropped folder is imported recursively (see
// ImportPathIntoVault).
//
// The reverse direction - dragging a file *out* of CryptVault to
// Explorer - isn't implemented here. That's a fundamentally different
// thing to build: GLFW has no API for initiating an OS-level drag at
// all, so it would mean real per-platform code (COM's IDropSource/
// DoDragDrop on Windows, implementing XDND as a source rather than
// just a target on Linux), and it would also mean decrypting a file
// out to a real temporary path on disk first (since both of those
// mechanisms hand the OS a real file path, not a byte buffer) -
// worth flagging as its own security consideration whenever it's
// built, not something to fold in quietly here.
void DropFilesIntoVault(AppState& state, vaultstore::Vault& vault, vaultstore::TreeNode& current_folder,
                         int hovered_folder_index) {
    if (state.input.dropped_paths.empty()) {
        return;
    }

    std::vector<vaultstore::TreeNode>& folders = current_folder.children;
    vaultstore::TreeNode* target = &current_folder;
    if (hovered_folder_index >= 0 && hovered_folder_index < static_cast<int>(folders.size())
        && folders[hovered_folder_index].is_folder) {
        target = &folders[hovered_folder_index];
    }

    int succeeded = 0;
    int failed = 0;
    for (const std::string& path : state.input.dropped_paths) {
        auto [s, f] = ImportPathIntoVault(vault, *target, std::filesystem::path(path));
        succeeded += s;
        failed += f;
    }

    if (succeeded > 0 && failed == 0) {
        PushToast(state, std::to_string(succeeded) + (succeeded == 1 ? " FILE ENCRYPTED" : " FILES ENCRYPTED"));
    } else if (succeeded > 0 && failed > 0) {
        PushToast(state, std::to_string(succeeded) + " ENCRYPTED, " + std::to_string(failed) + " FAILED");
    } else {
        PushToast(state, "COULDN'T ENCRYPT DROPPED FILE(S)");
    }
}

// --- small helpers ---------------------------------------------------------

// The dt every eased micro-interaction (hover glow, icon pop, press-
// scale, sliding tab indicator) should use - state.dt_seconds when
// Settings::animations_enabled is on, or something far larger than
// any anim::MoveTowards duration in this file when it's off, so the
// very next frame after a state change already reaches its target.
// One helper here instead of an `if (animations_enabled)` branch at
// every call site.
float EffectiveDt(const AppState& state) {
    return state.settings.animations_enabled ? state.dt_seconds : 1.0f;
}

bool HitTest(const appshell::UiInput& input, float x, float y, float w, float h) {
    return input.mouse_x >= x && input.mouse_x <= x + w
           && input.mouse_y >= y && input.mouse_y <= y + h;
}

appshell::UiColor LerpColor(appshell::UiColor a, appshell::UiColor b, float t) {
    return appshell::UiColor{
        anim::Lerp(a.r, b.r, t),
        anim::Lerp(a.g, b.g, t),
        anim::Lerp(a.b, b.b, t),
        anim::Lerp(a.a, b.a, t),
    };
}

// The actual fix for "I loaded a background image but I still don't
// see it": every panel background (toolbar, tab bar, tile grid,
// status bar) draws a fully OPAQUE fill as its first thing, which -
// regardless of whether the background image itself loaded
// successfully - painted over the entire window before the frame was
// even half-drawn, since those fills run after the background image
// in draw order. Loading the image was never the missing piece; showing
// through the UI drawn on top of it was. This wraps every one of
// those opaque fills: when a background image is active, the fill's
// alpha drops to `alpha_with_background` instead (letting the image
// show through, tinted/darkened enough to keep text and icons
// readable over whatever's in the photo); otherwise the color is
// returned completely unchanged, so nothing about the no-background
// (default) look changes at all.
appshell::UiColor BackdropColor(appshell::UiColor opaque, bool has_background, float alpha_with_background) {
    if (!has_background) {
        return opaque;
    }
    appshell::UiColor translucent = opaque;
    translucent.a = alpha_with_background;
    return translucent;
}

void PushToast(AppState& state, const std::string& text) {
    AppState::Toast toast;
    toast.text = text;
    toast.start_seconds = state.now_seconds;
    state.toasts.push_back(std::move(toast));
}

// --- Multi-background support (see Settings' BACKGROUND picker grid,
// DrawSettingsPanel) -------------------------------------------------------
//
// Every background choice is a real file under assets/backgrounds/
// (scanned once at startup by ScanBackgroundChoices, and appended to
// whenever the user picks a new one via the "+" tile) - plus, for
// backward compatibility with the single assets/background.png
// convention this app used before this feature existed, that file
// (if present) is included as an extra choice too, so a background
// someone already had set up isn't silently dropped by this change.
//
// A GIF plays back as a real animation (see UiRenderer::LoadAnimatedImage
// and AppState::Background) - every other format is treated as a
// single-frame "animation" with an effectively infinite delay, so the
// same playback/draw code handles both without a static-vs-animated
// branch anywhere else in the app.

// Where CryptVault remembers which background is currently selected
// across restarts - a sibling file to known_vaults.txt (see
// KnownVaultsPath), same reasoning: just a path, not sensitive, plain
// text is fine.
std::filesystem::path SelectedBackgroundPath() { return KnownVaultsPath().parent_path() / "background.txt"; }

void SaveSelectedBackgroundChoice(const std::string& path) {
    std::filesystem::path file = SelectedBackgroundPath();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (out) {
        out << path;
    }
}

std::string LoadSelectedBackgroundChoice() {
    std::ifstream in(SelectedBackgroundPath());
    if (!in) {
        return "";
    }
    std::string path;
    std::getline(in, path);
    return path;
}

// assets/backgrounds/ (created if missing) plus the legacy single
// assets/background.png, if present - see this section's own comment
// above for why both are included.
std::vector<std::string> ScanBackgroundChoices() {
    std::vector<std::string> paths;

    std::filesystem::path legacy = ExecutableDir() / "assets" / "background.png";
    std::error_code ec;
    if (std::filesystem::exists(legacy, ec)) {
        paths.push_back(legacy.string());
    }

    std::filesystem::path dir = ExecutableDir() / "assets" / "backgrounds";
    std::filesystem::create_directories(dir, ec);
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file()) {
                paths.push_back(entry.path().string());
            }
        }
    }
    return paths;
}

// Loads `path` as the active background - a GIF (checked by
// extension, case-insensitively) plays back via
// UiRenderer::LoadAnimatedImage; anything else loads as one static
// frame via the ordinary LoadIcon (with a delay long enough it will
// never actually advance). Leaves whatever background was previously
// active in place if this one fails to load, rather than blanking the
// background entirely over one bad file. Also updates icons.background
// to frame 0's handle purely as an "is a background active at all"
// flag - see the panel/grid/status-bar translucency checks
// (BackdropColor) that read it for that, not for drawing (the actual
// per-frame draw uses state.background.frame_icons directly - see
// UiAppBeginFrame).
void LoadBackgroundChoice(appshell::UiRenderer& ui, AppState& state, Icons& icons, const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::vector<int> frames;
    std::vector<int> delays;
    bool ok;
    if (ext == ".gif") {
        ok = ui.LoadAnimatedImage(path, frames, delays);
    } else {
        int icon = ui.LoadIcon(path);
        ok = icon >= 0;
        if (ok) {
            frames.push_back(icon);
            delays.push_back(1000 * 60 * 60);  // static - practically never advances
        }
    }
    if (!ok) {
        PushToast(state, "COULDN'T LOAD BACKGROUND: " + std::filesystem::path(path).filename().string());
        return;
    }
    state.background.frame_icons = std::move(frames);
    state.background.frame_delays_ms = std::move(delays);
    state.background.current_frame = 0;
    state.background.frame_started_at = state.now_seconds;
    icons.background = state.background.frame_icons.front();
}

// Shortens `text` (appending "...") so it fits within `max_width`
// pixels at pixel_size 1.0, or returns it unchanged if it already
// fits. Used everywhere user-supplied/auto-generated names (tab
// labels, tile labels) get drawn into a fixed-width box - without
// this, a name like "New Vault (12)" simply overflows past its box's
// edge and overlaps whatever's drawn next to/after it (exactly what
// happened before this existed). Character-by-character rather than
// a width/char-count estimate since the bitmap font is fixed-width,
// so this is exact and still cheap at these string lengths.
std::string TruncateToWidth(ContentRenderer& ui, const std::string& text, float max_width) {
    if (ui.MeasureText(text, 1.0f) <= max_width) {
        return text;
    }
    std::string truncated = text;
    while (!truncated.empty() && ui.MeasureText(truncated + "...", 1.0f) > max_width) {
        truncated.pop_back();
    }
    return truncated.empty() ? "" : truncated + "...";
}

// Builds the toolbar address-bar text for a breadcrumb (the open
// vault's name, then each folder walked into) fit to `max_width`:
// shows every segment in full, separated by " / " so each hierarchy
// level actually reads as a boundary rather than blurring together,
// if there's room for all of it. If there isn't, keeps the first
// segment (the vault) and the last (the folder actually being
// viewed) in full with "..." standing in for whatever's between them
// - those two are usually the most useful parts to keep readable,
// same idea a lot of real file-explorer address bars use for a path
// too long to show in full - only falling back to truncating that
// collapsed form further if even it doesn't fit.
std::string BuildAddressBreadcrumb(ContentRenderer& ui, const std::vector<std::string>& segments,
                                    float max_width) {
    if (segments.empty()) {
        return "";
    }
    std::string full;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i) full += " / ";
        full += segments[i];
    }
    if (ui.MeasureText(full, 1.0f) <= max_width) {
        return full;
    }
    if (segments.size() <= 2) {
        // Nothing meaningful to collapse (there's no "middle" segment
        // between a first and a last that aren't the same two shown
        // above) - just truncate normally.
        return TruncateToWidth(ui, full, max_width);
    }
    std::string collapsed = segments.front() + " / ... / " + segments.back();
    if (ui.MeasureText(collapsed, 1.0f) <= max_width) {
        return collapsed;
    }
    return TruncateToWidth(ui, collapsed, max_width);
}

// A square icon button: brightens on hover, and (if `scale_state` is
// given - a persistent float the caller owns, since this function
// itself holds no state between calls) briefly compresses to 0.94x
// while held down, easing back to 1.0x on release. Returns true on a
// click release inside its bounds this frame.
bool IconButton(ContentRenderer& ui, float x, float y, float size, int icon,
                const appshell::UiInput& input, float* scale_state = nullptr,
                float dt_seconds = 0.0f) {
    bool hovered = HitTest(input, x, y, size, size);
    appshell::UiColor tint = hovered ? palette::kIconTintHover : palette::kIconTint;

    float scale = 1.0f;
    if (scale_state) {
        float target = (hovered && input.mouse_down) ? 0.94f : 1.0f;
        *scale_state = anim::MoveTowards(*scale_state, target, dt_seconds, anim::duration::kMicro);
        scale = *scale_state;
    }

    float drawn_size = size * scale;
    float offset = (size - drawn_size) * 0.5f;
    ui.DrawImage(x + offset, y + offset, drawn_size, drawn_size, icon, tint);
    return hovered && input.clicked;
}

// --- drawing ---------------------------------------------------------------

// Toolbar: left-hand icon cluster differs by screen (VaultList gets a
// folder glyph + "NEW DIR" button; VaultFolder gets folder/back/home/
// up), then a shared address-bar strip, then a shared settings/
// search/refresh/plus cluster on the right.
void DrawToolbar(ContentRenderer& ui, const Icons& icons, AppState& state) {
    ui.DrawRect(
        0,
        0,
        state.content_width,
        kToolbarHeight,
        BackdropColor(palette::kToolbar, icons.background >= 0, 1.0f)
    );

    // ---------------------------------------------------------------------
    // Rounded rectangle helper.
    //
    // Backed by UiRenderer::DrawRoundedRect - a real anti-aliased,
    // shader-computed rounded rect (see ui_renderer.cpp), not an
    // approximation built from several axis-aligned DrawRect() calls.
    // Kept as a thin local alias so call sites below didn't all need
    // to change from `DrawRoundedRect(...)` to `ui.DrawRoundedRect(...)`.
    // ---------------------------------------------------------------------
    auto DrawRoundedRect =
        [&](float x,
            float y,
            float w,
            float h,
            float radius,
            appshell::UiColor color) {
            ui.DrawRoundedRect(x, y, w, h, radius, color);
        };

    // ---------------------------------------------------------------------
    // Shared toolbar geometry.
    // ---------------------------------------------------------------------
    constexpr float kIconSize = 30.0f;

    constexpr float kIconY =
        (kToolbarHeight - kIconSize) * 0.5f;

    // Address box stays at the same position on both screens.
    constexpr float kAddressX = 105.0f;

    constexpr float kAddressY =
        (kToolbarHeight - kAddressBarHeight) * 0.5f;

    constexpr float kBoxRadius = 4.0f;

    float cursor_x = 5.0f;

    // ---------------------------------------------------------------------
    // Folder icon
    // ---------------------------------------------------------------------
    {
        bool hovered = HitTest(
            state.input,
            cursor_x,
            kIconY,
            kIconSize,
            kIconSize
        );

        // This is a decorative brand icon, not a "go home" button
        // (Home already exists, see the VaultFolder-screen controls
        // below) - it only reacts to a click that's actually on it.
        //
        // NOTE: this used to fire on ANY click anywhere in the app,
        // not just on this icon - both the hovered and not-hovered
        // branches below had their own `if (state.input.clicked)`,
        // neither guarded by `hovered`. That meant a single click
        // anywhere (Settings, Search, "ADD FOLDER", a file tile, ...)
        // silently forced state.screen back to VaultList on the very
        // next frame, before whatever that click was actually meant
        // to do got a chance to matter - it only ever *looked* like it
        // worked when a double-click's own handling (e.g. opening a
        // vault) happened to run afterward in the same frame and
        // overwrote the screen back. This is the actual root cause
        // behind "Add Folder sends me back to the vault list",
        // "opening a file sends me back", and made Settings/Search
        // feel broken too - not a separate bug in any of those
        // features themselves.
        if (hovered) {
            // Glassmorphism background.
            DrawRoundedRect(
                cursor_x,
                kIconY,
                kIconSize,
                kIconSize,
                4.0f,
                appshell::UiColor{
                    0.32f,
                    0.35f,
                    0.40f,
                    1.0f
                }
            );

            // Top highlight.
            ui.DrawRect(
                cursor_x + 3.0f,
                kIconY + 2.0f,
                kIconSize - 6.0f,
                1.0f,
                appshell::UiColor{
                    1.0f,
                    1.0f,
                    1.0f,
                    1.00f
                }
            );

            ui.DrawImage(
                cursor_x,
                kIconY,
                kIconSize,
                kIconSize,
                icons.folder,
                palette::kIconTintHover
            );
        } else {
            ui.DrawImage(
                cursor_x,
                kIconY,
                kIconSize,
                kIconSize,
                icons.folder,
                palette::kIconTint
            );
        }

        if (hovered && state.input.clicked) {
            state.screen = Screen::VaultList;
        }
    }

    cursor_x += kIconSize + 6.0f;

    // ---------------------------------------------------------------------
    // Screen-specific controls.
    // ---------------------------------------------------------------------
    if (state.screen == Screen::VaultFolder) {
        constexpr float kNavIconSize = 24.0f;

        constexpr float kNavIconY =
            (kToolbarHeight - kNavIconSize) * 0.5f;

        // -------------------------------------------------------------
        // Back - steps up one folder level in the active tab's
        // current path (see AppState::tab_folder_paths), same as a
        // regular file explorer. A no-op at the vault's root (nothing
        // to step up to) rather than leaving the vault - only Home
        // does that (see below). Deliberately does NOT change screens
        // itself either way.
        // -------------------------------------------------------------
        float back_x =
            cursor_x +
            (kIconSize - kNavIconSize) * 0.5f;

        if (IconButton(
                ui,
                back_x,
                kNavIconY,
                kNavIconSize,
                icons.arrow_back,
                state.input,
                &state.scale_back,
                EffectiveDt(state))) {

            std::vector<std::string>* path = ActiveFolderPath(state);
            if (path && !path->empty()) {
                path->pop_back();
            }
            state.selected_folder = -1;
            state.folder_grid_scroll = 0.0f;
        }

        cursor_x += kIconSize + 6.0f;

        // -------------------------------------------------------------
        // Home - the only button that leaves the open vault and goes
        // back to the vault picker.
        // -------------------------------------------------------------
        float home_x =
            cursor_x +
            (kIconSize - kNavIconSize) * 0.5f;

        if (IconButton(
                ui,
                home_x,
                kNavIconY,
                kNavIconSize,
                icons.home,
                state.input,
                &state.scale_home,
                EffectiveDt(state))) {

            state.screen = Screen::VaultList;
        }

        cursor_x += kIconSize + 8.0f;
    } else {
        // -------------------------------------------------------------
        // ADD VAULT (was "NEW DIR" back when there was a DirTab layer
        // to add to - that layer's gone now, so this directly appends
        // to the flat state.vaults list, same as the grid's own "ADD
        // VAULT" tile).
        //
        // Vertically centered and now using rounded corners.
        // -------------------------------------------------------------
        constexpr float kNewDirW = 62.0f;

        constexpr float kNewDirY =
            (kToolbarHeight - kAddressBarHeight) * 0.5f;

        bool hovered = HitTest(
            state.input,
            cursor_x,
            kNewDirY,
            kNewDirW,
            kAddressBarHeight
        );

        DrawRoundedRect(
            cursor_x,
            kNewDirY,
            kNewDirW,
            kAddressBarHeight,
            kBoxRadius,
            hovered
                ? appshell::UiColor{
                      0.20f,
                      0.22f,
                      0.19f,
                      1.0f
                  }
                : appshell::UiColor{
                      0.16f,
                      0.18f,
                      0.15f,
                      1.0f
                  }
        );

        // Center the label horizontally and vertically.
        float new_dir_text_width =
            ui.MeasureText("ADD VAULT", 1.0f);

        float new_dir_text_x =
            cursor_x +
            (kNewDirW - new_dir_text_width) * 0.5f;

        float new_dir_text_y =
            kNewDirY +
            (kAddressBarHeight - 7.0f) * 0.5f;

        ui.DrawLabel(
            new_dir_text_x,
            new_dir_text_y,
            "ADD VAULT",
            palette::kTextPrimary,
            1.0f
        );

        if (hovered && state.input.clicked) {
            OpenCreateVaultWizard(state);
        }

        cursor_x += kNewDirW + 8.0f;
    }

    // ---------------------------------------------------------------------
    // Right-side icon cluster.
    // ---------------------------------------------------------------------
    constexpr float kRightIconSize = 14.0f;
    constexpr float kRightIconGap = 6.0f;
    constexpr int kRightIconCount = 4;

    float right_cluster_w =
        kRightIconCount * kRightIconSize +
        (kRightIconCount - 1) * kRightIconGap;

    float right_cluster_x =
        state.content_width -
        4.0f -
        right_cluster_w;

    constexpr float kRightIconY =
        (kToolbarHeight - kRightIconSize) * 0.5f;

    float rx = right_cluster_x;

    // Settings.
    if (IconButton(
            ui,
            rx,
            kRightIconY,
            kRightIconSize,
            icons.settings,
            state.input,
            &state.scale_settings,
            EffectiveDt(state))) {

        state.settings_panel_open = !state.settings_panel_open;
        if (state.settings_panel_open) {
            state.command_palette.open = false;  // only one panel at a time
            // Consume this click so DrawSettingsPanel's own "click
            // outside the panel closes it" check (drawn later this
            // same frame) doesn't ALSO see it - without this, opening
            // and closing happened in the same frame every time,
            // since the panel hasn't visually expanded yet at the
            // moment this click is processed, so almost the entire
            // screen (including this very icon) still counted as
            // "outside" it. This was a real, shipped bug: clicking
            // Settings looked like it did nothing at all.
            state.input.clicked = false;
        }
    }

    rx += kRightIconSize + kRightIconGap;

    // Search - doubles as "open the command palette" (see
    // DrawCommandPalette), covering both #7 ("search becomes a real
    // component") and #8 ("command palette") from the original
    // roadmap notes with one panel rather than two.
    if (IconButton(
            ui,
            rx,
            kRightIconY,
            kRightIconSize,
            icons.search,
            state.input,
            &state.scale_search,
            EffectiveDt(state))) {

        state.command_palette.open = !state.command_palette.open;
        if (state.command_palette.open) {
            state.settings_panel_open = false;
            state.command_palette.query.clear();
            state.command_palette.selected = 0;
            // Same reasoning as Settings above - consume the click so
            // DrawCommandPalette's own click-outside-closes check
            // doesn't immediately undo this same open.
            state.input.clicked = false;
        }
    }

    rx += kRightIconSize + kRightIconGap;

    // Refresh.
    if (IconButton(
            ui,
            rx,
            kRightIconY,
            kRightIconSize,
            icons.refresh,
            state.input,
            &state.scale_refresh,
            EffectiveDt(state))) {

        state.selected_vault = -1;
        state.selected_folder = -1;
    }

    rx += kRightIconSize + kRightIconGap;

    // Plus - mirrors whatever the current screen's own grid "Add" tile
    // does (VaultList: "ADD VAULT" tile / ADD VAULT button above;
    // VaultFolder: "ADD FOLDER" tile), just reachable without
    // scrolling. Previously did nothing at all - its return value was
    // being discarded outright.
    if (IconButton(
        ui,
        rx,
        kRightIconY,
        kRightIconSize,
        icons.plus,
        state.input,
        &state.scale_plus,
        EffectiveDt(state)
    )) {
        if (state.screen == Screen::VaultFolder) {
            AddFolderToActiveVault(state);
        } else {
            OpenCreateVaultWizard(state);
        }
    }

    // ---------------------------------------------------------------------
    // Fixed rounded address box.
    //
    // Same position, size and radius in VaultList and VaultFolder.
    // ---------------------------------------------------------------------
    float address_right =
        right_cluster_x - 6.0f;

    float address_width =
        address_right - kAddressX;

    if (address_width > 0.0f) {
        DrawRoundedRect(
            kAddressX,
            kAddressY,
            address_width,
            kAddressBarHeight,
            kBoxRadius,
            palette::kAddressBar
        );
    }

    // ---------------------------------------------------------------------
    // Address text.
    //
    // Centered horizontally and vertically inside the rounded box.
    //
    // NOTE: this used to prepend a hardcoded "C:\USERS\JAINS\" - since
    // there's no real filesystem/username behind this shell, that was
    // exactly the kind of fake seeded data being removed here. On
    // VaultFolder this is the open vault's name plus a breadcrumb of
    // however deep the active tab has navigated (see
    // AppState::tab_folder_paths) - "MyVault / Photos / 2025",
    // matching a real file explorer's address bar - built and fit to
    // the box by BuildAddressBreadcrumb, which shows every segment in
    // full when there's room and collapses the middle (keeping the
    // vault name and the actual current folder readable) when there
    // isn't. On VaultList there's no vault open yet, so it just
    // labels the screen.
    // ---------------------------------------------------------------------
    std::vector<std::string> address_segments;

    if (state.screen == Screen::VaultFolder) {
        // Display name comes from state.vaults (the vault_tabs entry
        // this tab points at), not from the open vaultstore::Vault
        // session, which only knows the decrypted tree, not its own
        // display name (see struct Vault's comment).
        int vault_index = (state.active_vault_tab >= 0
                            && state.active_vault_tab < static_cast<int>(state.vault_tabs.size()))
            ? state.vault_tabs[state.active_vault_tab]
            : -1;
        bool valid = vault_index >= 0 && vault_index < static_cast<int>(state.vaults.size());
        if (valid) {
            address_segments.push_back(state.vaults[vault_index].name);
            std::vector<std::string>* path = ActiveFolderPath(state);
            if (path) {
                for (const std::string& segment : *path) {
                    address_segments.push_back(segment);
                }
            }
        }
    } else {
        address_segments.push_back("VAULTS");
    }

    std::string address_text =
        BuildAddressBreadcrumb(ui, address_segments, std::max(0.0f, address_width - 8.0f));

    float address_text_width =
        ui.MeasureText(address_text, 1.0f);

    float address_text_x =
        kAddressX +
        (address_width - address_text_width) * 0.5f;

    float address_text_y =
        kAddressY +
        (kAddressBarHeight - 7.0f) * 0.5f;

    ui.DrawLabel(
        address_text_x,
        address_text_y,
        address_text,
        palette::kTextPrimary,
        1.0f
    );
}

// A single tab: folder icon, name, and (except when there's only one
// tab, which can't be closed) an "x" close glyph. Reports which
// action (if any) the user took this frame via the out params.
void DrawTab(ContentRenderer& ui, const Icons& icons, float x, float y, float w, float h,
             const std::string& name, bool active, bool closable, const appshell::UiInput& input,
             bool* out_activate, bool* out_close) {
    *out_activate = false;
    *out_close = false;

    appshell::UiColor bg = active ? palette::kTabActive : palette::kTabInactive;
    bool hovered = HitTest(input, x, y, w, h);
    ui.DrawRect(x, y, w, h, bg);

    constexpr float kIconSize = 22.0f;
    float icon_y = y + (h - kIconSize) * 0.5f;
    ui.DrawImage(x + 4.0f, icon_y, kIconSize, kIconSize, icons.folder, palette::kIconTint);

    constexpr float kCloseSize = 10.0f;
    float close_x = x + w - kCloseSize - 4.0f;
    float close_y = y + (h - 7.0f) * 0.5f;

    float label_x = x + 4.0f + kIconSize + 4.0f;
    float label_y = y + (h - 7.0f) * 0.5f;
    // Reserve space for the close "X" (if this tab can be closed) so
    // a long name truncates before reaching it rather than drawing
    // underneath/through it.
    float label_max_width = (closable ? close_x - 4.0f : x + w - 4.0f) - label_x;
    std::string label = TruncateToWidth(ui, name, label_max_width);
    ui.DrawLabel(label_x, label_y, label, palette::kTextPrimary, 1.0f);

    if (closable) {
        bool close_hovered = HitTest(input, close_x - 1.0f, y, kCloseSize + 2.0f, h);
        appshell::UiColor x_color = close_hovered ? palette::kTextPrimary : palette::kTextMuted;
        ui.DrawLabel(close_x, close_y, "X", x_color, 1.0f);
        if (close_hovered && input.clicked) {
            *out_close = true;
            return;
        }
    }

    if (hovered && input.clicked) {
        *out_activate = true;
    }
}

// Active-tab underline that eases toward the active tab's position
// instead of snapping there - used by DrawVaultTabs, the only tab bar
// left now that VaultList's DirTab-based one is gone. Snaps immediately
// (no slide) the first time it's drawn, or right after a screen
// switch, so it doesn't visibly slide in from wherever the *other*
// tab bar's indicator last was.
void DrawSlidingTabIndicator(ContentRenderer& ui, AppState& state, float target_x, float target_w) {
    constexpr float kIndicatorH = 2.0f;
    float y = kToolbarHeight + kTabBarHeight - kIndicatorH;

    bool screen_changed = state.tab_indicator_screen != state.screen;
    if (!state.tab_indicator_valid || screen_changed) {
        state.tab_indicator_x = target_x;
        state.tab_indicator_w = target_w;
        state.tab_indicator_valid = true;
        state.tab_indicator_screen = state.screen;
    } else {
        state.tab_indicator_x =
            anim::MoveTowards(state.tab_indicator_x, target_x, EffectiveDt(state), anim::duration::kUi);
        state.tab_indicator_w =
            anim::MoveTowards(state.tab_indicator_w, target_w, EffectiveDt(state), anim::duration::kUi);
    }

    ui.DrawRect(state.tab_indicator_x, y, state.tab_indicator_w, kIndicatorH, palette::kAccent);
}

void DrawVaultTabs(ContentRenderer& ui, const Icons& icons, AppState& state) {
    ui.DrawRect(0, kToolbarHeight, state.content_width, kTabBarHeight,
                BackdropColor(palette::kTabBarBg, icons.background >= 0, 0.55f));

    constexpr float kTabW = 96.0f;
    constexpr float kTabGap = 4.0f;
    float x = 4.0f;
    float y = kToolbarHeight + 2.0f;
    float h = kTabBarHeight - 4.0f;
    float active_tab_x = x;
    bool have_active_tab = false;

    for (size_t i = 0; i < state.vault_tabs.size(); ++i) {
        int vault_index = state.vault_tabs[i];
        vaultstore::Vault* open_vault = ResolveOpenVault(state, vault_index);
        bool valid_vault_ref = vault_index >= 0 && vault_index < static_cast<int>(state.vaults.size());
        if (!open_vault || !valid_vault_ref) {
            // Stale reference (the vault itself was deleted, or its
            // session got closed out from under this tab some other
            // way) - drop the tab and let the redraw next frame pick
            // up a valid index.
            EraseVaultTab(state, static_cast<int>(i));
            break;
        }
        // The tab's label is the vault's own display name (state.vaults),
        // not anything from the decrypted session - open_vault above is
        // only used to confirm this tab's vault is actually unlocked.
        const std::string& display_name = state.vaults[vault_index].name;

        bool activate = false, close = false;
        DrawTab(ui, icons, x, y, kTabW, h, display_name,
                static_cast<int>(i) == state.active_vault_tab, true, state.input,
                &activate, &close);
        if (static_cast<int>(i) == state.active_vault_tab) {
            active_tab_x = x;
            have_active_tab = true;
        }
        if (activate) {
            state.active_vault_tab = static_cast<int>(i);
            state.selected_folder = -1;
            state.folder_grid_scroll = 0.0f;
        }
        if (close) {
            CloseVaultTab(state, static_cast<int>(i));
            break;
        }
        x += kTabW + kTabGap;
    }

    if (have_active_tab) {
        DrawSlidingTabIndicator(ui, state, active_tab_x, kTabW);
    } else {
        state.tab_indicator_valid = false;
    }

    // "+" duplicates the *current* vault into a brand-new tab - it
    // does NOT go pick a different vault (that's what Home is for,
    // see DrawToolbar). Uses DuplicateVaultTab specifically (always
    // pushes a fresh tab), not SwitchOrOpenVaultTab (which dedups by
    // vault and would just re-select the tab you're already on -
    // that was a real, shipped bug: this button silently did nothing
    // because the vault it was "duplicating" was, trivially, always
    // already open in the very tab you clicked "+" from). Safe to
    // assume a session already exists for the current tab's vault
    // (that's what "current tab" means), so this can go straight to
    // DuplicateVaultTab without an unlock prompt.
    constexpr float kAddSize = 18.0f;
    float add_y = kToolbarHeight + (kTabBarHeight - kAddSize) * 0.5f;
    if (IconButton(ui, x + 2.0f, add_y, kAddSize, icons.new_tab, state.input,
                   &state.scale_vault_tab_add, EffectiveDt(state))) {
        int current_vault_index = (state.active_vault_tab >= 0
                                    && state.active_vault_tab < static_cast<int>(state.vault_tabs.size()))
            ? state.vault_tabs[state.active_vault_tab]
            : -1;
        if (current_vault_index >= 0) {
            DuplicateVaultTab(state, current_vault_index);
        }
    }
}

// Return value for DrawTileGrid: at most one of these is set per
// frame (a click is either on the tile itself, its delete "x", a
// right-click, or neither) - kept as separate indices rather than one
// enum+index pair since callers already destructure by name at the
// call site.
struct TileGridResult {
    int double_clicked = -1;
    int delete_requested = -1;
    int right_clicked = -1;

    // Whichever real (non-"Add"-tile) item the cursor is over right
    // now, live every frame - not an edge/one-shot like the fields
    // above. Used by DrawVaultFolderScreen to decide whether a file
    // dropped from the OS this frame should land inside a specific
    // folder or at the vault's root - reusing this rather than
    // duplicating DrawTileGrid's own hit-testing math a second time
    // elsewhere.
    int hovered_index = -1;
};

// Shared 5-columns-wide tile grid: `items` are drawn as icon+label
// tiles (T must have a `.name` and a `.hover_amount` member - see
// Tile/Vault); if `trailing_action_icon` >= 0, one extra "add" tile is
// appended after the last item, labeled "ADD" / `trailing_label_line2`
// (e.g. "VAULT" or "FOLDER"), using `trailing_hover_amount` for its
// own hover animation since it has no backing item to store one on.
// `selected` is updated in place on a single click; a double-clicked
// tile's index is returned via TileGridResult::double_clicked.
//
// Hover feedback (see the reference mockup's "smoothly brighten /
// soft glow / icon scale 1.0->1.05" notes) is driven by hover_amount:
// eased 0->1 with anim::MoveTowards rather than snapping, so leaving
// a tile mid-hover fades back out instead of popping. The same
// hover_amount also fades in a small "x" in the tile's corner (not
// shown on the trailing "Add" tile) - clicking it reports that tile's
// index via TileGridResult::delete_requested rather than deleting
// immediately; callers are expected to confirm via a modal (see
// DrawDeleteModal) before actually removing anything.
//
// Rows past what fits in `area_h` scroll rather than overflowing past
// the grid's bounds into whatever's drawn next (this used to just let
// the grid grow as tall as it needed to, which looked fine with a
// handful of items and broke completely with a dozen+ - tiles drawn
// on top of the toolbar/status bar, unclickable, unreadable).
// `scroll_offset` is caller-owned (persists across frames, one per
// screen - see AppState) so switching screens doesn't lose/share
// scroll position between e.g. the vault grid and a folder grid.
// A sentinel `tile_icon`/per-item icon value meaning "draw a small
// procedural document glyph instead of an icon asset" - used for file
// tiles (see TileIconFor below), since there's no file.png in
// assets/icons (only folder/vault ever existed before real files
// could be dropped into a vault - see the cross-app drag-and-drop
// work this is part of). Drawn the same way DrawLockToggle's padlock
// is: plain rects, no new asset needed.
constexpr int kProceduralFileIcon = -2;

// Resolves which icon a given grid item should draw. Two overloads
// rather than one templated/branching function: Vault (used by the
// vault-list grid) has no notion of "file vs folder" at all, so it
// always just draws tile_icon; vaultstore::TreeNode (used by the
// open-vault grid, which can now hold a mix of folders and files -
// see AddFolderToActiveVault and the drop-to-encrypt handling in
// DrawVaultFolderScreen) draws tile_icon for folders and the
// procedural document glyph for files. Ordinary overload resolution
// picks the right one for DrawTileGrid<T>'s deduced T - see the
// comment on PushToast's forward declaration above for why this
// pattern (declared before the template that calls it) is needed at
// all in a single-TU, no-header-declarations file like this one.
int TileIconFor(const Vault&, int tile_icon) { return tile_icon; }
int TileIconFor(const vaultstore::TreeNode& item, int tile_icon) {
    return item.is_folder ? tile_icon : kProceduralFileIcon;
}

template <typename T>
TileGridResult DrawTileGrid(ContentRenderer& ui, const Icons& icons, float area_y, float area_h,
                             float content_width, std::vector<T>& items, int tile_icon, int* selected,
                             int trailing_action_icon, const char* trailing_label_line2,
                             float& trailing_hover_amount, float& scroll_offset, float dt_seconds,
                             const appshell::UiInput& input, AppState::TileDrag& drag) {
    ui.DrawRect(0, area_y, content_width, area_h,
                BackdropColor(palette::kContentBg, icons.background >= 0, 0.0f));

    constexpr int kColumns = 5;
    constexpr float kPaddingX = 8.0f;
    constexpr float kPaddingY = 6.0f;
    // Fixed per-row height (not area_h / row-count) is what makes
    // scrolling possible at all - a row-count-dependent height meant
    // more items always fit by getting shorter, right up until they
    // stopped fitting at all and just drew outside the grid instead.
    constexpr float kRowH = 62.0f;
    constexpr float kIconSize = 26.0f;

    float grid_w = content_width - 2.0f * kPaddingX;
    float visible_h = area_h - 2.0f * kPaddingY;

    size_t total_tiles = items.size() + (trailing_action_icon >= 0 ? 1 : 0);
    int total_rows = static_cast<int>((total_tiles + kColumns - 1) / kColumns);
    float content_h = total_rows * kRowH;
    float max_scroll = content_h > visible_h ? content_h - visible_h : 0.0f;

    // Mouse wheel scrolls this grid when the cursor is anywhere over
    // it - clamped every frame (not just when scrolling) so removing
    // items or shrinking the window can't leave scroll_offset pointing
    // past the (now shorter) content.
    if (max_scroll > 0.0f && HitTest(input, 0, area_y, content_width, area_h)) {
        constexpr float kScrollSpeed = 18.0f;  // pixels per wheel "notch"
        scroll_offset -= input.scroll_delta_y * kScrollSpeed;
    }
    scroll_offset = std::max(0.0f, std::min(scroll_offset, max_scroll));

    // A thin scrollbar track+thumb on the right edge - purely a
    // position indicator (dragging it isn't wired up), but its
    // presence/size already tells you there's more below the fold,
    // which the previous "just draw everything and hope" version had
    // no way to communicate at all.
    bool show_scrollbar = max_scroll > 0.0f;
    if (show_scrollbar) {
        constexpr float kScrollbarW = 4.0f;
        float track_x = content_width - kScrollbarW - 2.0f;
        float track_y = area_y + kPaddingY;
        float track_h = visible_h;
        ui.DrawRect(track_x, track_y, kScrollbarW, track_h,
                    appshell::UiColor{1.0f, 1.0f, 1.0f, 0.06f});

        float thumb_h = std::max(16.0f, (visible_h / content_h) * track_h);
        float thumb_travel = track_h - thumb_h;
        float thumb_y = track_y + (max_scroll > 0.0f ? (scroll_offset / max_scroll) * thumb_travel : 0.0f);
        ui.DrawRoundedRect(track_x, thumb_y, kScrollbarW, thumb_h, 2.0f,
                            appshell::UiColor{1.0f, 1.0f, 1.0f, 0.28f});
    }

    // Everything past here can be scrolled out of [area_y, area_y +
    // area_h] - clip so scrolled-away tiles are actually invisible
    // (GPU scissor test, see UiRenderer::PushClipRect) instead of
    // just being drawn past the grid's edge into the toolbar/status
    // bar above/below it.
    ui.PushClipRect(0, area_y, content_width, area_h);

    // Leave room for the scrollbar so tile content doesn't sit under it.
    float usable_grid_w = show_scrollbar ? grid_w - 8.0f : grid_w;
    float usable_cell_w = usable_grid_w / kColumns;

    // Maps a content-space point to the item index of the tile slot
    // it's over, or -1 if it's outside the grid, outside any column,
    // or past the last real item (i.e. over/after the trailing "Add"
    // tile, which can neither be dragged nor be a drop target).
    auto SlotAt = [&](float px, float py) -> int {
        float local_x = px - kPaddingX;
        float local_y = py - (area_y + kPaddingY - scroll_offset);
        if (local_x < 0.0f || local_x >= usable_grid_w || local_y < 0.0f) return -1;
        int col = static_cast<int>(local_x / usable_cell_w);
        int row = static_cast<int>(local_y / kRowH);
        if (col < 0 || col >= kColumns) return -1;
        int idx = row * kColumns + col;
        if (idx < 0 || idx >= static_cast<int>(items.size())) return -1;
        return idx;
    };

    // --- Drag-to-reorder lifecycle (see AppState::TileDrag) ------------
    // Runs before the draw loop below so a drop this frame can reorder
    // `items` before the loop lays out tiles from it.
    bool just_pressed = input.mouse_down && !drag.mouse_was_down;
    if (drag.source_index < 0 && just_pressed) {
        int idx = SlotAt(input.mouse_x, input.mouse_y);
        if (idx >= 0) {
            drag.pressed = true;
            drag.active = false;
            drag.source_index = idx;
            drag.press_x = input.mouse_x;
            drag.press_y = input.mouse_y;
            drag.hover_slot = idx;
        }
    }

    if (drag.pressed) {
        if (input.mouse_down) {
            constexpr float kDragThreshold = 6.0f;  // pixels of movement before a press becomes a drag
            float dx = input.mouse_x - drag.press_x;
            float dy = input.mouse_y - drag.press_y;
            if (!drag.active && (dx * dx + dy * dy) > kDragThreshold * kDragThreshold) {
                drag.active = true;
            }
            if (drag.active) {
                int slot = SlotAt(input.mouse_x, input.mouse_y);
                if (slot >= 0) drag.hover_slot = slot;
            }
        } else {
            // Released - reorder if this was a real drag (past the
            // threshold) that landed on a different slot than it
            // started from. A press that never crossed the threshold
            // falls through untouched, so it's still handled as a
            // plain click/double-click by the loop below, same as
            // before drag-and-drop existed.
            if (drag.active && drag.hover_slot >= 0 && drag.hover_slot != drag.source_index) {
                int from = drag.source_index;
                int to = drag.hover_slot;
                if (from < to) {
                    std::rotate(items.begin() + from, items.begin() + from + 1, items.begin() + to + 1);
                } else {
                    std::rotate(items.begin() + to, items.begin() + from, items.begin() + from + 1);
                }
                // Keep selection following the moved tile rather than
                // silently pointing at whatever now sits at its old index.
                if (*selected == from) *selected = to;
            }
            drag.pressed = false;
            drag.active = false;
            drag.source_index = -1;
            drag.hover_slot = -1;
        }
    }
    drag.mouse_was_down = input.mouse_down;

    TileGridResult result;

    for (size_t i = 0; i < total_tiles; ++i) {
        int col = static_cast<int>(i) % kColumns;
        int row = static_cast<int>(i) / kColumns;
        float cell_x = kPaddingX + col * usable_cell_w;
        float cell_y = area_y + kPaddingY + row * kRowH - scroll_offset;

        // Skip rows that are entirely outside the visible band - they
        // wouldn't render (clip handles that), but skipping their
        // hover/click detection here too means a scrolled-away tile
        // can't be interacted with just because its old screen
        // position happens to overlap something else that's visible.
        bool row_visible = cell_y + kRowH > area_y && cell_y < area_y + area_h;
        if (!row_visible) {
            continue;
        }

        float icon_x = cell_x + (usable_cell_w - kIconSize) * 0.5f;
        float icon_y = cell_y + 6.0f;

        bool is_trailing = (i == items.size());

        // The tile currently being dragged draws as an empty "hole"
        // in its home slot instead of its normal icon/label - the
        // real thing follows the cursor as a ghost, drawn once after
        // this loop. Also skips this tile's own hover/click/delete-x
        // handling while it's lifted.
        if (drag.active && !is_trailing && static_cast<int>(i) == drag.source_index) {
            ui.DrawRoundedRect(cell_x + 2.0f, cell_y, usable_cell_w - 4.0f, kRowH - 4.0f, 4.0f,
                                appshell::UiColor{1.0f, 1.0f, 1.0f, 0.14f});
            continue;
        }

        bool hovered = HitTest(input, cell_x, cell_y, usable_cell_w, kRowH - 2.0f);
        if (hovered && !is_trailing) {
            result.hovered_index = static_cast<int>(i);
        }

        // The slot the dragged tile would land in if dropped right
        // now gets an accent outline so the user can see where it's
        // headed before releasing.
        if (drag.active && !is_trailing && static_cast<int>(i) == drag.hover_slot) {
            appshell::UiColor outline = palette::kAccent;
            outline.a = 0.6f;
            ui.DrawRoundedRect(cell_x + 1.0f, cell_y + 1.0f, usable_cell_w - 2.0f, kRowH - 3.0f, 4.0f,
                                appshell::UiColor{outline.r, outline.g, outline.b, 0.10f});
        }

        float& hover_amount = is_trailing ? trailing_hover_amount : items[i].hover_amount;
        hover_amount = anim::MoveTowards(hover_amount, hovered ? 1.0f : 0.0f, dt_seconds,
                                          anim::duration::kMicro);

        bool is_selected = (*selected == static_cast<int>(i));
        if (is_selected) {
            ui.DrawRect(cell_x + 2.0f, cell_y, usable_cell_w - 4.0f, kRowH - 4.0f, palette::kCellSelected);
        } else if (hover_amount > 0.001f) {
            appshell::UiColor glow = palette::kCellHover;
            glow.a *= hover_amount;
            ui.DrawRect(cell_x + 2.0f, cell_y, usable_cell_w - 4.0f, kRowH - 4.0f, glow);
        }

        // Subtle "pop": icon grows up to 5% and brightens toward
        // kIconTintHover as hover_amount rises, growing/shrinking
        // around its own center so the label position below doesn't
        // need to move with it.
        float icon_size = kIconSize * anim::Lerp(1.0f, 1.05f, hover_amount);
        float scaled_icon_x = icon_x - (icon_size - kIconSize) * 0.5f;
        float scaled_icon_y = icon_y - (icon_size - kIconSize) * 0.5f;

        int icon = is_trailing ? trailing_action_icon : TileIconFor(items[i], tile_icon);
        appshell::UiColor tint = LerpColor(palette::kIconTint, palette::kIconTintHover, hover_amount);
        if (icon == kProceduralFileIcon) {
            // A small document glyph: a page (rounded rect) with a
            // folded top-right corner and a couple of lines standing
            // in for text - same "plain primitives, no asset" idea as
            // DrawLockToggle's padlock. Uses the same scaled_icon_*/
            // icon_size box every other tile's icon uses, so hover
            // pop/glow behave identically to a real DrawImage icon.
            float pw = icon_size * 0.62f;
            float ph = icon_size * 0.82f;
            float px = scaled_icon_x + (icon_size - pw) * 0.5f;
            float py = scaled_icon_y + (icon_size - ph) * 0.5f;
            ui.DrawRoundedRect(px, py, pw, ph, 2.0f, tint);
            float fold = pw * 0.32f;
            appshell::UiColor fold_color = LerpColor(palette::kContentBg, palette::kCellHover, 0.0f);
            fold_color.a = 1.0f;
            ui.DrawRect(px + pw - fold, py, fold, fold, palette::kContentBg);
            appshell::UiColor line_color = palette::kContentBg;
            for (int line = 0; line < 3; ++line) {
                float line_y = py + ph * 0.42f + static_cast<float>(line) * (ph * 0.16f);
                ui.DrawRect(px + pw * 0.16f, line_y, pw * 0.68f, std::max(1.0f, ph * 0.06f), line_color);
            }
        } else {
            ui.DrawImage(scaled_icon_x, scaled_icon_y, icon_size, icon_size, icon, tint);
        }

        // Labels are truncated to the cell's width (minus a small
        // margin) - previously a long name (e.g. an auto-generated
        // "New Vault (12)") just overflowed past the tile into its
        // neighbors, which is exactly the overlapping-text bug this
        // is fixing.
        float label_max_width = usable_cell_w - 4.0f;

        if (is_trailing) {
            // "Add" style tile: small centered plus badge over the
            // icon plus a two-line label underneath, matching the
            // reference mockup's "Add\nVault" tile. Anchored to the
            // *unscaled* icon box so the badge doesn't drift as the
            // icon pops on hover.
            constexpr float kBadgeSize = 12.0f;
            ui.DrawImage(icon_x + (kIconSize - kBadgeSize) * 0.5f,
                         icon_y + (kIconSize - kBadgeSize) * 0.5f, kBadgeSize, kBadgeSize,
                         icons.plus, appshell::UiColor{1.0f, 1.0f, 1.0f, 1.0f});
            std::string line2 = TruncateToWidth(ui, trailing_label_line2, label_max_width);
            float label_y = icon_y + kIconSize + 4.0f;
            ui.DrawLabel(cell_x + (usable_cell_w - ui.MeasureText("ADD", 1.0f)) * 0.5f, label_y,
                         "ADD", palette::kTextMuted, 1.0f);
            ui.DrawLabel(cell_x + (usable_cell_w - ui.MeasureText(line2, 1.0f)) * 0.5f,
                         label_y + 8.0f, line2, palette::kTextMuted, 1.0f);
        } else {
            std::string label = TruncateToWidth(ui, items[i].name, label_max_width);
            float label_w = ui.MeasureText(label, 1.0f);
            float label_x = cell_x + (usable_cell_w - label_w) * 0.5f;
            float label_y = icon_y + kIconSize + 6.0f;
            ui.DrawLabel(label_x, label_y, label, palette::kTextMuted, 1.0f);
        }

        // Small "delete" x, fading in with the same hover_amount as
        // everything else - not drawn on the trailing "Add" tile
        // (there's nothing there to delete). Reports its index via
        // delete_requested rather than deleting on the spot; see
        // DrawDeleteModal for the confirm step.
        bool over_delete_x = false;
        if (!is_trailing && hover_amount > 0.01f) {
            constexpr float kXSize = 12.0f;
            float x_x = cell_x + usable_cell_w - kXSize - 2.0f;
            float x_y = cell_y + 2.0f;
            over_delete_x = HitTest(input, x_x, x_y, kXSize, kXSize);

            appshell::UiColor x_bg{0.0f, 0.0f, 0.0f, 0.35f * hover_amount};
            ui.DrawRoundedRect(x_x, x_y, kXSize, kXSize, 3.0f, x_bg);
            appshell::UiColor x_color = over_delete_x ? palette::kTextPrimary : palette::kTextMuted;
            x_color.a *= hover_amount;
            ui.DrawLabel(x_x + 3.0f, x_y + 2.0f, "X", x_color, 1.0f);

            if (over_delete_x && input.clicked) {
                result.delete_requested = static_cast<int>(i);
            }
        }

        // Guarded by !over_delete_x so a click on the little "x"
        // doesn't *also* register as selecting/opening the tile it's
        // sitting on top of. Also guarded by !is_trailing - the "Add"
        // tile isn't a real item, so it must never end up as
        // *selected (that's items.size(), out of bounds for anything
        // that later indexes items[*selected]).
        if (!is_trailing && hovered && input.clicked && !over_delete_x) {
            *selected = static_cast<int>(i);
        }
        if (hovered && input.double_clicked && !over_delete_x) {
            result.double_clicked = static_cast<int>(i);
        }
        if (!is_trailing && hovered && input.right_clicked) {
            result.right_clicked = static_cast<int>(i);
        }
    }

    // The dragged tile itself, floating at the cursor - drawn last so
    // it renders on top of every tile it passes over. Kept inside the
    // grid's own clip rect (rather than after PopClipRect) so it
    // can't visually spill onto the toolbar/status bar above/below.
    if (drag.active && drag.source_index >= 0 && drag.source_index < static_cast<int>(items.size())) {
        constexpr float kGhostW = 84.0f;
        float ghost_x = input.mouse_x - kGhostW * 0.5f;
        float ghost_y = input.mouse_y - kIconSize * 0.5f - 4.0f;

        ui.DrawRoundedRect(ghost_x, ghost_y, kGhostW, kIconSize + 22.0f, 6.0f,
                            appshell::UiColor{0.0f, 0.0f, 0.0f, 0.55f});
        appshell::UiColor ghost_tint = palette::kIconTintHover;
        ghost_tint.a = 0.9f;
        ui.DrawImage(ghost_x + (kGhostW - kIconSize) * 0.5f, ghost_y + 4.0f, kIconSize, kIconSize,
                     tile_icon, ghost_tint);
        std::string ghost_label = TruncateToWidth(ui, items[drag.source_index].name, kGhostW - 6.0f);
        appshell::UiColor ghost_text = palette::kTextPrimary;
        ghost_text.a = 0.9f;
        ui.DrawLabel(ghost_x + (kGhostW - ui.MeasureText(ghost_label, 1.0f)) * 0.5f,
                     ghost_y + kIconSize + 8.0f, ghost_label, ghost_text, 1.0f);
    }

    ui.PopClipRect();

    return result;
}

void DrawStatusBar(ContentRenderer& ui, float content_width, float content_height,
                    const std::string& text, bool has_background) {
    float y = content_height - kStatusBarHeight;
    ui.DrawRect(0, y, content_width, kStatusBarHeight, BackdropColor(palette::kStatusBar, has_background, 0.55f));
    ui.DrawLabel(8.0f, y + (kStatusBarHeight - 7.0f) * 0.5f, text, palette::kTextMuted, 1.0f);
}

void DrawVaultListScreen(ContentRenderer& ui, const Icons& icons, AppState& state) {
    // No tab bar here anymore - there's nothing to be tabs *of* now
    // that the DirTab layer is gone (see AppState::vaults' comment).
    // Every vault just lives in this one flat grid.
    float grid_y = kToolbarHeight;
    float grid_h = state.content_height - grid_y - kStatusBarHeight;

    TileGridResult grid_result = DrawTileGrid(ui, icons, grid_y, grid_h, state.content_width,
                                                state.vaults, icons.vault, &state.selected_vault,
                                                icons.folder, "VAULT",
                                                state.add_vault_hover, state.vault_grid_scroll,
                                                EffectiveDt(state), state.input, state.grid_drag);

    // There's no vault open here to encrypt a dropped file into (see
    // DropFilesIntoVault, called from DrawVaultFolderScreen instead) -
    // just say so rather than silently swallowing the drop.
    if (!state.input.dropped_paths.empty()) {
        PushToast(state, "OPEN A VAULT FIRST TO DROP FILES INTO IT");
    }

    if (grid_result.delete_requested >= 0) {
        // See DrawDeleteModal's vault-deletion branch: this removes
        // the vault from CryptVault's own list, it does NOT delete the
        // real encrypted directory on disk - deliberately, since that's
        // a much more dangerous, harder-to-undo action that deserves
        // its own, more emphatic confirmation flow rather than being
        // silently bundled into "remove from list".
        AppState::PendingDelete pending;
        pending.active = true;
        pending.is_vault = true;
        pending.index = grid_result.delete_requested;
        pending.item_name = state.vaults[grid_result.delete_requested].name;
        state.pending_delete = pending;
    }

    if (grid_result.right_clicked >= 0) {
        AppState::ContextMenu menu;
        menu.open = true;
        menu.is_vault = true;
        menu.index = grid_result.right_clicked;
        menu.x = state.input.mouse_x;
        menu.y = state.input.mouse_y;
        state.context_menu = menu;
    }

    if (grid_result.double_clicked >= 0) {
        if (grid_result.double_clicked == static_cast<int>(state.vaults.size())) {
            // "Add Vault" tile double-clicked.
            OpenCreateVaultWizard(state);
        } else {
            // Open (unlocking it first if it isn't already) the double-clicked vault.
            RequestOpenVault(state, grid_result.double_clicked);
        }
    }

    DrawStatusBar(ui, state.content_width, state.content_height,
                  std::to_string(state.vaults.size()) + " VAULTS FOUND", icons.background >= 0);
}

void DrawVaultFolderScreen(ContentRenderer& ui, const Icons& icons, AppState& state) {
    DrawVaultTabs(ui, icons, state);

    vaultstore::Vault* vault = ActiveOpenVault(state);
    if (!vault) {
        // Every open vault got closed (or the active-tab index is
        // stale) - fall back rather than drawing an address bar/grid
        // with nothing to show.
        state.screen = Screen::VaultList;
        return;
    }
    std::vector<std::string>* path = ActiveFolderPath(state);
    vaultstore::TreeNode& current_folder = CurrentFolder(*vault, path);
    std::vector<vaultstore::TreeNode>& folders = current_folder.children;

    float grid_y = kToolbarHeight + kTabBarHeight;
    float grid_h = state.content_height - grid_y - kStatusBarHeight;

    TileGridResult grid_result = DrawTileGrid(ui, icons, grid_y, grid_h, state.content_width,
                                                folders, icons.folder, &state.selected_folder,
                                                icons.folder, "FOLDER",
                                                state.add_folder_hover, state.folder_grid_scroll,
                                                EffectiveDt(state), state.input, state.grid_drag);

    // Cross-app drag-and-drop, receiving half: files dragged in from
    // Explorer/Finder/a file manager (see main.cpp's on_files_dropped)
    // land here. Needs grid_result.hovered_index (which folder, if
    // any, the cursor was over at drop time), so this has to run
    // after DrawTileGrid rather than before.
    DropFilesIntoVault(state, *vault, current_folder, grid_result.hovered_index);

    if (grid_result.delete_requested >= 0) {
        AppState::PendingDelete pending;
        pending.active = true;
        pending.is_vault = false;
        pending.index = grid_result.delete_requested;
        pending.item_name = folders[grid_result.delete_requested].name;
        state.pending_delete = pending;
    }

    if (grid_result.right_clicked >= 0) {
        AppState::ContextMenu menu;
        menu.open = true;
        menu.is_vault = false;
        menu.index = grid_result.right_clicked;
        menu.x = state.input.mouse_x;
        menu.y = state.input.mouse_y;
        state.context_menu = menu;
    }

    if (grid_result.double_clicked == static_cast<int>(folders.size())) {
        // "Add Folder" tile double-clicked.
        AddFolderToActiveVault(state);
    } else if (grid_result.double_clicked >= 0
               && grid_result.double_clicked < static_cast<int>(folders.size())) {
        vaultstore::TreeNode& item = folders[grid_result.double_clicked];
        if (item.is_folder) {
            // Navigate in - see AppState::tab_folder_paths' comment
            // for why this is per-tab rather than a single shared
            // "current folder".
            if (path) {
                path->push_back(item.name);
            }
            state.selected_folder = -1;
            state.folder_grid_scroll = 0.0f;
        } else {
            // Decrypt to a real temp file and hand it to the OS - see
            // OpenFileFromVault's comment for the security trade-off
            // this necessarily involves.
            OpenFileFromVault(state, *vault, item);
        }
    }

    DrawStatusBar(ui, state.content_width, state.content_height,
                  std::to_string(folders.size()) + " ITEMS", icons.background >= 0);

    // See AppState::vault_open_flourish's comment for scope. Drawn
    // last (on top of the grid/status bar) so it reads as a flash
    // over the screen that just appeared, not underneath it.
    if (state.vault_open_flourish.started() && !state.vault_open_flourish.Finished(state.now_seconds)) {
        float t = state.vault_open_flourish.Progress(state.now_seconds);
        float alpha = 0.28f * (1.0f - anim::Ease(anim::Easing::kEaseOutCubic, t));
        ui.DrawRect(0, 0, state.content_width, state.content_height,
                    appshell::UiColor{palette::kAccent.r, palette::kAccent.g, palette::kAccent.b, alpha});
    }
}

// Toast notifications: slide in from the top-right, hold, fade out.
// One anim::AnimationProgress against a fixed kLifetime covers all
// three phases (a single Timeline per toast would work too, but a
// plain start_seconds + one shared duration constant is simpler here
// since every toast animates identically). Expired toasts are culled
// at the top of this function rather than needing a separate "tick"
// pass anywhere else.
void DrawToasts(ContentRenderer& ui, AppState& state) {
    constexpr float kLifetime = 3.0f;
    constexpr float kSlideInFrac = 0.15f;   // first 15% of lifetime: slide/ease in
    constexpr float kFadeOutFrac = 0.15f;   // last 15% of lifetime: fade out
    constexpr float kToastW = 130.0f;
    constexpr float kToastH = 22.0f;
    constexpr float kGap = 4.0f;
    constexpr float kMargin = 6.0f;

    state.toasts.erase(
        std::remove_if(state.toasts.begin(), state.toasts.end(),
                       [&](const AppState::Toast& t) {
                           return anim::AnimationProgress(state.now_seconds, t.start_seconds,
                                                           kLifetime) >= 1.0f;
                       }),
        state.toasts.end());

    float y = kMargin;
    for (const AppState::Toast& toast : state.toasts) {
        float p = anim::AnimationProgress(state.now_seconds, toast.start_seconds, kLifetime);

        float slide_in = anim::Ease(anim::Easing::kEaseOutCubic,
                                     anim::Clamp01(p / kSlideInFrac));
        float alpha = (p < 1.0f - kFadeOutFrac)
            ? 1.0f
            : 1.0f - anim::Clamp01((p - (1.0f - kFadeOutFrac)) / kFadeOutFrac);

        float slide_offset = (1.0f - slide_in) * (kToastW + kMargin);
        float x = state.content_width - kMargin - kToastW + slide_offset;

        appshell::UiColor bg{0.10f, 0.12f, 0.11f, 0.92f * alpha};
        ui.DrawRoundedRect(x, y, kToastW, kToastH, 4.0f, bg);

        appshell::UiColor accent = palette::kAccent;
        accent.a *= alpha;
        ui.DrawRect(x, y, 3.0f, kToastH, accent);

        appshell::UiColor text_color = palette::kTextPrimary;
        text_color.a *= alpha;
        ui.DrawLabel(x + 10.0f, y + (kToastH - 7.0f) * 0.5f, toast.text, text_color, 1.0f);

        y += kToastH + kGap;
    }
}

// Confirm-delete modal: a dim overlay across the whole content area
// plus a centered panel that fades in and scales up from 0.95->1.0
// (see the reference mockup's "opacity 0->1, scale 0.95->1" note for
// destructive-action dialogs). Opened by DrawTileGrid's per-tile "x"
// (see AppState::PendingDelete); does nothing if no delete is pending.
//
// While this is showing, UiAppBeginFrame masks clicked/double_clicked
// out of the screen underneath (see its comment) so Cancel/Delete are
// the only things clickable - real deletion happens here, in
// response to the Delete button, not at the point the "x" was
// originally clicked.
void DrawDeleteModal(ContentRenderer& ui, AppState& state) {
    if (!state.pending_delete.active) {
        return;
    }

    if (!state.pending_delete.timeline.started()) {
        state.pending_delete.timeline.duration_seconds =
            state.settings.animations_enabled ? anim::duration::kUi : 0.001f;
        state.pending_delete.timeline.Start(state.now_seconds);
    }
    float eased = anim::Ease(anim::Easing::kEaseOutCubic,
                              state.pending_delete.timeline.Progress(state.now_seconds));

    ui.DrawRect(0, 0, state.content_width, state.content_height,
                appshell::UiColor{0.0f, 0.0f, 0.0f, 0.5f * eased});

    constexpr float kPanelW = 190.0f;
    constexpr float kPanelH = 92.0f;
    float scale = anim::Lerp(0.95f, 1.0f, eased);
    float panel_w = kPanelW * scale;
    float panel_h = kPanelH * scale;
    float panel_x = (state.content_width - panel_w) * 0.5f;
    float panel_y = (state.content_height - panel_h) * 0.5f;

    ui.DrawRoundedRect(panel_x, panel_y, panel_w, panel_h, 6.0f,
                        appshell::UiColor{0.15f, 0.17f, 0.16f, eased});

    const char* title = state.pending_delete.is_vault ? "DELETE VAULT" : "DELETE FOLDER";
    ui.DrawLabel(panel_x + 12.0f, panel_y + 12.0f, title, palette::kTextPrimary, 1.0f);

    std::string message = TruncateToWidth(ui, state.pending_delete.item_name + "?", panel_w - 24.0f);
    ui.DrawLabel(panel_x + 12.0f, panel_y + 30.0f, message, palette::kTextMuted, 1.0f);

    constexpr float kBtnW = 62.0f;
    constexpr float kBtnH = 18.0f;
    float btn_y = panel_y + panel_h - kBtnH - 10.0f;
    float delete_x = panel_x + panel_w - kBtnW - 10.0f;
    float cancel_x = delete_x - kBtnW - 6.0f;

    bool cancel_hovered = HitTest(state.input, cancel_x, btn_y, kBtnW, kBtnH);
    bool delete_hovered = HitTest(state.input, delete_x, btn_y, kBtnW, kBtnH);

    ui.DrawRoundedRect(cancel_x, btn_y, kBtnW, kBtnH, 3.0f,
                        cancel_hovered ? palette::kTabActive : palette::kTabInactive);
    ui.DrawLabel(cancel_x + (kBtnW - ui.MeasureText("CANCEL", 1.0f)) * 0.5f,
                 btn_y + (kBtnH - 7.0f) * 0.5f, "CANCEL", palette::kTextPrimary, 1.0f);

    ui.DrawRoundedRect(delete_x, btn_y, kBtnW, kBtnH, 3.0f,
                        delete_hovered ? palette::kDangerHover : palette::kDanger);
    ui.DrawLabel(delete_x + (kBtnW - ui.MeasureText("DELETE", 1.0f)) * 0.5f,
                 btn_y + (kBtnH - 7.0f) * 0.5f, "DELETE", palette::kTextPrimary, 1.0f);

    if (cancel_hovered && state.input.clicked) {
        state.pending_delete = AppState::PendingDelete{};
        return;
    }

    if (delete_hovered && state.input.clicked) {
        std::string deleted_name = state.pending_delete.item_name;
        bool delete_ok = true;  // vault-list removal below can't fail; a real vaultstore::Delete can

        if (state.pending_delete.is_vault) {
            int deleted_vault_index = state.pending_delete.index;
            if (deleted_vault_index >= 0
                && deleted_vault_index < static_cast<int>(state.vaults.size())) {
                state.vaults.erase(state.vaults.begin() + deleted_vault_index);

                // Erasing shifts every later vault down one index - an
                // open vault_tabs entry pointing at index 5 now needs
                // to point at 4, or it silently shows the vault that
                // slid into its old slot instead of the one the user
                // actually opened. A tab pointing at the deleted vault
                // itself gets dropped outright (same as the existing
                // stale-reference handling in DrawVaultTabs, just
                // applied immediately instead of waiting a frame).
                // Note more than one tab can point at the same index
                // now (see DrawVaultTabs' "+"), so this doesn't stop
                // at the first match.
                for (size_t i = 0; i < state.vault_tabs.size();) {
                    if (state.vault_tabs[i] == deleted_vault_index) {
                        EraseVaultTab(state, static_cast<int>(i));
                        continue;  // don't advance i - next element shifted into position i
                    }
                    if (state.vault_tabs[i] > deleted_vault_index) {
                        --state.vault_tabs[i];
                    }
                    ++i;
                }
                if (state.vault_tabs.empty() && state.screen == Screen::VaultFolder) {
                    state.screen = Screen::VaultList;
                }

                // Same shift-or-drop treatment for any open session
                // referencing this vault - "remove from list" should
                // also mean "and stop holding its master key in
                // memory", not just "no tab shows it anymore".
                for (size_t i = 0; i < state.open_sessions.size();) {
                    int& session_vault_index = state.open_sessions[i].vault_index;
                    if (session_vault_index == deleted_vault_index) {
                        state.open_sessions.erase(state.open_sessions.begin() + i);
                        continue;
                    }
                    if (session_vault_index > deleted_vault_index) {
                        --session_vault_index;
                    }
                    ++i;
                }

                if (state.selected_vault >= static_cast<int>(state.vaults.size())) {
                    state.selected_vault = -1;
                }
                SaveKnownVaults(state);
            }
        } else if (vaultstore::Vault* vault = ActiveOpenVault(state)) {
            vaultstore::TreeNode& current_folder = CurrentFolder(*vault, ActiveFolderPath(state));
            std::vector<vaultstore::TreeNode>& folders = current_folder.children;
            if (state.pending_delete.index >= 0
                && state.pending_delete.index < static_cast<int>(folders.size())) {
                std::string delete_error;
                std::string child_name = folders[state.pending_delete.index].name;
                if (vault->Delete(current_folder, child_name, delete_error)) {
                    if (state.selected_folder >= static_cast<int>(folders.size())) {
                        state.selected_folder = -1;
                    }
                } else {
                    delete_ok = false;
                    PushToast(state, "COULDN'T DELETE: " + delete_error);
                }
            }
        }

        if (delete_ok) {
            PushToast(state, deleted_name + " DELETED");
        }
        state.pending_delete = AppState::PendingDelete{};
    }
}

// Confirmation for Settings' "CLEAR ALL DATA" row. Same "forget from
// the list, never touch the real files on disk" stance as deleting a
// single vault (see DrawDeleteModal's own comment on that) - clears
// every vault from CryptVault's own list (and the on-disk
// known_vaults.txt that persists it - see SaveKnownVaults) and the
// whole-app lock password, but never deletes a single real encrypted
// vault directory. The confirmation message says this explicitly
// rather than assuming "clear all data" obviously means "forget,
// don't destroy" - it's a broad enough label that assuming the
// reading everyone would land on is risky for something irreversible
// from the app's own perspective.
void DrawClearAllDataModal(ContentRenderer& ui, AppState& state) {
    if (!state.clear_all_data_pending) {
        return;
    }

    ui.DrawRect(0, 0, state.content_width, state.content_height,
                appshell::UiColor{0.0f, 0.0f, 0.0f, 0.5f});

    constexpr float kPanelW = 210.0f;
    constexpr float kPanelH = 108.0f;
    float panel_x = (state.content_width - kPanelW) * 0.5f;
    float panel_y = (state.content_height - kPanelH) * 0.5f;

    ui.DrawRoundedRect(panel_x, panel_y, kPanelW, kPanelH, 6.0f,
                        appshell::UiColor{0.15f, 0.17f, 0.16f, 1.0f});

    ui.DrawLabel(panel_x + 12.0f, panel_y + 12.0f, "CLEAR ALL DATA", palette::kTextPrimary, 1.0f);

    const char* line1 = "Forgets every vault from this app's";
    const char* line2 = "list and resets the app password.";
    const char* line3 = "Real encrypted vault folders on disk";
    const char* line4 = "are NOT deleted.";
    ui.DrawLabel(panel_x + 12.0f, panel_y + 30.0f, line1, palette::kTextMuted, 1.0f);
    ui.DrawLabel(panel_x + 12.0f, panel_y + 42.0f, line2, palette::kTextMuted, 1.0f);
    ui.DrawLabel(panel_x + 12.0f, panel_y + 58.0f, line3, palette::kTextMuted, 1.0f);
    ui.DrawLabel(panel_x + 12.0f, panel_y + 70.0f, line4, palette::kTextMuted, 1.0f);

    constexpr float kBtnW = 74.0f;
    constexpr float kBtnH = 18.0f;
    float btn_y = panel_y + kPanelH - kBtnH - 10.0f;
    float clear_x = panel_x + kPanelW - kBtnW - 10.0f;
    float cancel_x = clear_x - kBtnW - 6.0f;

    bool cancel_hovered = HitTest(state.input, cancel_x, btn_y, kBtnW, kBtnH);
    bool clear_hovered = HitTest(state.input, clear_x, btn_y, kBtnW, kBtnH);

    ui.DrawRoundedRect(cancel_x, btn_y, kBtnW, kBtnH, 3.0f,
                        cancel_hovered ? palette::kTabActive : palette::kTabInactive);
    ui.DrawLabel(cancel_x + (kBtnW - ui.MeasureText("CANCEL", 1.0f)) * 0.5f,
                 btn_y + (kBtnH - 7.0f) * 0.5f, "CANCEL", palette::kTextPrimary, 1.0f);

    ui.DrawRoundedRect(clear_x, btn_y, kBtnW, kBtnH, 3.0f,
                        clear_hovered ? palette::kDangerHover : palette::kDanger);
    ui.DrawLabel(clear_x + (kBtnW - ui.MeasureText("CLEAR", 1.0f)) * 0.5f,
                 btn_y + (kBtnH - 7.0f) * 0.5f, "CLEAR", palette::kTextPrimary, 1.0f);

    if (cancel_hovered && state.input.clicked) {
        state.clear_all_data_pending = false;
        return;
    }

    if (clear_hovered && state.input.clicked) {
        // Sessions first (scrubs every still-open vault's master key
        // from memory via ~vaultstore::Vault - see that destructor's
        // own comment), then everything downstream of them.
        state.open_sessions.clear();
        state.vault_tabs.clear();
        state.tab_folder_paths.clear();
        state.vaults.clear();
        state.selected_vault = -1;
        state.selected_folder = -1;
        state.active_vault_tab = 0;
        state.screen = Screen::VaultList;

        state.lock.password.clear();
        state.lock.locked = false;
        state.lock.prompt_open = false;

        SaveKnownVaults(state);  // persist the now-empty list

        state.clear_all_data_pending = false;
        PushToast(state, "ALL DATA CLEARED (REAL VAULT FILES ON DISK WERE NOT TOUCHED)");
    }
}

// Settings panel: slides in from the right edge (see the original
// roadmap notes' "Slide from the right" for the settings panel).
// Two *functional* toggles - see AppState::Settings' comment for
// exactly what each one does and doesn't reach yet. Closes via the
// gear icon again (see DrawToolbar) or by clicking the dimmed scrim
// to its left.
void DrawSettingsPanel(ContentRenderer& ui, AppState& state, Icons& icons) {
    float target = state.settings_panel_open ? 1.0f : 0.0f;
    state.settings_panel_progress =
        anim::MoveTowards(state.settings_panel_progress, target, EffectiveDt(state), anim::duration::kUi);

    if (state.settings_panel_progress <= 0.001f && !state.settings_panel_open) {
        return;
    }

    float progress = anim::Ease(anim::Easing::kEaseOutCubic, state.settings_panel_progress);

    constexpr float kPanelW = 190.0f;
    float panel_x = state.content_width - kPanelW * progress;

    // Dimmed scrim over the rest of the content, to its left - click
    // it to close, same "click outside to dismiss" idea as the delete
    // modal's Cancel button, just without a button to click.
    ui.DrawRect(0, 0, state.content_width, state.content_height,
                appshell::UiColor{0.0f, 0.0f, 0.0f, 0.25f * progress});
    if (state.settings_panel_open
        && HitTest(state.input, 0, 0, panel_x, state.content_height) && state.input.clicked) {
        state.settings_panel_open = false;
    }

    float panel_alpha = state.settings.glass_effect_enabled ? 0.90f : 0.99f;
    ui.DrawRect(panel_x, 0, kPanelW, state.content_height,
                appshell::UiColor{0.13f, 0.15f, 0.14f, panel_alpha * progress});
    ui.DrawRect(panel_x, 0, 2.0f, state.content_height,
                appshell::UiColor{1.0f, 1.0f, 1.0f, 0.06f * progress});  // left-edge highlight

    // Too thin a sliver to bother with text (it'd just get clipped
    // oddly mid-slide) - the panel itself is still visible above.
    if (state.settings_panel_progress < 0.15f) {
        return;
    }

    float x = panel_x + 12.0f;
    float title_y = 14.0f;
    ui.DrawLabel(x, title_y, "SETTINGS", palette::kTextPrimary, 1.0f);

    // Everything below the title scrolls - the panel's content
    // (toggles, security, the API status, every known vault's
    // location, data controls, quit) can easily run taller than the
    // window, especially at the small default window size or with
    // several vaults listed - the QUIT button in particular was
    // getting cut off at the bottom with no way to reach it before
    // this existed. y starts pre-offset by -state.settings_scroll and
    // every row below just adds to it as before, so scrolling is a
    // one-line change here rather than needing every individual
    // DrawLabel/HitTest call in this function to know about it
    // separately.
    constexpr float kContentTop = 34.0f;
    float visible_bottom = state.content_height - 8.0f;
    ui.PushClipRect(panel_x, kContentTop, kPanelW, std::max(0.0f, visible_bottom - kContentTop));

    float y = kContentTop + 4.0f - state.settings_scroll;
    ui.DrawLabel(x, y, "APPEARANCE", palette::kTextMuted, 1.0f);
    y += 14.0f;

    auto DrawToggleRow = [&](const char* label, bool& value) {
        ui.DrawLabel(x, y + 4.0f, label, palette::kTextPrimary, 1.0f);

        constexpr float kToggleW = 34.0f;
        constexpr float kToggleH = 14.0f;
        float toggle_x = panel_x + kPanelW - kToggleW - 12.0f;
        bool hovered = HitTest(state.input, toggle_x, y, kToggleW, kToggleH);

        ui.DrawRoundedRect(toggle_x, y, kToggleW, kToggleH, 7.0f,
                            value ? palette::kAccent : palette::kTabInactive);
        const char* state_label = value ? "ON" : "OFF";
        ui.DrawLabel(toggle_x + (kToggleW - ui.MeasureText(state_label, 1.0f)) * 0.5f, y + 3.5f,
                     state_label, palette::kTextPrimary, 1.0f);

        if (hovered && state.input.clicked) {
            value = !value;
        }
        y += 22.0f;
    };

    DrawToggleRow("ANIMATIONS", state.settings.animations_enabled);
    DrawToggleRow("GLASS EFFECT", state.settings.glass_effect_enabled);

    y += 8.0f;
    ui.DrawLabel(x, y, "BACKGROUND", palette::kTextMuted, 1.0f);
    y += 14.0f;
    {
        // A grid of small square thumbnails - one per real file under
        // assets/backgrounds/ (see ScanBackgroundChoices), plus a
        // trailing "+" tile that opens a native file picker (see
        // platform::PickFile). Picking a file copies it into
        // assets/backgrounds/ and appends it here as a new, permanent
        // choice - the grid only ever grows via a real pick, never
        // shrinks on its own (removing a background isn't implemented -
        // deleting the file from assets/backgrounds/ by hand and
        // restarting is the way to do that today).
        constexpr float kSquare = 26.0f;
        constexpr float kGap = 5.0f;
        float available_w = kPanelW - 24.0f;
        int per_row = std::max(1, static_cast<int>((available_w + kGap) / (kSquare + kGap)));
        int total_tiles = static_cast<int>(state.background_choices.size()) + 1;

        for (int i = 0; i < total_tiles; ++i) {
            int col = i % per_row;
            int row = i / per_row;
            float tile_x = x + static_cast<float>(col) * (kSquare + kGap);
            float tile_y = y + static_cast<float>(row) * (kSquare + kGap);
            bool hovered = HitTest(state.input, tile_x, tile_y, kSquare, kSquare);
            bool is_plus_tile = (i == total_tiles - 1);
            bool is_selected = !is_plus_tile && i == state.background_selected_index;

            ui.DrawRoundedRect(tile_x, tile_y, kSquare, kSquare, 4.0f,
                                is_selected ? palette::kAccent
                                            : (hovered ? palette::kTabActive : palette::kTabInactive));

            if (is_plus_tile) {
                ui.DrawLabel(tile_x + (kSquare - ui.MeasureText("+", 1.0f)) * 0.5f, tile_y + (kSquare - 7.0f) * 0.5f,
                             "+", palette::kTextPrimary, 1.0f);
            } else if (i < static_cast<int>(state.background_thumbnails.size())
                       && state.background_thumbnails[i] >= 0) {
                ui.DrawImage(tile_x + 2.0f, tile_y + 2.0f, kSquare - 4.0f, kSquare - 4.0f,
                             state.background_thumbnails[i]);
            }

            if (hovered && state.input.clicked) {
                if (is_plus_tile) {
                    std::string picked;
                    if (platform::PickFile("Choose a background image or GIF", "Images and GIFs",
                                            {"png", "jpg", "jpeg", "gif"}, picked)) {
                        std::error_code ec;
                        std::filesystem::path dest_dir = ExecutableDir() / "assets" / "backgrounds";
                        std::filesystem::create_directories(dest_dir, ec);
                        std::filesystem::path dest = dest_dir / std::filesystem::path(picked).filename();
                        std::filesystem::copy_file(picked, dest, std::filesystem::copy_options::overwrite_existing,
                                                     ec);
                        if (!ec) {
                            std::string dest_str = dest.string();
                            state.background_choices.push_back(dest_str);
                            state.background_thumbnails.push_back(ui.Raw().LoadIcon(dest_str));
                            state.background_selected_index = static_cast<int>(state.background_choices.size()) - 1;
                            LoadBackgroundChoice(ui.Raw(), state, icons, dest_str);
                            SaveSelectedBackgroundChoice(dest_str);
                            PushToast(state, "BACKGROUND ADDED");
                        } else {
                            PushToast(state, "COULDN'T COPY THE PICKED FILE INTO ASSETS/BACKGROUNDS");
                        }
                    }
                } else {
                    state.background_selected_index = i;
                    LoadBackgroundChoice(ui.Raw(), state, icons, state.background_choices[i]);
                    SaveSelectedBackgroundChoice(state.background_choices[i]);
                }
            }
        }
        int rows = (total_tiles + per_row - 1) / per_row;
        y += static_cast<float>(rows) * (kSquare + kGap) + 4.0f;
    }

    y += 8.0f;
    ui.DrawLabel(x, y, "SECURITY", palette::kTextMuted, 1.0f);
    y += 14.0f;

    ui.DrawLabel(x, y, state.lock.password.empty() ? "NO PASSWORD SET" : "PASSWORD SET",
                 palette::kTextMuted, 1.0f);
    y += 16.0f;

    // Button row (not a toggle) - opens DrawChangePasswordPanel.
    // Label reflects whether this sets a password for the first time
    // or replaces an existing one.
    {
        constexpr float kBtnH = 16.0f;
        float btn_w = kPanelW - 24.0f;
        bool hovered = HitTest(state.input, x, y, btn_w, kBtnH);
        ui.DrawRoundedRect(x, y, btn_w, kBtnH, 3.0f, hovered ? palette::kTabActive : palette::kTabInactive);
        const char* label = state.lock.password.empty() ? "SET PASSWORD" : "CHANGE PASSWORD";
        ui.DrawLabel(x + (btn_w - ui.MeasureText(label, 1.0f)) * 0.5f, y + (kBtnH - 7.0f) * 0.5f, label,
                     palette::kTextPrimary, 1.0f);
        if (hovered && state.input.clicked) {
            AppState::ChangePassword change;
            change.open = true;
            change.stage = state.lock.password.empty() ? 1 : 0;
            state.change_password = change;
        }
        y += kBtnH + 6.0f;
    }

    y += 6.0f;
    DrawToggleRow("THIRD-PARTY API", state.api_enabled);

    // Status line: only meaningful once the server has actually
    // finished binding a port (see UiAppBeginFrame's start/stop sync)
    // - api_enabled can be true for one frame before api_port is set.
    if (state.api_enabled && state.api_port != 0) {
        std::string status = "127.0.0.1:" + std::to_string(state.api_port);
        ui.DrawLabel(x, y, TruncateToWidth(ui, status, kPanelW - 24.0f), palette::kTextMuted, 1.0f);
        y += 12.0f;
        // The actual resolved path (matching ApiServer's own
        // DiscoveryFilePath exactly), not a hardcoded "~/.cryptvault/
        // api.json" guess - "~" isn't even a real Windows path
        // convention, and the real path is what's actually useful to
        // know here.
        std::string token_line = "TOKEN: " + (KnownVaultsPath().parent_path() / "api.json").string();
        ui.DrawLabel(x, y, TruncateToWidth(ui, token_line, kPanelW - 24.0f), palette::kTextMuted, 1.0f);
        y += 14.0f;
    } else if (state.api_enabled) {
        ui.DrawLabel(x, y, "STARTING...", palette::kTextMuted, 1.0f);
        y += 14.0f;
    }

    y += 8.0f;
    ui.DrawLabel(x, y, "VAULT LOCATIONS", palette::kTextMuted, 1.0f);
    y += 14.0f;
    // Real folder-on-disk paths for every known vault - so migrating
    // to a new machine (or just backing up) means knowing exactly
    // which real directories to copy, not guessing. Truncated per
    // line to fit the panel rather than wrapped, same as everything
    // else here; a long list just runs past the visible panel height
    // in the small window size - resize the window larger (see the
    // resize toggle) if that happens.
    if (state.vaults.empty()) {
        ui.DrawLabel(x, y, "NO VAULTS YET", palette::kTextMuted, 1.0f);
        y += 14.0f;
    } else {
        for (const Vault& vault : state.vaults) {
            std::string line = vault.name + ": " + vault.path;
            ui.DrawLabel(x, y, TruncateToWidth(ui, line, kPanelW - 24.0f), palette::kTextMuted, 1.0f);
            y += 12.0f;
        }
    }

    y += 10.0f;
    ui.DrawLabel(x, y, "DATA", palette::kTextMuted, 1.0f);
    y += 14.0f;
    {
        // Same "forget from the list, never touch the real files"
        // stance as deleting a single vault (see DrawDeleteModal's
        // comment) - a real confirmation modal (DrawClearAllDataModal)
        // spells this out explicitly rather than assuming it's
        // obvious for an action this broad-sounding.
        constexpr float kBtnH = 16.0f;
        float btn_w = kPanelW - 24.0f;
        bool hovered = HitTest(state.input, x, y, btn_w, kBtnH);
        ui.DrawRoundedRect(x, y, btn_w, kBtnH, 3.0f, hovered ? palette::kDangerHover : palette::kDanger);
        const char* label = "CLEAR ALL DATA";
        ui.DrawLabel(x + (btn_w - ui.MeasureText(label, 1.0f)) * 0.5f, y + (kBtnH - 7.0f) * 0.5f, label,
                     palette::kTextPrimary, 1.0f);
        if (hovered && state.input.clicked) {
            state.clear_all_data_pending = true;
            state.settings_panel_open = false;
        }
        y += kBtnH + 10.0f;
    }

    {
        constexpr float kBtnH = 18.0f;
        float btn_w = kPanelW - 24.0f;
        bool hovered = HitTest(state.input, x, y, btn_w, kBtnH);
        ui.DrawRoundedRect(x, y, btn_w, kBtnH, 3.0f, hovered ? palette::kTabActive : palette::kTabInactive);
        ui.DrawLabel(x + (btn_w - ui.MeasureText("QUIT", 1.0f)) * 0.5f, y + (kBtnH - 7.0f) * 0.5f, "QUIT",
                     palette::kTextPrimary, 1.0f);
        if (hovered && state.input.clicked) {
            state.quit_requested = true;
        }
        y += kBtnH + 6.0f;
    }

    // y has been accumulating from kContentTop + 4.0f - state.settings_scroll
    // the whole way through, so adding the scroll back gives the true,
    // unscrolled height this content would need if nothing were clipped.
    float natural_bottom = y + state.settings_scroll;
    ui.PopClipRect();

    float visible_h = std::max(0.0f, visible_bottom - kContentTop);
    float content_h = natural_bottom - kContentTop;
    float max_scroll = std::max(0.0f, content_h - visible_h);

    bool panel_hovered = HitTest(state.input, panel_x, 0.0f, kPanelW, state.content_height);
    if (panel_hovered && state.input.scroll_delta_y != 0.0f) {
        state.settings_scroll -= state.input.scroll_delta_y * 16.0f;
    }
    state.settings_scroll = std::clamp(state.settings_scroll, 0.0f, max_scroll);

    // A simple scrollbar thumb along the panel's right edge, only
    // drawn when there's actually more content than fits - same idea
    // as the tile grid's own scrollbar (see DrawTileGrid).
    if (max_scroll > 0.0f && content_h > 0.0f) {
        float thumb_h = std::max(16.0f, visible_h * (visible_h / content_h));
        float thumb_y = kContentTop + (visible_h - thumb_h) * (state.settings_scroll / max_scroll);
        ui.DrawRoundedRect(panel_x + kPanelW - 5.0f, thumb_y, 3.0f, thumb_h, 1.5f,
                            appshell::UiColor{1.0f, 1.0f, 1.0f, 0.25f});
    }
}

// One command palette entry: a label to match/display and an action
// to run when it's chosen. `enabled` lets a command exist in the list
// but be visibly (and functionally) unavailable given the current
// state - e.g. "Add Vault" with no dir tab to add it to - rather than
// silently doing nothing or being hidden entirely.
struct PaletteCommand {
    std::string label;
    bool enabled = true;
    std::function<void(AppState&)> run;
};

// Builds the command list fresh each frame from current state (which
// vault/dir tab is active, etc.) rather than a fixed static list, so
// e.g. "Add Folder" only appears while a vault is actually open.
std::vector<PaletteCommand> BuildPaletteCommands(AppState& state) {
    std::vector<PaletteCommand> commands;

    commands.push_back({"GO TO VAULT LIST", true, [](AppState& s) { s.screen = Screen::VaultList; }});

    commands.push_back({"ADD VAULT", true, [](AppState& s) { OpenCreateVaultWizard(s); }});

    vaultstore::Vault* open_vault = ActiveOpenVault(state);
    commands.push_back({"ADD FOLDER", open_vault != nullptr, [](AppState& s) { AddFolderToActiveVault(s); }});

    // Engages the real (UI-level, not cryptographic - see
    // AppState::Lock's comment) lock screen, same as clicking
    // DrawLockToggle directly.
    commands.push_back({"LOCK APPLICATION", true, [](AppState& s) {
        s.lock.locked = true;
        s.settings_panel_open = false;
        s.command_palette.open = false;
        s.context_menu.open = false;
        s.rename = AppState::RenameState{};
        s.pending_delete = AppState::PendingDelete{};
        s.unlock_prompt = AppState::UnlockPrompt{};
        s.create_vault_wizard = AppState::CreateVaultWizard{};
        s.lock.prompt_open = false;
        s.lock.buffer.clear();
        s.lock.error.clear();
    }});

    commands.push_back({"OPEN SETTINGS", true, [](AppState& s) {
        s.settings_panel_open = true;
        s.command_palette.open = false;
    }});

    commands.push_back({"REFRESH", true, [](AppState& s) {
        s.selected_vault = -1;
        s.selected_folder = -1;
    }});

    commands.push_back({state.big_mode ? "SWITCH TO SMALL WINDOW" : "SWITCH TO LARGE WINDOW", true,
                        [](AppState& s) {
                            s.big_mode = !s.big_mode;
                            s.resize_requested = true;
                        }});

    return commands;
}

// Case-insensitive substring match - the whole filter this palette
// needs, given the command list is a handful of short fixed labels
// rather than something warranting fuzzy/ranked matching.
bool MatchesQuery(const std::string& label, const std::string& query) {
    if (query.empty()) {
        return true;
    }
    std::string lower_label = label;
    std::string lower_query = query;
    std::transform(lower_label.begin(), lower_label.end(), lower_label.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower_label.find(lower_query) != std::string::npos;
}

// Command palette: Ctrl+K is the conventional trigger for this kind
// of UI, but wiring up modifier-key combos wasn't done in this pass
// (see the reply text) - opened via the toolbar's search icon
// instead (see DrawToolbar), which doubles as giving that
// previously-dead button a real purpose. Typed text filters the
// command list (see MatchesQuery); Up/Down move the selection; Enter
// or a click runs the selected/clicked command. Opens with the
// "opacity 0->1, scale 0.96->1" animation the original roadmap notes
// call out for this exact kind of panel.
void DrawCommandPalette(ContentRenderer& ui, AppState& state) {
    if (!state.command_palette.open) {
        return;
    }

    if (!state.command_palette.timeline.started()) {
        state.command_palette.timeline.duration_seconds =
            state.settings.animations_enabled ? anim::duration::kUi : 0.001f;
        state.command_palette.timeline.Start(state.now_seconds);
    }
    float eased = anim::Ease(anim::Easing::kEaseOutCubic,
                              state.command_palette.timeline.Progress(state.now_seconds));

    // Typing/backspace/navigation, applied before building the
    // (filtered) list below so this frame's keystroke is reflected
    // immediately rather than one frame late.
    state.command_palette.query += state.input.text_input;
    if (state.input.key_backspace && !state.command_palette.query.empty()) {
        state.command_palette.query.pop_back();
    }

    std::vector<PaletteCommand> all_commands = BuildPaletteCommands(state);
    std::vector<PaletteCommand*> visible;
    for (auto& cmd : all_commands) {
        if (MatchesQuery(cmd.label, state.command_palette.query)) {
            visible.push_back(&cmd);
        }
    }
    if (visible.empty()) {
        state.command_palette.selected = 0;
    } else {
        state.command_palette.selected =
            std::max(0, std::min(state.command_palette.selected, static_cast<int>(visible.size()) - 1));
    }
    if (state.input.key_down && !visible.empty()) {
        state.command_palette.selected = (state.command_palette.selected + 1) % static_cast<int>(visible.size());
    }
    if (state.input.key_up && !visible.empty()) {
        state.command_palette.selected =
            (state.command_palette.selected - 1 + static_cast<int>(visible.size())) % static_cast<int>(visible.size());
    }

    ui.DrawRect(0, 0, state.content_width, state.content_height,
                appshell::UiColor{0.0f, 0.0f, 0.0f, 0.45f * eased});

    constexpr float kPanelW = 200.0f;
    constexpr float kRowH = 16.0f;
    constexpr float kHeaderH = 26.0f;
    float panel_h_target = kHeaderH + std::min<size_t>(visible.size(), 6) * kRowH + 8.0f;
    float scale = anim::Lerp(0.96f, 1.0f, eased);
    float panel_w = kPanelW * scale;
    float panel_h = panel_h_target * scale;
    float panel_x = (state.content_width - panel_w) * 0.5f;
    float panel_y = std::max(4.0f, state.content_height * 0.22f);

    float panel_alpha = state.settings.glass_effect_enabled ? 0.94f : 1.0f;
    ui.DrawRoundedRect(panel_x, panel_y, panel_w, panel_h, 6.0f,
                        appshell::UiColor{0.12f, 0.14f, 0.13f, panel_alpha * eased});

    // Clicking the scrim outside the panel closes it - same pattern
    // as the settings panel.
    bool inside_panel = HitTest(state.input, panel_x, panel_y, panel_w, panel_h);
    if (state.input.clicked && !inside_panel) {
        state.command_palette.open = false;
        return;
    }

    if (eased < 0.3f) {
        return;  // avoid drawing text into a barely-open panel
    }

    std::string query_display = state.command_palette.query.empty()
        ? "SEARCH COMMANDS..."
        : state.command_palette.query;
    appshell::UiColor query_color =
        state.command_palette.query.empty() ? palette::kTextMuted : palette::kTextPrimary;
    ui.DrawLabel(panel_x + 10.0f, panel_y + 9.0f,
                 TruncateToWidth(ui, query_display, panel_w - 20.0f), query_color, 1.0f);
    ui.DrawRect(panel_x + 8.0f, panel_y + kHeaderH - 2.0f, panel_w - 16.0f, 1.0f,
                appshell::UiColor{1.0f, 1.0f, 1.0f, 0.08f});

    float row_y = panel_y + kHeaderH + 4.0f;
    for (size_t i = 0; i < visible.size() && i < 6; ++i) {
        PaletteCommand& cmd = *visible[i];
        bool is_selected = (static_cast<int>(i) == state.command_palette.selected);
        bool hovered = HitTest(state.input, panel_x + 4.0f, row_y, panel_w - 8.0f, kRowH);

        if (is_selected || hovered) {
            ui.DrawRect(panel_x + 4.0f, row_y, panel_w - 8.0f, kRowH,
                        appshell::UiColor{1.0f, 1.0f, 1.0f, is_selected ? 0.10f : 0.06f});
        }

        appshell::UiColor text_color = cmd.enabled ? palette::kTextPrimary : palette::kTextMuted;
        ui.DrawLabel(panel_x + 10.0f, row_y + (kRowH - 7.0f) * 0.5f,
                     TruncateToWidth(ui, cmd.label, panel_w - 20.0f), text_color, 1.0f);

        if (cmd.enabled && ((hovered && state.input.clicked)
                            || (is_selected && state.input.key_enter))) {
            cmd.run(state);
            state.command_palette.open = false;
            state.command_palette.timeline.Reset();
            return;  // state (and `all_commands`/`visible`) may now be stale
        }

        row_y += kRowH;
    }
}

// Right-click context menu (see the original roadmap notes' #9) for
// a single vault or folder tile - "OPEN" (vaults only; there's no
// nested-browsing model for folders, see the standing question about
// that), "RENAME" (opens DrawRenamePanel), "DELETE" (opens the same
// DrawDeleteModal confirmation the tile's hover "x" does). Positioned
// at the click location, clamped so it stays fully on-screen; closes
// on choosing an item or clicking anywhere outside it.
void DrawContextMenu(ContentRenderer& ui, AppState& state) {
    if (!state.context_menu.open) {
        return;
    }

    // Gone stale (its vault/folder was deleted, or its vault's tab
    // was closed) since the menu was opened - just drop it rather
    // than act on/display a menu for something that no longer exists.
    if (!ResolveItemName(state, state.context_menu.is_vault, state.context_menu.index)) {
        state.context_menu.open = false;
        return;
    }

    if (!state.context_menu.timeline.started()) {
        state.context_menu.timeline.duration_seconds =
            state.settings.animations_enabled ? anim::duration::kMicro : 0.001f;
        state.context_menu.timeline.Start(state.now_seconds);
    }
    float eased = anim::Ease(anim::Easing::kEaseOutCubic,
                              state.context_menu.timeline.Progress(state.now_seconds));

    std::vector<std::string> items;
    if (state.context_menu.is_vault) {
        items.push_back("OPEN");
    } else if (ActiveOpenVault(state)) {
        // "OPEN" here means "navigate in" for a folder, or "open with
        // the OS default app" for a file - same as double-clicking
        // it (see DrawVaultFolderScreen).
        items.push_back("OPEN");
    }
    items.push_back("RENAME");
    items.push_back("DELETE");

    constexpr float kItemH = 16.0f;
    constexpr float kMenuW = 90.0f;
    float menu_h = items.size() * kItemH + 6.0f;

    // Clamped to stay fully inside the content area regardless of
    // where the right-click landed (e.g. near the right/bottom edge).
    float menu_x = std::max(2.0f, std::min(state.context_menu.x, state.content_width - kMenuW - 2.0f));
    float menu_y = std::max(2.0f, std::min(state.context_menu.y, state.content_height - menu_h - 2.0f));

    float scale = anim::Lerp(0.95f, 1.0f, eased);
    float w = kMenuW * scale;
    float h = menu_h * scale;

    float bg_alpha = state.settings.glass_effect_enabled ? 0.95f : 1.0f;
    ui.DrawRoundedRect(menu_x, menu_y, w, h, 4.0f,
                        appshell::UiColor{0.15f, 0.17f, 0.16f, bg_alpha * eased});

    if (state.input.clicked && !HitTest(state.input, menu_x, menu_y, w, h)) {
        state.context_menu.open = false;
        return;
    }

    if (eased < 0.4f) {
        return;  // avoid drawing labels into a barely-open menu
    }

    // Copied out before any item's action runs, since RENAME/DELETE
    // both end by closing this menu (state.context_menu.open =
    // false), which would otherwise invalidate the reference this
    // loop is reading index from mid-iteration.
    AppState::ContextMenu target = state.context_menu;

    float row_y = menu_y + 3.0f;
    for (const std::string& label : items) {
        bool hovered = HitTest(state.input, menu_x + 2.0f, row_y, w - 4.0f, kItemH);
        if (hovered) {
            ui.DrawRect(menu_x + 2.0f, row_y, w - 4.0f, kItemH, appshell::UiColor{1.0f, 1.0f, 1.0f, 0.08f});
        }
        ui.DrawLabel(menu_x + 8.0f, row_y + (kItemH - 7.0f) * 0.5f, label, palette::kTextPrimary, 1.0f);

        if (hovered && state.input.clicked) {
            state.context_menu.open = false;

            if (label == "OPEN") {
                if (target.is_vault) {
                    RequestOpenVault(state, target.index);
                } else if (vaultstore::Vault* vault = ActiveOpenVault(state)) {
                    std::vector<std::string>* path = ActiveFolderPath(state);
                    std::vector<vaultstore::TreeNode>& children = CurrentFolder(*vault, path).children;
                    if (target.index >= 0 && target.index < static_cast<int>(children.size())) {
                        vaultstore::TreeNode& item = children[target.index];
                        if (item.is_folder) {
                            if (path) path->push_back(item.name);
                            state.selected_folder = -1;
                            state.folder_grid_scroll = 0.0f;
                        } else {
                            OpenFileFromVault(state, *vault, item);
                        }
                    }
                }
            } else if (label == "RENAME") {
                AppState::RenameState rename;
                rename.active = true;
                rename.is_vault = target.is_vault;
                rename.index = target.index;
                std::string* name = ResolveItemName(state, target.is_vault, target.index);
                rename.buffer = name ? *name : "";
                state.rename = rename;
            } else if (label == "DELETE") {
                AppState::PendingDelete pending;
                pending.active = true;
                pending.is_vault = target.is_vault;
                pending.index = target.index;
                std::string* name = ResolveItemName(state, target.is_vault, target.index);
                pending.item_name = name ? *name : "";
                state.pending_delete = pending;
            }
            return;
        }
        row_y += kItemH;
    }
}

// Rename panel: opened by the context menu's "RENAME" entry. A small
// floating panel with an editable text field (using the same
// text_input/key_backspace plumbing the command palette's search
// field uses) rather than true in-place inline editing on the tile
// itself - simpler to get right than embedding a text field inside a
// scrolling, clipped grid cell, at no real cost to how it reads to
// the user. Enter or the OK button commits; clicking outside or
// Cancel discards.
//
// Renamed items are allowed to collide with an existing name -
// unlike the auto-generated "New Vault (2)"-style names elsewhere,
// a name the user *chose* shouldn't get silently suffixed.
void DrawRenamePanel(ContentRenderer& ui, AppState& state) {
    if (!state.rename.active) {
        return;
    }

    state.rename.buffer += state.input.text_input;
    if (state.input.key_backspace && !state.rename.buffer.empty()) {
        state.rename.buffer.pop_back();
    }

    ui.DrawRect(0, 0, state.content_width, state.content_height,
                appshell::UiColor{0.0f, 0.0f, 0.0f, 0.5f});

    constexpr float kPanelW = 180.0f;
    constexpr float kPanelH = 70.0f;
    float panel_x = (state.content_width - kPanelW) * 0.5f;
    float panel_y = (state.content_height - kPanelH) * 0.5f;

    float bg_alpha = state.settings.glass_effect_enabled ? 0.95f : 1.0f;
    ui.DrawRoundedRect(panel_x, panel_y, kPanelW, kPanelH, 6.0f,
                        appshell::UiColor{0.15f, 0.17f, 0.16f, bg_alpha});

    const char* title = state.rename.is_vault ? "RENAME VAULT" : "RENAME FOLDER";
    ui.DrawLabel(panel_x + 12.0f, panel_y + 10.0f, title, palette::kTextPrimary, 1.0f);

    constexpr float kFieldH = 16.0f;
    float field_x = panel_x + 12.0f;
    float field_y = panel_y + 28.0f;
    float field_w = kPanelW - 24.0f;
    ui.DrawRoundedRect(field_x, field_y, field_w, kFieldH, 3.0f, palette::kAddressBar);

    // Blinking text cursor - a plain visibility toggle on a fixed
    // clock, not eased, since a cursor blink is conventionally a
    // sharp on/off rather than a fade.
    std::string display = TruncateToWidth(ui, state.rename.buffer, field_w - 10.0f);
    ui.DrawLabel(field_x + 5.0f, field_y + (kFieldH - 7.0f) * 0.5f, display, palette::kTextPrimary, 1.0f);
    if (std::fmod(state.now_seconds, 1.0) < 0.5) {
        float cursor_x = field_x + 5.0f + ui.MeasureText(display, 1.0f) + 1.0f;
        ui.DrawRect(cursor_x, field_y + 3.0f, 1.0f, kFieldH - 6.0f, palette::kTextPrimary);
    }

    constexpr float kBtnW = 60.0f;
    constexpr float kBtnH = 16.0f;
    float btn_y = panel_y + kPanelH - kBtnH - 8.0f;
    float ok_x = panel_x + kPanelW - kBtnW - 10.0f;
    float cancel_x = ok_x - kBtnW - 6.0f;

    bool cancel_hovered = HitTest(state.input, cancel_x, btn_y, kBtnW, kBtnH);
    bool ok_hovered = HitTest(state.input, ok_x, btn_y, kBtnW, kBtnH);

    ui.DrawRoundedRect(cancel_x, btn_y, kBtnW, kBtnH, 3.0f,
                        cancel_hovered ? palette::kTabActive : palette::kTabInactive);
    ui.DrawLabel(cancel_x + (kBtnW - ui.MeasureText("CANCEL", 1.0f)) * 0.5f, btn_y + (kBtnH - 7.0f) * 0.5f,
                 "CANCEL", palette::kTextPrimary, 1.0f);

    ui.DrawRoundedRect(ok_x, btn_y, kBtnW, kBtnH, 3.0f,
                        ok_hovered ? palette::kAccent : palette::kTabActive);
    ui.DrawLabel(ok_x + (kBtnW - ui.MeasureText("OK", 1.0f)) * 0.5f, btn_y + (kBtnH - 7.0f) * 0.5f,
                 "OK", palette::kTextPrimary, 1.0f);

    bool commit = (ok_hovered && state.input.clicked) || state.input.key_enter;
    bool cancel = (cancel_hovered && state.input.clicked)
                  || (state.input.clicked && !HitTest(state.input, panel_x, panel_y, kPanelW, kPanelH));

    if (commit) {
        if (!state.rename.buffer.empty()) {
            if (state.rename.is_vault) {
                // Vault names are pure UI-level bookkeeping (see struct
                // Vault's comment) - no vaultstore call needed, just
                // update state.vaults directly (and persist the known-
                // vaults list, so the new name survives a restart).
                std::string* name = ResolveItemName(state, true, state.rename.index);
                if (name) {
                    *name = state.rename.buffer;
                    SaveKnownVaults(state);
                }
            } else if (vaultstore::Vault* vault = ActiveOpenVault(state)) {
                std::vector<vaultstore::TreeNode>& folders =
                    CurrentFolder(*vault, ActiveFolderPath(state)).children;
                if (state.rename.index >= 0 && state.rename.index < static_cast<int>(folders.size())) {
                    std::string error;
                    if (!vault->Rename(folders[state.rename.index], state.rename.buffer, error)) {
                        PushToast(state, "COULDN'T RENAME: " + error);
                    }
                }
            }
        }
        state.rename = AppState::RenameState{};
    } else if (cancel) {
        state.rename = AppState::RenameState{};
    }
}

// Resize-toggle button: sits in the top-left corner of the outer
// border strip, drawn via negative content-space coordinates (see
// ContentRenderer - it just adds +kBorder before handing off to the
// real UiRenderer, so e.g. x=-(kBorder-2) lands 2px inside the
// window's left edge, i.e. inside the border, not the content band).
// Toggles the actual OS window between kSmallWindow*/kLargeWindow*;
// the real resize happens in main.cpp (only it owns the GLFW window)
// - this just flips state.big_mode and sets state.resize_requested so
// UiAppConsumeResizeRequest can hand main.cpp the target size next
// time it polls (once per frame - see main.cpp's DrawFrame).
void DrawResizeToggle(ContentRenderer& ui, AppState& state) {
    constexpr float kSize = 13.0f;
    constexpr float kMargin = 2.0f;
    float x = -(kBorder - kMargin);
    float y = -(kBorder - kMargin);

    bool hovered = HitTest(state.input, x, y, kSize, kSize);

    appshell::UiColor bg = hovered
        ? appshell::UiColor{0.32f, 0.35f, 0.40f, 0.55f}
        : appshell::UiColor{0.32f, 0.35f, 0.40f, 0.30f};
    ui.DrawRoundedRect(x, y, kSize, kSize, 3.0f, bg);

    // Shows the *current* size class (S = 400x300, B = 900x600);
    // clicking toggles it. (Could instead show what clicking will
    // switch *to* - went with "current state" since it's simpler to
    // reason about as a status indicator + toggle, but easy to flip
    // if the other reads better.)
    const char* label = state.big_mode ? "B" : "S";
    float label_w = ui.MeasureText(label, 1.0f);
    ui.DrawLabel(x + (kSize - label_w) * 0.5f, y + (kSize - 7.0f) * 0.5f,
                 label, palette::kTextPrimary, 1.0f);

    if (hovered && state.input.clicked) {
        state.big_mode = !state.big_mode;
        state.resize_requested = true;
    }
}

// Lock-toggle button: sits in the top-right corner of the outer
// border strip, mirroring DrawResizeToggle's top-left placement and
// the same negative-content-space-coordinate trick (see that
// function's comment). One click engages the lock - see
// AppState::Lock's comment for what "lock" does and doesn't mean
// here. Closes any other open overlay first (settings, command
// palette, a context menu, a rename, a pending delete) so locking
// mid-edit doesn't leave one of those stuck open underneath the lock
// screen for when you unlock again.
void DrawLockToggle(ContentRenderer& ui, AppState& state) {
    constexpr float kSize = 13.0f;
    constexpr float kMargin = 2.0f;
    float x = state.content_width + (kBorder - kMargin) - kSize;
    float y = -(kBorder - kMargin);

    bool hovered = HitTest(state.input, x, y, kSize, kSize);

    appshell::UiColor bg = hovered
        ? appshell::UiColor{0.32f, 0.35f, 0.40f, 0.55f}
        : appshell::UiColor{0.32f, 0.35f, 0.40f, 0.30f};
    ui.DrawRoundedRect(x, y, kSize, kSize, 3.0f, bg);

    // A tiny procedural padlock glyph rather than a label/icon asset -
    // consistent with how e.g. the settings toggles or tab close "x"
    // are drawn (plain primitives), and this app doesn't have a lock
    // icon PNG in assets/icons yet.
    float shackle_w = kSize * 0.44f;
    float shackle_h = kSize * 0.34f;
    float shackle_x = x + (kSize - shackle_w) * 0.5f;
    float shackle_y = y + kSize * 0.16f;
    ui.DrawRoundedRect(shackle_x, shackle_y, shackle_w, shackle_h, shackle_w * 0.5f, palette::kTextPrimary);
    // Hollow it out (matches the button's own background so it reads
    // as a ring rather than a solid blob).
    float inner_w = shackle_w * 0.5f;
    ui.DrawRoundedRect(shackle_x + (shackle_w - inner_w) * 0.5f, shackle_y, inner_w, shackle_h * 0.85f,
                        inner_w * 0.5f, bg);
    float body_w = kSize * 0.62f;
    float body_h = kSize * 0.4f;
    ui.DrawRoundedRect(x + (kSize - body_w) * 0.5f, y + kSize * 0.42f, body_w, body_h, 1.5f,
                        palette::kTextPrimary);

    if (hovered && state.input.clicked) {
        state.lock.locked = true;
        state.settings_panel_open = false;
        state.command_palette.open = false;
        state.context_menu.open = false;
        state.rename = AppState::RenameState{};
        state.pending_delete = AppState::PendingDelete{};
        state.unlock_prompt = AppState::UnlockPrompt{};
        state.create_vault_wizard = AppState::CreateVaultWizard{};
        state.lock.prompt_open = false;
        state.lock.buffer.clear();
        state.lock.error.clear();
    }
}

// Right-click-anywhere-on-the-window-border menu: CLOSE, and a
// checkbox-style ALWAYS ON TOP toggle. Drawn unconditionally (like
// DrawResizeToggle/DrawLockToggle above) rather than gated by screen,
// since the border itself is the same regardless of which screen is
// showing inside it. state.input.mouse_x/mouse_y are in the same
// window-relative coordinate space DrawResizeToggle/DrawLockToggle
// already use their own negative offsets within (content-space, which
// ContentRenderer's kBorder offset maps back to actual window
// coordinates) - so "in the border" is simply "outside the content
// rectangle [0,0]..[content_width,content_height]."
void DrawBorderRightClickMenu(ContentRenderer& ui, AppState& state) {
    bool in_border = state.input.mouse_x < 0.0f || state.input.mouse_x > state.content_width
                      || state.input.mouse_y < 0.0f || state.input.mouse_y > state.content_height;

    if (!state.border_menu.open && in_border && state.input.right_clicked) {
        state.border_menu.open = true;
        state.border_menu.x = state.input.mouse_x;
        state.border_menu.y = state.input.mouse_y;
        // Close whatever else might be open rather than stacking a
        // second floating panel on top of it.
        state.settings_panel_open = false;
        state.command_palette.open = false;
        state.context_menu.open = false;
        state.rename = AppState::RenameState{};
    }

    if (!state.border_menu.open) {
        return;
    }

    if (!state.border_menu.timeline.started()) {
        state.border_menu.timeline.duration_seconds =
            state.settings.animations_enabled ? anim::duration::kMicro : 0.001f;
        state.border_menu.timeline.Start(state.now_seconds);
    }
    float t = state.border_menu.timeline.Progress(state.now_seconds);
    float eased = anim::Ease(anim::Easing::kEaseOutCubic, t);

    constexpr float kMenuW = 130.0f;
    constexpr float kItemH = 20.0f;
    constexpr int kItemCount = 2;
    float menu_h = kItemH * kItemCount + 6.0f;

    // Clamped so a right-click near an edge/corner doesn't draw the
    // menu partly off-window - same idea as DrawContextMenu.
    float menu_x = std::min(state.border_menu.x, state.content_width - kMenuW);
    float menu_y = std::min(state.border_menu.y, state.content_height - menu_h);
    menu_x = std::max(0.0f, menu_x);
    menu_y = std::max(0.0f, menu_y);

    ui.DrawRoundedRect(menu_x, menu_y, kMenuW, menu_h, 5.0f,
                        appshell::UiColor{0.15f, 0.17f, 0.16f, 0.85f + 0.15f * eased});

    float row_y = menu_y + 3.0f;

    // ALWAYS ON TOP - drawn first, with a checkmark reflecting
    // state.always_on_top (main.cpp applies the actual OS-level
    // change - see UiAppGetAlwaysOnTop's comment).
    bool top_hovered = HitTest(state.input, menu_x + 2.0f, row_y, kMenuW - 4.0f, kItemH);
    if (top_hovered) {
        ui.DrawRect(menu_x + 2.0f, row_y, kMenuW - 4.0f, kItemH, appshell::UiColor{1.0f, 1.0f, 1.0f, 0.08f});
    }
    if (state.always_on_top) {
        ui.DrawLabel(menu_x + 8.0f, row_y + (kItemH - 7.0f) * 0.5f, "X", palette::kAccent, 1.0f);
    }
    ui.DrawLabel(menu_x + 22.0f, row_y + (kItemH - 7.0f) * 0.5f, "ALWAYS ON TOP", palette::kTextPrimary, 1.0f);
    if (top_hovered && state.input.clicked) {
        state.always_on_top = !state.always_on_top;
        state.border_menu = AppState::BorderMenu{};
        return;
    }
    row_y += kItemH;

    // CLOSE
    bool close_hovered = HitTest(state.input, menu_x + 2.0f, row_y, kMenuW - 4.0f, kItemH);
    if (close_hovered) {
        ui.DrawRect(menu_x + 2.0f, row_y, kMenuW - 4.0f, kItemH, appshell::UiColor{1.0f, 1.0f, 1.0f, 0.08f});
    }
    ui.DrawLabel(menu_x + 8.0f, row_y + (kItemH - 7.0f) * 0.5f, "CLOSE", palette::kTextPrimary, 1.0f);
    if (close_hovered && state.input.clicked) {
        state.quit_requested = true;
        state.border_menu = AppState::BorderMenu{};
        return;
    }

    // Click anywhere else closes without acting - same "click outside
    // dismisses" idea as every other floating panel. Opened by a
    // RIGHT click (state.input.right_clicked), a different flag from
    // the left click (state.input.clicked) this dismissal checks, so
    // there's no self-cancelling race between the two the way
    // Settings/Search had (see DrawToolbar's comment on that fix).
    bool inside_menu = HitTest(state.input, menu_x, menu_y, kMenuW, menu_h);
    if (state.input.clicked && !inside_menu) {
        state.border_menu = AppState::BorderMenu{};
    }
}

// Masks a password buffer for display: bullet dots, same length as
// the real text - length is still visible (helps catch a fat-
// fingered attempt before submitting) but the characters themselves
// never are.
std::string MaskPassword(const std::string& text) {
    std::string masked;
    masked.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        masked += "*";
    }
    return masked;
}

// Full-screen lock overlay - drawn *instead of* the normal toolbar/
// tabs/grid (see UiAppBeginFrame) while state.lock.locked is set, not
// layered on top of them, so there's nothing real underneath a casual
// glance could catch. Just a centered padlock; clicking it opens the
// password prompt (skipped, unlocking immediately, if no password has
// ever been set - see AppState::Lock's comment).
void DrawLockScreen(ContentRenderer& ui, AppState& state) {
    ui.DrawRect(0, 0, state.content_width, state.content_height, palette::kToolbar);

    constexpr float kGlyphSize = 64.0f;
    float cx = state.content_width * 0.5f;
    float cy = state.content_height * 0.42f;

    bool hovered = HitTest(state.input, cx - kGlyphSize * 0.5f, cy - kGlyphSize * 0.5f, kGlyphSize, kGlyphSize);
    appshell::UiColor glyph_color = hovered ? palette::kAccent : palette::kTextPrimary;

    float shackle_w = kGlyphSize * 0.46f;
    float shackle_h = kGlyphSize * 0.36f;
    float shackle_x = cx - shackle_w * 0.5f;
    float shackle_y = cy - kGlyphSize * 0.42f;
    ui.DrawRoundedRect(shackle_x, shackle_y, shackle_w, shackle_h, shackle_w * 0.5f, glyph_color);
    float inner_w = shackle_w * 0.5f;
    ui.DrawRoundedRect(shackle_x + (shackle_w - inner_w) * 0.5f, shackle_y, inner_w, shackle_h * 0.85f,
                        inner_w * 0.5f, palette::kToolbar);

    float body_w = kGlyphSize * 0.66f;
    float body_h = kGlyphSize * 0.46f;
    float body_x = cx - body_w * 0.5f;
    float body_y = cy - kGlyphSize * 0.08f;
    ui.DrawRoundedRect(body_x, body_y, body_w, body_h, 6.0f, glyph_color);

    // Keyhole: a small round dot plus a short slot beneath it.
    constexpr float kHoleSize = 7.0f;
    ui.DrawRoundedRect(cx - kHoleSize * 0.5f, body_y + body_h * 0.28f, kHoleSize, kHoleSize, kHoleSize * 0.5f,
                        palette::kToolbar);
    ui.DrawRoundedRect(cx - 1.5f, body_y + body_h * 0.28f + kHoleSize * 0.6f, 3.0f, body_h * 0.28f, 1.0f,
                        palette::kToolbar);

    std::string label = state.lock.locked ? "LOCKED" : "";
    float label_w = ui.MeasureText(label, 1.0f);
    ui.DrawLabel(cx - label_w * 0.5f, cy + kGlyphSize * 0.42f, label, palette::kTextMuted, 1.0f);

    if (hovered && state.input.clicked && !state.lock.prompt_open) {
        if (state.lock.password.empty()) {
            state.lock.locked = false;
            PushToast(state, "UNLOCKED (NO PASSWORD SET)");
            return;
        }
        state.lock.prompt_open = true;
        state.lock.buffer.clear();
        state.lock.error.clear();
    }

    if (!state.lock.prompt_open) {
        return;
    }

    // --- Password prompt panel -----------------------------------------
    state.lock.buffer += state.input.text_input;
    if (state.input.key_backspace && !state.lock.buffer.empty()) {
        state.lock.buffer.pop_back();
    }

    ui.DrawRect(0, 0, state.content_width, state.content_height,
                appshell::UiColor{0.0f, 0.0f, 0.0f, 0.55f});

    constexpr float kPanelW = 180.0f;
    constexpr float kPanelH = 82.0f;
    float panel_x = (state.content_width - kPanelW) * 0.5f;
    float panel_y = (state.content_height - kPanelH) * 0.5f;

    ui.DrawRoundedRect(panel_x, panel_y, kPanelW, kPanelH, 6.0f,
                        appshell::UiColor{0.15f, 0.17f, 0.16f, 1.0f});
    ui.DrawLabel(panel_x + 12.0f, panel_y + 10.0f, "ENTER PASSWORD", palette::kTextPrimary, 1.0f);

    constexpr float kFieldH = 16.0f;
    float field_x = panel_x + 12.0f;
    float field_y = panel_y + 26.0f;
    float field_w = kPanelW - 24.0f;
    ui.DrawRoundedRect(field_x, field_y, field_w, kFieldH, 3.0f, palette::kAddressBar);

    std::string masked = MaskPassword(state.lock.buffer);
    std::string display = TruncateToWidth(ui, masked, field_w - 10.0f);
    ui.DrawLabel(field_x + 5.0f, field_y + (kFieldH - 7.0f) * 0.5f, display, palette::kTextPrimary, 1.0f);
    if (std::fmod(state.now_seconds, 1.0) < 0.5) {
        float cursor_x = field_x + 5.0f + ui.MeasureText(display, 1.0f) + 1.0f;
        ui.DrawRect(cursor_x, field_y + 3.0f, 1.0f, kFieldH - 6.0f, palette::kTextPrimary);
    }

    if (!state.lock.error.empty()) {
        ui.DrawLabel(field_x, field_y + kFieldH + 4.0f, state.lock.error, palette::kDanger, 1.0f);
    }

    constexpr float kBtnW = 60.0f;
    constexpr float kBtnH = 16.0f;
    float btn_y = panel_y + kPanelH - kBtnH - 8.0f;
    float ok_x = panel_x + kPanelW - kBtnW - 10.0f;
    float cancel_x = ok_x - kBtnW - 6.0f;

    bool cancel_hovered = HitTest(state.input, cancel_x, btn_y, kBtnW, kBtnH);
    bool ok_hovered = HitTest(state.input, ok_x, btn_y, kBtnW, kBtnH);

    ui.DrawRoundedRect(cancel_x, btn_y, kBtnW, kBtnH, 3.0f,
                        cancel_hovered ? palette::kTabActive : palette::kTabInactive);
    ui.DrawLabel(cancel_x + (kBtnW - ui.MeasureText("CANCEL", 1.0f)) * 0.5f, btn_y + (kBtnH - 7.0f) * 0.5f,
                 "CANCEL", palette::kTextPrimary, 1.0f);

    ui.DrawRoundedRect(ok_x, btn_y, kBtnW, kBtnH, 3.0f, ok_hovered ? palette::kAccent : palette::kTabActive);
    ui.DrawLabel(ok_x + (kBtnW - ui.MeasureText("UNLOCK", 1.0f)) * 0.5f, btn_y + (kBtnH - 7.0f) * 0.5f,
                 "UNLOCK", palette::kTextPrimary, 1.0f);

    bool submit = (ok_hovered && state.input.clicked) || state.input.key_enter;
    bool cancel = cancel_hovered && state.input.clicked;

    if (submit) {
        if (state.lock.buffer == state.lock.password) {
            state.lock.locked = false;
            state.lock.prompt_open = false;
            state.lock.buffer.clear();
            state.lock.error.clear();
        } else {
            state.lock.error = "WRONG PASSWORD";
            state.lock.buffer.clear();
        }
    } else if (cancel) {
        state.lock.prompt_open = false;
        state.lock.buffer.clear();
        state.lock.error.clear();
    }
}

// Change-password wizard: opened by Settings' "CHANGE PASSWORD" row
// (see DrawSettingsPanel). Steps through verify-current (skipped if
// no password is set yet) -> enter-new -> confirm-new, one field at a
// time - see AppState::ChangePassword's comment for why it's a
// sequence of single-field steps rather than one three-field form.
void DrawChangePasswordPanel(ContentRenderer& ui, AppState& state) {
    if (!state.change_password.open) {
        return;
    }

    state.change_password.buffer += state.input.text_input;
    if (state.input.key_backspace && !state.change_password.buffer.empty()) {
        state.change_password.buffer.pop_back();
    }

    ui.DrawRect(0, 0, state.content_width, state.content_height,
                appshell::UiColor{0.0f, 0.0f, 0.0f, 0.5f});

    constexpr float kPanelW = 190.0f;
    constexpr float kPanelH = 84.0f;
    float panel_x = (state.content_width - kPanelW) * 0.5f;
    float panel_y = (state.content_height - kPanelH) * 0.5f;

    ui.DrawRoundedRect(panel_x, panel_y, kPanelW, kPanelH, 6.0f,
                        appshell::UiColor{0.15f, 0.17f, 0.16f, 1.0f});

    const char* title = "CHANGE PASSWORD";
    if (state.change_password.stage == 0) title = "CURRENT PASSWORD";
    else if (state.change_password.stage == 1) title = "NEW PASSWORD";
    else if (state.change_password.stage == 2) title = "CONFIRM NEW PASSWORD";
    ui.DrawLabel(panel_x + 12.0f, panel_y + 10.0f, title, palette::kTextPrimary, 1.0f);

    constexpr float kFieldH = 16.0f;
    float field_x = panel_x + 12.0f;
    float field_y = panel_y + 28.0f;
    float field_w = kPanelW - 24.0f;
    ui.DrawRoundedRect(field_x, field_y, field_w, kFieldH, 3.0f, palette::kAddressBar);

    std::string display = TruncateToWidth(ui, MaskPassword(state.change_password.buffer), field_w - 10.0f);
    ui.DrawLabel(field_x + 5.0f, field_y + (kFieldH - 7.0f) * 0.5f, display, palette::kTextPrimary, 1.0f);
    if (std::fmod(state.now_seconds, 1.0) < 0.5) {
        float cursor_x = field_x + 5.0f + ui.MeasureText(display, 1.0f) + 1.0f;
        ui.DrawRect(cursor_x, field_y + 3.0f, 1.0f, kFieldH - 6.0f, palette::kTextPrimary);
    }

    if (!state.change_password.error.empty()) {
        ui.DrawLabel(field_x, field_y + kFieldH + 4.0f, state.change_password.error, palette::kDanger, 1.0f);
    }

    constexpr float kBtnW = 62.0f;
    constexpr float kBtnH = 16.0f;
    float btn_y = panel_y + kPanelH - kBtnH - 8.0f;
    float ok_x = panel_x + kPanelW - kBtnW - 10.0f;
    float cancel_x = ok_x - kBtnW - 6.0f;

    bool cancel_hovered = HitTest(state.input, cancel_x, btn_y, kBtnW, kBtnH);
    bool ok_hovered = HitTest(state.input, ok_x, btn_y, kBtnW, kBtnH);

    ui.DrawRoundedRect(cancel_x, btn_y, kBtnW, kBtnH, 3.0f,
                        cancel_hovered ? palette::kTabActive : palette::kTabInactive);
    ui.DrawLabel(cancel_x + (kBtnW - ui.MeasureText("CANCEL", 1.0f)) * 0.5f, btn_y + (kBtnH - 7.0f) * 0.5f,
                 "CANCEL", palette::kTextPrimary, 1.0f);

    const char* ok_label = (state.change_password.stage == 2) ? "SAVE" : "NEXT";
    ui.DrawRoundedRect(ok_x, btn_y, kBtnW, kBtnH, 3.0f, ok_hovered ? palette::kAccent : palette::kTabActive);
    ui.DrawLabel(ok_x + (kBtnW - ui.MeasureText(ok_label, 1.0f)) * 0.5f, btn_y + (kBtnH - 7.0f) * 0.5f,
                 ok_label, palette::kTextPrimary, 1.0f);

    bool submit = (ok_hovered && state.input.clicked) || state.input.key_enter;
    bool cancel = cancel_hovered && state.input.clicked;

    if (submit) {
        if (state.change_password.stage == 0) {
            if (state.change_password.buffer == state.lock.password) {
                state.change_password.stage = 1;
                state.change_password.buffer.clear();
                state.change_password.error.clear();
            } else {
                state.change_password.error = "WRONG PASSWORD";
                state.change_password.buffer.clear();
            }
        } else if (state.change_password.stage == 1) {
            if (state.change_password.buffer.empty()) {
                state.change_password.error = "CAN'T BE EMPTY";
            } else {
                state.change_password.new_password = state.change_password.buffer;
                state.change_password.buffer.clear();
                state.change_password.error.clear();
                state.change_password.stage = 2;
            }
        } else {
            if (state.change_password.buffer == state.change_password.new_password) {
                state.lock.password = state.change_password.new_password;
                state.change_password = AppState::ChangePassword{};
                PushToast(state, "PASSWORD CHANGED");
            } else {
                state.change_password.error = "DOESN'T MATCH";
                state.change_password.buffer.clear();
            }
        }
    } else if (cancel) {
        state.change_password = AppState::ChangePassword{};
    }
}

// The per-vault unlock prompt (see RequestOpenVault/AppState::UnlockPrompt) -
// visually similar to DrawLockScreen's own password panel, but this
// one is a real vaultstore::OpenVault() call against a specific
// vault's real password, not a comparison against a stored string.
// On success, holds the resulting session open (see
// AppState::OpenSession) and immediately opens a tab for it - no
// reason to make the user watch a second screen after typing the
// password correctly.
void DrawUnlockPrompt(ContentRenderer& ui, AppState& state) {
    if (!state.unlock_prompt.open) {
        return;
    }
    if (state.unlock_prompt.vault_index < 0
        || state.unlock_prompt.vault_index >= static_cast<int>(state.vaults.size())) {
        state.unlock_prompt = AppState::UnlockPrompt{};  // stale - the vault was removed while this was open
        return;
    }

    state.unlock_prompt.buffer += state.input.text_input;
    if (state.input.key_backspace && !state.unlock_prompt.buffer.empty()) {
        state.unlock_prompt.buffer.pop_back();
    }

    ui.DrawRect(0, 0, state.content_width, state.content_height,
                appshell::UiColor{0.0f, 0.0f, 0.0f, 0.55f});

    constexpr float kPanelW = 190.0f;
    constexpr float kPanelH = 82.0f;
    float panel_x = (state.content_width - kPanelW) * 0.5f;
    float panel_y = (state.content_height - kPanelH) * 0.5f;

    ui.DrawRoundedRect(panel_x, panel_y, kPanelW, kPanelH, 6.0f,
                        appshell::UiColor{0.15f, 0.17f, 0.16f, 1.0f});

    std::string title = "UNLOCK \"" + state.vaults[state.unlock_prompt.vault_index].name + "\"";
    ui.DrawLabel(panel_x + 12.0f, panel_y + 10.0f,
                 TruncateToWidth(ui, title, kPanelW - 24.0f), palette::kTextPrimary, 1.0f);

    constexpr float kFieldH = 16.0f;
    float field_x = panel_x + 12.0f;
    float field_y = panel_y + 26.0f;
    float field_w = kPanelW - 24.0f;
    ui.DrawRoundedRect(field_x, field_y, field_w, kFieldH, 3.0f, palette::kAddressBar);

    std::string display = TruncateToWidth(ui, MaskPassword(state.unlock_prompt.buffer), field_w - 10.0f);
    ui.DrawLabel(field_x + 5.0f, field_y + (kFieldH - 7.0f) * 0.5f, display, palette::kTextPrimary, 1.0f);
    if (std::fmod(state.now_seconds, 1.0) < 0.5) {
        float cursor_x = field_x + 5.0f + ui.MeasureText(display, 1.0f) + 1.0f;
        ui.DrawRect(cursor_x, field_y + 3.0f, 1.0f, kFieldH - 6.0f, palette::kTextPrimary);
    }

    if (!state.unlock_prompt.error.empty()) {
        ui.DrawLabel(field_x, field_y + kFieldH + 4.0f, state.unlock_prompt.error, palette::kDanger, 1.0f);
    }

    constexpr float kBtnW = 60.0f;
    constexpr float kBtnH = 16.0f;
    float btn_y = panel_y + kPanelH - kBtnH - 8.0f;
    float ok_x = panel_x + kPanelW - kBtnW - 10.0f;
    float cancel_x = ok_x - kBtnW - 6.0f;

    bool cancel_hovered = HitTest(state.input, cancel_x, btn_y, kBtnW, kBtnH);
    bool ok_hovered = HitTest(state.input, ok_x, btn_y, kBtnW, kBtnH);

    ui.DrawRoundedRect(cancel_x, btn_y, kBtnW, kBtnH, 3.0f,
                        cancel_hovered ? palette::kTabActive : palette::kTabInactive);
    ui.DrawLabel(cancel_x + (kBtnW - ui.MeasureText("CANCEL", 1.0f)) * 0.5f, btn_y + (kBtnH - 7.0f) * 0.5f,
                 "CANCEL", palette::kTextPrimary, 1.0f);

    ui.DrawRoundedRect(ok_x, btn_y, kBtnW, kBtnH, 3.0f, ok_hovered ? palette::kAccent : palette::kTabActive);
    ui.DrawLabel(ok_x + (kBtnW - ui.MeasureText("UNLOCK", 1.0f)) * 0.5f, btn_y + (kBtnH - 7.0f) * 0.5f,
                 "UNLOCK", palette::kTextPrimary, 1.0f);

    bool submit = (ok_hovered && state.input.clicked) || state.input.key_enter;
    bool cancel = cancel_hovered && state.input.clicked;

    if (submit) {
        int vault_index = state.unlock_prompt.vault_index;
        std::string error;
        std::unique_ptr<vaultstore::Vault> opened =
            vaultstore::OpenVault(state.vaults[vault_index].path, state.unlock_prompt.buffer, error);
        if (opened) {
            AppState::OpenSession session;
            session.vault_index = vault_index;
            session.vault = std::move(opened);
            state.open_sessions.push_back(std::move(session));
            state.unlock_prompt = AppState::UnlockPrompt{};
            SwitchOrOpenVaultTab(state, vault_index);
        } else {
            state.unlock_prompt.error = error;  // vault_store.h returns "WRONG PASSWORD" for the common case
            state.unlock_prompt.buffer.clear();
        }
    } else if (cancel) {
        state.unlock_prompt = AppState::UnlockPrompt{};
    }
}

// The "create a new vault" wizard (see
// AppState::CreateVaultWizard/OpenCreateVaultWizard) - name, then a
// real directory on disk to hold it (there's no native OS folder-
// picker dialog wired up yet, hence a plain text field pre-filled
// with a reasonable default rather than a system "Choose Folder..."
// dialog - that's real, platform-specific work: IFileDialog on
// Windows, a portal/GTK dialog on Linux, each entirely different code
// - flagging it as a known gap rather than quietly skipping it), then
// a password, then confirming it. The actual vaultstore::CreateVault
// call only happens once all four are in hand.
void DrawCreateVaultWizard(ContentRenderer& ui, AppState& state) {
    if (!state.create_vault_wizard.open) {
        return;
    }

    state.create_vault_wizard.buffer += state.input.text_input;
    if (state.input.key_backspace && !state.create_vault_wizard.buffer.empty()) {
        state.create_vault_wizard.buffer.pop_back();
    }

    ui.DrawRect(0, 0, state.content_width, state.content_height,
                appshell::UiColor{0.0f, 0.0f, 0.0f, 0.55f});

    constexpr float kPanelW = 230.0f;
    constexpr float kPanelH = 86.0f;
    float panel_x = (state.content_width - kPanelW) * 0.5f;
    float panel_y = (state.content_height - kPanelH) * 0.5f;

    ui.DrawRoundedRect(panel_x, panel_y, kPanelW, kPanelH, 6.0f,
                        appshell::UiColor{0.15f, 0.17f, 0.16f, 1.0f});

    int stage = state.create_vault_wizard.stage;
    const char* title = "NAME THIS VAULT";
    if (stage == 1) title = "LOCATION ON DISK";
    else if (stage == 2) title = "SET A PASSWORD";
    else if (stage == 3) title = "CONFIRM PASSWORD";
    ui.DrawLabel(panel_x + 12.0f, panel_y + 10.0f, title, palette::kTextPrimary, 1.0f);

    constexpr float kFieldH = 16.0f;
    float field_x = panel_x + 12.0f;
    float field_y = panel_y + 28.0f;
    // Stage 1 (location) reserves a bit of width on the right for a
    // "..." button that opens a real native folder picker (see
    // src/platform/folder_picker.h) - every other stage's field uses
    // the full width.
    bool show_browse = (stage == 1);
    constexpr float kBrowseW = 20.0f;
    float field_w = kPanelW - 24.0f - (show_browse ? (kBrowseW + 4.0f) : 0.0f);
    ui.DrawRoundedRect(field_x, field_y, field_w, kFieldH, 3.0f, palette::kAddressBar);

    if (show_browse) {
        float browse_x = field_x + field_w + 4.0f;
        bool browse_hovered = HitTest(state.input, browse_x, field_y, kBrowseW, kFieldH);
        ui.DrawRoundedRect(browse_x, field_y, kBrowseW, kFieldH, 3.0f,
                            browse_hovered ? palette::kTabActive : palette::kTabInactive);
        ui.DrawLabel(browse_x + (kBrowseW - ui.MeasureText("...", 1.0f)) * 0.5f,
                     field_y + (kFieldH - 7.0f) * 0.5f, "...", palette::kTextPrimary, 1.0f);
        if (browse_hovered && state.input.clicked) {
            // Blocks this thread (and therefore the render loop) until
            // the native dialog closes - expected and fine, since a
            // folder picker is inherently modal; nothing else in the
            // app should be interactive while it's open anyway.
            std::string picked;
            if (platform::PickFolder("Choose where to create the vault", state.create_vault_wizard.buffer,
                                      picked)) {
                state.create_vault_wizard.buffer = picked;
            }
            // No dialog available (see folder_picker.h's comment on
            // when that happens) or the user cancelled - either way,
            // the existing typed/default path in buffer is left
            // exactly as it was, so this never destructively clears
            // what the user already had.
        }
    }

    bool masked = (stage == 2 || stage == 3);
    std::string raw_display = masked ? MaskPassword(state.create_vault_wizard.buffer)
                                      : state.create_vault_wizard.buffer;
    std::string display = TruncateToWidth(ui, raw_display, field_w - 10.0f);
    ui.DrawLabel(field_x + 5.0f, field_y + (kFieldH - 7.0f) * 0.5f, display, palette::kTextPrimary, 1.0f);
    if (std::fmod(state.now_seconds, 1.0) < 0.5) {
        float cursor_x = field_x + 5.0f + ui.MeasureText(display, 1.0f) + 1.0f;
        ui.DrawRect(cursor_x, field_y + 3.0f, 1.0f, kFieldH - 6.0f, palette::kTextPrimary);
    }

    if (!state.create_vault_wizard.error.empty()) {
        ui.DrawLabel(field_x, field_y + kFieldH + 4.0f,
                     TruncateToWidth(ui, state.create_vault_wizard.error, field_w), palette::kDanger, 1.0f);
    }

    constexpr float kBtnW = 62.0f;
    constexpr float kBtnH = 16.0f;
    float btn_y = panel_y + kPanelH - kBtnH - 8.0f;
    float ok_x = panel_x + kPanelW - kBtnW - 10.0f;
    float cancel_x = ok_x - kBtnW - 6.0f;

    bool cancel_hovered = HitTest(state.input, cancel_x, btn_y, kBtnW, kBtnH);
    bool ok_hovered = HitTest(state.input, ok_x, btn_y, kBtnW, kBtnH);

    ui.DrawRoundedRect(cancel_x, btn_y, kBtnW, kBtnH, 3.0f,
                        cancel_hovered ? palette::kTabActive : palette::kTabInactive);
    ui.DrawLabel(cancel_x + (kBtnW - ui.MeasureText("CANCEL", 1.0f)) * 0.5f, btn_y + (kBtnH - 7.0f) * 0.5f,
                 "CANCEL", palette::kTextPrimary, 1.0f);

    const char* ok_label = (stage == 3) ? "CREATE" : "NEXT";
    ui.DrawRoundedRect(ok_x, btn_y, kBtnW, kBtnH, 3.0f, ok_hovered ? palette::kAccent : palette::kTabActive);
    ui.DrawLabel(ok_x + (kBtnW - ui.MeasureText(ok_label, 1.0f)) * 0.5f, btn_y + (kBtnH - 7.0f) * 0.5f,
                 ok_label, palette::kTextPrimary, 1.0f);

    bool submit = (ok_hovered && state.input.clicked) || state.input.key_enter;
    bool cancel = cancel_hovered && state.input.clicked;

    if (submit) {
        AppState::CreateVaultWizard& wizard = state.create_vault_wizard;
        if (stage == 0) {
            if (wizard.buffer.empty()) {
                wizard.error = "CAN'T BE EMPTY";
            } else {
                wizard.name = wizard.buffer;
                wizard.location = DefaultVaultsRoot() + "/" + wizard.buffer;
                wizard.buffer = wizard.location;
                wizard.error.clear();
                wizard.stage = 1;
            }
        } else if (stage == 1) {
            if (wizard.buffer.empty()) {
                wizard.error = "CAN'T BE EMPTY";
            } else {
                wizard.location = wizard.buffer;
                wizard.buffer.clear();
                wizard.error.clear();
                wizard.stage = 2;
            }
        } else if (stage == 2) {
            if (wizard.buffer.empty()) {
                wizard.error = "CAN'T BE EMPTY";
            } else {
                wizard.password = wizard.buffer;
                wizard.buffer.clear();
                wizard.error.clear();
                wizard.stage = 3;
            }
        } else {
            if (wizard.buffer != wizard.password) {
                wizard.error = "DOESN'T MATCH";
                wizard.buffer.clear();
            } else {
                std::string error;
                std::unique_ptr<vaultstore::Vault> created =
                    vaultstore::CreateVault(wizard.location, wizard.password, error);
                if (created) {
                    Vault display_vault;
                    display_vault.name = wizard.name;
                    display_vault.path = wizard.location;
                    state.vaults.push_back(display_vault);
                    int vault_index = static_cast<int>(state.vaults.size()) - 1;

                    AppState::OpenSession session;
                    session.vault_index = vault_index;
                    session.vault = std::move(created);
                    state.open_sessions.push_back(std::move(session));

                    SaveKnownVaults(state);
                    state.create_vault_wizard = AppState::CreateVaultWizard{};
                    PushToast(state, "VAULT CREATED");
                } else {
                    // Most likely cause is the location (already
                    // exists and isn't empty, or isn't writable) -
                    // send the user back there rather than leaving
                    // them stuck re-typing a password that was never
                    // the problem.
                    wizard.stage = 1;
                    wizard.buffer = wizard.location;
                    wizard.error = error;
                }
            }
        }
    } else if (cancel) {
        state.create_vault_wizard = AppState::CreateVaultWizard{};
    }
}

// --- Third-party API ---------------------------------------------------
//
// The design agreed on before any of this was written: a local-only
// HTTP server (127.0.0.1, OS-assigned port - never a fixed port, and
// never any other interface), authenticated with a random bearer
// token regenerated every time the server (re)starts, exposing only
// vaults that are ALREADY unlocked in the app right now - the API has
// no password-checking path of its own and no way to unlock a vault
// itself, so it can never expose more than what's already open in the
// UI. Off by default (see AppState::api_enabled's comment).
//
// A third-party app discovers the port/token via a small local file
// (~/.cryptvault/api.json, owner-read/write only - see
// WriteDiscoveryFile) written while the server is running and removed
// when it stops, the same idea as how e.g. Jupyter publishes its own
// connection info for other tools to pick up.
//
// Endpoints (all but /v1/ping require `Authorization: Bearer <token>`):
//   GET  /v1/ping                        - no auth; liveness check
//   GET  /v1/vaults                      - unlocked vaults: [{index,name}]
//   GET  /v1/vaults/{index}/tree         - that vault's decrypted folder tree
//   GET  /v1/vaults/{index}/file?path=.. - a file's decrypted bytes
//   POST /v1/vaults/{index}/file?path=.. - encrypts the request body as
//                                          that file (overwriting if it
//                                          already exists; the parent
//                                          folder must already exist -
//                                          this never auto-creates
//                                          folders, that stays an
//                                          app-only action)
//
// Threading: httplib's Server::listen() blocks, so it runs on its own
// std::thread (see the `thread_` member) separate from the render
// thread that calls UiAppBeginFrame every frame. Both threads touch
// the same AppState (vaults, open_sessions, and the vaultstore::Vault
// objects those sessions hold), so every request handler AND the
// entirety of UiAppBeginFrame both take UiApp::frame_mutex for their
// whole duration - coarse-grained (the whole frame vs. the whole
// request) rather than locking each container individually. That's a
// deliberate simplification: this API is used occasionally by another
// process, not every frame, so a brief stall on either side while the
// other holds the lock is an acceptable trade for not having to audit
// every AppState-mutating call site in this file for thread-safety
// individually.
//
// Lifetime: owned by UiApp (see the struct below), started only when
// AppState::api_enabled is toggled on and stopped+joined in
// UiAppDestroy, so process shutdown never leaves this background
// thread holding a reference into freed memory.
class ApiServer {
public:
    ApiServer(AppState& state, std::mutex& frame_mutex) : state_(state), frame_mutex_(frame_mutex) {
        RegisterRoutes();
    }
    ~ApiServer() { Stop(); }

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    bool running() const { return thread_.joinable(); }

    // Returns the bound port (0 on failure - e.g. no loopback
    // interface available at all) and fills out_token with the fresh
    // token generated for this run. Deliberately does NOT lock
    // frame_mutex_ itself - unlike every request handler below, this
    // is called from inside UiAppBeginFrame while it already holds
    // that same (non-recursive) lock; re-locking it here would
    // deadlock rather than no-op. The caller is responsible for
    // writing the returned port/token into AppState.
    uint16_t Start(std::string& out_token) {
        if (running()) {
            out_token = token_;
            return port_;
        }

        if (sodium_init() < 0) {
            return 0;
        }
        unsigned char token_bytes[16];
        randombytes_buf(token_bytes, sizeof(token_bytes));
        token_ = ToHex(token_bytes, sizeof(token_bytes));

        int bound = server_.bind_to_any_port("127.0.0.1");
        if (bound <= 0) {
            return 0;
        }
        port_ = static_cast<uint16_t>(bound);
        thread_ = std::thread([this] { server_.listen_after_bind(); });

        WriteDiscoveryFile();
        out_token = token_;
        return port_;
    }

    // Same non-locking contract as Start() above, for the same reason.
    void Stop() {
        if (!running()) {
            return;
        }
        server_.stop();
        thread_.join();
        RemoveDiscoveryFile();
        port_ = 0;
        token_.clear();
    }

private:
    static std::string ToHex(const unsigned char* data, size_t len) {
        static const char kHexDigits[] = "0123456789abcdef";
        std::string out(len * 2, '0');
        for (size_t i = 0; i < len; ++i) {
            out[2 * i] = kHexDigits[data[i] >> 4];
            out[2 * i + 1] = kHexDigits[data[i] & 0x0F];
        }
        return out;
    }

    std::filesystem::path DiscoveryFilePath() const { return KnownVaultsPath().parent_path() / "api.json"; }

    void WriteDiscoveryFile() const {
        std::filesystem::path path = DiscoveryFilePath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::trunc);
        if (!f) {
            return;
        }
        f << "{\"port\":" << port_ << ",\"token\":\"" << token_ << "\"}";
        f.close();
#ifndef _WIN32
        // This file is effectively a credential (the token grants API
        // access to every currently-unlocked vault) and shouldn't be
        // group/world-readable. Windows already restricts a per-user
        // profile directory like this by default; there's no POSIX-
        // style chmod equivalent needed there.
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                      std::filesystem::perm_options::replace, ec);
#endif
    }

    void RemoveDiscoveryFile() const {
        std::error_code ec;
        std::filesystem::remove(DiscoveryFilePath(), ec);
    }

    // Constant-time comparison against the expected `Authorization:
    // Bearer <token>` header - a timing side-channel on token
    // comparison is a real, well-known bug class even for a
    // localhost-only API, and sodium_memcmp costs nothing extra to
    // use here instead of `==`.
    bool CheckAuth(const httplib::Request& req, httplib::Response& res) const {
        std::string expected = "Bearer " + token_;
        std::string actual = req.get_header_value("Authorization");
        bool ok = actual.size() == expected.size()
                  && sodium_memcmp(actual.data(), expected.data(), expected.size()) == 0;
        if (!ok) {
            res.status = 401;
            res.set_content(R"({"error":"missing or invalid Authorization: Bearer <token>"})", "application/json");
        }
        return ok;
    }

    vaultstore::Vault* FindOpenVault(int vault_index) const {
        for (const AppState::OpenSession& session : state_.open_sessions) {
            if (session.vault_index == vault_index) {
                return session.vault.get();
            }
        }
        return nullptr;
    }

    static void WriteError(httplib::Response& res, int status, const std::string& message) {
        res.status = status;
        res.set_content(R"({"error":")" + JsonEscape(message) + "\"}", "application/json");
    }

    static std::string JsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    static void SerializeNodeJson(const vaultstore::TreeNode& node, std::string& out) {
        out += R"({"name":")" + JsonEscape(node.name) + R"(","is_folder":)";
        out += node.is_folder ? "true" : "false";
        if (node.is_folder) {
            out += R"(,"children":[)";
            for (size_t i = 0; i < node.children.size(); ++i) {
                if (i) out += ",";
                SerializeNodeJson(node.children[i], out);
            }
            out += "]";
        } else {
            out += R"(,"size":)" + std::to_string(node.size);
        }
        out += "}";
    }

    // Walks `path` (forward-slash-separated, e.g.
    // "Documents/2025/tax.pdf") from `root`'s children downward.
    // Returns nullptr if any segment doesn't exist. Empty segments (a
    // leading/trailing/doubled slash) are skipped rather than treated
    // as errors, so "/Documents/tax.pdf" and "Documents/tax.pdf"
    // behave the same.
    static vaultstore::TreeNode* FindNodeByPath(vaultstore::TreeNode& root, const std::string& path) {
        vaultstore::TreeNode* current = &root;
        size_t pos = 0;
        while (pos < path.size()) {
            size_t next = path.find('/', pos);
            std::string segment = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            pos = (next == std::string::npos) ? path.size() : next + 1;
            if (segment.empty()) {
                continue;
            }
            if (!current->is_folder) {
                return nullptr;
            }
            auto it = std::find_if(current->children.begin(), current->children.end(),
                                    [&](vaultstore::TreeNode& n) { return n.name == segment; });
            if (it == current->children.end()) {
                return nullptr;
            }
            current = &*it;
        }
        return current;
    }

    // Splits `path` into (parent folder node, leaf name) - e.g.
    // "Documents/tax.pdf" -> (the "Documents" node, "tax.pdf").
    // Returns false if the parent folder doesn't exist or isn't a
    // folder - the write endpoint requires the folder to already
    // exist rather than auto-creating intermediate folders, keeping
    // folder creation as something only the app's own UI does.
    static bool FindParentAndLeaf(vaultstore::TreeNode& root, const std::string& path,
                                   vaultstore::TreeNode** out_parent, std::string& out_leaf) {
        size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) {
            *out_parent = &root;
            out_leaf = path;
            return !out_leaf.empty();
        }
        std::string parent_path = path.substr(0, slash);
        out_leaf = path.substr(slash + 1);
        if (out_leaf.empty()) {
            return false;
        }
        vaultstore::TreeNode* parent = FindNodeByPath(root, parent_path);
        if (!parent || !parent->is_folder) {
            return false;
        }
        *out_parent = parent;
        return true;
    }

    void RegisterRoutes() {
        server_.Get("/v1/ping", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"app":"CryptVault","api_version":1})", "application/json");
        });

        server_.Get("/v1/vaults", [this](const httplib::Request& req, httplib::Response& res) {
            if (!CheckAuth(req, res)) return;
            std::lock_guard<std::mutex> lock(frame_mutex_);
            std::string json = "[";
            bool first = true;
            for (const AppState::OpenSession& session : state_.open_sessions) {
                if (session.vault_index < 0 || session.vault_index >= static_cast<int>(state_.vaults.size())) {
                    continue;
                }
                if (!first) json += ",";
                first = false;
                json += R"({"index":)" + std::to_string(session.vault_index) + R"(,"name":")"
                        + JsonEscape(state_.vaults[session.vault_index].name) + "\"}";
            }
            json += "]";
            res.set_content(json, "application/json");
        });

        server_.Get(R"(/v1/vaults/(\d+)/tree)", [this](const httplib::Request& req, httplib::Response& res) {
            if (!CheckAuth(req, res)) return;
            std::lock_guard<std::mutex> lock(frame_mutex_);
            vaultstore::Vault* vault = FindOpenVault(std::stoi(req.matches[1]));
            if (!vault) {
                WriteError(res, 404, "vault not found or not unlocked");
                return;
            }
            std::string json;
            SerializeNodeJson(vault->root(), json);
            res.set_content(json, "application/json");
        });

        server_.Get(R"(/v1/vaults/(\d+)/file)", [this](const httplib::Request& req, httplib::Response& res) {
            if (!CheckAuth(req, res)) return;
            if (!req.has_param("path")) {
                WriteError(res, 400, "missing ?path=");
                return;
            }
            std::lock_guard<std::mutex> lock(frame_mutex_);
            vaultstore::Vault* vault = FindOpenVault(std::stoi(req.matches[1]));
            if (!vault) {
                WriteError(res, 404, "vault not found or not unlocked");
                return;
            }
            vaultstore::TreeNode* node = FindNodeByPath(vault->root(), req.get_param_value("path"));
            if (!node || node->is_folder) {
                WriteError(res, 404, "file not found");
                return;
            }
            std::vector<uint8_t> content;
            std::string error;
            if (!vault->ReadFile(*node, content, error)) {
                WriteError(res, 500, error);
                return;
            }
            res.set_content(reinterpret_cast<const char*>(content.data()), content.size(),
                             "application/octet-stream");
        });

        server_.Post(R"(/v1/vaults/(\d+)/file)", [this](const httplib::Request& req, httplib::Response& res) {
            if (!CheckAuth(req, res)) return;
            if (!req.has_param("path")) {
                WriteError(res, 400, "missing ?path=");
                return;
            }
            std::lock_guard<std::mutex> lock(frame_mutex_);
            vaultstore::Vault* vault = FindOpenVault(std::stoi(req.matches[1]));
            if (!vault) {
                WriteError(res, 404, "vault not found or not unlocked");
                return;
            }
            vaultstore::TreeNode* parent = nullptr;
            std::string leaf;
            if (!FindParentAndLeaf(vault->root(), req.get_param_value("path"), &parent, leaf)) {
                WriteError(res, 404, "parent folder doesn't exist");
                return;
            }
            std::vector<uint8_t> content(req.body.begin(), req.body.end());
            std::string error;
            auto it = std::find_if(parent->children.begin(), parent->children.end(),
                                    [&](vaultstore::TreeNode& n) { return n.name == leaf; });
            bool ok;
            if (it != parent->children.end()) {
                if (it->is_folder) {
                    WriteError(res, 409, "a folder with that name already exists there");
                    return;
                }
                ok = vault->WriteFile(*it, content, error);
            } else {
                ok = vault->AddFile(*parent, leaf, content, error);
            }
            if (!ok) {
                WriteError(res, 500, error);
                return;
            }
            res.set_content(R"({"ok":true})", "application/json");
        });
    }

    AppState& state_;
    std::mutex& frame_mutex_;
    httplib::Server server_;
    std::thread thread_;
    uint16_t port_ = 0;
    std::string token_;
};

}  // namespace

struct UiApp {
    appshell::UiRenderer ui;
    AppState state;
    Icons icons;

    // Guards AppState against concurrent access from the API
    // server's background thread - see ApiServer's own comment for
    // the full threading model. Declared before api_server so it's
    // already constructed by the time ApiServer's constructor (which
    // stores a reference to it) runs.
    std::mutex frame_mutex;
    std::unique_ptr<ApiServer> api_server;
};

extern "C" APPSHELL_UI_API UiApp* UiAppCreate(WGPUDevice device, WGPUQueue queue,
                                                WGPUTextureFormat surface_format) {
    auto* app = new UiApp();
    if (!app->ui.Create(device, queue, surface_format)) {
        std::fprintf(stderr, "[cryptvault] UI setup failed: %s\n", app->ui.last_error().c_str());
        delete app;
        return nullptr;
    }
    if (!LoadIcons(app->ui, app->icons)) {
        std::fprintf(stderr, "[cryptvault] required UI icons failed to load\n");
        delete app;
        return nullptr;
    }
    LoadKnownVaults(app->state);

    // Background choices are scanned and thumbnailed up front (there
    // are normally only a handful) so Settings' picker grid never has
    // to do it lazily mid-frame. The persisted selection (see
    // SelectedBackgroundPath) is applied if it's still a valid choice;
    // otherwise this falls back to the first available choice (if
    // any), which in practice is almost always the legacy
    // assets/background.png someone already had in place, so it
    // doesn't just silently vanish the first time they run a build
    // with this feature in it.
    app->state.background_choices = ScanBackgroundChoices();
    for (const std::string& path : app->state.background_choices) {
        app->state.background_thumbnails.push_back(app->ui.LoadIcon(path));
    }
    std::string selected = LoadSelectedBackgroundChoice();
    auto it = std::find(app->state.background_choices.begin(), app->state.background_choices.end(), selected);
    if (it != app->state.background_choices.end()) {
        app->state.background_selected_index = static_cast<int>(it - app->state.background_choices.begin());
    } else if (!app->state.background_choices.empty()) {
        app->state.background_selected_index = 0;
    }
    if (app->state.background_selected_index >= 0) {
        LoadBackgroundChoice(app->ui, app->state, app->icons,
                              app->state.background_choices[app->state.background_selected_index]);
    }

    app->api_server = std::make_unique<ApiServer>(app->state, app->frame_mutex);
    return app;
}

extern "C" APPSHELL_UI_API void UiAppDestroy(UiApp* app) {
    if (app && app->api_server) {
        // Explicit, not just relying on ~ApiServer via the unique_ptr
        // destructor below - makes the ordering intentional (stop the
        // background thread before anything else about this UiApp
        // starts tearing down) rather than incidental. Stop() is
        // idempotent, so this is never redundant/harmful even though
        // the destructor would also call it.
        app->api_server->Stop();
    }
    if (app) {
        // Best-effort cleanup of decrypted temp copies from
        // OpenFileFromVault - see that function's comment for why
        // they aren't removed immediately after opening. A file still
        // open in whatever application launched it may fail to
        // delete here (still in use, e.g. on Windows) - that's fine,
        // it just means the OS's own temp-directory housekeeping
        // eventually reclaims it instead of this app managing to.
        std::error_code ec;
        for (const std::string& path : app->state.temp_files_to_cleanup) {
            std::filesystem::remove(path, ec);
        }
    }
    delete app;
}

extern "C" APPSHELL_UI_API void UiAppBeginFrame(UiApp* app, uint32_t width, uint32_t height,
                                                   const UiAppInput* input) {
    if (!app || !input || width == 0 || height == 0) return;

    // See ApiServer's comment: the whole frame is one critical
    // section, held for this entire function, so nothing here can
    // race the API server's background thread over AppState's
    // vaults/open_sessions.
    std::lock_guard<std::mutex> frame_lock(app->frame_mutex);

    // Start/stop the API server to match AppState::api_enabled (set
    // by DrawSettingsPanel's toggle) - done here rather than directly
    // in the toggle's own click handler since Start()/Stop() must NOT
    // be called while frame_mutex is already held from somewhere
    // else, and this is the one place that's guaranteed true of.
    if (app->state.api_enabled && !app->api_server->running()) {
        std::string token;
        uint16_t port = app->api_server->Start(token);
        app->state.api_port = port;
        app->state.api_token = token;
        if (port == 0) {
            app->state.api_enabled = false;  // couldn't bind - don't retry every single frame
            PushToast(app->state, "COULDN'T START API SERVER");
        }
    } else if (!app->state.api_enabled && app->api_server->running()) {
        app->api_server->Stop();
        app->state.api_port = 0;
        app->state.api_token.clear();
    }

    app->state.input.mouse_x = input->mouse_x;
    app->state.input.mouse_y = input->mouse_y;
    app->state.input.mouse_down = input->mouse_down != 0;
    app->state.input.clicked = input->clicked != 0;
    app->state.input.double_clicked = input->double_clicked != 0;
    app->state.input.right_clicked = input->right_clicked != 0;
    app->state.input.scroll_delta_y = input->scroll_delta_y;
    app->state.input.text_input = input->text_input;  // already null-terminated, safe to copy in
    app->state.input.key_backspace = input->key_backspace != 0;
    app->state.input.key_enter = input->key_enter != 0;
    app->state.input.key_up = input->key_up != 0;
    app->state.input.key_down = input->key_down != 0;
    app->state.input.dropped_paths.clear();
    for (int i = 0; i < static_cast<int>(input->dropped_paths_count); ++i) {
        app->state.input.dropped_paths.emplace_back(input->dropped_paths[i]);
    }
    if (input->dropped_paths_count_actual > input->dropped_paths_count) {
        PushToast(app->state, "ONLY THE FIRST " + std::to_string(static_cast<int>(input->dropped_paths_count))
                                   + " DROPPED FILES WERE USED");
    }
    app->state.now_seconds = input->now_seconds;
    app->state.dt_seconds = input->dt_seconds;
    app->state.content_width = static_cast<float>(width) - 2.0f * kBorder;
    app->state.content_height = static_cast<float>(height) - 2.0f * kBorder;

    app->ui.BeginFrame(width, height);
    app->ui.DrawRect(0, 0, static_cast<float>(width), static_cast<float>(height), kBorderColor);
    ContentRenderer content(app->ui, kBorder, kBorder);

    // Advances the active background's animation, if it has more than
    // one frame (a real GIF - see LoadBackgroundChoice; anything else
    // is a single frame with a delay long enough this never fires).
    if (app->state.background.frame_icons.size() > 1) {
        double elapsed_ms = (app->state.now_seconds - app->state.background.frame_started_at) * 1000.0;
        int current = app->state.background.current_frame;
        if (elapsed_ms >= static_cast<double>(app->state.background.frame_delays_ms[current])) {
            app->state.background.current_frame =
                (current + 1) % static_cast<int>(app->state.background.frame_icons.size());
            app->state.background.frame_started_at = app->state.now_seconds;
        }
    }
    if (!app->state.background.frame_icons.empty()) {
        int frame_icon = app->state.background.frame_icons[app->state.background.current_frame];
        content.DrawImage(0, 0, app->state.content_width, app->state.content_height, frame_icon);
    }

    // Whole-app lock (see AppState::Lock/DrawLockScreen) takes over
    // the entire content area - nothing else (toolbar, tabs, grid, or
    // any of the other overlays below) draws or reacts to input while
    // it's up, so there's nothing real to accidentally click through
    // to. The resize toggle is the one exception: resizing the window
    // doesn't expose anything, so there's no reason to block it.
    if (app->state.lock.locked) {
        DrawLockScreen(content, app->state);
        DrawResizeToggle(content, app->state);
        DrawBorderRightClickMenu(content, app->state);
        return;
    }

    // Drawn here - before the overlay-masking below - rather than
    // from inside DrawVaultListScreen/DrawVaultFolderScreen like it
    // used to be. It used to be called from *inside* the masked
    // block, which meant that the moment any overlay (most commonly
    // the settings panel or the command palette, since those are the
    // two the toolbar itself opens) was open, the toolbar's own
    // clicked flag got masked out along with everything else - so the
    // gear/search icons could open their panel once but could never
    // be clicked again afterwards to close it or switch to the other
    // one. This was the actual cause of "Settings/Search aren't
    // working". The toolbar's buttons should always be live
    // regardless of what overlay (if any) is open; only the grid/tab
    // bar underneath needs masking while one is up.
    DrawToolbar(content, app->icons, app->state);

    // While a delete confirmation, the settings panel, the command
    // palette, a context menu, a rename panel, or the change-password
    // wizard is up, the screen underneath still draws (so vaults/tabs
    // stay visible) but shouldn't react to clicks - only that
    // overlay's own controls should be. Restored immediately after,
    // so the overlays themselves (drawn below) still see real input.
    bool overlay_active = app->state.pending_delete.active || app->state.settings_panel_open
                           || app->state.command_palette.open || app->state.context_menu.open
                           || app->state.rename.active || app->state.change_password.open
                           || app->state.unlock_prompt.open || app->state.create_vault_wizard.open
                           || app->state.clear_all_data_pending || app->state.border_menu.open;
    bool real_clicked = app->state.input.clicked;
    bool real_double_clicked = app->state.input.double_clicked;
    bool real_right_clicked = app->state.input.right_clicked;
    float real_scroll_delta_y = app->state.input.scroll_delta_y;
    if (overlay_active) {
        app->state.input.clicked = false;
        app->state.input.double_clicked = false;
        app->state.input.right_clicked = false;
        // Also masks scroll while an overlay is up - without this, a
        // scroll-wheel nudge meant for e.g. the settings panel (see
        // its own scroll handling) could also reach the vault/folder
        // grid underneath it, since the panel visually sits on top of
        // the grid but doesn't otherwise know it's "covering" it.
        app->state.input.scroll_delta_y = 0.0f;
    }

    if (app->state.screen == Screen::VaultList) {
        DrawVaultListScreen(content, app->icons, app->state);
    } else {
        DrawVaultFolderScreen(content, app->icons, app->state);
    }

    if (overlay_active) {
        app->state.input.clicked = real_clicked;
        app->state.input.double_clicked = real_double_clicked;
        app->state.input.right_clicked = real_right_clicked;
        app->state.input.scroll_delta_y = real_scroll_delta_y;
    }

    DrawToasts(content, app->state);
    DrawResizeToggle(content, app->state);
    DrawLockToggle(content, app->state);
    DrawBorderRightClickMenu(content, app->state);
    DrawSettingsPanel(content, app->state, app->icons);
    DrawCommandPalette(content, app->state);
    DrawContextMenu(content, app->state);
    DrawRenamePanel(content, app->state);
    DrawChangePasswordPanel(content, app->state);
    DrawUnlockPrompt(content, app->state);
    DrawCreateVaultWizard(content, app->state);
    DrawDeleteModal(content, app->state);
    DrawClearAllDataModal(content, app->state);
}

extern "C" APPSHELL_UI_API void UiAppEndFrame(UiApp* app, WGPURenderPassEncoder pass) {
    if (!app || !pass) return;
    app->ui.EndFrame(pass);
}

extern "C" APPSHELL_UI_API uint8_t UiAppConsumeResizeRequest(UiApp* app, uint32_t* out_width,
                                                                uint32_t* out_height) {
    if (!app || !out_width || !out_height || !app->state.resize_requested) {
        return 0;
    }
    app->state.resize_requested = false;
    if (app->state.big_mode) {
        *out_width = static_cast<uint32_t>(kLargeWindowWidth);
        *out_height = static_cast<uint32_t>(kLargeWindowHeight);
    } else {
        *out_width = static_cast<uint32_t>(kSmallWindowWidth);
        *out_height = static_cast<uint32_t>(kSmallWindowHeight);
    }
    return 1;
}

extern "C" APPSHELL_UI_API uint8_t UiAppConsumeQuitRequest(UiApp* app) {
    if (!app || !app->state.quit_requested) {
        return 0;
    }
    app->state.quit_requested = false;
    return 1;
}

extern "C" APPSHELL_UI_API uint8_t UiAppGetAlwaysOnTop(UiApp* app) {
    return (app && app->state.always_on_top) ? 1 : 0;
}

// --- Internal dev diagnostic export - not used by the shipped app --------
// Dumps key AppState fields as a single line of text for
// src/debug_repro.cpp (a scripted synthetic-input harness) to inspect
// between frames - the practical way to observe internal state
// transitions deterministically rather than guessing from reading
// code, or needing a human to reproduce a bug by hand every time. This
// found the actual root cause behind several real, reported bugs
// (state.screen silently resetting on ANY click, not just a click on
// the icon meant to trigger it) that weren't obvious from code review
// alone. Kept as ordinary dev tooling going forward (see
// debug_repro's own header comment) rather than deleted after one use.
extern "C" APPSHELL_UI_API void UiAppDebugDump(UiApp* app, char* buf, int buf_size) {
    if (!app || !buf || buf_size <= 0) return;
    std::string s;
    s += "screen=" + std::to_string(static_cast<int>(app->state.screen));
    s += " vaults=" + std::to_string(app->state.vaults.size());
    s += " vault_tabs=" + std::to_string(app->state.vault_tabs.size());
    s += " active_vault_tab=" + std::to_string(app->state.active_vault_tab);
    s += " open_sessions=" + std::to_string(app->state.open_sessions.size());
    s += " selected_vault=" + std::to_string(app->state.selected_vault);
    s += " selected_folder=" + std::to_string(app->state.selected_folder);
    s += " wizard_open=" + std::to_string(app->state.create_vault_wizard.open);
    s += " wizard_stage=" + std::to_string(app->state.create_vault_wizard.stage);
    s += " wizard_err=" + app->state.create_vault_wizard.error;
    s += " unlock_open=" + std::to_string(app->state.unlock_prompt.open);
    s += " settings_open=" + std::to_string(app->state.settings_panel_open);
    s += " palette_open=" + std::to_string(app->state.command_palette.open);
    s += " ctxmenu_open=" + std::to_string(app->state.context_menu.open);
    s += " toasts=" + std::to_string(app->state.toasts.size());
    for (auto& t : app->state.toasts) s += " toast[" + t.text + "]";
    if (!app->state.vaults.empty()) {
        s += " vault0={" + app->state.vaults[0].name + "," + app->state.vaults[0].path + "}";
    }
    vaultstore::Vault* v = ActiveOpenVault(app->state);
    if (v) {
        std::vector<std::string>* fp = ActiveFolderPath(app->state);
        s += " folder_path=[";
        if (fp) for (auto& seg : *fp) s += seg + "/";
        s += "]";
        vaultstore::TreeNode& current = CurrentFolder(*v, fp);
        s += " current_children=" + std::to_string(current.children.size());
        for (auto& c : current.children) {
            s += " [" + c.name + (c.is_folder ? "/" : "") + "]";
        }
        s += " temp_files=" + std::to_string(app->state.temp_files_to_cleanup.size());
    } else {
        s += " active_vault=null";
    }
    std::snprintf(buf, buf_size, "%s", s.c_str());
}