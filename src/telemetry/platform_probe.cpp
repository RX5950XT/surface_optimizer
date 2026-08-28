#include "telemetry/platform_probe.hpp"
#include "core/utils.hpp"
#include <powrprof.h>
#include <intrin.h>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace surface_optimizer {

namespace {

const GUID kSubProcessor =
    { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
const GUID kEpp =
    { 0x36687f9e, 0xe3a5, 0x4dbf, { 0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63 } };
const GUID kBoost =
    { 0xbe337238, 0x0d82, 0x4146, { 0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7 } };
const GUID kMin =
    { 0x893dee8e, 0x2bef, 0x41e0, { 0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c } };
const GUID kMax =
    { 0xbc5038f7, 0x23e0, 0x4960, { 0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec } };
const GUID kCpMin =
    { 0x0cc5b647, 0xc1df, 0x4637, { 0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83 } };
const GUID kCpMax =
    { 0xea062031, 0x0e34, 0x4ff1, { 0x9b, 0x6d, 0xeb, 0x10, 0x59, 0x33, 0x40, 0x28 } };
const GUID kSoftPark =
    { 0x97cfac41, 0x2217, 0x47eb, { 0x99, 0x2d, 0x61, 0x8b, 0x19, 0x77, 0xc9, 0x07 } };
const GUID kSmtUnpark =
    { 0xb28a6829, 0xc5f7, 0x444e, { 0x8f, 0x61, 0x10, 0xe2, 0x4e, 0x85, 0xc5, 0x32 } };
const GUID kAutonomous =
    { 0x8baa4a8a, 0x14c6, 0x4451, { 0x8e, 0x8b, 0x14, 0xbd, 0xbd, 0x19, 0x75, 0x37 } };
const GUID kAutoWindow =
    { 0xcfeda3d0, 0x7697, 0x4566, { 0xa9, 0x22, 0xa9, 0x08, 0x6c, 0xd4, 0x9d, 0xfa } };
const GUID kPciExpress =
    { 0x501a4d13, 0x42af, 0x4429, { 0x9f, 0xd1, 0xa8, 0x21, 0x8c, 0x26, 0x8e, 0x20 } };
const GUID kAspm =
    { 0xee12f906, 0xd277, 0x404b, { 0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5 } };

struct ProcessorPowerInformation {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

#ifndef CPU_SET_INFORMATION_TYPE_DEFINED_LOCAL
struct LocalCpuSetInformation {
    ULONG Size;
    ULONG Type;
    struct {
        ULONG Id;
        USHORT Group;
        UCHAR LogicalProcessorIndex;
        UCHAR CoreIndex;
        UCHAR LastLevelCacheIndex;
        UCHAR NumaNodeIndex;
        UCHAR EfficiencyClass;
        UCHAR AllFlags;
        UCHAR SchedulingClass;
        UCHAR ReservedPad[2];
        ULONG Reserved;
        ULONG64 AllocationTag;
    } CpuSet;
};
#endif

typedef BOOL (WINAPI *GetSystemCpuSetInformationFn)(
    void* Information,
    ULONG BufferLength,
    PULONG ReturnedLength,
    HANDLE Process,
    ULONG Flags
);

bool read_index_sub(GUID* scheme, const GUID& subgroup, const GUID& setting, bool ac, uint32_t& out) {
    DWORD val = 0;
    DWORD res = ac
        ? PowerReadACValueIndex(nullptr, scheme, const_cast<GUID*>(&subgroup), const_cast<GUID*>(&setting), &val)
        : PowerReadDCValueIndex(nullptr, scheme, const_cast<GUID*>(&subgroup), const_cast<GUID*>(&setting), &val);
    if (res != ERROR_SUCCESS) {
        return false;
    }
    out = val;
    return true;
}

bool read_index(GUID* scheme, const GUID& setting, bool ac, uint32_t& out) {
    return read_index_sub(scheme, kSubProcessor, setting, ac, out);
}

CpuidPower read_cpuid() {
    CpuidPower out{};
    int regs[4] = {};

    __cpuid(regs, 0x80000000);
    unsigned max_ext = static_cast<unsigned>(regs[0]);
    if (max_ext >= 0x80000004) {
        char brand[49] = {};
        __cpuid(regs, 0x80000002);
        std::memcpy(brand + 0, regs, 16);
        __cpuid(regs, 0x80000003);
        std::memcpy(brand + 16, regs, 16);
        __cpuid(regs, 0x80000004);
        std::memcpy(brand + 32, regs, 16);
        std::snprintf(out.brand, sizeof(out.brand), "%s", brand);
    }

    __cpuid(regs, 6);
    out.eax06 = static_cast<uint32_t>(regs[0]);
    out.digital_temp_sensor = (regs[0] & (1u << 0)) != 0;
    out.turbo_boost = (regs[0] & (1u << 1)) != 0;
    out.hwp = (regs[0] & (1u << 7)) != 0;
    out.hwp_notification = (regs[0] & (1u << 8)) != 0;
    out.hwp_activity_window = (regs[0] & (1u << 9)) != 0;
    out.hwp_epp = (regs[0] & (1u << 10)) != 0;
    out.hwp_package_request = (regs[0] & (1u << 11)) != 0;
    out.hdc = (regs[0] & (1u << 13)) != 0;
    out.turbo_boost_max_3 = (regs[0] & (1u << 14)) != 0;
    out.peci_override = (regs[0] & (1u << 16)) != 0;

    __cpuidex(regs, 7, 0);
    out.hybrid = (regs[3] & (1u << 15)) != 0;
    return out;
}

} // namespace

PlatformSnapshot PlatformProbe::capture() {
    PlatformSnapshot snap{};
    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    snap.logical = sys.dwNumberOfProcessors;

    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
        std::vector<uint8_t> buf(len);
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(buf.data());
        if (GetLogicalProcessorInformation(info, &len)) {
            DWORD count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
            for (DWORD i = 0; i < count; ++i) {
                if (info[i].Relationship == RelationProcessorCore) {
                    snap.cores++;
                }
            }
        }
    }

    snap.cpuid = read_cpuid();

    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
        snap.ppm.scheme_name = Utils::guid_to_wstring(*scheme);
        bool ok = true;
        ok = read_index(scheme, kEpp, true, snap.ppm.epp_ac) && ok;
        ok = read_index(scheme, kEpp, false, snap.ppm.epp_dc) && ok;
        ok = read_index(scheme, kBoost, true, snap.ppm.boost_ac) && ok;
        ok = read_index(scheme, kBoost, false, snap.ppm.boost_dc) && ok;
        ok = read_index(scheme, kMin, true, snap.ppm.min_ac) && ok;
        ok = read_index(scheme, kMin, false, snap.ppm.min_dc) && ok;
        ok = read_index(scheme, kMax, true, snap.ppm.max_ac) && ok;
        ok = read_index(scheme, kMax, false, snap.ppm.max_dc) && ok;
        ok = read_index(scheme, kCpMin, true, snap.ppm.cpmin_ac) && ok;
        ok = read_index(scheme, kCpMin, false, snap.ppm.cpmin_dc) && ok;
        ok = read_index(scheme, kCpMax, true, snap.ppm.cpmax_ac) && ok;
        ok = read_index(scheme, kCpMax, false, snap.ppm.cpmax_dc) && ok;
        read_index(scheme, kSoftPark, true, snap.ppm.softpark_ac);
        read_index(scheme, kSoftPark, false, snap.ppm.softpark_dc);
        read_index(scheme, kSmtUnpark, true, snap.ppm.smt_unpark_ac);
        read_index(scheme, kSmtUnpark, false, snap.ppm.smt_unpark_dc);
        read_index(scheme, kAutonomous, true, snap.ppm.autonomous_ac);
        read_index(scheme, kAutonomous, false, snap.ppm.autonomous_dc);
        read_index(scheme, kAutoWindow, true, snap.ppm.autonomous_window_ac);
        read_index(scheme, kAutoWindow, false, snap.ppm.autonomous_window_dc);
        read_index_sub(scheme, kPciExpress, kAspm, true, snap.ppm.aspm_ac);
        read_index_sub(scheme, kPciExpress, kAspm, false, snap.ppm.aspm_dc);
        snap.ppm.read_ok = ok;
        LocalFree(scheme);
    }

    auto get_cpu_sets = reinterpret_cast<GetSystemCpuSetInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation")));
    ULONG cpu_set_len = 0;
    if (get_cpu_sets) {
        get_cpu_sets(nullptr, 0, &cpu_set_len, nullptr, 0);
    }
    if (get_cpu_sets && cpu_set_len > 0) {
        std::vector<uint8_t> buf(cpu_set_len);
        if (get_cpu_sets(buf.data(), cpu_set_len, &cpu_set_len, nullptr, 0)) {
            uint8_t* p = buf.data();
            uint8_t* end = buf.data() + cpu_set_len;
            bool first = true;
            while (p + sizeof(ULONG) < end) {
                auto* entry = reinterpret_cast<LocalCpuSetInformation*>(p);
                if (entry->Size == 0) {
                    break;
                }
                CpuSetEntry row{};
                row.id = entry->CpuSet.Id;
                row.logical_index = entry->CpuSet.LogicalProcessorIndex;
                row.core_index = entry->CpuSet.CoreIndex;
                row.efficiency_class = entry->CpuSet.EfficiencyClass;
                row.parked = (entry->CpuSet.AllFlags & 0x1) != 0;
                snap.cpu_sets.push_back(row);
                if (first) {
                    snap.efficiency_class_min = row.efficiency_class;
                    snap.efficiency_class_max = row.efficiency_class;
                    first = false;
                } else {
                    snap.efficiency_class_min = std::min(snap.efficiency_class_min, row.efficiency_class);
                    snap.efficiency_class_max = std::max(snap.efficiency_class_max, row.efficiency_class);
                }
                if (row.parked) {
                    snap.any_parked = true;
                }
                p += entry->Size;
            }
        }
    }
    snap.homogeneous = (snap.efficiency_class_min == snap.efficiency_class_max) && !snap.cpuid.hybrid;

    std::vector<ProcessorPowerInformation> mhz(snap.logical ? snap.logical : 8);
    NTSTATUS st = CallNtPowerInformation(
        ProcessorInformation,
        nullptr,
        0,
        mhz.data(),
        static_cast<ULONG>(mhz.size() * sizeof(ProcessorPowerInformation))
    );
    if (st == 0) {
        for (size_t i = 0; i < mhz.size(); ++i) {
            if (mhz[i].MaxMhz == 0 && mhz[i].CurrentMhz == 0) {
                continue;
            }
            LogicalMhz row{};
            row.number = mhz[i].Number;
            row.current_mhz = mhz[i].CurrentMhz;
            row.max_mhz = mhz[i].MaxMhz;
            row.mhz_limit = mhz[i].MhzLimit;
            snap.mhz.push_back(row);
        }
    }

    snap.rapl_readable = false;
    snap.rapl_note = L"RAPL PL1/PL2/Tau needs a kernel MSR driver; not read. DTT/PEP present on this SKU, usermode WMI empty.";
    return snap;
}

EppApplyLatency PlatformProbe::measure_epp_apply_latency() {
    EppApplyLatency out{};
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) {
        return out;
    }

    DWORD original = 60;
    PowerReadACValueIndex(nullptr, scheme, const_cast<GUID*>(&kSubProcessor), const_cast<GUID*>(&kEpp), &original);
    DWORD target = (original == 0) ? 50 : 0;

    LARGE_INTEGER freq{}, start{}, end{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    DWORD wr = PowerWriteACValueIndex(nullptr, scheme, const_cast<GUID*>(&kSubProcessor), const_cast<GUID*>(&kEpp), target);
    DWORD act = PowerSetActiveScheme(nullptr, scheme);

    DWORD readback = original;
    bool matched = false;
    for (int i = 0; i < 2000; ++i) {
        PowerReadACValueIndex(nullptr, scheme, const_cast<GUID*>(&kSubProcessor), const_cast<GUID*>(&kEpp), &readback);
        if (readback == target) {
            QueryPerformanceCounter(&end);
            matched = true;
            break;
        }
    }

    PowerWriteACValueIndex(nullptr, scheme, const_cast<GUID*>(&kSubProcessor), const_cast<GUID*>(&kEpp), original);
    PowerSetActiveScheme(nullptr, scheme);
    LocalFree(scheme);

    out.ok = (wr == ERROR_SUCCESS && act == ERROR_SUCCESS && matched);
    out.from_epp = original;
    out.to_epp = target;
    if (matched && freq.QuadPart != 0) {
        out.activate_us = static_cast<double>(end.QuadPart - start.QuadPart) * 1000000.0 / static_cast<double>(freq.QuadPart);
    }
    return out;
}

std::wstring PlatformProbe::format_snapshot(const PlatformSnapshot& snap) {
    std::wstringstream ss;
    ss << L"  CPU:              " << Utils::utf8_to_wide(snap.cpuid.brand) << L"\n";
    ss << L"  Topology:         " << snap.cores << L" cores / " << snap.logical << L" threads";
    ss << (snap.homogeneous ? L" (homogeneous, no P/E)" : L" (heterogeneous)") << L"\n";
    ss << L"  EfficiencyClass:  min=" << snap.efficiency_class_min
       << L" max=" << snap.efficiency_class_max << L"\n";
    ss << L"  HWP:              " << (snap.cpuid.hwp ? L"yes" : L"no")
       << L"  EPP=" << (snap.cpuid.hwp_epp ? L"yes" : L"no")
       << L"  ActivityWindow=" << (snap.cpuid.hwp_activity_window ? L"yes" : L"no")
       << L"  PECI override=" << (snap.cpuid.peci_override ? L"yes" : L"no") << L"\n";
    ss << L"  Turbo Boost Max 3.0 / hybrid: "
       << (snap.cpuid.turbo_boost_max_3 ? L"yes" : L"no") << L" / "
       << (snap.cpuid.hybrid ? L"yes" : L"no") << L"\n";
    ss << L"  CPUID.06H EAX:    0x" << std::hex << snap.cpuid.eax06 << std::dec << L"\n";

    if (snap.ppm.read_ok) {
        ss << L"  Scheme:           " << snap.ppm.scheme_name << L"\n";
        ss << L"  PERFEPP:          AC=" << snap.ppm.epp_ac << L"  DC=" << snap.ppm.epp_dc << L"\n";
        ss << L"  PERFBOOSTMODE:    AC=" << snap.ppm.boost_ac << L"  DC=" << snap.ppm.boost_dc << L"\n";
        ss << L"  PROCTHROTTLEMIN:  AC=" << snap.ppm.min_ac << L"%  DC=" << snap.ppm.min_dc << L"%\n";
        ss << L"  PROCTHROTTLEMAX:  AC=" << snap.ppm.max_ac << L"%  DC=" << snap.ppm.max_dc << L"%\n";
        ss << L"  CPMINCORES:       AC=" << snap.ppm.cpmin_ac << L"%  DC=" << snap.ppm.cpmin_dc << L"%\n";
        ss << L"  CPMAXCORES:       AC=" << snap.ppm.cpmax_ac << L"%  DC=" << snap.ppm.cpmax_dc << L"%\n";
        ss << L"  SOFTPARKLATENCY:  AC=" << snap.ppm.softpark_ac << L"  DC=" << snap.ppm.softpark_dc << L"\n";
        ss << L"  SMTUNPARKPOLICY:  AC=" << snap.ppm.smt_unpark_ac << L"  DC=" << snap.ppm.smt_unpark_dc << L"\n";
        ss << L"  PERFAUTONOMOUS:   AC=" << snap.ppm.autonomous_ac << L"  DC=" << snap.ppm.autonomous_dc << L"\n";
        ss << L"  AUTONOMOUSWINDOW: AC=" << snap.ppm.autonomous_window_ac
           << L"  DC=" << snap.ppm.autonomous_window_dc << L" us\n";
        ss << L"  PCIE ASPM:        AC=" << snap.ppm.aspm_ac
           << L"  DC=" << snap.ppm.aspm_dc << L" (0=Off, 2=Max savings)\n";
    }

    if (!snap.cpu_sets.empty()) {
        ss << L"  CpuSets:\n";
        for (const auto& e : snap.cpu_sets) {
            ss << L"    LP" << static_cast<unsigned>(e.logical_index)
               << L" core=" << static_cast<unsigned>(e.core_index)
               << L" eff=" << static_cast<unsigned>(e.efficiency_class)
               << L" parked=" << (e.parked ? L"yes" : L"no")
               << L" id=" << e.id << L"\n";
        }
    }

    if (!snap.mhz.empty()) {
        ss << L"  Current MHz (NtPower; Max is not Turbo 3.90 GHz):\n";
        for (const auto& m : snap.mhz) {
            ss << L"    CPU" << m.number << L": " << m.current_mhz
               << L" / max " << m.max_mhz << L" / limit " << m.mhz_limit << L"\n";
        }
    }

    ss << L"  RAPL:             " << snap.rapl_note << L"\n";
    return ss.str();
}

} // namespace surface_optimizer
