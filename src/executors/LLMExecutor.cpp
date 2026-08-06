#include "hivemind/executors/LLMExecutor.h"
#include "hivemind/Graph.h"
#include "hivemind/LLMClient.h"
#include "hivemind/LLMSettings.h"
#include <sstream>
#include <stdexcept>

static std::string join_all(const std::unordered_map<std::string, std::string>& inputs) {
    std::ostringstream ss;
    for (auto& [k, v] : inputs) {
        if (!ss.str().empty()) ss << "\n";
        ss << v;
    }
    return ss.str();
}

std::vector<NodeResult>
LLMExecutor::execute(const Node& node,
                      const std::unordered_map<std::string, std::string>& inputs,
                      const std::string& /*run_id*/) {

    // ---- Resolve model ----
    std::string model_name;
    if (node.config_json.contains("model")
        && node.config_json["model"].is_string()) {
        model_name = node.config_json["model"].get<std::string>();
    }

    if (model_name.empty()) {
        auto def = llm_settings().default_model();
        if (!def)
            throw std::runtime_error{
                "no default model configured in ~/.codemax/settings/settings.json"};
        model_name = def->name;
    }

    auto mc_opt = llm_settings().get_model(model_name);
    if (!mc_opt)
        throw std::invalid_argument{"unknown model: " + model_name};

    const ModelConfig& mc = *mc_opt;

    // ---- Build messages ----
    std::vector<LLMMessage> msgs;

    // System prompt (optional)
    if (node.config_json.contains("system_prompt")
        && node.config_json["system_prompt"].is_string()) {
        msgs.push_back({
            .role = "system",
            .content = node.config_json["system_prompt"].get<std::string>()
        });
    }

    // User prompt (from inputs, joined)
    std::string user = join_all(inputs);
    if (user.empty()) user = "(no input)";
    msgs.push_back({.role = "user", .content = std::move(user)});

    // ---- Call LLM (non-streaming) ----
    LLMClient llm{mc.base_url, mc.api_key, mc.model, mc.chat_endpoint};
    auto r = llm.complete(msgs);

    if (!r.ok)
        throw std::runtime_error{"LLM error: " + r.error};

    return {NodeResult{node.id, "out", std::move(r.content)}};
}
