// Typed host-service grants and the only pasta network syntax the worker owns.
#include "../src/runner.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

using nlohmann::json;

int main() {
    std::string error;
    auto ports = hivemind::parse_host_tcp_ports(
        json::array({8765, 8090, 8765, 8188}), error);
    assert(ports && error.empty());
    assert((*ports == hivemind::HostTcpPorts{8090, 8188, 8765}));
    assert(hivemind::host_tcp_ports_json(*ports) ==
           json::array({8090, 8188, 8765}));

    const json invalid[] = {
        json(nullptr), json::object(), json("8090"), json(true), json(1.5),
        json::array({0}), json::array({-1}), json::array({65536}),
        json::array({"8090"}), json::array({"-T,auto"}),
        json::array({true}), json::array({nullptr}),
        json::array({std::numeric_limits<std::uint64_t>::max()}),
    };
    for (const auto& value : invalid) {
        error.clear();
        assert(!hivemind::parse_host_tcp_ports(value, error));
        assert(!error.empty());
    }
    json too_many = json::array();
    for (int i = 0; i <= static_cast<int>(hivemind::kMaxHostTcpPorts); ++i)
        too_many.push_back(i + 1);
    assert(!hivemind::parse_host_tcp_ports(too_many, error));
    assert(hivemind::parse_host_tcp_ports(json::array(), error)->empty());
    assert(hivemind::host_tcp_ports_from_db("not-json").empty());

    assert(hwrunner::pasta_network_mode({}) == "pasta");
    assert(hwrunner::pasta_network_mode({8765, 8090, 8765, 8188}) ==
           "pasta:-T,8090,-T,8188,-T,8765");
    auto parsed = hwrunner::parse_pasta_network_mode(
        "  pasta:-T,8765,-T,8090,-T,8765\n");
    assert((parsed && *parsed == hivemind::HostTcpPorts{8090, 8765}));
    assert(hwrunner::parse_pasta_network_mode("pasta")->empty());

    const json create_command = {"podman", "create", "--network",
                                 "pasta:-T,8765,-T,8090", "image"};
    parsed = hwrunner::parse_podman_pasta_network(create_command, " pasta\n");
    assert((parsed && *parsed == hivemind::HostTcpPorts{8090, 8765}));
    assert(hwrunner::parse_podman_pasta_network(
               json::array({"podman", "create", "image"}), "pasta")->empty());
    assert(!hwrunner::parse_podman_pasta_network(create_command, "host"));
    assert(!hwrunner::parse_podman_pasta_network(
        json::array({"podman", "create", "--network=pasta:-T,8090",
                     "--network=pasta:-T,8765", "image"}), "pasta"));
    assert(!hwrunner::parse_podman_pasta_network(
        json::array({"podman", "create", "--network"}), "pasta"));

    const char* foreign[] = {
        "", "host", "slirp4netns", "pasta:", "pasta:-T,auto",
        "pasta:-T,none", "pasta:-T,0", "pasta:-T,65536",
        "pasta:-T,8090,--map-gw", "pasta:--map-gw",
        "pasta:-t,8090", "pasta:-T,8090,-U,53", "bridge",
    };
    for (const char* mode : foreign)
        assert(!hwrunner::parse_pasta_network_mode(mode));

    std::cout << "test_host_ports: all assertions passed\n";
    return 0;
}
