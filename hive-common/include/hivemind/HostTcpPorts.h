#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hivemind {

using HostTcpPorts = std::vector<std::uint16_t>;
inline constexpr std::size_t kMaxHostTcpPorts = 64;

inline HostTcpPorts normalize_host_tcp_ports(HostTcpPorts ports) {
    std::ranges::sort(ports);
    ports.erase(std::ranges::unique(ports).begin(), ports.end());
    return ports;
}

/// Parse the administrative JSON allowlist. No strings or free-form Podman
/// options cross this boundary: the worker alone renders pasta arguments.
inline std::optional<HostTcpPorts> parse_host_tcp_ports(
        const nlohmann::json& value, std::string& error) {
    if (!value.is_array()) {
        error = "host_tcp_ports must be an array of TCP port integers";
        return std::nullopt;
    }
    if (value.size() > kMaxHostTcpPorts) {
        error = "host_tcp_ports accepts at most " +
                std::to_string(kMaxHostTcpPorts) + " entries";
        return std::nullopt;
    }
    HostTcpPorts ports;
    ports.reserve(value.size());
    for (const auto& entry : value) {
        std::uint64_t port = 0;
        if (entry.is_number_unsigned()) {
            port = entry.get<std::uint64_t>();
        } else if (entry.is_number_integer()) {
            const auto signed_port = entry.get<std::int64_t>();
            if (signed_port < 0) {
                error = "host_tcp_ports entries must be between 1 and 65535";
                return std::nullopt;
            }
            port = static_cast<std::uint64_t>(signed_port);
        } else {
            error = "host_tcp_ports entries must be integers, not strings or Podman options";
            return std::nullopt;
        }
        if (port < 1 || port > 65535) {
            error = "host_tcp_ports entries must be between 1 and 65535";
            return std::nullopt;
        }
        ports.push_back(static_cast<std::uint16_t>(port));
    }
    return normalize_host_tcp_ports(std::move(ports));
}

inline nlohmann::json host_tcp_ports_json(const HostTcpPorts& ports) {
    auto out = nlohmann::json::array();
    for (const auto port : ports) out.push_back(port);
    return out;
}

inline std::string host_tcp_ports_dump(const HostTcpPorts& ports) {
    return host_tcp_ports_json(ports).dump();
}

/// A damaged database value fails closed to no grants. Routes and spawn frames
/// still use parse_host_tcp_ports so malformed external input is reported.
inline HostTcpPorts host_tcp_ports_from_db(const std::string& text) {
    if (text.empty()) return {};
    const auto value = nlohmann::json::parse(text, nullptr, false);
    std::string error;
    const auto ports = parse_host_tcp_ports(value, error);
    return ports.value_or(HostTcpPorts{});
}

} // namespace hivemind
