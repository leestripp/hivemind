#include "hivemind/ExecutorRegistry.h"
#include "hivemind/executors/PromptExecutor.h"
#include "hivemind/executors/AgentExecutor.h"
#include "hivemind/executors/LoopStartExecutor.h"
#include "hivemind/executors/AgentLoopStartExecutor.h"
#include "hivemind/executors/AgentLoopEndExecutor.h"
#include "hivemind/executors/LoopEndExecutor.h"
#include "hivemind/executors/InputExecutor.h"
#include "hivemind/executors/OutputExecutor.h"
#include "hivemind/executors/LogExecutor.h"
#include "hivemind/executors/MergeExecutor.h"
#include <map>
#include <string_view>

struct ExecutorRegistry::Impl {
    std::map<std::string_view, std::unique_ptr<NodeExecutor>> map_;
    Impl() {
        map_["prompt"]     = std::make_unique<PromptExecutor>();
        map_["agent"]      = std::make_unique<AgentExecutor>();
        map_["loop_start"] = std::make_unique<LoopStartExecutor>();
        map_["agent_loop_start"] = std::make_unique<AgentLoopStartExecutor>();
        map_["agent_loop_end"]   = std::make_unique<AgentLoopEndExecutor>();
        map_["loop_end"]   = std::make_unique<LoopEndExecutor>();
        map_["input"]      = std::make_unique<InputExecutor>();
        map_["output"]     = std::make_unique<OutputExecutor>();
        map_["log"]        = std::make_unique<LogExecutor>();
        map_["merge"]      = std::make_unique<MergeExecutor>();
    }
};

ExecutorRegistry::ExecutorRegistry() : pImpl_(std::make_unique<Impl>()) {}
ExecutorRegistry::~ExecutorRegistry() = default;

NodeExecutor* ExecutorRegistry::get(std::string_view type) const {
    auto it = pImpl_->map_.find(type);
    if (it != pImpl_->map_.end()) return it->second.get();
    return nullptr;
}

std::vector<std::string_view> ExecutorRegistry::all_types() const {
    std::vector<std::string_view> out;
    for (auto& [k, _] : pImpl_->map_) out.push_back(k);
    return out;
}
