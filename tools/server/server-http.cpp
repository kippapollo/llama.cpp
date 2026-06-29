#include "common.h"
#include "server-http.h"
#include "server-common.h"

#include <cpp-httplib/httplib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <netpacket/packet.h>
#elif defined(__APPLE__) && defined(__MACH__)
#include <net/if_dl.h>
#endif
#endif

#ifdef LLAMA_BUILD_WEBUI
// auto generated files (see README.md for details)
#include "index.html.gz.hpp"
#include "loading.html.hpp"
#endif

//
// HTTP implementation using cpp-httplib
//

namespace {

struct access_allow_entry {
    std::string alias;
    std::string ip;
    std::unordered_set<std::string> macs;
};

using access_allow_rules = std::unordered_map<std::string, access_allow_entry>;

struct access_allow_file_state {
    std::string path;
    std::shared_ptr<access_allow_rules> rules;
    std::filesystem::file_time_type last_write_time{};
    bool has_last_write_time = false;
    bool timestamp_warning_logged = false;
    std::mutex mutex;
};

static std::string strip_comment(const std::string & line) {
    const auto pos = line.find('#');
    return string_strip(pos == std::string::npos ? line : line.substr(0, pos));
}

static bool normalize_ip_address(const std::string & value, std::string * out) {
    if (out == nullptr) {
        return false;
    }

    const std::string trimmed = string_strip(value);
    if (trimmed.empty()) {
        return false;
    }

    in_addr addr4{};
    if (inet_pton(AF_INET, trimmed.c_str(), &addr4) == 1) {
        char buffer[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &addr4, buffer, sizeof(buffer)) != nullptr) {
            *out = buffer;
            return true;
        }
        return false;
    }

    in6_addr addr6{};
    if (inet_pton(AF_INET6, trimmed.c_str(), &addr6) == 1) {
        if (IN6_IS_ADDR_V4MAPPED(&addr6)) {
            const uint8_t * mapped = addr6.s6_addr + 12;
            char buffer[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, mapped, buffer, sizeof(buffer)) != nullptr) {
                *out = buffer;
                return true;
            }
            return false;
        }

        char buffer[INET6_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET6, &addr6, buffer, sizeof(buffer)) != nullptr) {
            *out = buffer;
            return true;
        }
        return false;
    }

    return false;
}

static bool normalize_mac_address(const std::string & value, std::string * out) {
    if (out == nullptr) {
        return false;
    }

    std::string hex;
    hex.reserve(12);
    for (const unsigned char c : string_strip(value)) {
        if (std::isxdigit(c)) {
            hex.push_back(static_cast<char>(std::tolower(c)));
            continue;
        }
        if (c == ':' || c == '-' || c == '.' || std::isspace(c)) {
            continue;
        }
        return false;
    }

    if (hex.size() != 12) {
        return false;
    }

    std::string normalized;
    normalized.reserve(17);
    for (size_t i = 0; i < 6; ++i) {
        if (i != 0) {
            normalized.push_back(':');
        }
        normalized.push_back(hex[i * 2]);
        normalized.push_back(hex[i * 2 + 1]);
    }

    *out = std::move(normalized);
    return true;
}

static std::string mac_from_bytes(const unsigned char * bytes, size_t length) {
    if (bytes == nullptr || length == 0) {
        return {};
    }

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string mac;
    mac.reserve(length * 3 - 1);
    for (size_t i = 0; i < length; ++i) {
        if (i != 0) {
            mac.push_back(':');
        }
        mac.push_back(hex_digits[bytes[i] >> 4]);
        mac.push_back(hex_digits[bytes[i] & 0x0f]);
    }

    std::string normalized;
    if (!normalize_mac_address(mac, &normalized)) {
        return {};
    }
    return normalized;
}

static std::vector<std::string> split_csv_fields(const std::string & line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;

    while (std::getline(stream, field, ',')) {
        fields.push_back(string_strip(field));
    }

    return fields;
}

static bool parse_access_allow_line(
        const std::string & line,
        access_allow_entry * out,
        std::string * error) {
    if (out == nullptr) {
        if (error) {
            *error = "access allow entry output is null";
        }
        return false;
    }

    *out = {};

    const std::string stripped = strip_comment(line);
    if (stripped.empty()) {
        return false;
    }

    const auto fields = split_csv_fields(stripped);
    if (fields.size() < 3) {
        if (error) {
            *error = "expected alias, ip, mac1[,mac2...]";
        }
        return false;
    }

    out->alias = fields[0];
    if (out->alias.empty()) {
        if (error) {
            *error = "access allow alias cannot be empty";
        }
        return false;
    }

    if (!normalize_ip_address(fields[1], &out->ip)) {
        if (error) {
            *error = "invalid access allow ip address: " + fields[1];
        }
        return false;
    }

    for (size_t i = 2; i < fields.size(); ++i) {
        if (fields[i].empty()) {
            continue;
        }

        std::string mac;
        if (!normalize_mac_address(fields[i], &mac)) {
            if (error) {
                *error = "invalid access allow MAC address: " + fields[i];
            }
            return false;
        }
        out->macs.insert(std::move(mac));
    }

    if (out->macs.empty()) {
        if (error) {
            *error = "access allow entry must contain at least one MAC address";
        }
        return false;
    }

    return true;
}

static bool load_access_allow_file(
        const std::string & path,
        access_allow_rules * out,
        std::string * error) {
    if (out == nullptr) {
        if (error) {
            *error = "access allow rules output is null";
        }
        return false;
    }

    std::ifstream file(path);
    if (!file) {
        if (error) {
            *error = string_format("failed to open access allow file '%s'", path.c_str());
        }
        return false;
    }

    access_allow_rules rules;
    std::string line;
    size_t line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;

        access_allow_entry entry;
        std::string parse_error;
        if (!parse_access_allow_line(line, &entry, &parse_error)) {
            if (!parse_error.empty()) {
                if (error) {
                    *error = string_format("%s (line %zu)", parse_error.c_str(), line_no);
                }
                return false;
            }
            continue;
        }

        if (rules.find(entry.ip) != rules.end()) {
            if (error) {
                *error = string_format("duplicate access allow ip '%s' on line %zu", entry.ip.c_str(), line_no);
            }
            return false;
        }

        rules.emplace(entry.ip, std::move(entry));
    }

    if (rules.empty()) {
        if (error) {
            *error = string_format("access allow file '%s' did not contain any valid entries", path.c_str());
        }
        return false;
    }

    *out = std::move(rules);
    return true;
}

static std::shared_ptr<access_allow_file_state> make_access_allow_file_state(
        const std::string & path,
        std::string * error) {
    access_allow_rules loaded_rules;
    if (!load_access_allow_file(path, &loaded_rules, error)) {
        return {};
    }

    auto state = std::make_shared<access_allow_file_state>();
    state->path = path;
    state->rules = std::make_shared<access_allow_rules>(std::move(loaded_rules));

    std::error_code ec;
    state->last_write_time = std::filesystem::last_write_time(path, ec);
    state->has_last_write_time = !ec;
    if (ec) {
        state->timestamp_warning_logged = true;
        LOG_WRN("%s: unable to read access allow file timestamp '%s': %s\n",
                __func__,
                path.c_str(),
                ec.message().c_str());
    }

    return state;
}

static std::shared_ptr<access_allow_rules> access_allow_rules_snapshot(
        const std::shared_ptr<access_allow_file_state> & state) {
    if (!state || state->path.empty()) {
        return {};
    }

    std::lock_guard<std::mutex> lock(state->mutex);

    std::error_code ec;
    const auto current_write_time = std::filesystem::last_write_time(state->path, ec);
    if (ec) {
        if (!state->timestamp_warning_logged) {
            LOG_WRN("%s: unable to read access allow file timestamp '%s': %s\n",
                    __func__,
                    state->path.c_str(),
                    ec.message().c_str());
            state->timestamp_warning_logged = true;
        }
        return state->rules;
    }

    state->timestamp_warning_logged = false;

    if (!state->has_last_write_time || current_write_time != state->last_write_time) {
        access_allow_rules loaded_rules;
        std::string load_error;
        if (load_access_allow_file(state->path, &loaded_rules, &load_error)) {
            state->rules = std::make_shared<access_allow_rules>(std::move(loaded_rules));
            LOG_INF("%s: reloaded %zu access allow rule(s) from '%s'\n",
                    __func__,
                    state->rules->size(),
                    state->path.c_str());
        } else {
            LOG_WRN("%s: failed to reload access allow file '%s': %s\n",
                    __func__,
                    state->path.c_str(),
                    load_error.c_str());
        }

        state->last_write_time = current_write_time;
        state->has_last_write_time = true;
    }

    return state->rules;
}

static std::string build_query_string_from_params(const httplib::Params & params) {
    std::string qs;
    for (const auto & [key, value] : params) {
        if (!qs.empty()) {
            qs += '&';
        }
        qs += httplib::encode_query_component(key) + "=" + httplib::encode_query_component(value);
    }
    return qs;
}

#if defined(_WIN32)
static bool resolve_local_mac_for_ip(const std::string & ip, std::string * out) {
    std::string normalized_ip;
    if (!normalize_ip_address(ip, &normalized_ip)) {
        return false;
    }

    ULONG buffer_size = 15 * 1024;
    std::vector<unsigned char> buffer(buffer_size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(
        AF_UNSPEC,
        flags,
        nullptr,
        reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),
        &buffer_size);

    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        result = GetAdaptersAddresses(
            AF_UNSPEC,
            flags,
            nullptr,
            reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),
            &buffer_size);
    }

    if (result != NO_ERROR) {
        return false;
    }

    for (auto * adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter != nullptr; adapter = adapter->Next) {
        const std::string mac = mac_from_bytes(adapter->PhysicalAddress, adapter->PhysicalAddressLength);
        if (mac.empty()) {
            continue;
        }

        for (auto * unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr) {
                continue;
            }

            char addr_buf[INET6_ADDRSTRLEN] = {};
            const auto * sa = unicast->Address.lpSockaddr;
            if (sa->sa_family == AF_INET) {
                const auto * sin = reinterpret_cast<const sockaddr_in *>(sa);
                if (inet_ntop(AF_INET, &sin->sin_addr, addr_buf, sizeof(addr_buf)) != nullptr) {
                    std::string addr;
                    if (normalize_ip_address(addr_buf, &addr) && addr == normalized_ip) {
                        *out = mac;
                        return true;
                    }
                }
            } else if (sa->sa_family == AF_INET6) {
                const auto * sin6 = reinterpret_cast<const sockaddr_in6 *>(sa);
                if (inet_ntop(AF_INET6, &sin6->sin6_addr, addr_buf, sizeof(addr_buf)) != nullptr) {
                    std::string addr;
                    if (normalize_ip_address(addr_buf, &addr) && addr == normalized_ip) {
                        *out = mac;
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

static bool resolve_remote_mac_for_ip(const std::string & ip, std::string * out) {
    std::string normalized_ip;
    if (!normalize_ip_address(ip, &normalized_ip)) {
        return false;
    }

    IN_ADDR addr4{};
    if (InetPtonA(AF_INET, normalized_ip.c_str(), &addr4) != 1) {
        return false;
    }

    ULONG mac_addr[2] = {};
    ULONG mac_len = sizeof(mac_addr);
    const DWORD result = SendARP(addr4.S_un.S_addr, 0, mac_addr, &mac_len);
    if (result != NO_ERROR || mac_len == 0) {
        return false;
    }

    const auto * mac_bytes = reinterpret_cast<const unsigned char *>(mac_addr);
    const std::string mac = mac_from_bytes(mac_bytes, std::min<size_t>(mac_len, 6));
    if (mac.empty()) {
        return false;
    }

    *out = mac;
    return true;
}
#else
static bool resolve_local_mac_for_ip(const std::string & ip, std::string * out) {
    std::string normalized_ip;
    if (!normalize_ip_address(ip, &normalized_ip)) {
        return false;
    }

    struct ifaddrs * ifap = nullptr;
    if (getifaddrs(&ifap) != 0 || ifap == nullptr) {
        return false;
    }

    auto free_ifaddrs = [&]() {
        if (ifap != nullptr) {
            freeifaddrs(ifap);
            ifap = nullptr;
        }
    };

    std::unordered_map<std::string, std::string> mac_by_ifname;
    for (auto * ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_name == nullptr) {
            continue;
        }

#if defined(AF_PACKET)
        if (ifa->ifa_addr->sa_family == AF_PACKET) {
            const auto * sll = reinterpret_cast<const sockaddr_ll *>(ifa->ifa_addr);
            const std::string mac = mac_from_bytes(
                reinterpret_cast<const unsigned char *>(sll->sll_addr),
                static_cast<size_t>(sll->sll_halen));
            if (!mac.empty()) {
                mac_by_ifname[ifa->ifa_name] = mac;
            }
            continue;
        }
#endif
#if defined(AF_LINK)
        if (ifa->ifa_addr->sa_family == AF_LINK) {
            const auto * sdl = reinterpret_cast<const sockaddr_dl *>(ifa->ifa_addr);
            const auto * bytes = reinterpret_cast<const unsigned char *>(LLADDR(sdl));
            const std::string mac = mac_from_bytes(bytes, static_cast<size_t>(sdl->sdl_alen));
            if (!mac.empty()) {
                mac_by_ifname[ifa->ifa_name] = mac;
            }
            continue;
        }
#endif
    }

    for (auto * ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_name == nullptr) {
            continue;
        }

        std::string addr;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            char addr_buf[INET_ADDRSTRLEN] = {};
            const auto * sin = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr);
            if (inet_ntop(AF_INET, &sin->sin_addr, addr_buf, sizeof(addr_buf)) != nullptr) {
                if (!normalize_ip_address(addr_buf, &addr)) {
                    continue;
                }
            }
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            char addr_buf[INET6_ADDRSTRLEN] = {};
            const auto * sin6 = reinterpret_cast<const sockaddr_in6 *>(ifa->ifa_addr);
            if (inet_ntop(AF_INET6, &sin6->sin6_addr, addr_buf, sizeof(addr_buf)) != nullptr) {
                if (!normalize_ip_address(addr_buf, &addr)) {
                    continue;
                }
            }
        } else {
            continue;
        }

        if (addr != normalized_ip) {
            continue;
        }

        const auto it = mac_by_ifname.find(ifa->ifa_name);
        if (it != mac_by_ifname.end() && !it->second.empty()) {
            *out = it->second;
            free_ifaddrs();
            return true;
        }
    }

    free_ifaddrs();
    return false;
}

#if defined(__linux__)
static bool resolve_remote_mac_for_ip(const std::string & ip, std::string * out) {
    std::string normalized_ip;
    if (!normalize_ip_address(ip, &normalized_ip)) {
        return false;
    }

    std::ifstream arp_file("/proc/net/arp");
    if (!arp_file) {
        return false;
    }

    std::string line;
    if (!std::getline(arp_file, line)) {
        return false;
    }

    while (std::getline(arp_file, line)) {
        std::istringstream stream(line);
        std::string entry_ip;
        std::string hw_type;
        std::string flags;
        std::string hw_addr;
        std::string mask;
        std::string device;
        if (!(stream >> entry_ip >> hw_type >> flags >> hw_addr >> mask >> device)) {
            continue;
        }

        std::string normalized_entry_ip;
        if (!normalize_ip_address(entry_ip, &normalized_entry_ip) || normalized_entry_ip != normalized_ip) {
            continue;
        }

        std::string normalized_mac;
        if (!normalize_mac_address(hw_addr, &normalized_mac) || normalized_mac == "00:00:00:00:00:00") {
            continue;
        }

        *out = std::move(normalized_mac);
        return true;
    }

    return false;
}
#endif
#endif

static bool resolve_request_mac_address(const std::string & ip, std::string * out) {
    if (resolve_local_mac_for_ip(ip, out)) {
        return true;
    }

#if !defined(_WIN32)
#if defined(__linux__)
    if (resolve_remote_mac_for_ip(ip, out)) {
        return true;
    }
#endif
#else
    if (resolve_remote_mac_for_ip(ip, out)) {
        return true;
    }
#endif

    return false;
}

static bool request_allowed(
        const access_allow_rules & rules,
        const std::string & remote_addr,
        std::string * deny_reason,
        std::string * matched_alias = nullptr) {
    if (rules.empty()) {
        return true;
    }

    std::string normalized_ip;
    if (!normalize_ip_address(remote_addr, &normalized_ip)) {
        if (deny_reason) {
            *deny_reason = string_format("unable to parse remote address '%s'", remote_addr.c_str());
        }
        return false;
    }

    const auto it = rules.find(normalized_ip);
    if (it == rules.end()) {
        if (deny_reason) {
            *deny_reason = string_format("remote address '%s' is not in the access allow file", normalized_ip.c_str());
        }
        return false;
    }

    std::string resolved_mac;
    if (!resolve_request_mac_address(normalized_ip, &resolved_mac)) {
        if (deny_reason) {
            *deny_reason = string_format("unable to resolve MAC address for '%s'", normalized_ip.c_str());
        }
        return false;
    }

    if (it->second.macs.find(resolved_mac) == it->second.macs.end()) {
        if (deny_reason) {
            *deny_reason = string_format(
                "MAC address '%s' is not allowed for IP '%s'",
                resolved_mac.c_str(),
                normalized_ip.c_str());
        }
        return false;
    }

    if (matched_alias) {
        *matched_alias = it->second.alias;
    }
    return true;
}

static bool is_loopback_remote_addr(const std::string & remote_addr) {
    std::string normalized_ip;
    if (!normalize_ip_address(remote_addr, &normalized_ip)) {
        return false;
    }

    return normalized_ip == "127.0.0.1" || normalized_ip == "::1";
}

static bool request_allowed(
        const std::shared_ptr<access_allow_file_state> & state,
        const std::string & remote_addr,
        std::string * deny_reason,
        std::string * matched_alias = nullptr) {
    if (is_loopback_remote_addr(remote_addr)) {
        if (matched_alias) {
            *matched_alias = "loopback";
        }
        return true;
    }

    const auto rules = access_allow_rules_snapshot(state);
    if (!rules || rules->empty()) {
        return true;
    }

    return request_allowed(*rules, remote_addr, deny_reason, matched_alias);
}

static void deny_request(httplib::Response & res, const std::string & message) {
    res.status = 403;
    res.set_content(
        safe_json_to_str(json {
            {"error", format_error_response(message, ERROR_TYPE_PERMISSION)}
        }),
        "application/json; charset=utf-8"
    );
}

static std::tm localtime_safe(std::time_t t) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

static std::string format_local_date(std::time_t t) {
    char buffer[32] = {};
    const std::tm tm = localtime_safe(t);
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
    return buffer;
}

static std::string format_local_timestamp(std::time_t t) {
    char buffer[64] = {};
    const std::tm tm = localtime_safe(t);
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
    return buffer;
}

class server_http_request_logger {
public:
    explicit server_http_request_logger(std::filesystem::path log_dir)
        : log_dir_(std::move(log_dir)) {
        std::error_code ec;
        std::filesystem::create_directories(log_dir_, ec);
        if (ec) {
            throw std::runtime_error(string_format("failed to create log directory '%s': %s",
                                                   log_dir_.string().c_str(),
                                                   ec.message().c_str()));
        }
        const std::time_t now = std::time(nullptr);
        open_locked(format_local_date(now));
    }

    void write(const server_http_req & req, int status, const std::string & response_body) {
        write_impl(req.method, req.path, req.remote_addr, req.query_string, req.body, status, response_body);
    }

    void write(const httplib::Request & req, int status, const std::string & response_body) {
        write_impl(
            req.method,
            req.path,
            req.remote_addr,
            build_query_string_from_params(req.params),
            req.body,
            status,
            response_body);
    }

private:
    void write_impl(
            const std::string & method,
            const std::string & path,
            const std::string & remote_addr,
            const std::string & query_string,
            const std::string & request_body,
            int status,
            const std::string & response_body) {
        if (log_dir_.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        try {
            const std::time_t now = std::time(nullptr);
            const std::string today = format_local_date(now);
            if (today != current_day_) {
                open_locked(today);
            }

            if (!file_.is_open()) {
                return;
            }

            file_ << json {
                {"timestamp", format_local_timestamp(now)},
                {"method", method},
                {"path", path},
                {"remote_addr", remote_addr},
                {"query_string", query_string},
                {"status", status},
                {"request_body", request_body},
                {"response_body", response_body},
            }.dump() << '\n';
            file_.flush();
        } catch (const std::exception & e) {
            LOG_ERR("%s: failed to write request log: %s\n", __func__, e.what());
        }
    }

    void open_locked(const std::string & day) {
        const auto path = log_dir_ / string_format("llama-server-%s.jsonl", day.c_str());
        std::ofstream new_file;
        new_file.open(path, std::ios::out | std::ios::app);
        if (!new_file) {
            throw std::runtime_error(string_format("failed to open log file '%s'", path.string().c_str()));
        }
        file_.close();
        file_ = std::move(new_file);
        current_day_ = day;
    }

    std::filesystem::path log_dir_;
    std::string current_day_;
    std::ofstream file_;
    std::mutex mutex_;
};

} // namespace

class server_http_context::Impl {
public:
    std::unique_ptr<httplib::Server> srv;
    std::shared_ptr<server_http_request_logger> request_logger;
    std::shared_ptr<access_allow_file_state> access_allow_state;
};

server_http_context::server_http_context()
    : pimpl(std::make_unique<server_http_context::Impl>())
{}

server_http_context::~server_http_context() = default;

static void log_server_request(const httplib::Request & req, const httplib::Response & res) {
    // skip logging requests that are regularly sent, to avoid log spam
    if (req.path == "/health"
        || req.path == "/v1/health"
        || req.path == "/models"
        || req.path == "/v1/models"
        || req.path == "/props"
        || req.path == "/metrics"
    ) {
        return;
    }

    // reminder: this function is not covered by httplib's exception handler; if someone does more complicated stuff, think about wrapping it in try-catch

    SRV_INF("done request: %s %s %s %d\n", req.method.c_str(), req.path.c_str(), req.remote_addr.c_str(), res.status);

    SRV_DBG("request:  %s\n", req.body.c_str());
    SRV_DBG("response: %s\n", res.body.c_str());
}

bool server_http_context::init(const common_params & params, const server_http_extra_params & extra_params) {
    path_prefix = params.api_prefix;
    port = params.port;
    hostname = params.hostname;

    auto & srv = pimpl->srv;

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (params.ssl_file_key != "" && params.ssl_file_cert != "") {
        LOG_INF("Running with SSL: key = %s, cert = %s\n", params.ssl_file_key.c_str(), params.ssl_file_cert.c_str());
        srv.reset(
            new httplib::SSLServer(params.ssl_file_cert.c_str(), params.ssl_file_key.c_str())
        );
    } else {
        LOG_INF("Running without SSL\n");
        srv.reset(new httplib::Server());
    }
#else
    if (params.ssl_file_key != "" && params.ssl_file_cert != "") {
        LOG_ERR("Server is built without SSL support\n");
        return false;
    }
    srv.reset(new httplib::Server());
#endif

    if (!extra_params.log_dir.empty()) {
        try {
            pimpl->request_logger = std::make_shared<server_http_request_logger>(extra_params.log_dir);
            LOG_INF("%s: logging request and response bodies to '%s'\n", __func__, extra_params.log_dir.c_str());
        } catch (const std::exception & e) {
            LOG_ERR("%s: %s\n", __func__, e.what());
            return false;
        }
    }

    srv->set_default_headers({{"Server", "llama.cpp"}});
    srv->set_logger([request_logger = pimpl->request_logger](const httplib::Request & req, const httplib::Response & res) {
        log_server_request(req, res);
        if (request_logger && res.content_provider_ == nullptr) {
            request_logger->write(req, res.status, res.body);
        }
    });
    srv->set_exception_handler([](const httplib::Request &, httplib::Response & res, const std::exception_ptr & ep) {
        // this is fail-safe; exceptions should already handled by `ex_wrapper`

        std::string message;
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception & e) {
            message = e.what();
        } catch (...) {
            message = "Unknown Exception";
        }

        res.status = 500;
        res.set_content(message, "text/plain");
        LOG_ERR("got exception: %s\n", message.c_str());
    });

    srv->set_error_handler([](const httplib::Request &, httplib::Response & res) {
        if (res.status == 404) {
            res.set_content(
                safe_json_to_str(json {
                    {"error", {
                        {"message", "File Not Found"},
                        {"type", "not_found_error"},
                        {"code", 404}
                    }}
                }),
                "application/json; charset=utf-8"
            );
        }
        // for other error codes, we skip processing here because it's already done by res->error()
    });

    // set timeouts and change hostname and port
    srv->set_read_timeout (params.timeout_read);
    srv->set_write_timeout(params.timeout_write);
    srv->set_socket_options([reuse_port = params.reuse_port](socket_t sock) {
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
        if (reuse_port) {
#ifdef SO_REUSEPORT
            httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEPORT, 1);
#else
            LOG_WRN("%s: SO_REUSEPORT is not supported\n", __func__);
#endif
        }
    });

    if (params.api_keys.size() == 1) {
        auto key = params.api_keys[0];
        std::string substr = key.substr(std::max((int)(key.length() - 4), 0));
        LOG_INF("%s: api_keys: ****%s\n", __func__, substr.c_str());
    } else if (params.api_keys.size() > 1) {
        LOG_INF("%s: api_keys: %zu keys loaded\n", __func__, params.api_keys.size());
    }

    pimpl->access_allow_state.reset();
    if (!extra_params.access_allow_file.empty()) {
        std::string load_error;
        pimpl->access_allow_state = make_access_allow_file_state(extra_params.access_allow_file, &load_error);
        if (!pimpl->access_allow_state) {
            LOG_ERR("%s: %s\n", __func__, load_error.c_str());
            return false;
        }
        LOG_INF("%s: loaded %zu access allow rule(s) from '%s'\n",
                __func__,
                pimpl->access_allow_state->rules ? pimpl->access_allow_state->rules->size() : 0,
                extra_params.access_allow_file.c_str());
    }

    //
    // Middlewares
    //

    auto middleware_access_allow = [access_allow_state = pimpl->access_allow_state](const httplib::Request & req, httplib::Response & res) {
        if (!access_allow_state) {
            return true;
        }

        std::string deny_reason;
        std::string matched_alias;
        if (request_allowed(access_allow_state, req.remote_addr, &deny_reason, &matched_alias)) {
            if (!matched_alias.empty()) {
                LOG_DBG("Access allowed for alias '%s' from %s\n", matched_alias.c_str(), req.remote_addr.c_str());
            }
            return true;
        }

        deny_request(res, deny_reason.empty() ? "access denied" : deny_reason);
        LOG_WRN("Access denied for %s %s from %s: %s\n",
                req.method.c_str(),
                req.path.c_str(),
                req.remote_addr.c_str(),
                deny_reason.empty() ? "access denied" : deny_reason.c_str());
        return false;
    };

    auto middleware_validate_api_key = [api_keys = params.api_keys](const httplib::Request & req, httplib::Response & res) {
        static const std::unordered_set<std::string> public_endpoints = {
            "/health",
            "/v1/health",
            "/models",
            "/v1/models",
            "/api/tags"
        };

        // If API key is not set, skip validation
        if (api_keys.empty()) {
            return true;
        }

        // If path is public or is static file, skip validation
        if (public_endpoints.find(req.path) != public_endpoints.end() || req.path == "/") {
            return true;
        }

        // Check for API key in the Authorization header
        std::string req_api_key = req.get_header_value("Authorization");
        if (req_api_key.empty()) {
            // retry with anthropic header
            req_api_key = req.get_header_value("X-Api-Key");
        }

        // remove the "Bearer " prefix if needed
        std::string prefix = "Bearer ";
        if (req_api_key.substr(0, prefix.size()) == prefix) {
            req_api_key = req_api_key.substr(prefix.size());
        }

        // validate the API key
        if (std::find(api_keys.begin(), api_keys.end(), req_api_key) != api_keys.end()) {
            return true; // API key is valid
        }

        // API key is invalid or not provided
        res.status = 401;
        res.set_content(
            safe_json_to_str(json {
                {"error", {
                    {"message", "Invalid API Key"},
                    {"type", "authentication_error"},
                    {"code", 401}
                }}
            }),
            "application/json; charset=utf-8"
        );

        LOG_WRN("Unauthorized: Invalid API Key\n");

        return false;
    };

    auto middleware_server_state = [this](const httplib::Request & req, httplib::Response & res) {
        bool ready = is_ready.load();
        if (!ready) {
#ifdef LLAMA_BUILD_WEBUI
            auto tmp = string_split<std::string>(req.path, '.');
            if (req.path == "/" || tmp.back() == "html") {
                res.status = 503;
                res.set_content(reinterpret_cast<const char*>(loading_html), loading_html_len, "text/html; charset=utf-8");
            } else
#endif
            {
                // no endpoints is allowed to be accessed when the server is not ready
                // this is to prevent any data races or inconsistent states
                res.status = 503;
                res.set_content(
                    safe_json_to_str(json {
                        {"error", {
                            {"message", "Loading model"},
                            {"type", "unavailable_error"},
                            {"code", 503}
                        }}
                    }),
                    "application/json; charset=utf-8"
                );
            }
            return false;
        }
        return true;
    };

    // register server middlewares
    srv->set_pre_routing_handler([middleware_access_allow, middleware_validate_api_key, middleware_server_state](const httplib::Request & req, httplib::Response & res) {
        res.set_header("Access-Control-Allow-Origin", req.get_header_value("Origin"));
        if (!middleware_access_allow(req, res)) {
            return httplib::Server::HandlerResponse::Handled;
        }
        // If this is OPTIONS request, skip validation because browsers don't include Authorization header
        if (req.method == "OPTIONS") {
            res.set_header("Access-Control-Allow-Credentials", "true");
            res.set_header("Access-Control-Allow-Methods",     "GET, POST");
            res.set_header("Access-Control-Allow-Headers",     "*");
            res.set_content("", "text/html"); // blank response, no data
            return httplib::Server::HandlerResponse::Handled; // skip further processing
        }
        if (!middleware_server_state(req, res)) {
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!middleware_validate_api_key(req, res)) {
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    int n_threads_http = params.n_threads_http;
    if (n_threads_http < 1) {
        // +4 threads for monitoring, health and some threads reserved for MCP and other tasks in the future
        n_threads_http = std::max(params.n_parallel + 4, (int32_t) std::thread::hardware_concurrency() - 1);
    }
    LOG_INF("%s: using %d threads for HTTP server\n", __func__, n_threads_http);
    srv->new_task_queue = [n_threads_http] {
        // spawn n_threads_http fixed thread (always alive), while allow up to 1024 max possible additional threads
        // when n_threads_http is used, server will create new "dynamic" threads that will be destroyed after processing each request
        // ref: https://github.com/yhirose/cpp-httplib/pull/2368
        size_t max_threads = (size_t)n_threads_http + 1024;
        return new httplib::ThreadPool(n_threads_http, max_threads);
    };

    //
    // Web UI setup
    //

    if (!params.webui) {
        LOG_INF("Web UI is disabled\n");
    } else {
        // register static assets routes
        if (!params.public_path.empty()) {
            // Set the base directory for serving static files
            bool is_found = srv->set_mount_point(params.api_prefix + "/", params.public_path);
            if (!is_found) {
                LOG_ERR("%s: static assets path not found: %s\n", __func__, params.public_path.c_str());
                return 1;
            }
        } else {
#ifdef LLAMA_BUILD_WEBUI
            // using embedded static index.html
            srv->Get(params.api_prefix + "/", [](const httplib::Request & req, httplib::Response & res) {
                if (req.get_header_value("Accept-Encoding").find("gzip") == std::string::npos) {
                    res.set_content("Error: gzip is not supported by this browser", "text/plain");
                } else {
                    res.set_header("Content-Encoding", "gzip");
                    // COEP and COOP headers, required by pyodide (python interpreter)
                    res.set_header("Cross-Origin-Embedder-Policy", "require-corp");
                    res.set_header("Cross-Origin-Opener-Policy", "same-origin");
                    res.set_content(reinterpret_cast<const char*>(index_html_gz), index_html_gz_len, "text/html; charset=utf-8");
                }
                return false;
            });
#endif
        }
    }
    return true;
}

bool server_http_context::start() {
    // Bind and listen

    auto & srv = pimpl->srv;
    bool was_bound = false;
    bool is_sock = false;
    if (string_ends_with(std::string(hostname), ".sock")) {
        is_sock = true;
        LOG_INF("%s: setting address family to AF_UNIX\n", __func__);
        srv->set_address_family(AF_UNIX);
        // bind_to_port requires a second arg, any value other than 0 should
        // simply get ignored
        was_bound = srv->bind_to_port(hostname, 8080);
    } else {
        LOG_INF("%s: binding port with default address family\n", __func__);
        // bind HTTP listen port
        if (port == 0) {
            int bound_port = srv->bind_to_any_port(hostname);
            was_bound = (bound_port >= 0);
            if (was_bound) {
                port = bound_port;
            }
        } else {
            was_bound = srv->bind_to_port(hostname, port);
        }
    }

    if (!was_bound) {
        LOG_ERR("%s: couldn't bind HTTP server socket, hostname: %s, port: %d\n", __func__, hostname.c_str(), port);
        return false;
    }

    // run the HTTP server in a thread
    thread = std::thread([this]() { pimpl->srv->listen_after_bind(); });
    srv->wait_until_ready();

    listening_address = is_sock ? string_format("unix://%s",    hostname.c_str())
                                : string_format("http://%s:%d", hostname.c_str(), port);
    return true;
}

void server_http_context::stop() const {
    if (pimpl->srv) {
        pimpl->srv->stop();
    }
}

static void set_headers(httplib::Response & res, const std::map<std::string, std::string> & headers) {
    for (const auto & [key, value] : headers) {
        res.set_header(key, value);
    }
}

static std::map<std::string, std::string> get_params(const httplib::Request & req) {
    std::map<std::string, std::string> params;
    for (const auto & [key, value] : req.params) {
        params[key] = value;
    }
    for (const auto & [key, value] : req.path_params) {
        params[key] = value;
    }
    return params;
}

static std::map<std::string, std::string> get_headers(const httplib::Request & req) {
    std::map<std::string, std::string> headers;
    for (const auto & [key, value] : req.headers) {
        headers[key] = value;
    }
    return headers;
}

static std::string build_query_string(const httplib::Request & req) {
    std::string qs;
    for (const auto & [key, value] : req.params) {
        if (!qs.empty()) {
            qs += '&';
        }
        qs += httplib::encode_query_component(key) + "=" + httplib::encode_query_component(value);
    }
    return qs;
}

// using unique_ptr for request to allow safe capturing in lambdas
using server_http_req_ptr = std::unique_ptr<server_http_req>;

static void process_handler_response(
        server_http_req_ptr && request,
        server_http_res_ptr & response,
        httplib::Response & res,
        const std::shared_ptr<server_http_request_logger> & request_logger) {
    if (response->is_stream()) {
        res.status = response->status;
        set_headers(res, response->headers);
        std::string content_type = response->content_type;
        // convert to shared_ptr as both chunked_content_provider() and on_complete() need to use it
        std::shared_ptr<server_http_req> q_ptr = std::move(request);
        std::shared_ptr<server_http_res> r_ptr = std::move(response);
        auto response_body = std::make_shared<std::string>();
        const auto chunked_content_provider = [response = r_ptr, response_body](size_t, httplib::DataSink & sink) -> bool {
            std::string chunk;
            bool has_next = response->next(chunk);
            if (!chunk.empty()) {
                // TODO: maybe handle sink.write unsuccessful? for now, we rely on is_connection_closed()
                *response_body += chunk;
                sink.write(chunk.data(), chunk.size());
                SRV_DBG("http: streamed chunk: %s\n", chunk.c_str());
            }
            if (!has_next) {
                sink.done();
                SRV_DBG("%s", "http: stream ended\n");
            }
            return has_next;
        };
        const auto on_complete = [request = q_ptr, response = r_ptr, request_logger, response_body](bool) mutable {
            if (request_logger) {
                request_logger->write(*request, response->status, *response_body);
            }
            response.reset(); // trigger the destruction of the response object
            request.reset();  // trigger the destruction of the request object
        };
        res.set_chunked_content_provider(content_type, chunked_content_provider, on_complete);
    } else {
        res.status = response->status;
        set_headers(res, response->headers);
        res.set_content(response->data, response->content_type);
    }
}

void server_http_context::get(const std::string & path, const server_http_context::handler_t & handler) const {
    pimpl->srv->Get(path_prefix + path, [handler, request_logger = pimpl->request_logger](const httplib::Request & req, httplib::Response & res) {
        server_http_req_ptr request = std::make_unique<server_http_req>(server_http_req{
            req.method,
            req.remote_addr,
            get_params(req),
            get_headers(req),
            req.path,
            build_query_string(req),
            req.body,
            req.is_connection_closed
        });
        server_http_res_ptr response = handler(*request);
        process_handler_response(std::move(request), response, res, request_logger);
    });
}

void server_http_context::post(const std::string & path, const server_http_context::handler_t & handler) const {
    pimpl->srv->Post(path_prefix + path, [handler, request_logger = pimpl->request_logger](const httplib::Request & req, httplib::Response & res) {
        server_http_req_ptr request = std::make_unique<server_http_req>(server_http_req{
            req.method,
            req.remote_addr,
            get_params(req),
            get_headers(req),
            req.path,
            build_query_string(req),
            req.body,
            req.is_connection_closed
        });
        server_http_res_ptr response = handler(*request);
        process_handler_response(std::move(request), response, res, request_logger);
    });
}

