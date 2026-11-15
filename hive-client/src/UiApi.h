#pragma once

#include <memory>

#include <OmniNet.h>

namespace hive {

class HiveWindow;

/**
 * UiApi — localhost-only REST API embedded in hive-client (Phase 11.5).
 *
 * Purpose: make the GTK client testable end-to-end from scripts/agents.
 * State endpoints report what the UI actually shows (rendered bubbles
 * included); action endpoints drive the exact same code paths as the
 * buttons — no parallel "test mode" logic.
 *
 * Bind: 127.0.0.1:8481 (HIVEMIND_UI_PORT overrides; "0"/"off" disables).
 */
class UiApi {
public:
    explicit UiApi(HiveWindow& window);
    ~UiApi();

    void start();
    void stop();

private:
    HiveWindow& window_;
    std::unique_ptr<OmniNet::HTTP::Server> server_;
};

} // namespace hive
