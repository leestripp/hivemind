// Conversation response validation: compatibility without wrong-DM fallback.

#include <hivemind/ThreadResponse.h>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using nlohmann::json;
using hivemind::validated_thread_response;
using hivemind::validated_message_id;

namespace {

json thread(const std::string& id, const std::vector<std::string>& agents) {
    return json{{"id", id}, {"agent_ids", agents}, {"kind", "dm"}};
}

template <typename F>
void must_reject(F&& fn) {
    bool rejected = false;
    try { fn(); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}

} // namespace

int main() {
    const std::vector<std::string> target{"honeybee"};
    const auto other = thread("other-thread", {"scout"});
    const auto wanted = thread("honeybee-thread", target);

    // Canonical hub response.
    assert(validated_thread_response(wanted, target) == wanted);

    // Compatibility with the deployed envelope: never inherit threads[0].
    const json envelope{{"threads", json::array({other, wanted})}};
    assert(validated_thread_response(envelope, target) == wanted);

    // Group membership is an exact set, independent of wire order.
    const auto group = thread("group-thread", {"a", "b"});
    assert(validated_thread_response(group, {"b", "a"}) == group);

    must_reject([&] { validated_thread_response(other, target); });
    must_reject([&] { validated_thread_response(json{{"threads", {other}}},
                                                 target); });
    must_reject([&] { validated_thread_response(json{{"threads", json::array()}},
                                                 target); });
    must_reject([&] { validated_thread_response(thread("", target), target); });
    must_reject([&] { validated_thread_response(json{{"id", 42},
                                                      {"agent_ids", target}},
                                                 target); });
    must_reject([&] { validated_thread_response(json{{"id", "missing-membership"}},
                                                 target); });
    must_reject([&] { validated_thread_response(thread("extra-agent",
                                                       {"honeybee", "scout"}),
                                                 target); });
    must_reject([&] { validated_thread_response(json::array({wanted}), target); });

    assert(validated_message_id(json{{"message_id", "message-1"}}) == "message-1");
    assert(validated_message_id(json{{"id", "legacy-message"}}) == "legacy-message");
    assert(validated_message_id(json{{"message", {{"id", "nested-message"}}}}) ==
           "nested-message");
    must_reject([&] { validated_message_id(json::object()); });
    must_reject([&] { validated_message_id(json{{"message_id", ""}}); });
    must_reject([&] { validated_message_id(json{{"status", "processing"}}); });

    std::cout << "test_thread_response OK\n";
}
