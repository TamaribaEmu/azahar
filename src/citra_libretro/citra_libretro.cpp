// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <numeric>
#include <vector>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ENABLE_OPENGL
#include "glad/glad.h"
#include "video_core/renderer_opengl/gl_vars.h"
#endif
#include "libretro.h"

#include "audio_core/libretro_input.h"
#include "audio_core/libretro_sink.h"
#include "video_core/gpu.h"
#ifdef ENABLE_OPENGL
#include "video_core/renderer_opengl/renderer_opengl.h"
#endif
#ifdef ENABLE_VULKAN
#include "citra_libretro/libretro_vk.h"
#endif
#include "video_core/renderer_software/renderer_software.h"
#include "video_core/video_core.h"

#include "citra_libretro/citra_libretro.h"
#include "citra_libretro/core_settings.h"
#include "citra_libretro/environment.h"
#include "citra_libretro/input/input_factory.h"
#include "citra_libretro/tamariba_rooms.h"
#include "common/tamariba_net.h"

#include "common/arch.h"
#if CITRA_ARCH(x86_64)
#include "common/x64/cpu_detect.h"
#endif
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/image_interface.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/process.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/perf_stats.h"
#include "core/tamariba_inet.h"

#ifdef HAVE_LIBRETRO_VFS
#include <streams/file_stream_transforms.h>
#endif

class CitraLibRetro {
public:
    CitraLibRetro() : log_filter(Common::Log::Level::Debug) {}

    Common::Log::Filter log_filter;
    std::unique_ptr<EmuWindow_LibRetro> emu_window;
    bool game_loaded = false;
    struct retro_hw_render_callback hw_render{};
};

CitraLibRetro* emu_instance;

void retro_init() {
    emu_instance = new CitraLibRetro();
    Common::Log::LibRetroStart(LibRetro::GetLoggingBackend());
    Common::Log::SetGlobalFilter(emu_instance->log_filter);

    LOG_DEBUG(Frontend, "Initializing core...");

    // Set up LLE cores
    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false);
    }

    // Setup default, stub handlers for HLE applets
    Frontend::RegisterDefaultApplets(Core::System::GetInstance());

    // Register generic image interface
    Core::System::GetInstance().RegisterImageInterface(
        std::make_shared<Frontend::ImageInterface>());

    LibRetro::Input::Init();

    // Tamariba: Azahar's rooms (local wireless between consoles), before any game starts:
    // NWM_UDS binds to the room member when the game's services start.
    Tamariba::InitRooms();
}

void retro_deinit() {
    LOG_DEBUG(Frontend, "Shutting down core...");
    if (Core::System::GetInstance().IsPoweredOn()) {
        Core::System::GetInstance().Shutdown();
    }
    Tamariba::ShutdownRooms();
    Tamariba::Inet::Shutdown();

    LibRetro::Input::Shutdown();

    delete emu_instance;

    Common::Log::Stop();
}

unsigned retro_api_version() {
    return RETRO_API_VERSION;
}

/**
 * Updates Citra's settings with Libretro's.
 */
static void UpdateSettings() {
    LibRetro::ParseCoreOptions();

    struct retro_input_descriptor desc[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "ZL"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "ZR"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Home/Swap screens"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Touch Screen Touch"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,
         "Circle Pad X"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,
         "Circle Pad Y"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X,
         "C-Stick / Pointer X"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y,
         "C-Stick / Pointer Y"},
        {0, 0},
    };

    LibRetro::SetInputDescriptors(desc);

    Settings::values.current_input_profile.touch_device = "engine:emu_window";

    // Hardcode buttons to bind to libretro - it is entirely redundant to have
    //  two methods of rebinding controls.
    // Citra: A = RETRO_DEVICE_ID_JOYPAD_A (8)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::A] =
        "button:8,joystick:0,engine:libretro";
    // Citra: B = RETRO_DEVICE_ID_JOYPAD_B (0)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::B] =
        "button:0,joystick:0,engine:libretro";
    // Citra: X = RETRO_DEVICE_ID_JOYPAD_X (9)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::X] =
        "button:9,joystick:0,engine:libretro";
    // Citra: Y = RETRO_DEVICE_ID_JOYPAD_Y (1)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::Y] =
        "button:1,joystick:0,engine:libretro";
    // Citra: UP = RETRO_DEVICE_ID_JOYPAD_UP (4)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::Up] =
        "button:4,joystick:0,engine:libretro";
    // Citra: DOWN = RETRO_DEVICE_ID_JOYPAD_DOWN (5)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::Down] =
        "button:5,joystick:0,engine:libretro";
    // Citra: LEFT = RETRO_DEVICE_ID_JOYPAD_LEFT (6)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::Left] =
        "button:6,joystick:0,engine:libretro";
    // Citra: RIGHT = RETRO_DEVICE_ID_JOYPAD_RIGHT (7)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::Right] =
        "button:7,joystick:0,engine:libretro";
    // Citra: L = RETRO_DEVICE_ID_JOYPAD_L (10)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::L] =
        "button:10,joystick:0,engine:libretro";
    // Citra: R = RETRO_DEVICE_ID_JOYPAD_R (11)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::R] =
        "button:11,joystick:0,engine:libretro";
    // Citra: START = RETRO_DEVICE_ID_JOYPAD_START (3)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::Start] =
        "button:3,joystick:0,engine:libretro";
    // Citra: SELECT = RETRO_DEVICE_ID_JOYPAD_SELECT (2)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::Select] =
        "button:2,joystick:0,engine:libretro";
    // Citra: ZL = RETRO_DEVICE_ID_JOYPAD_L2 (12)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::ZL] =
        "button:12,joystick:0,engine:libretro";
    // Citra: ZR = RETRO_DEVICE_ID_JOYPAD_R2 (13)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::ZR] =
        "button:13,joystick:0,engine:libretro";
    // Citra: HOME = RETRO_DEVICE_ID_JOYPAD_L3 (as per above bindings) (14)
    Settings::values.current_input_profile.buttons[Settings::NativeButton::Values::Home] =
        "button:14,joystick:0,engine:libretro";

    // Circle Pad
    Settings::values.current_input_profile.analogs[0] = "axis:0,joystick:0,engine:libretro";
    // C-Stick
    if (LibRetro::settings.analog_function != LibRetro::CStickFunction::Touchscreen) {
        Settings::values.current_input_profile.analogs[1] = "axis:1,joystick:0,engine:libretro";
    } else {
        Settings::values.current_input_profile.analogs[1] = "";
    }

    if (!emu_instance->emu_window) {
        emu_instance->emu_window = std::make_unique<EmuWindow_LibRetro>();
    }

    // Update the framebuffer sizing.
    emu_instance->emu_window->UpdateLayout();

    Core::System::GetInstance().ApplySettings();
}

/**
 * libretro callback; Called every game tick.
 */
void retro_run() {
    const bool profiling_enabled = Core::IsPerformanceProfilingEnabled();
    const auto retro_run_started = profiling_enabled ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
    if (!emu_instance->game_loaded) {
        // Game failed to load (e.g. encrypted ROM, bad path).
        // Present an empty frame so RetroArch doesn't hang.
        LibRetro::PollInput();
        LibRetro::UploadVideoFrame(nullptr, 0, 0, 0);
        return;
    }

    // Check to see if we actually have any config updates to process.
    if (LibRetro::HasUpdatedConfig()) {
        LibRetro::ParseCoreOptions();
        Core::System::GetInstance().ApplySettings();
        emu_instance->emu_window->UpdateLayout();
    }

    // Poll microphone input from the frontend and buffer it for the emulator
    // This must be done from the main thread as LibRetro's mic interface is not thread-safe
    if (auto* mic_input = AudioCore::GetLibRetroInput()) {
        mic_input->PollMicrophone();
    }

    // Check if the screen swap button is pressed
    static bool screen_swap_button_state = false;
    static bool screens_swapped = false;
    bool screen_swap_btn =
        !!LibRetro::CheckInput(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);
    if (screen_swap_btn != screen_swap_button_state) {
        if (LibRetro::settings.swap_screen_mode == "Toggle") {
            if (!screen_swap_button_state)
                screens_swapped = !screens_swapped;

            if (screens_swapped)
                Settings::values.swap_screen =
                    LibRetro::FetchVariable("citra_swap_screen", "Top") != "Bottom";
            else
                Settings::values.swap_screen =
                    LibRetro::FetchVariable("citra_swap_screen", "Top") == "Bottom";
        } else {
            if (screen_swap_btn)
                Settings::values.swap_screen =
                    LibRetro::FetchVariable("citra_swap_screen", "Top") != "Bottom";
            else
                Settings::values.swap_screen =
                    LibRetro::FetchVariable("citra_swap_screen", "Top") == "Bottom";
        }

        Core::System::GetInstance().ApplySettings();

        // Update the framebuffer sizing.
        emu_instance->emu_window->UpdateLayout();

        screen_swap_button_state = screen_swap_btn;
    }

#ifdef ENABLE_OPENGL
    if (Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::OpenGL) {
        // We can't assume that the frontend has been nice and preserved all OpenGL settings. Reset.
        auto last_state = OpenGL::OpenGLState::GetCurState();
        ResetGLState();
        last_state.Apply();
    }
#endif

    const auto run_loop_started = profiling_enabled ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
    std::size_t run_loop_count = 0;
    while (!emu_instance->emu_window->HasSubmittedFrame()) {
        if (profiling_enabled) {
            ++run_loop_count;
        }
        auto result = Core::System::GetInstance().RunLoop();

        if (result != Core::System::ResultStatus::Success) {
            std::string errorContent = Core::System::GetInstance().GetStatusDetails();
            std::string msg;

            switch (result) {
            case Core::System::ResultStatus::ErrorSystemFiles:
                msg = "Azahar was unable to locate a 3DS system archive: " + errorContent;
                break;
            default:
                msg = "Fatal Error encountered (" + std::to_string(static_cast<int>(result)) +
                      "): " + errorContent;
                break;
            }

            LibRetro::DisplayMessage(msg.c_str());
        }
    }
    if (profiling_enabled) {
        const auto retro_run_finished = std::chrono::steady_clock::now();
        const double total_ms = std::chrono::duration<double, std::milli>(
                                    retro_run_finished - retro_run_started)
                                    .count();
        if (total_ms > 45.0) {
            const double setup_ms = std::chrono::duration<double, std::milli>(
                                        run_loop_started - retro_run_started)
                                        .count();
            const double loops_ms = std::chrono::duration<double, std::milli>(
                                        retro_run_finished - run_loop_started)
                                        .count();
            LOG_INFO(Frontend,
                     "Slow retro_run total={:.3f} setup={:.3f} run_loop={:.3f} count={}",
                     total_ms, setup_ms, loops_ms, run_loop_count);
        }
    }
}

extern "C" void azahar_set_performance_profiling_enabled(bool enabled) {
    Core::SetPerformanceProfilingEnabled(enabled);
}

extern "C" u64 azahar_take_frame_limiter_sleep_ns() {
    return Core::System::GetInstance().frame_limiter.TakeAccumulatedSleepNanoseconds();
}

extern "C" bool azahar_get_and_reset_perf_stats(double* values, std::size_t count) {
    if (values == nullptr || count < 7 || !Core::System::GetInstance().IsPoweredOn()) {
        return false;
    }
    const auto stats = Core::System::GetInstance().GetAndResetPerfStats();
    values[0] = stats.time_vblank_interval;
    values[1] = stats.time_hle_svc;
    values[2] = stats.time_hle_ipc;
    values[3] = stats.time_gpu;
    values[4] = stats.time_swap;
    values[5] = stats.time_remaining;
    values[6] = stats.emulation_speed;
    if (count >= 17) {
        values[7] = static_cast<double>(stats.software_draw_count);
        values[8] = static_cast<double>(stats.software_draw_nanoseconds) / 1'000'000'000.0;
        values[9] = static_cast<double>(stats.software_texture_copy_count);
        values[10] = static_cast<double>(stats.software_texture_copy_nanoseconds) / 1'000'000'000.0;
        values[11] = static_cast<double>(stats.flush_count);
        values[12] = static_cast<double>(stats.flush_nanoseconds) / 1'000'000'000.0;
        values[13] = static_cast<double>(stats.flush_bytes);
        values[14] = static_cast<double>(stats.download_count);
        values[15] = static_cast<double>(stats.download_nanoseconds) / 1'000'000'000.0;
        values[16] = static_cast<double>(stats.download_bytes);
    }
    return true;
}

extern "C" bool azahar_get_and_reset_jit_cache_stats(double* values, std::size_t count) {
    if (values == nullptr || count < 8 || !Core::System::GetInstance().IsPoweredOn()) {
        return false;
    }
    const auto stats = Core::System::GetInstance().GetAndResetJitCacheStats();
    values[0] = static_cast<double>(stats.emitted_blocks);
    values[1] = static_cast<double>(stats.emitted_bytes);
    values[2] = static_cast<double>(stats.invalidated_blocks);
    values[3] = static_cast<double>(stats.cache_clears);
    values[4] = static_cast<double>(stats.live_blocks);
    values[5] = static_cast<double>(stats.used_bytes);
    values[6] = static_cast<double>(stats.capacity_bytes);
    values[7] = static_cast<double>(stats.address_spaces);
    if (count >= 10) {
        values[8] = static_cast<double>(stats.precompile_translate_nanoseconds);
        values[9] = static_cast<double>(stats.precompile_emit_nanoseconds);
    }
    if (count >= 15) {
        values[10] = static_cast<double>(stats.precompile_body_nanoseconds);
        values[11] = static_cast<double>(stats.precompile_terminal_nanoseconds);
        values[12] = static_cast<double>(stats.precompile_deferred_nanoseconds);
        values[13] = static_cast<double>(stats.precompile_metadata_nanoseconds);
        values[14] = static_cast<double>(stats.precompile_link_nanoseconds);
    }
    return true;
}

// GBAShield runtime extension. Keep the write and Timing update on the libretro/emulation thread;
// the frontend invokes this immediately before retro_run(). This bypasses the generic variable
// update handshake, which does not provide an acknowledgement that the live Timing object changed.
extern "C" bool azahar_set_cpu_clock_percentage(int percentage) {
    if (percentage < 5 || percentage > 100) {
        return false;
    }
    Settings::values.cpu_clock_percentage.SetValue(percentage);
    auto& system = Core::System::GetInstance();
    if (system.IsPoweredOn()) {
        system.CoreTiming().UpdateClockSpeed(static_cast<u32>(percentage));
    }
    LOG_INFO(Frontend, "Runtime CPU clock applied directly: {}%", percentage);
    return true;
}

// Tamariba: Azahar's rooms (tamariba_rooms.h). The frontend calls these between retro_run
// calls; none of them waits for the network.

static Network::MacAddress RoomMac(const uint8_t* mac) {
    Network::MacAddress out = Network::NoPreferredMac;
    if (mac) {
        std::copy(mac, mac + out.size(), out.begin());
    }
    return out;
}

static std::string RoomConsoleIdHash(const Network::MacAddress& mac) {
    auto& system = Core::System::GetInstance();
    return system.IsPoweredOn() ? Service::CFG::GetConsoleIdHash(system)
                                : Tamariba::FallbackConsoleIdHash(mac);
}

// Starts this console's room on 127.0.0.1:port (0: a free port) for at most max_members
// consoles and joins it as `nickname` with `mac` (6 bytes; NULL: the room picks one).
// Returns the port, or 0.
extern "C" RETRO_API uint16_t azahar_room_host(uint16_t port, unsigned max_members,
                                               const char* nickname, const uint8_t* mac) {
    auto* rooms = Tamariba::Rooms();
    if (!rooms) {
        return 0;
    }
    const auto address = RoomMac(mac);
    return rooms->Host(port, max_members, nickname ? nickname : "", address,
                       RoomConsoleIdHash(address));
}

// Joins the room at address:port; keeps trying in the background until in, or left.
extern "C" RETRO_API bool azahar_room_join(const char* address, uint16_t port, const char* nickname,
                                           const uint8_t* mac) {
    auto* rooms = Tamariba::Rooms();
    if (!rooms || !address) {
        return false;
    }
    const auto mac_address = RoomMac(mac);
    return rooms->Join(address, port, nickname ? nickname : "", mac_address,
                       RoomConsoleIdHash(mac_address));
}

extern "C" RETRO_API void azahar_room_leave() {
    if (auto* rooms = Tamariba::Rooms()) {
        rooms->Leave();
    }
}

// Tamariba::RoomState: 0 not in a room, 1 joining, 2 in it, negative: stopped trying.
extern "C" RETRO_API int azahar_room_state() {
    auto* rooms = Tamariba::Rooms();
    return rooms ? rooms->State() : Tamariba::RoomIdle;
}

// The consoles in the room by nickname, one per line, into buf (NUL-terminated when cap > 0).
// Returns the length of the whole list (without the NUL), as snprintf does.
extern "C" RETRO_API size_t azahar_room_members(char* buf, size_t cap) {
    std::string list;
    if (auto* rooms = Tamariba::Rooms()) {
        for (const auto& name : rooms->Members()) {
            list += name;
            list += '\n';
        }
    }
    if (buf && cap > 0) {
        const size_t n = std::min(list.size(), cap - 1);
        std::memcpy(buf, list.data(), n);
        buf[n] = '\0';
    }
    return list.size();
}

// Tamariba: the console's own internet traffic (Pretendo) through the Tamariba Plaza's exit
// (common/tamariba_net.h, core/tamariba_inet.h). NULL: the console has no network at all.
extern "C" RETRO_API void retro_tamariba_set_inet(const struct tamariba_inet_interface* inet) {
    Tamariba::Inet::SetInterface(inet);
}

static void setup_memory_maps() {
    auto process = Core::System::GetInstance().Kernel().GetCurrentProcess();
    if (!process)
        return;

    std::vector<retro_memory_descriptor> descs;

    for (const auto& [addr, vma] : process->vm_manager.vma_map) {
        if (vma.type != Kernel::VMAType::BackingMemory)
            continue;
        if (vma.size == 0 || !vma.backing_memory)
            continue;

        // Only expose the well-known user-accessible memory regions
        uint64_t flags = 0;
        if (vma.base >= Memory::HEAP_VADDR && vma.base < Memory::HEAP_VADDR_END) {
            flags = RETRO_MEMDESC_SYSTEM_RAM;
        } else if (vma.base >= Memory::LINEAR_HEAP_VADDR &&
                   vma.base < Memory::LINEAR_HEAP_VADDR_END) {
            flags = RETRO_MEMDESC_SYSTEM_RAM;
        } else if (vma.base >= Memory::NEW_LINEAR_HEAP_VADDR &&
                   vma.base < Memory::NEW_LINEAR_HEAP_VADDR_END) {
            flags = RETRO_MEMDESC_SYSTEM_RAM;
        } else if (vma.base >= Memory::VRAM_VADDR && vma.base < Memory::VRAM_VADDR_END) {
            flags = RETRO_MEMDESC_VIDEO_RAM;
        } else {
            continue;
        }

        retro_memory_descriptor desc = {};
        desc.flags = flags;
        desc.ptr = const_cast<u8*>(vma.backing_memory.GetPtr());
        desc.start = vma.base;
        desc.len = vma.size;

        // select=0 requires power-of-2 len AND start aligned to len.
        // When that doesn't hold, compute a select mask instead.
        bool need_select = (vma.size & (vma.size - 1)) != 0;
        if (!need_select && (vma.base & (vma.size - 1)) != 0)
            need_select = true;

        if (need_select) {
            uint64_t np2 = 1;
            while (np2 < vma.size)
                np2 <<= 1;
            if (vma.base & (np2 - 1)) {
                LOG_WARNING(Frontend, "VMA at 0x{:08X} size 0x{:X} not aligned, skipping", vma.base,
                            vma.size);
                continue;
            }
            desc.select = ~(np2 - 1);
        }

        descs.push_back(desc);
    }

    if (!descs.empty()) {
        retro_memory_map map = {descs.data(), static_cast<unsigned>(descs.size())};
        LibRetro::SetMemoryMaps(&map);
    }
}

static bool do_load_game() {
    const Core::System::ResultStatus load_result{
        Core::System::GetInstance().Load(*emu_instance->emu_window, LibRetro::settings.file_path)};

    switch (load_result) {
    case Core::System::ResultStatus::Success:
        break; // Expected case
    case Core::System::ResultStatus::ErrorGetLoader:
        LibRetro::DisplayMessage("Failed to obtain loader for specified ROM!");
        return false;
    case Core::System::ResultStatus::ErrorLoader:
        LibRetro::DisplayMessage("Failed to load ROM!");
        return false;
    case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted:
        LibRetro::DisplayMessage("The game that you are trying to load must be decrypted before "
                                 "being used with Azahar.");
        return false;
    case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
        LibRetro::DisplayMessage("Error while loading ROM: The ROM format is not supported.");
        return false;
    case Core::System::ResultStatus::ErrorLoader_ErrorGbaTitle:
        LibRetro::DisplayMessage(
            "Error loading the specified application as it is GBA Virtual Console");
        return false;
    case Core::System::ResultStatus::ErrorNotInitialized:
        LibRetro::DisplayMessage("CPUCore not initialized");
        return false;
    case Core::System::ResultStatus::ErrorSystemMode:
        LibRetro::DisplayMessage("Failed to determine system mode!");
        return false;
    default:
        LibRetro::DisplayMessage(
            ("Unknown error: " + std::to_string(static_cast<int>(load_result))).c_str());
        return false;
    }

    u64 program_id{};
    Core::System::GetInstance().GetAppLoader().ReadProgramId(program_id);
    Core::System::GetInstance().GPU().ApplyPerProgramSettings(program_id);

    if (Settings::values.use_disk_shader_cache) {
        struct ProgressState {
            std::mutex mutex;
            int last_stage = -1;
            int last_percent = -2;
            std::string last_object;
        };
        const auto progress_state = std::make_shared<ProgressState>();
        const VideoCore::DiskResourceLoadCallback progress_callback =
            [progress_state](VideoCore::LoadCallbackStage stage, std::size_t value,
                             std::size_t total, const std::string& object) {
                int percent = -1;
                if (total != 0) {
                    percent = static_cast<int>(std::min<std::size_t>(100, value * 100 / total));
                }

                const int stage_number = static_cast<int>(stage);
                {
                    std::lock_guard lock(progress_state->mutex);
                    if (stage_number == progress_state->last_stage &&
                        percent == progress_state->last_percent &&
                        object == progress_state->last_object && value != total) {
                        return;
                    }
                    progress_state->last_stage = stage_number;
                    progress_state->last_percent = percent;
                    progress_state->last_object = object;
                }

                std::string detail;
                switch (stage) {
                case VideoCore::LoadCallbackStage::Prepare:
                    detail = "Preparing graphics caches";
                    break;
                case VideoCore::LoadCallbackStage::Preload:
                    detail = "Preloading textures";
                    break;
                case VideoCore::LoadCallbackStage::Decompile:
                    detail = "Preparing shaders";
                    break;
                case VideoCore::LoadCallbackStage::Build:
                    detail = object.empty() ? "Loading graphics cache" : "Loading " + object;
                    break;
                case VideoCore::LoadCallbackStage::Complete:
                    detail = "Graphics cache ready";
                    percent = 100;
                    break;
                }
                if (total != 0) {
                    detail += " " + std::to_string(value) + " / " + std::to_string(total);
                }
                const std::string message = "AZAHAR_CACHE_PROGRESS|" +
                                            std::to_string(percent) + "|" + detail;
                LibRetro::DisplayMessage(message.c_str());
            };
        Core::System::GetInstance().GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(
            false, progress_callback);
    }

    setup_memory_maps();

    return true;
}

#ifdef ENABLE_OPENGL
static void* load_opengl_func(const char* name) {
    return (void*)emu_instance->hw_render.get_proc_address(name);
}
#endif

static void context_reset() {
    LOG_DEBUG(Frontend, "context_reset");

    switch (Settings::values.graphics_api.GetValue()) {
#ifdef ENABLE_OPENGL
    case Settings::GraphicsAPI::OpenGL:
#if defined(USING_GLES)
        Settings::values.use_gles = true;
        // Set the global GLES flag immediately to ensure any shader compilation
        // that happens before the Driver is created uses the correct version
        OpenGL::GLES = true;
#else
        Settings::values.use_gles = false;
        OpenGL::GLES = false;
#endif
        // Check to see if the frontend provides us with OpenGL symbols
        if (emu_instance->hw_render.get_proc_address != nullptr) {
            bool loaded = Settings::values.use_gles
                              ? gladLoadGLES2Loader((GLADloadproc)load_opengl_func)
                              : gladLoadGLLoader((GLADloadproc)load_opengl_func);

            if (!loaded) {
                LOG_CRITICAL(Frontend, "Glad failed to load (frontend-provided symbols)!");
                return;
            }
        } else {
            // Else, try to load them on our own
            if (!gladLoadGL()) {
                LOG_CRITICAL(Frontend, "Glad failed to load (internal symbols)!");
                return;
            }
        }
        break;
#endif
#ifdef ENABLE_VULKAN
    case Settings::GraphicsAPI::Vulkan:
        LibRetro::VulkanResetContext();
        break;
#endif
    default:
        // software renderer never gets here
        break;
    }

    emu_instance->emu_window->CreateContext();

    if (!emu_instance->game_loaded) {
        emu_instance->game_loaded = do_load_game();
    } else {
        // Game is already loaded, just recreate the renderer for the new GL context
        if (Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::OpenGL) {
            Core::System::GetInstance().GPU().RecreateRenderer(*emu_instance->emu_window, nullptr);
        }
    }
}

static void context_destroy() {
    LOG_DEBUG(Frontend, "context_destroy");
    if (emu_instance->game_loaded &&
        Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::OpenGL) {
        // Release the renderer's OpenGL resources
        Core::System::GetInstance().GPU().ReleaseRenderer();
    }
    emu_instance->emu_window->DestroyContext();
}

void retro_reset() {
    LOG_DEBUG(Frontend, "retro_reset");
    Core::System::GetInstance().Shutdown();
    emu_instance->game_loaded = do_load_game();
}

/**
 * libretro callback; Called when a game is to be loaded.
 */
bool retro_load_game(const struct retro_game_info* info) {
    LOG_INFO(Frontend, "Starting Azahar RetroArch game...");

#if CITRA_ARCH(x86_64) && CITRA_HAS_SSE42
    if (!Common::GetCPUCaps().sse4_2) {
        LOG_CRITICAL(Frontend, "This CPU does not support SSE4.2, which is required by this build");
        LibRetro::DisplayMessage(
            "This CPU does not support SSE4.2, which is required by this build");
        return false;
    }
#endif

    UpdateSettings();

    // If using HW rendering, don't actually load the game here. azahar wants
    // the graphics context ready and available before calling System::Load.
    LibRetro::settings.file_path = info->path;

    // Early validation: check that the ROM can be loaded before committing to
    // the HW renderer setup. Without this, failures (encrypted ROMs, bad files)
    // are only detected in context_reset after retro_load_game already returned
    // true, leaving the frontend stuck on a black screen.
    // GetLoader + LoadKernelMemoryMode only read ROM headers — no renderer needed.
    {
        auto loader = Loader::GetLoader(LibRetro::settings.file_path);
        if (!loader) {
            LibRetro::DisplayMessage("Failed to obtain loader for the specified ROM.");
            return false;
        }
        auto [memory_mode, result] = loader->LoadKernelMemoryMode();
        if (result != Loader::ResultStatus::Success) {
            switch (result) {
            case Loader::ResultStatus::ErrorEncrypted:
                LibRetro::DisplayMessage(
                    "This ROM is encrypted and must be decrypted before use with Azahar.");
                break;
            case Loader::ResultStatus::ErrorInvalidFormat:
                LibRetro::DisplayMessage("The ROM format is not supported.");
                break;
            case Loader::ResultStatus::ErrorGbaTitle:
                LibRetro::DisplayMessage("GBA Virtual Console titles are not supported.");
                break;
            default:
                LibRetro::DisplayMessage("Failed to load ROM metadata.");
                break;
            }
            return false;
        }
        // Stash the loader so System::Load can reuse it instead of re-opening
        Core::System::GetInstance().RegisterAppLoaderEarly(loader);
    }

    if (!LibRetro::SetPixelFormat(RETRO_PIXEL_FORMAT_XRGB8888)) {
        LibRetro::DisplayMessage("XRGB8888 is not supported.");
        return false;
    }

    emu_instance->emu_window->UpdateLayout();

    switch (Settings::values.graphics_api.GetValue()) {
    case Settings::GraphicsAPI::OpenGL: {
#ifdef ENABLE_OPENGL
        LOG_INFO(Frontend, "Using OpenGL hw renderer");
        LibRetro::SetHWSharedContext();
#if defined(USING_GLES)
        emu_instance->hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
        emu_instance->hw_render.version_major = 3;
        emu_instance->hw_render.version_minor = 2;
#else
        emu_instance->hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
        emu_instance->hw_render.version_major = 4;
        emu_instance->hw_render.version_minor = 3;
#endif
        emu_instance->hw_render.context_reset = context_reset;
        emu_instance->hw_render.context_destroy = context_destroy;
        emu_instance->hw_render.cache_context = false;
        emu_instance->hw_render.bottom_left_origin = true;
        if (!LibRetro::SetHWRenderer(&emu_instance->hw_render)) {
            LibRetro::DisplayMessage("Failed to set HW renderer");
            return false;
        }
        LibRetro::SetFramebufferCallback(emu_instance->hw_render.get_current_framebuffer);
#endif
        break;
    }
    case Settings::GraphicsAPI::Vulkan: {
        // These braces are required (not only for consistency): vk_negotiation
        // below is declared with an initializer, so without an explicit scope
        // the following case label would jump past that initialization. MSVC
        // rejects that as error C2360 under /permissive- /WX.
#ifdef ENABLE_VULKAN
        LOG_INFO(Frontend, "Using Vulkan hw renderer");
        emu_instance->hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
        emu_instance->hw_render.version_major = VK_MAKE_VERSION(1, 1, 0);
        emu_instance->hw_render.version_minor = 0;
        emu_instance->hw_render.context_reset = context_reset;
        emu_instance->hw_render.context_destroy = context_destroy;
        emu_instance->hw_render.cache_context = true;
        if (!LibRetro::SetHWRenderer(&emu_instance->hw_render)) {
            LibRetro::DisplayMessage("Failed to set HW renderer");
            return false;
        }

        // Set up Vulkan context negotiation interface
        static const struct retro_hw_render_context_negotiation_interface_vulkan vk_negotiation = {
            RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
            RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
            LibRetro::GetVulkanApplicationInfo,
            LibRetro::CreateVulkanDevice,
            nullptr, // destroy_device - not needed (frontend owns the device)
        };
        LibRetro::SetHWRenderContextNegotiationInterface((void**)&vk_negotiation);
#endif
        break;
    }
    case Settings::GraphicsAPI::Software: {
        emu_instance->emu_window->CreateContext();
        emu_instance->game_loaded = do_load_game();
        if (!emu_instance->game_loaded)
            return false;
        break;
    }
    }

    uint64_t quirks =
        RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE | RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE;
    LibRetro::SetSerializationQuirks(quirks);

    return true;
}

void retro_unload_game() {
    LOG_DEBUG(Frontend, "Unloading game...");
    if (auto* rooms = Tamariba::Rooms()) {
        rooms->Leave();
    }
    Core::System::GetInstance().Shutdown();
}

unsigned retro_get_region() {
    return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info,
                             size_t num_info) {
    return retro_load_game(info);
}

/// Drain any pending async kernel operations by running the emulation loop.
///
/// Savestates are unsafe to create while RunAsync operations (file I/O, network, etc.)
/// are in flight. The Qt frontend handles this by deferring serialization inside
/// System::RunLoop(): it sets a request flag via SendSignal(Signal::Save), and RunLoop
/// only performs the save when !kernel->AreAsyncOperationsPending() (see core.cpp).
///
/// The Qt frontend needs that indirection because its UI and emulation run on separate
/// threads. In libretro, the frontend calls API entry points (retro_run, retro_serialize,
/// etc.) sequentially, so we can call RunLoop() directly from here to drain pending ops,
/// then call SaveStateBuffer()/LoadStateBuffer() ourselves.
///
/// Note: RunLoop() can itself start new async operations (CPU executes HLE service calls),
/// so the pending count may not decrease monotonically. In practice games reach quiescent
/// points between frames; the 5-second timeout (matching RunLoop's existing handler)
/// covers the pathological case.
static bool DrainAsyncOperations(Core::System& system) {
    if (!system.KernelRunning() || !system.Kernel().AreAsyncOperationsPending()) {
        return true;
    }

    emu_instance->emu_window->suppressPresentation = true;
    auto start = std::chrono::steady_clock::now();

    while (system.Kernel().AreAsyncOperationsPending()) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            LOG_ERROR(Frontend, "Timed out waiting for async operations to complete");
            emu_instance->emu_window->suppressPresentation = false;
            return false;
        }
        auto result = system.RunLoop();
        if (result != Core::System::ResultStatus::Success) {
            emu_instance->emu_window->suppressPresentation = false;
            return false;
        }
    }

    emu_instance->emu_window->suppressPresentation = false;
    return true;
}

std::optional<std::vector<u8>> savestate = {};

size_t retro_serialize_size() {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn())
        return 0;

    if (!DrainAsyncOperations(system)) {
        savestate.reset();
        return 0;
    }

    try {
        savestate = system.SaveStateBuffer();
        return savestate->size();
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "Error saving state: {}", e.what());
        savestate.reset();
        return 0;
    }
}

bool retro_serialize(void* data, size_t size) {
    if (!savestate.has_value())
        return false;
    if (size < savestate->size())
        return false;
    memcpy(data, savestate->data(), savestate->size());
    savestate.reset();
    return true;
}

bool retro_unserialize(const void* data, size_t size) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn())
        return false;

    if (!DrainAsyncOperations(system)) {
        return false;
    }

    std::vector<u8> buffer(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
    try {
        return system.LoadStateBuffer(std::move(buffer));
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "Error loading state: {}", e.what());
        return false;
    }
}

void* retro_get_memory_data(unsigned id) {
    // Memory is exposed via RETRO_ENVIRONMENT_SET_MEMORY_MAPS instead,
    // using virtual addresses for stable cheat/achievement support.
    return NULL;
}

size_t retro_get_memory_size(unsigned id) {
    return 0;
}

void retro_cheat_reset() {}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {}
