#include "optimizer/burst_policy.hpp"
#include "optimizer/cpu_reserve.hpp"
#include "optimizer/memory_leak_policy.hpp"
#include <iostream>
#include <cstdlib>

using surface_optimizer::BurstHoldInput;
using surface_optimizer::affinity_uses_os_core;
using surface_optimizer::cpu_percent_one_core;
using surface_optimizer::is_memory_growth_suspicious;
using surface_optimizer::should_hold_boost;
using surface_optimizer::should_reserve_os_core;
using surface_optimizer::should_restrict_off_os_core;
using surface_optimizer::system_busy_percent;
using surface_optimizer::user_affinity_mask;

static int g_failed = 0;

static void expect(bool cond, const char* name) {
    if (cond) {
        std::cout << "[PASS] " << name << "\n";
    } else {
        std::cout << "[FAIL] " << name << "\n";
        ++g_failed;
    }
}

int main() {
    expect(cpu_percent_one_core(80000, 1000000) > 7.9 && cpu_percent_one_core(80000, 1000000) < 8.1,
           "8ms CPU in 100ms is ~8% of one core");
    expect(cpu_percent_one_core(0, 1000000) == 0.0, "zero delta is 0%");
    expect(cpu_percent_one_core(1, 0) == 0.0, "zero elapsed does not divide by zero");

    expect(system_busy_percent(800000, 1000000, 0) > 19.0 && system_busy_percent(800000, 1000000, 0) < 21.0,
           "kernel=100 idle=80 user=0 is 20% busy");

    BurstHoldInput in{};
    in.grace_ms = 200;
    in.fallback_hold_ms = 3000;

    in.elapsed_ms = 50;
    expect(should_hold_boost(in), "grace window holds even when idle");

    in.elapsed_ms = 500;
    in.sampled = true;
    in.cpu_busy = false;
    in.io_busy = false;
    in.recent_input = false;
    expect(!should_hold_boost(in), "sampled idle drops after grace");

    in.cpu_busy = true;
    expect(should_hold_boost(in), "sampled CPU busy holds");

    in.cpu_busy = false;
    in.io_busy = true;
    expect(should_hold_boost(in), "sampled IO busy holds");

    in.io_busy = false;
    in.recent_input = true;
    expect(should_hold_boost(in), "recent input holds");

    in.recent_input = false;
    in.sampled = false;
    in.elapsed_ms = 1000;
    expect(should_hold_boost(in), "unsampled uses fallback hold");

    in.elapsed_ms = 4000;
    expect(!should_hold_boost(in), "unsampled drops after fallback");

    in.use_system_busy = true;
    in.system_busy = true;
    expect(should_hold_boost(in), "pid-unknown system busy holds");

    in.sampled = true;
    in.cpu_busy = false;
    in.io_busy = false;
    in.system_busy = true;
    in.use_system_busy = true;
    expect(!should_hold_boost(in), "sampled idle ignores background system busy");

    in.system_busy = false;
    in.use_system_busy = false;
    in.gpu_busy = true;
    expect(should_hold_boost(in), "GPU busy holds even when CPU/IO idle");

    in.gpu_busy = false;
    expect(!should_hold_boost(in), "sampled idle with GPU quiet drops");

    in.sampled = false;
    in.elapsed_ms = 4000;
    in.gpu_busy = true;
    expect(should_hold_boost(in), "GPU busy holds after fallback window");

    expect(should_reserve_os_core(4), "4 physical cores reserve one");
    expect(!should_reserve_os_core(2), "2 physical cores do not reserve");
    expect(user_affinity_mask(0xFF, 0x03) == 0xFC, "8 LP mask drops first SMT pair");
    expect(user_affinity_mask(0x03, 0x03) == 0x03, "do not leave a zero user mask");
    expect(should_restrict_off_os_core(false, false, false), "normal app is restricted");
    expect(!should_restrict_off_os_core(true, false, false), "allowlist stays on all cores");
    expect(!should_restrict_off_os_core(false, true, false), "self stays on all cores");
    expect(!should_restrict_off_os_core(false, false, true), "PID 0/4 stay on all cores");
    expect(affinity_uses_os_core(0xFF, 0x03), "full mask uses OS core");
    expect(!affinity_uses_os_core(0xFC, 0x03), "restricted mask is off OS core");

    constexpr int64_t mib = 1024LL * 1024;
    expect(!is_memory_growth_suspicious(0), "flat working set is not a memory leak");
    expect(!is_memory_growth_suspicious(99 * mib), "99 MB growth is not a memory leak");
    expect(!is_memory_growth_suspicious(100 * mib), "100 MB growth is not a memory leak");
    expect(is_memory_growth_suspicious(101 * mib), "over 100 MB growth is a memory leak");

    if (g_failed != 0) {
        std::cout << g_failed << " failed\n";
        return 1;
    }
    std::cout << "all burst policy tests passed\n";
    return 0;
}
