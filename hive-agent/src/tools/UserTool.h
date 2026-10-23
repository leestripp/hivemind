#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

class FileIO;

using json = nlohmann::json;

// Forward declarations
class TellUserTool;
class CompressContextTool;
class StopTool;

/**
 * UserTool.h - Contains user interaction tool classes
 */

// ========== TellUserTool ==========
class TellUserTool : public BaseTool {
public:
    std::string get_name() const override { return "tell_user"; }
    std::string get_description() const override { return "Send a message, status update, or question to the user."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== CompressContextTool ==========
// §COMPRESS-TOOL (2026-08-14): model-facing compression trigger, restored
// after the 2026-08-01 removal. The tool does NOT compress inline — it
// returns status "compress_context" and LLMClient::run_agent_cycle() sets
// the same compress_requested_ flag as Agent::compress_context(), so the
// actual compression runs at the cycle-boundary checkpoint (§AutoCompress).
// Safe mid-turn: in-flight tool calls are never interrupted.
class CompressContextTool : public BaseTool {
public:
    std::string get_name() const override { return "compress_context"; }
    std::string get_description() const override { return "Request context compression when a long-running task is nearing the memory limit and older details are no longer needed. This does NOT end your turn, post a reply, or save a record of finished work — those are what the stop tool is for. Never compress as a final/cleanup step: it erases working detail and the turn simply continues."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== StopTool ==========
class StopTool : public BaseTool {
public:
    std::string get_name() const override { return "stop"; }
    std::string get_description() const override { return "Signal that the task is complete"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
