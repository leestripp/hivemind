#include "hivemind/ModelCatalog.h"

#include <cassert>
#include <iostream>

using hivemind::model_metadata;

int main() {
    const auto k31 = model_metadata("", "https://api.moonshot.ai",
                                    "kimi-k3.1-pro");
    assert(k31.max_context == 1000000 && k31.temperature == 1.0);
    const auto k31hs = model_metadata("moonshot", "https://api.kimi.com",
                                      "Kimi-K3.1-Pro-Highspeed");
    assert(k31hs.max_context == 1000000 && k31hs.temperature == 1.0);
    const auto code_alias = model_metadata("", "https://api.kimi.com/coding",
                                           "k3.1-pro-highspeed");
    assert(code_alias.max_context == 1000000 && code_alias.temperature == 1.0);
    const auto k26 = model_metadata("", "https://api.moonshot.cn", "kimi-k2.6");
    assert(k26.max_context == 262144 && k26.temperature == 1.0);
    const auto mm = model_metadata("", "https://api.minimax.io", "minimax-m3");
    assert(mm.max_context == 500000 && mm.temperature == 1.0);

    const nlohmann::json reported = {
        {"context_length", 262144}, {"default_temperature", 0.6}};
    const auto from_api = model_metadata("", "https://api.kimi.com",
                                         "kimi-k3.1-pro", &reported);
    assert(from_api.max_context == 262144 && from_api.temperature == 0.6);

    const nlohmann::json generic = {{"max_context_tokens", 131072},
                                    {"temperature", 0.4}};
    const auto custom = model_metadata("", "https://llm.example", "custom", &generic);
    assert(custom.max_context == 131072 && custom.temperature == 0.4);
    const auto unknown = model_metadata("", "https://llm.example", "custom");
    assert(unknown.max_context == 0 && unknown.temperature == 0.0);

    std::cout << "test_model_catalog: all OK\n";
}
