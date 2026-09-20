/**
 * @file src/tools/ListToolsTool.h
 * @brief §LIST-TOOLS (2026-09-20): ground-truth tool introspection.
 *
 * Models asked "what tools do you have?" tend to answer from conversation
 * history instead of their live tool definitions — a restored session that
 * predates a newly-shipped tool makes the model swear the tool does not
 * exist while it is sitting in the request payload (observed with Honey
 * and secrets_create, 2026-09-20). This tool reads the live ToolFactory
 * registry and returns exactly what the agent can call RIGHT NOW,
 * filtered by the calling agent's ToolFilter (via FilterAwareTool), so
 * the answer is authoritative rather than recollected.
 */
#pragma once

#include "BaseTool.h"
#include "ToolFilter.h"

class ListToolsTool : public BaseTool, public FilterAwareTool {
public:
    [[nodiscard]] std::string get_name() const override { return "list_tools"; }
    [[nodiscard]] std::string get_description() const override;
    [[nodiscard]] json        get_definition() const override;

    json execute(const json& args, FileIO& file_io) override;

    /// FilterAwareTool — valid only for the duration of execute().
    void set_active_filter(const ToolFilter* filter) override {
        active_filter_ = filter;
    }

private:
    const ToolFilter* active_filter_ = nullptr;
};
