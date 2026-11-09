#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/SecondBrain.h"

namespace fs = std::filesystem;
using hive::SecondBrain;

namespace {

struct TempBrain {
    fs::path root;
    TempBrain() {
        root = fs::temp_directory_path() /
               ("hive-second-brain-" + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempBrain() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void write(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

TEST_CASE("SecondBrain shares Markdown and FTS across independent agents") {
    TempBrain temp;
    SecondBrain alice(temp.root);
    SecondBrain bob(temp.root);

    for (const auto& folder : SecondBrain::folders())
        CHECK(fs::is_directory(temp.root / folder));

    const auto note = alice.capture("resources", "HiveMind Brain",
                                    {"hivemind", "design"},
                                    "# Shared knowledge\n\nCapture organise distill. "
                                    "See [[deployment-runbook]]. Café 東京.");
    CHECK(note.id == "hivemind-brain");
    CHECK(fs::exists(temp.root / "resources" / "hivemind-brain.md"));

    // Bob sees Alice's write without an explicit reindex.
    const auto from_bob = bob.read(note.id);
    REQUIRE(from_bob.has_value());
    CHECK(from_bob->title == "HiveMind Brain");
    CHECK(from_bob->tags.size() == 2);
    CHECK(from_bob->links == std::vector<std::string>{"deployment-runbook"});

    REQUIRE(bob.capture("areas", "Deployment Runbook", {"ops"},
                        "Per-rig verification lives here.").id == "deployment-runbook");
    CHECK(alice.backlinks("deployment-runbook") ==
          std::vector<std::string>{"hivemind-brain"});

    REQUIRE(bob.append(note.id, "A second agent appended this fact."));
    CHECK(alice.read(note.id)->body.find("second agent appended") != std::string::npos);
    REQUIRE(alice.update(note.id, std::string("HiveMind Shared Brain"),
                         std::vector<std::string>{"hivemind", "shared"}, std::nullopt));
    CHECK(bob.read(note.id)->title == "HiveMind Shared Brain");

    auto hits = bob.search("organizing");
    REQUIRE(!hits.empty());
    CHECK(hits.front().note.id == note.id);
    CHECK(!hits.front().snippet.empty());
    CHECK(!alice.search("Café").empty());
    CHECK(!alice.search("東京").empty());
    CHECK(alice.search("\" OR * NEAR( (").empty());

    CHECK(bob.list("", "shared").size() == 1);
    CHECK(bob.list("resources").size() == 1);
    REQUIRE(bob.move(note.id, "projects"));
    CHECK(!fs::exists(temp.root / "resources" / "hivemind-brain.md"));
    CHECK(alice.read(note.id)->folder == "projects");

    // Same-size, same-second hand edits must still invalidate the index.
    const fs::path manual = temp.root / "inbox" / "manual.md";
    write(manual, "# Manual\n\nfirst-token\n");
    CHECK(bob.search("first-token").size() == 1);
    write(manual, "# Manual\n\nother-token\n");
    CHECK(alice.search("first-token").empty());
    CHECK(alice.search("other-token").size() == 1);
    fs::remove(manual);
    CHECK(bob.search("other-token").empty());

    CHECK(!alice.read("../../../etc/passwd").has_value());
    CHECK(!alice.remove("../escape"));
    CHECK_THROWS_AS(alice.list("not-a-folder"), std::invalid_argument);
    CHECK(alice.reindex().indexed == 0);
    REQUIRE(bob.remove(note.id));
    CHECK(!alice.read(note.id).has_value());
    CHECK(alice.search("organizing").empty());
}

TEST_CASE("SecondBrain serializes competing captures") {
    TempBrain temp;
    constexpr int kAgents = 6;
    constexpr int kNotesEach = 5;
    std::vector<std::unique_ptr<SecondBrain>> brains;
    for (int i = 0; i < kAgents; ++i)
        brains.push_back(std::make_unique<SecondBrain>(temp.root));

    std::mutex ids_mutex;
    std::vector<std::string> ids;
    std::vector<std::thread> threads;
    for (int agent = 0; agent < kAgents; ++agent) {
        threads.emplace_back([&, agent] {
            for (int note = 0; note < kNotesEach; ++note) {
                const auto captured = brains[static_cast<size_t>(agent)]->capture(
                    "inbox", "Concurrent Decision", {"test"},
                    "Concurrent unique capture body " + std::to_string(agent) + "-" +
                        std::to_string(note));
                std::lock_guard lock(ids_mutex);
                ids.push_back(captured.id);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    REQUIRE(ids.size() == kAgents * kNotesEach);
    CHECK(std::set<std::string>(ids.begin(), ids.end()).size() == ids.size());
    CHECK(brains.front()->list("", {}, 100).size() == ids.size());
    CHECK(brains.back()->search("concurrent", 100).size() == ids.size());
}
