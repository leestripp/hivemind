// test_delivery_store (Phase 66a): DeliveryStore state machine,
// idempotency, orphan requeue, relay links, read high-water marks.

#include "hivemind/DeliveryStore.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <thread>

using hivemind::Delivery;
using hivemind::DeliveryStore;

namespace fs = std::filesystem;

static Delivery make_wake(const std::string& target,
                          const std::string& src_msg,
                          const std::string& sender = "") {
    Delivery d;
    d.kind = "wake";
    d.thread_id = "thread-1";
    d.target_agent_id = target;
    d.sender_agent_id = sender;
    d.sender_label = sender.empty() ? "You" : sender;
    d.body = "hello " + target;
    d.generation = 1;
    d.source_message_id = src_msg;
    return d;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "hm_test_deliveries";
    fs::remove_all(dir);
    fs::create_directories(dir);
    DeliveryStore store(dir / "deliveries.db");

    // ── enqueue + get ────────────────────────────────────────────────
    const std::string id1 = store.enqueue(make_wake("agent-a", "msg-1"));
    assert(!id1.empty());
    auto d = store.get(id1);
    assert(d && d->state == "queued" && d->target_agent_id == "agent-a");
    assert(d->kind == "wake" && !d->created_at.empty());

    // ── oldest_queued FIFO per agent ─────────────────────────────────
    const std::string id2 = store.enqueue(make_wake("agent-a", "msg-2"));
    store.enqueue(make_wake("agent-b", "msg-3"));
    auto oldest = store.oldest_queued("agent-a");
    assert(oldest && oldest->id == id1);

    // ── idempotency ──────────────────────────────────────────────────
    assert(store.has_live_wake("agent-a", "msg-1"));
    assert(!store.has_live_wake("agent-a", "msg-99"));
    assert(!store.has_live_wake("agent-b", "msg-1"));

    // ── dispatch -> done ─────────────────────────────────────────────
    store.mark_dispatched(id1, "t-100");
    d = store.get(id1);
    assert(d && d->state == "dispatched" && d->task_id == "t-100");
    assert(!d->dispatched_at.empty());
    // idempotency still holds while dispatched
    assert(store.has_live_wake("agent-a", "msg-1"));
    store.mark_done(id1);
    d = store.get(id1);
    assert(d && d->state == "done" && !d->completed_at.empty());
    // terminal: no longer blocks re-wake
    assert(!store.has_live_wake("agent-a", "msg-1"));
    // oldest_queued skips done rows
    oldest = store.oldest_queued("agent-a");
    assert(oldest && oldest->id == id2);

    // ── error + dropped ──────────────────────────────────────────────
    store.mark_dispatched(id2, "t-101");
    store.mark_error(id2, "worker offline");
    d = store.get(id2);
    assert(d && d->state == "error" && d->error == "worker offline");
    const std::string id3 = store.enqueue(make_wake("agent-c", "msg-4"));
    store.mark_dropped(id3, "muted");
    d = store.get(id3);
    assert(d && d->state == "dropped" && d->error == "muted");

    // ── orphan requeue ───────────────────────────────────────────────
    const std::string id4 = store.enqueue(make_wake("agent-d", "msg-5"));
    store.mark_dispatched(id4, "t-102");
    const int requeued = store.requeue_orphans();
    assert(requeued == 1);           // only id4 was dispatched
    d = store.get(id4);
    assert(d && d->state == "queued" && d->task_id.empty());
    assert(d->dispatched_at.empty());

    // ── list / sent_by ───────────────────────────────────────────────
    store.enqueue(make_wake("agent-a", "msg-6", "sender-x"));
    auto all = store.list("", "", 50);
    assert(all.size() == 6);
    auto for_a = store.list("agent-a", "", 50);
    assert(for_a.size() == 3);       // all three as target
    auto queued_only = store.list("", "queued", 50);
    assert(queued_only.size() == 3); // msg-3 (b), msg-5 (d), msg-6 (a)
    auto by_x = store.sent_by("sender-x", 10);
    assert(by_x.size() == 1 && by_x[0].source_message_id == "msg-6");

    // ── relay links ──────────────────────────────────────────────────
    store.arm_relay("pair-1", "requester-r", "home-1");
    // requester's own message does NOT consume
    assert(!store.consume_relay("pair-1", "requester-r"));
    // peer's reply consumes (one-shot)
    auto link = store.consume_relay("pair-1", "peer-p");
    assert(link && link->requester_id == "requester-r");
    assert(link->home_thread_id == "home-1");
    // second consume returns nothing
    assert(!store.consume_relay("pair-1", "peer-p"));
    // re-arm resets consumed (latest wins)
    store.arm_relay("pair-1", "requester-r", "home-2");
    link = store.consume_relay("pair-1", "peer-p");
    assert(link && link->home_thread_id == "home-2");
    // expiry: cutoff now+1s deletes every armed row (consumed rows stay
    // in the table until expired, so pair-1 counts too)
    store.arm_relay("pair-old", "r", "h");
    const int expired = store.expire_relays(-1);
    assert(expired >= 2);
    assert(!store.consume_relay("pair-1", "peer-p"));
    assert(!store.consume_relay("pair-old", "peer-p"));

    // ── read high-water marks ────────────────────────────────────────
    assert(!store.read_mark("thread-1", "agent-a"));
    store.mark_read("thread-1", "agent-a", "msg-1", "2026-09-27T09:00:00Z");
    auto mark = store.read_mark("thread-1", "agent-a");
    assert(mark && *mark == "2026-09-27T09:00:00Z");
    // advance
    store.mark_read("thread-1", "agent-a", "msg-2", "2026-09-27T10:00:00Z");
    mark = store.read_mark("thread-1", "agent-a");
    assert(mark && *mark == "2026-09-27T10:00:00Z");
    // monotonic: older message never drags the mark back
    store.mark_read("thread-1", "agent-a", "msg-0", "2026-09-27T08:00:00Z");
    mark = store.read_mark("thread-1", "agent-a");
    assert(mark && *mark == "2026-09-27T10:00:00Z");
    // per-agent independence
    assert(!store.read_mark("thread-1", "agent-b"));
    store.mark_read("thread-1", "agent-b", "msg-1", "2026-09-27T09:30:00Z");
    assert(*store.read_mark("thread-1", "agent-b") == "2026-09-27T09:30:00Z");
    assert(*store.read_mark("thread-1", "agent-a") == "2026-09-27T10:00:00Z");

    // ── prune ────────────────────────────────────────────────────────
    // Nothing is old enough yet.
    assert(store.prune_terminal_older_than(7) == 0);

    fs::remove_all(dir);
    std::puts("test_delivery_store: OK");
    return 0;
}
