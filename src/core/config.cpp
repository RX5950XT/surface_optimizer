#include "core/config.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cwctype>

namespace surface_optimizer {

namespace {

std::wstring trim(const std::wstring& str) {
    if (str.empty()) return L"";
    size_t first = 0;
    while (first < str.size() && (str[first] == L' ' || str[first] == L'\t' || str[first] == L'\r' || str[first] == L'\n')) {
        ++first;
    }
    if (first >= str.size()) return L"";
    size_t last = str.size() - 1;
    while (last > first && (str[last] == L' ' || str[last] == L'\t' || str[last] == L'\r' || str[last] == L'\n')) {
        --last;
    }
    return str.substr(first, last - first + 1);
}

std::wstring strip_quotes(const std::wstring& str) {
    if (str.size() >= 2 && str.front() == L'"' && str.back() == L'"') {
        return str.substr(1, str.size() - 2);
    }
    return str;
}

std::wstring to_lower(std::wstring str) {
    std::transform(str.begin(), str.end(), str.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return str;
}

bool parse_bool(const std::wstring& val, bool default_val) {
    std::wstring lower = to_lower(trim(val));
    if (lower == L"true" || lower == L"1" || lower == L"yes" || lower == L"on") return true;
    if (lower == L"false" || lower == L"0" || lower == L"no" || lower == L"off") return false;
    return default_val;
}

uint32_t parse_uint32(const std::wstring& val, uint32_t default_val) {
    try {
        return static_cast<uint32_t>(std::stoul(trim(val)));
    } catch (...) {
        return default_val;
    }
}

double parse_double(const std::wstring& val, double default_val) {
    try {
        return std::stod(trim(val));
    } catch (...) {
        return default_val;
    }
}

std::vector<std::wstring> parse_list(const std::wstring& val) {
    std::vector<std::wstring> result;
    std::wstring content = trim(val);
    if (!content.empty() && content.front() == L'[' && content.back() == L']') {
        content = content.substr(1, content.length() - 2);
    }
    std::wstringstream ss(content);
    std::wstring item;
    while (std::getline(ss, item, L',')) {
        std::wstring cleaned = trim(item);
        if (!cleaned.empty() && cleaned.front() == L'\"' && cleaned.back() == L'\"') {
            cleaned = cleaned.substr(1, cleaned.length() - 2);
        }
        if (!cleaned.empty()) {
            result.push_back(cleaned);
        }
    }
    return result;
}

} // anonymous namespace

Config& Config::get_instance() {
    static Config instance;
    return instance;
}

Config::Config() {
    reset_to_defaults();
}

void Config::reset_to_defaults() {
    power = PowerConfig{};
    memory = MemoryConfig{};
    governor = ProcessGovernorConfig{};
    daemon = DaemonConfig{};
}

bool Config::load_from_file(const std::wstring& file_path) {
    std::wifstream file(file_path.c_str());
    if (!file.is_open()) {
        return false;
    }

    std::wstring line;
    std::wstring current_section = L"";

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';') {
            continue;
        }

        if (line.front() == L'[' && line.back() == L']') {
            current_section = to_lower(line.substr(1, line.length() - 2));
            continue;
        }

        auto eq_pos = line.find(L'=');
        if (eq_pos == std::wstring::npos) {
            continue;
        }

        std::wstring key = to_lower(trim(line.substr(0, eq_pos)));
        std::wstring val = trim(line.substr(eq_pos + 1));

        if (current_section == L"power") {
            if (key == L"ac_epp_idle") power.ac_epp_idle = parse_uint32(val, power.ac_epp_idle);
            else if (key == L"ac_epp_active") power.ac_epp_active = parse_uint32(val, power.ac_epp_active);
            else if (key == L"dc_epp_idle") power.dc_epp_idle = parse_uint32(val, power.dc_epp_idle);
            else if (key == L"dc_epp_active") power.dc_epp_active = parse_uint32(val, power.dc_epp_active);
            else if (key == L"battery_saver_epp") power.battery_saver_epp = parse_uint32(val, power.battery_saver_epp);
            else if (key == L"battery_saver_threshold_percent") power.battery_saver_threshold_percent = parse_uint32(val, power.battery_saver_threshold_percent);
            else if (key == L"ac_boost_mode") power.ac_boost_mode = parse_uint32(val, power.ac_boost_mode);
            else if (key == L"dc_boost_mode") power.dc_boost_mode = parse_uint32(val, power.dc_boost_mode);
            else if (key == L"fast_ramp_duration_ms") power.fast_ramp_duration_ms = parse_uint32(val, power.fast_ramp_duration_ms);
            else if (key == L"busy_poll_interval_ms") power.busy_poll_interval_ms = parse_uint32(val, power.busy_poll_interval_ms);
            else if (key == L"idle_poll_interval_ms") power.idle_poll_interval_ms = parse_uint32(val, power.idle_poll_interval_ms);
            else if (key == L"idle_hysteresis_ms") power.idle_hysteresis_ms = parse_uint32(val, power.idle_hysteresis_ms);
            else if (key == L"last_input_hold_ms") power.last_input_hold_ms = parse_uint32(val, power.last_input_hold_ms);
            else if (key == L"boost_grace_ms") power.boost_grace_ms = parse_uint32(val, power.boost_grace_ms);
            else if (key == L"busy_cpu_percent_threshold") power.busy_cpu_percent_threshold = parse_double(val, power.busy_cpu_percent_threshold);
            else if (key == L"busy_gpu_percent_threshold") power.busy_gpu_percent_threshold = parse_double(val, power.busy_gpu_percent_threshold);
        } else if (current_section == L"memory") {
            if (key == L"pressure_threshold_percent") memory.pressure_threshold_percent = parse_uint32(val, memory.pressure_threshold_percent);
            else if (key == L"trim_interval_seconds") memory.trim_interval_seconds = parse_uint32(val, memory.trim_interval_seconds);
            else if (key == L"enable_standby_purge") memory.enable_standby_purge = parse_bool(val, memory.enable_standby_purge);
            else if (key == L"trim_idle_processes_on_ac") memory.trim_idle_processes_on_ac = parse_bool(val, memory.trim_idle_processes_on_ac);
            else if (key == L"trim_idle_processes_on_dc") memory.trim_idle_processes_on_dc = parse_bool(val, memory.trim_idle_processes_on_dc);
        } else if (current_section == L"governor") {
            if (key == L"cpu_hog_threshold_percent") governor.cpu_hog_threshold_percent = parse_double(val, governor.cpu_hog_threshold_percent);
            else if (key == L"cpu_hog_sustain_seconds") governor.cpu_hog_sustain_seconds = parse_uint32(val, governor.cpu_hog_sustain_seconds);
            else if (key == L"enable_eco_qos") governor.enable_eco_qos = parse_bool(val, governor.enable_eco_qos);
            else if (key == L"enable_priority_demotion") governor.enable_priority_demotion = parse_bool(val, governor.enable_priority_demotion);
            else if (key == L"allowlist") {
                auto items = parse_list(val);
                if (!items.empty()) governor.allowlist = items;
            }
        } else if (current_section == L"daemon") {
            if (key == L"housekeeping_interval_ac_ms") daemon.housekeeping_interval_ac_ms = parse_uint32(val, daemon.housekeeping_interval_ac_ms);
            else if (key == L"housekeeping_interval_dc_ms") daemon.housekeeping_interval_dc_ms = parse_uint32(val, daemon.housekeeping_interval_dc_ms);
            else if (key == L"housekeeping_interval_idle_ms") daemon.housekeeping_interval_idle_ms = parse_uint32(val, daemon.housekeeping_interval_idle_ms);
            else if (key == L"log_level") daemon.log_level = strip_quotes(val);
            else if (key == L"log_file_path") daemon.log_file_path = strip_quotes(val);
            else if (key == L"log_to_console") daemon.log_to_console = parse_bool(val, daemon.log_to_console);
            else if (key == L"log_to_file") daemon.log_to_file = parse_bool(val, daemon.log_to_file);
        }
    }

    return true;
}

bool Config::save_to_file(const std::wstring& file_path) const {
    std::wofstream file(file_path.c_str());
    if (!file.is_open()) {
        return false;
    }

    file << L"# Surface Pro 7 Optimizer Configuration\n\n";

    file << L"[power]\n";
    file << L"ac_epp_idle = " << power.ac_epp_idle << L"\n";
    file << L"ac_epp_active = " << power.ac_epp_active << L"\n";
    file << L"dc_epp_idle = " << power.dc_epp_idle << L"\n";
    file << L"dc_epp_active = " << power.dc_epp_active << L"\n";
    file << L"battery_saver_epp = " << power.battery_saver_epp << L"\n";
    file << L"battery_saver_threshold_percent = " << power.battery_saver_threshold_percent << L"\n";
    file << L"ac_boost_mode = " << power.ac_boost_mode << L"\n";
    file << L"dc_boost_mode = " << power.dc_boost_mode << L"\n";
    file << L"fast_ramp_duration_ms = " << power.fast_ramp_duration_ms << L"\n";
    file << L"busy_poll_interval_ms = " << power.busy_poll_interval_ms << L"\n";
    file << L"idle_poll_interval_ms = " << power.idle_poll_interval_ms << L"\n";
    file << L"idle_hysteresis_ms = " << power.idle_hysteresis_ms << L"\n";
    file << L"last_input_hold_ms = " << power.last_input_hold_ms << L"\n";
    file << L"boost_grace_ms = " << power.boost_grace_ms << L"\n";
    file << L"busy_cpu_percent_threshold = " << power.busy_cpu_percent_threshold << L"\n";
    file << L"busy_gpu_percent_threshold = " << power.busy_gpu_percent_threshold << L"\n\n";

    file << L"[memory]\n";
    file << L"pressure_threshold_percent = " << memory.pressure_threshold_percent << L"\n";
    file << L"trim_interval_seconds = " << memory.trim_interval_seconds << L"\n";
    file << L"enable_standby_purge = " << (memory.enable_standby_purge ? L"true" : L"false") << L"\n";
    file << L"trim_idle_processes_on_ac = " << (memory.trim_idle_processes_on_ac ? L"true" : L"false") << L"\n";
    file << L"trim_idle_processes_on_dc = " << (memory.trim_idle_processes_on_dc ? L"true" : L"false") << L"\n\n";

    file << L"[governor]\n";
    file << L"cpu_hog_threshold_percent = " << governor.cpu_hog_threshold_percent << L"\n";
    file << L"cpu_hog_sustain_seconds = " << governor.cpu_hog_sustain_seconds << L"\n";
    file << L"enable_eco_qos = " << (governor.enable_eco_qos ? L"true" : L"false") << L"\n";
    file << L"enable_priority_demotion = " << (governor.enable_priority_demotion ? L"true" : L"false") << L"\n";
    file << L"allowlist = [";
    for (size_t i = 0; i < governor.allowlist.size(); ++i) {
        file << L"\"" << governor.allowlist[i] << L"\"";
        if (i + 1 < governor.allowlist.size()) file << L", ";
    }
    file << L"]\n\n";

    file << L"[daemon]\n";
    file << L"housekeeping_interval_ac_ms = " << daemon.housekeeping_interval_ac_ms << L"\n";
    file << L"housekeeping_interval_dc_ms = " << daemon.housekeeping_interval_dc_ms << L"\n";
    file << L"housekeeping_interval_idle_ms = " << daemon.housekeeping_interval_idle_ms << L"\n";
    file << L"log_level = \"" << daemon.log_level << L"\"\n";
    file << L"log_file_path = \"" << daemon.log_file_path << L"\"\n";
    file << L"log_to_console = " << (daemon.log_to_console ? L"true" : L"false") << L"\n";
    file << L"log_to_file = " << (daemon.log_to_file ? L"true" : L"false") << L"\n";

    return true;
}

} // namespace surface_optimizer
