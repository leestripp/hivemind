// Opt-in disposable container driver for test_host_ports_live.py.
// No arguments: CTest skips. With arguments: create/reconcile exactly one
// caller-owned container under a caller-supplied temporary HOME.
#include "../src/runner.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using nlohmann::json;

int main(int argc, char** argv) {
    if (argc == 1) return 77;   // opt-in integration test, never default CTest
    if (argc < 4) {
        std::cerr << "usage: test_host_ports_live AGENT IMAGE PORTS_CSV|pin\n";
        return 64;
    }
    const std::string agent = argv[1];
    const std::string image = argv[2];
    const std::string mode = argv[3];
    try {
        if (mode == "pin") {
            // Simulates a fresh spec-less worker path: only the persisted pin
            // may supply policy. The container id must remain unchanged.
            hwrunner::ensure_container(agent, image, false);
        } else {
            json values = json::array();
            if (!mode.empty()) {
                std::size_t pos = 0;
                for (;;) {
                    const auto comma = mode.find(',', pos);
                    values.push_back(std::stoi(mode.substr(pos, comma - pos)));
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
            }
            std::string error;
            auto ports = hivemind::parse_host_tcp_ports(values, error);
            if (!ports) throw std::invalid_argument(error);
            hwrunner::ensure_container(agent, image, false, "1g", 1, &*ports);
        }
        std::string id, network, create_json, image_id;
        const std::string cname = hwrunner::container_name(agent);
        if (hwrunner::podman_simple({"inspect", "-f", "{{.Id}}", cname}, &id) != 0 ||
            hwrunner::podman_simple({"inspect", "-f", "{{.HostConfig.NetworkMode}}",
                                     cname}, &network) != 0 ||
            hwrunner::podman_simple({"inspect", "-f", "{{.Image}}", cname},
                                    &image_id) != 0 ||
            hwrunner::podman_simple({"inspect", "-f", "{{json .Config.CreateCommand}}",
                                     cname}, &create_json) != 0)
            throw std::runtime_error("failed to inspect disposable container");
        const auto create_command = json::parse(create_json);
        const auto effective = hwrunner::parse_podman_pasta_network(
            create_command, network);
        if (!effective) throw std::runtime_error("unrecognized disposable network");
        const auto trim = [](std::string value) {
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
                value.pop_back();
            return value;
        };
        std::cout << json{{"container", cname}, {"id", trim(id)},
                          {"network", hwrunner::pasta_network_mode(*effective)},
                          {"host_network", trim(network)},
                          {"image", trim(image_id)}}
                  << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_host_ports_live: " << e.what() << '\n';
        return 1;
    }
}
