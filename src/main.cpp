#include "core/config.hpp"
#include "core/logger.hpp"
#include "core/service.hpp"
#include "core/utils.hpp"
#include "optimizer/power_manager.hpp"
#include "optimizer/memory_manager.hpp"
#include "optimizer/process_guardian.hpp"
#include "telemetry/platform_probe.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdlib>

namespace surface_optimizer {

void print_header() {
    std::wcout << L"=================================================================\n";
    std::wcout << L" Surface Pro 7 Native Performance & Energy Optimizer (v1.0.0)\n";
    std::wcout << L" Hardware Target: Intel 10th Gen Core i7-1065G7 / Ice Lake\n";
    std::wcout << L"=================================================================\n\n";
}

void print_usage(const wchar_t* exe_name) {
    print_header();
    std::wcout << L"Usage: " << exe_name << L" [OPTION]\n\n";
    std::wcout << L"Service Control Options (Requires Administrator Privileges):\n";
    std::wcout << L"  --install         Install as Windows Service with auto-recovery actions\n";
    std::wcout << L"  --uninstall       Stop and uninstall the Windows Service\n";
    std::wcout << L"  --start           Start the installed Windows Service via SCM\n";
    std::wcout << L"  --stop            Stop the running Windows Service via SCM\n\n";
    std::wcout << L"Daemon Execution Options:\n";
    std::wcout << L"  --daemon          Run in background Windows Service mode (invoked by SCM)\n";
    std::wcout << L"  --interactive, -i Run in foreground console mode with live log output\n\n";
    std::wcout << L"Memory Optimization Options:\n";
    std::wcout << L"  --trim-memory     Trim background process working sets and purge standby cache\n";
    std::wcout << L"  --clean-cache     Alias for --trim-memory\n\n";
    std::wcout << L"Information & Telemetry:\n";
    std::wcout << L"  --status          Query service, live CPU/HWP/parking telemetry (read-only)\n";
    std::wcout << L"  --benchmark       Run governor verification plus EPP apply-latency measurement\n";
    std::wcout << L"  --version, -v     Display program version and build details\n";
    std::wcout << L"  --help, -h        Display this help documentation\n\n";
}

void print_version() {
    std::wcout << L"SurfaceOptimizer v1.0.0 (Native C++20, x86_64, UCRT64 static build)\n";
    std::wcout << L"Built with GNU GCC 14.2.0 with LTO (-O3 -flto -s -static)\n";
    std::wcout << L"Zero external runtime DLL dependencies.\n";
}

void show_status() {
    print_header();
    auto info = Service::get_instance().query_service_info();
    bool is_admin = Utils::is_running_as_admin();
    bool is_system = Utils::is_running_as_system();

    std::wcout << L"[System Privilege Context]\n";
    std::wcout << L"  Current Execution Context: " 
               << (is_system ? L"LocalSystem (NT AUTHORITY\\SYSTEM)" : (is_admin ? L"Elevated Administrator" : L"Standard User"))
               << L"\n\n";

    std::wcout << L"[Windows Service Status]\n";
    std::wcout << L"  Service Name:    " << Service::SERVICE_NAME << L"\n";
    std::wcout << L"  Display Name:    " << Service::SERVICE_DISPLAY_NAME << L"\n";
    std::wcout << L"  Service State:   " << info.status_text << L"\n";
    if (info.pid > 0) {
        std::wcout << L"  Process ID:      " << info.pid << L"\n";
    }
    if (!info.binary_path.empty()) {
        std::wcout << L"  Binary Path:     " << info.binary_path << L"\n";
    }
    std::wcout << L"\n";

    std::wcout << L"[Daemon Runtime State]\n";
    std::wcout << L"  Global Mutex:    " << Service::MUTEX_NAME << L"\n";
    std::wcout << L"  Daemon Running:  " << (info.is_daemon_mutex_held ? L"YES (Active instance holds global mutex)" : L"NO (No active daemon detected)") << L"\n";
    std::wcout << L"\n";

    std::wcout << L"[CPU / HWP / Parking (read-only probe)]\n";
    auto snap = PlatformProbe::capture();
    std::wcout << PlatformProbe::format_snapshot(snap) << L"\n";

    std::wcout << L"[Smart Memory Manager & Cache State]\n";
    auto& mm = MemoryManager::get_instance();
    mm.initialize();
    auto mem_stats = mm.get_memory_stats();
    double total_ram_mb = static_cast<double>(mem_stats.total_phys_bytes) / (1024.0 * 1024.0);
    double avail_ram_mb = static_cast<double>(mem_stats.avail_phys_bytes) / (1024.0 * 1024.0);
    double used_ram_mb = total_ram_mb - avail_ram_mb;

    std::wcout << L"  Memory Load:     " << mem_stats.memory_load_percent << L"%\n";
    std::wcout << L"  Physical RAM:    " << std::fixed << std::setprecision(1)
               << (used_ram_mb / 1024.0) << L" GB used / "
               << (total_ram_mb / 1024.0) << L" GB total ("
               << static_cast<uint64_t>(avail_ram_mb) << L" MB available)\n";
    std::wcout << L"  Pressure Limit:  " << Config::get_instance().memory.pressure_threshold_percent << L"% (Trigger threshold)\n";
    std::wcout << L"  Standby Purge:   auto="
               << (Config::get_instance().memory.enable_standby_purge ? L"ON at RAM>=85%" : L"OFF")
               << L"  (" << ((is_admin || is_system) ? L"elevated NTAPI" : L"unprivileged") << L")\n";
    std::wcout << L"  Stutter Guard:   ACTIVE (Active PID & 10s grace period protected)\n";
    std::wcout << L"\n";

    std::wcout << L"[Process Guardian & Anti-Aging]\n";
    auto& pg = ProcessGuardian::get_instance();
    pg.initialize();
    auto pg_stats = pg.get_last_stats();
    auto throttles = pg.get_active_throttles();
    std::wcout << L"  Processes Scanned:    " << pg_stats.processes_scanned << L"\n";
    std::wcout << L"  CPU Hogs Detected:    " << pg_stats.cpu_hogs_detected << L"\n";
    std::wcout << L"  Mem Leaks Detected:   " << pg_stats.mem_leaks_detected << L"\n";
    std::wcout << L"  Active Throttles:     " << throttles.size() << L"\n";
    std::wcout << L"  HighQoS (foreground): " << pg_stats.highqos_active << L"\n";
    std::wcout << L"  EcoQoS (background):  " << pg_stats.ecoqos_active << L"\n";
    std::wcout << L"  EcoQoS Governor:      ARMED (fg HighQoS, bg EcoQoS)\n";
    std::wcout << L"  OS CPU reserve:       1 physical core (non-system processes kept off it)\n";
    std::wcout << L"\n";
}
} // namespace surface_optimizer

int wmain(int argc, wchar_t* argv[]) {
    using namespace surface_optimizer;

    // Initialize Config with defaults
    auto& config = Config::get_instance();
    std::wstring exe_dir = Utils::get_executable_dir();
    std::wstring config_path = exe_dir + L"\\surface_optimizer.toml";
    config.load_from_file(config_path);

    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    std::wstring arg = argv[1];

    if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
        print_usage(argv[0]);
        return 0;
    }

    if (arg == L"--version" || arg == L"-v") {
        print_version();
        return 0;
    }

    if (arg == L"--status") {
        show_status();
        return 0;
    }

    if (arg == L"--foreground-watch") {
        DWORD parent = (argc >= 3) ? static_cast<DWORD>(wcstoul(argv[2], nullptr, 10)) : 0;
        if (parent == 0) {
            std::wcerr << L"Error: --foreground-watch requires parent PID.\n";
            return 1;
        }
        return Service::run_foreground_watch(parent);
    }

    if (arg == L"--install") {
        if (!Utils::is_running_as_admin()) {
            std::wcerr << L"Error: Installing Windows Service requires elevated Administrator privileges.\n";
            return 1;
        }
        return Service::get_instance().install_service() ? 0 : 1;
    }

    if (arg == L"--uninstall") {
        if (!Utils::is_running_as_admin()) {
            std::wcerr << L"Error: Uninstalling Windows Service requires elevated Administrator privileges.\n";
            return 1;
        }
        return Service::get_instance().uninstall_service() ? 0 : 1;
    }

    if (arg == L"--start") {
        if (!Utils::is_running_as_admin()) {
            std::wcerr << L"Error: Starting Windows Service via SCM requires elevated Administrator privileges.\n";
            return 1;
        }
        return Service::get_instance().start_service() ? 0 : 1;
    }

    if (arg == L"--stop") {
        if (!Utils::is_running_as_admin()) {
            std::wcerr << L"Error: Stopping Windows Service via SCM requires elevated Administrator privileges.\n";
            return 1;
        }
        return Service::get_instance().stop_service() ? 0 : 1;
    }

    if (arg == L"--daemon") {
        // Initialize logger for background service
        Logger::get_instance().initialize(
            Logger::parse_level(config.daemon.log_level),
            config.daemon.log_file_path,
            false, // no console in service
            config.daemon.log_to_file
        );
        return Service::get_instance().run_as_service();
    }

    if (arg == L"--interactive" || arg == L"-i") {
        // Initialize logger for interactive console
        Logger::get_instance().initialize(
            Logger::parse_level(config.daemon.log_level),
            config.daemon.log_file_path,
            true, // console active
            config.daemon.log_to_file
        );
        return Service::get_instance().run_interactive();
    }

    if (arg == L"--trim-memory" || arg == L"--clean-cache") {
        print_header();
        std::wcout << L"Executing on-demand background working set & standby memory optimization...\n\n";
        auto& mm = MemoryManager::get_instance();
        mm.initialize();

        auto before = mm.get_memory_stats();
        std::wcout << L"[Before Optimization]\n";
        std::wcout << L"  Memory Load:     " << before.memory_load_percent << L"%\n";
        std::wcout << L"  Available RAM:   " << (before.avail_phys_bytes / 1024 / 1024) << L" MB\n\n";

        std::wcout << L"Optimizing idle processes and purging standby list...\n";
        auto result = mm.optimize_memory(0, true);

        auto after = mm.get_memory_stats();
        std::wcout << L"\n[Optimization Results]\n";
        std::wcout << L"  Processes Trimmed: " << result.processes_trimmed << L"\n";
        std::wcout << L"  Physical RAM Freed: " << (result.bytes_freed / 1024 / 1024) << L" MB\n";
        std::wcout << L"  Standby Purged:    " << (result.standby_list_purged ? L"YES" : L"NO") << L"\n";
        std::wcout << L"  New Memory Load:   " << after.memory_load_percent << L"%\n";
        std::wcout << L"  New Available RAM: " << (after.avail_phys_bytes / 1024 / 1024) << L" MB\n\n";
        std::wcout << L"Memory optimization completed successfully.\n";
        return 0;
    }

    if (arg == L"--benchmark") {
        print_header();
        std::wcout << L"Running native performance and dynamic governor verification...\n";
        auto& pm = PowerManager::get_instance();
        auto snap = PlatformProbe::capture();
        std::wcout << PlatformProbe::format_snapshot(snap);
        auto lat = PlatformProbe::measure_epp_apply_latency();
        if (lat.ok) {
            std::wcout << L"  [OK] EPP apply latency: " << std::fixed << std::setprecision(1)
                       << lat.activate_us << L" us (" << lat.from_epp << L"% -> " << lat.to_epp << L"%)\n";
        } else {
            std::wcout << L"  [WARN] EPP apply latency measurement failed\n";
        }
        if (pm.initialize()) {
            std::wcout << L"  [OK] Active Scheme: " << pm.get_active_scheme_name() << L"\n";
            std::wcout << L"  [OK] Power Source:  " << (pm.get_current_power_source() == PowerSource::AC ? L"AC" : L"DC") << L"\n";
            std::wcout << L"  [OK] Current EPP:   " << pm.get_current_epp() << L"%\n";
            std::wcout << L"  [OK] Boost Mode:    " << pm.get_current_boost_mode() << L"\n";
            std::wcout << L"  [OK] Fast-ramp:     " << (pm.is_fast_ramp_active() ? L"active" : L"idle")
                       << L" poll=" << pm.desired_poll_interval_ms() << L"ms\n";
            pm.shutdown();
        }
        auto& mm = MemoryManager::get_instance();
        if (mm.initialize()) {
            std::wcout << L"  [OK] Memory Load:   " << mm.get_memory_stats().memory_load_percent << L"%\n";
            auto trim_res = mm.optimize_memory(0, true);
            std::wcout << L"  [OK] WorkingSet Trim: " << trim_res.processes_trimmed << L" processes trimmed ("
                       << (trim_res.bytes_freed / 1024 / 1024) << L" MB freed)\n";
            std::wcout << L"  [OK] Standby Purge: " << (trim_res.standby_list_purged ? L"PURGED" : L"LOW-PRIORITY/FALLBACK") << L"\n";
            mm.shutdown();
        }
        auto& pg = ProcessGuardian::get_instance();
        if (pg.initialize()) {
            auto stats = pg.on_housekeeping(0);
            std::wcout << L"  [OK] ProcessGuardian: " << stats.processes_scanned << L" processes scanned, "
                       << stats.skipped_allowlist << L" allowlisted, "
                       << stats.cpu_hogs_detected << L" CPU hogs, "
                       << stats.throttled_count << L" throttled\n";
            pg.shutdown();
        }
        std::wcout << L"Benchmark verification check completed successfully.\n";
        return 0;
    }

    std::wcerr << L"Unknown option: " << arg << L"\n\n";
    print_usage(argv[0]);
    return 1;
}
