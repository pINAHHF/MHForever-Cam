// Freedom Unite host: runs the recompiled game against the player's own ISO (docs/06-mhfu-module.md).
//
// usage: mhfu --iso <image.iso> --exe <decrypted executable> [--trace] [--memstick <dir>]
//             [--window] [--auto-dialogs] [--dump-frames <dir> [--dump-every N] [--dump-from S]] [--seconds S] [--quiet-overlays]
//             [--input "<seconds>:<button>,..."] [--name <text>] [--software] [--scale N] [--fullscreen]
//             [--smooth-textures] [--fps 30..180] [--music-volume 0-100] [--sfx-volume 0-100]
//             [--keymap "south=KeyK,east=KeyL,..."] [--padmap "south=south,left_shoulder=left_shoulder+left_trigger,..."]
//             [--controls-file <path>]
//             [--adhoc-server host[:port]] [--adhoc-nick <name>] [--adhoc-mac AA:BB:CC:DD:EE:FF] [--adhoc-room CODE]
// The executable supplies the initial data sections. The launcher's installer prepares it from the player's own disc image
// (launcher/native/install/); developer builds can also point --exe at a file that matches region.toml's dev_executable.

#include "mhf/psp/frame_dump.hpp"
#include "orbit_camera.hpp"
#include "mhf/psp/generated.hpp"
#include "mhf/psp/host.hpp"
#include "mhf/psp/runtime.hpp"
#include "iso9660.hpp"
#include "param_sfo.hpp"

#include "host/keymap.hpp"
#include "host/padmap.hpp"

#if __has_include("mhfu_camera_support.hpp")
#include "mhfu_camera_support.hpp"
#else
#define MHF_MHFU_HAS_ANALOG_CAMERA 0
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>
#include <system_error>

#if defined(MHF_HAS_FRONTEND)
#include "psp_frontend.hpp"
#endif
#include <cstdlib>
#include <cstring>
#include <exception>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif
#include <string>
#include <utility>
#include <vector>

namespace mhf::psp::generated {
std::span<const Program *const> mhfu_programs();
}

namespace {

using mhf::psp::Context;
using mhf::psp::Runtime;

bool g_log_overlays = true;

// The game used to write diagnostics to its console. Keep those messages,
// including automatic camera traces, in one file per session instead.
bool configure_automatic_logging() {
    namespace fs = std::filesystem;
    fs::path data_root;
    if (const char *custom_root = std::getenv("MHF_DATA_DIR");
        custom_root != nullptr && *custom_root != '\0') {
        data_root = fs::path(custom_root);
    }
#ifdef _WIN32
    else if (const char *app_data = std::getenv("APPDATA");
             app_data != nullptr && *app_data != '\0') {
        data_root = fs::path(app_data) / "MHForever";
    } else if (const char *local_app_data = std::getenv("LOCALAPPDATA");
               local_app_data != nullptr && *local_app_data != '\0') {
        data_root = fs::path(local_app_data) / "MHForever";
    }
#else
    else if (const char *xdg_data = std::getenv("XDG_DATA_HOME");
             xdg_data != nullptr && *xdg_data != '\0') {
        data_root = fs::path(xdg_data) / "MHForever";
    } else if (const char *home = std::getenv("HOME");
               home != nullptr && *home != '\0') {
        data_root = fs::path(home) / ".local" / "share" / "MHForever";
    }
#endif
    if (data_root.empty()) {
        std::error_code path_error;
        data_root = fs::temp_directory_path(path_error) / "MHForever";
        if (path_error) return false;
    }

    const fs::path log_directory = data_root / "logs";
    std::error_code directory_error;
    fs::create_directories(log_directory, directory_error);
    if (directory_error) return false;

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
#ifdef _WIN32
    if (localtime_s(&local_time, &now_time) != 0) return false;
#else
    if (localtime_r(&now_time, &local_time) == nullptr) return false;
#endif
    char timestamp[24]{};
    if (std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local_time) == 0u) return false;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;

#ifdef _WIN32
    const int process_id = _getpid();
#else
    const int process_id = static_cast<int>(getpid());
#endif
    std::ostringstream filename;
    filename << "mhfu-" << timestamp << '-'
             << std::setw(3) << std::setfill('0') << milliseconds
             << '-' << process_id << ".log";
    const fs::path log_path = log_directory / filename.str();

    static std::FILE *session_stdout = nullptr;
    static std::FILE *session_stderr = nullptr;
#ifdef _WIN32
    // GUI-subsystem processes may start without usable console descriptors.
    // freopen assigns the CRT streams directly to the session log.
    if (_wfreopen_s(&session_stdout, log_path.c_str(), L"ab", stdout) != 0 ||
        _wfreopen_s(&session_stderr, log_path.c_str(), L"ab", stderr) != 0) {
        return false;
    }
#else
    session_stdout = std::freopen(log_path.c_str(), "ab", stdout);
    session_stderr = std::freopen(log_path.c_str(), "ab", stderr);
    if (session_stdout == nullptr || session_stderr == nullptr) return false;
#endif
    std::setvbuf(session_stdout, nullptr, _IONBF, 0);
    std::setvbuf(session_stderr, nullptr, _IONBF, 0);
    std::fprintf(stderr, "[mhfu] automatic session log enabled\n");

    // camera_trace_enabled() reads this flag once, on its first use.
#ifdef _WIN32
    _putenv_s("MHF_CAMERA_LOG", "1");
#else
    setenv("MHF_CAMERA_LOG", "1", 1);
#endif
    return true;
}

// Scripted input for unattended runs: "12.5:cross,15:start" presses each button for 150 ms at that time;
// "20:sup/3" holds the analog stick up (sup, sdown, sleft, sright) or a button for 3 seconds.
struct ScriptedPress {
    double at_seconds;
    std::uint32_t buttons;
    double duration = 0.15;
    int stick_x = 0, stick_y = 0;
};

std::vector<ScriptedPress> parse_input_script(const std::string &text) {
    static const std::pair<const char *, std::uint32_t> names[] = {
        {"cross", mhf::psp::buttons::kCross}, {"circle", mhf::psp::buttons::kCircle},
        {"square", mhf::psp::buttons::kSquare}, {"triangle", mhf::psp::buttons::kTriangle},
        {"start", mhf::psp::buttons::kStart}, {"select", mhf::psp::buttons::kSelect},
        {"up", mhf::psp::buttons::kUp}, {"down", mhf::psp::buttons::kDown},
        {"left", mhf::psp::buttons::kLeft}, {"right", mhf::psp::buttons::kRight},
        {"l", mhf::psp::buttons::kLTrigger}, {"r", mhf::psp::buttons::kRTrigger},
    };
    std::vector<ScriptedPress> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto comma = text.find(',', start);
        const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const auto colon = item.find(':');
        if (colon != std::string::npos) {
            std::string name = item.substr(colon + 1);
            double duration = 0.15;
            if (const auto slash = name.find('/'); slash != std::string::npos) {
                duration = std::stod(name.substr(slash + 1));
                name.resize(slash);
            }
            const double at = std::stod(item.substr(0, colon));
            for (const auto &[n, bit] : names) {
                if (name == n) out.push_back({at, bit, duration});
            }
            if (name == "sup") out.push_back({at, 0, duration, 0, -127});
            if (name == "sdown") out.push_back({at, 0, duration, 0, 127});
            if (name == "sleft") out.push_back({at, 0, duration, -127, 0});
            if (name == "sright") out.push_back({at, 0, duration, 127, 0});
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// Hook on mh_overlay_prepare(image, size): the overlay image (header included) is already in guest memory and its
// static initializers are about to run, so its recompiled code must become the active one for its region now.
void on_overlay_prepare(Runtime &rt, Context &ctx) {
    const std::uint32_t image = ctx.gpr[4];
    const auto overlay_id = static_cast<std::int32_t>(rt.memory().load32(image + 4));
    const bool ok = rt.activate_overlay(overlay_id);
    if (g_log_overlays || !ok) {
        std::fprintf(stderr, "[mhfu] overlay %d %s at %08X\n", overlay_id, ok ? "activated" : "UNKNOWN", image);
    }
}

// The disc's product code ("ULES01213") from its PARAM.SFO, which ad hoc servers use to group the players of one game.
std::string disc_product_code(const std::filesystem::path &disc_image) {
    try {
        mhf::psp::IsoImage image(disc_image);
        if (const auto entry = image.find("/PSP_GAME/PARAM.SFO")) {
            if (const auto sfo = mhf::psp::ParamSfo::parse(image.read_file(*entry, 0, entry->size))) return sfo->string("DISC_ID");
        }
    } catch (const std::exception &) {
    }
    return {};
}

// "AA:BB:CC:DD:EE:FF" (or with '-') as bytes. False for anything else.
bool parse_mac(const std::string &text, std::array<std::uint8_t, 6> &mac) {
    unsigned v[6];
    char sep[5];
    if (std::sscanf(text.c_str(), "%2x%c%2x%c%2x%c%2x%c%2x%c%2x", &v[0], &sep[0], &v[1], &sep[1], &v[2], &sep[2], &v[3], &sep[3], &v[4],
                    &sep[4], &v[5]) != 11) {
        return false;
    }
    for (const char c : sep) {
        if (c != ':' && c != '-') return false;
    }
    for (int i = 0; i < 6; ++i) mac[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v[i]);
    return true;
}

// "MHForever - <the disc's own title>": the window names the game that is running. The title comes from the disc image's
// PARAM.SFO (upper-case titles are shown in title case); without a readable image the window is just "MHForever".
std::string window_title(const std::filesystem::path &disc_image) {
    std::string title;
    try {
        if (!disc_image.empty()) {
            mhf::psp::IsoImage image(disc_image);
            if (const auto entry = image.find("/PSP_GAME/PARAM.SFO")) {
                if (const auto sfo = mhf::psp::ParamSfo::parse(image.read_file(*entry, 0, entry->size))) {
                    title = sfo->string("TITLE");
                }
            }
        }
    } catch (const std::exception &) {
    }
    // The disc's title carries a trademark sign (UTF-8 "\xE2\x84\xA2" or "\xC2\xAE"): not wanted in a window title.
    for (const char *mark : {"\xE2\x84\xA2", "\xC2\xAE"}) {
        for (std::size_t at = title.find(mark); at != std::string::npos; at = title.find(mark)) title.erase(at, std::strlen(mark));
    }
    while (!title.empty() && (title.back() == ' ' || title.back() == '\n')) title.pop_back();
    if (title.empty()) return "MHForever";
    bool all_upper = true;
    for (const char c : title) all_upper = all_upper && !(c >= 'a' && c <= 'z');
    if (all_upper) {
        bool word_start = true;
        for (char &c : title) {
            const bool letter = c >= 'A' && c <= 'Z';
            if (letter && !word_start) c = static_cast<char>(c - 'A' + 'a');
            word_start = c == ' ' || c == '-';
        }
    }
    return "MHForever - " + title;
}

} // namespace

int main(int argc, char **argv) {
    bool log_ready = false;
    try {
        log_ready = configure_automatic_logging();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[mhfu] automatic logging setup failed: %s\\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "[mhfu] automatic logging setup failed with an unknown error\\n");
    }
#ifdef _WIN32
    // Detach only after stdout/stderr have been redirected. If log setup fails,
    // keep the console attached so startup errors remain visible.
    if (log_ready) FreeConsole();
#endif
    mhf::psp::RuntimeOptions options;
    std::string exe;
    std::filesystem::path dump_folder;
    unsigned dump_every = 30;
    double dump_from = 0.0; // seconds of run time before frames are dumped
    double seconds = 0;
    bool window = false;
    bool auto_dialogs = false;
    bool software = false; // --software: draw with the CPU renderer even in a window
    int render_scale = 4;  // --scale N: internal resolution = N x 480x272 (GPU renderer)
    bool fullscreen = false;
    bool smooth_textures = false;
    int target_fps = 30; // --fps N: 30 (the game's own rate) or a multiple of it up to 180
    mhf::host::KeyBindings keyboard; // --keymap: the launcher's keyboard bindings
    mhf::host::PadBindings controller; // --padmap: the launcher's controller bindings
    std::string controls_file;         // --controls-file: where the in-game menu writes the bindings the player changes
    std::string texture_pack;          // --texture-pack: a texture pack's folder (MHF_TEXTURE_PACK too; the argument wins)
    std::string friends_file, friend_actions_file; // --friends-file, --friend-actions-file: friends in the Online tab
    std::string adhoc_nick;            // --adhoc-nick: the name other players see online (default: --name)
    std::vector<ScriptedPress> script;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--iso" && i + 1 < argc) options.disc_image = argv[++i];
        else if (a == "--exe" && i + 1 < argc) exe = argv[++i];
        else if (a == "--memstick" && i + 1 < argc) options.memstick_root = argv[++i];
        else if (a == "--trace") options.trace_imports = true;
        else if (a == "--quiet-overlays") g_log_overlays = false;
        else if (a == "--dump-frames" && i + 1 < argc) dump_folder = argv[++i];
        else if (a == "--dump-every" && i + 1 < argc) dump_every = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (a == "--dump-from" && i + 1 < argc) dump_from = std::stod(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
        else if (a == "--window") window = true;
        else if (a == "--software") software = true;
        else if (a == "--scale" && i + 1 < argc) render_scale = std::clamp(std::stoi(argv[++i]), 1, 10);
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--smooth-textures") smooth_textures = true; // enhancement, off by default
        else if (a == "--fps" && i + 1 < argc) target_fps = std::clamp(std::stoi(argv[++i]), 30, 180); // enhancement
        else if (a == "--music-volume" && i + 1 < argc) options.music_volume = std::clamp(std::stoi(argv[++i]), 0, 100);
        else if (a == "--sfx-volume" && i + 1 < argc) options.sfx_volume = std::clamp(std::stoi(argv[++i]), 0, 100);
        else if (a == "--keymap" && i + 1 < argc) keyboard = mhf::host::parse_keymap(argv[++i]);
        else if (a == "--padmap" && i + 1 < argc) controller = mhf::host::parse_padmap(argv[++i]);
        else if (a == "--controls-file" && i + 1 < argc) controls_file = argv[++i];
        else if (a == "--texture-pack" && i + 1 < argc) texture_pack = argv[++i];
        else if (a == "--friends-file" && i + 1 < argc) friends_file = argv[++i];
        else if (a == "--friend-actions-file" && i + 1 < argc) friend_actions_file = argv[++i];
        else if (a == "--adhoc-server" && i + 1 < argc) options.adhoc_server = argv[++i]; // online play: an ad hoc server (PPSSPP's)
        else if (a == "--adhoc-nick" && i + 1 < argc) adhoc_nick = argv[++i];
        else if (a == "--adhoc-room" && i + 1 < argc) options.adhoc_room = argv[++i]; // a private room's code
        else if (a == "--adhoc-mac" && i + 1 < argc) {
            if (!parse_mac(argv[++i], options.adhoc_mac)) std::fprintf(stderr, "[mhfu] --adhoc-mac: not a MAC address, ignored\n");
        }
        else if (a == "--input" && i + 1 < argc) script = parse_input_script(argv[++i]);
        else if (a == "--name" && i + 1 < argc) options.osk_default_text = argv[++i]; // text typed into keyboards
        else if (a == "--auto-dialogs") auto_dialogs = true; // system dialogs answer at once even in a window
        else {
            std::fprintf(stderr, "usage: %s --iso <image.iso> --exe <decrypted executable> [--trace] [--memstick <dir>]\n",
                         argv[0]);
            return 2;
        }
    }
    if (options.disc_image.empty() || exe.empty()) {
        std::fprintf(stderr, "error: --iso and --exe are required\n");
        return 2;
    }
    // In a window the player answers the system dialogs (messages, keyboard, save/load) on screen.
    options.interactive_dialogs = window && !auto_dialogs;
    if (texture_pack.empty()) {
        if (const char *pack = std::getenv("MHF_TEXTURE_PACK")) texture_pack = pack;
    }
    if (options.adhoc_server.empty()) {
        if (const char *server = std::getenv("MHF_ADHOC_SERVER")) options.adhoc_server = server; // until the launcher sets it
    }
    options.adhoc_nickname = adhoc_nick.empty() ? options.osk_default_text : adhoc_nick;
    options.analog_camera_input = MHF_MHFU_HAS_ANALOG_CAMERA != 0;
    if (!options.adhoc_server.empty()) options.adhoc_product_code = disc_product_code(options.disc_image);
    // The halls' names start with these (docs/14-rooms-and-friends.md section 1): Freedom Unite's two modes, and the
    // Japanese release's "集会所" in Shift-JIS. Any other prefix the game uses is learnt when it joins a hall.
    options.adhoc_hall_prefixes = {"MHP2Q0", "MHP2A0", "\x8F\x57\x89\xEF\x8F\x8A"};
    try {
        Runtime runtime{options};
        runtime.load_elf(exe);
        const auto programs = mhf::psp::generated::mhfu_programs();
        for (std::size_t i = 1; i < programs.size(); ++i) {
            runtime.add_overlay(*programs[i]);
        }
        runtime.register_hook("mh_overlay_prepare", &on_overlay_prepare);
        register_mhfu_orbit_camera(runtime);
        std::function<void()> extra;
        if (!dump_folder.empty() || seconds > 0 || !script.empty()) {
            if (!dump_folder.empty()) std::filesystem::create_directories(dump_folder);
            auto *rt = &runtime;
            std::uint64_t last_vblank = 0;
            unsigned frame = 0;
            extra = [=]() mutable {
                const std::uint64_t now = rt->now_us();
                if (seconds > 0 && static_cast<double>(now) > seconds * 1e6) {
                    std::fprintf(stderr, "[mhfu] time limit reached\n");
                    rt->dump_threads();
                    rt->exit_program_from_host(0);
                }
                if (!script.empty()) {
                    mhf::psp::ControllerState pad;
                    for (const auto &p : script) {
                        const double t = static_cast<double>(now) / 1e6;
                        if (t < p.at_seconds || t >= p.at_seconds + p.duration) continue;
                        pad.buttons |= p.buttons;
                        if (p.stick_x != 0) pad.lx = static_cast<std::uint8_t>(128 + p.stick_x);
                        if (p.stick_y != 0) pad.ly = static_cast<std::uint8_t>(128 + p.stick_y);
                    }
                    mhf::psp::set_controller_state(*rt, pad);
                }
                // One dump per `dump_every` vblanks, even when the host is not polled on that exact vblank.
                const std::uint64_t slot = now / mhf::psp::kVblankPeriodUs / dump_every;
                if (dump_folder.empty() || slot == last_vblank || static_cast<double>(now) < dump_from * 1e6) return;
                last_vblank = slot;
                char name[32];
                std::snprintf(name, sizeof name, "frame_%06u.png", ++frame);
                mhf::psp::dump_display(*rt, dump_folder / name);
            };
        }
#if defined(MHF_HAS_FRONTEND)
        mhf::psp::Frontend frontend;
        if (window && frontend.open(window_title(options.disc_image).c_str(), false, 2, !software, render_scale, fullscreen, smooth_textures, target_fps, keyboard, controller)) {
            frontend.set_controls_file(controls_file);
            frontend.set_friends_files(friends_file, friend_actions_file);
            if (!texture_pack.empty()) frontend.set_texture_pack(texture_pack, disc_product_code(options.disc_image));
            frontend.attach(runtime, extra);
        } else if (window) {
            // A player asked for a window: never keep running invisibly.
            std::fprintf(stderr, "error: the game window could not be opened (see the messages above)\n");
            return 3;
        } else if (extra) {
            runtime.kernel_host_poll(extra);
        }
#else
        (void)keyboard; // only the windowed frontend maps the keyboard and the controller
        (void)controller;
        (void)controls_file;
        (void)friends_file;
        (void)friend_actions_file;
        if (window) std::fprintf(stderr, "[mhfu] built without the SDL frontend; running headless\n");
        if (extra) runtime.kernel_host_poll(extra);
#endif
        return runtime.run(*programs[0], "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
