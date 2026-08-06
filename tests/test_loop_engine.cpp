#include <iostream>
#include <cassert>
#include <stdexcept>
#include "hivemind/LoopEngine.h"

int main() {
    LoopEngine engine;

    // Register a 3-iteration loop
    engine.register_loop("loop-1", 3);
    assert(engine.target_iterations("loop-1") == 3);
    assert(engine.current_iteration("loop-1") == 0);
    assert(!engine.is_complete("loop-1"));

    // Iteration 1: loop_end says jump back, stores feedback
    bool again = engine.loop_end_reached("loop-1", "first body");
    (void)again;
    assert(again == true);
    assert(engine.current_iteration("loop-1") == 1);
    assert(engine.feedback_text("loop-1") == "first body");
    assert(!engine.is_complete("loop-1"));

    // Iteration 2
    again = engine.loop_end_reached("loop-1", "second body");
    assert(again == true);
    assert(engine.current_iteration("loop-1") == 2);
    assert(engine.feedback_text("loop-1") == "second body");
    assert(!engine.is_complete("loop-1"));

    // Iteration 3 (final) — no more jumping
    again = engine.loop_end_reached("loop-1", "third body");
    assert(again == false);
    assert(engine.current_iteration("loop-1") == 3);
    assert(engine.feedback_text("loop-1") == "third body");
    assert(engine.is_complete("loop-1"));

    // Reset
    engine.reset();
    assert(engine.current_iteration("loop-1") == 0);
    assert(!engine.is_complete("loop-1"));
    assert(engine.active_loop_count() == 0);

    // Edge case: one-iteration loop
    engine.register_loop("loop-2", 1);
    again = engine.loop_end_reached("loop-2", "only body");
    assert(again == false);
    assert(engine.current_iteration("loop-2") == 1);
    assert(engine.is_complete("loop-2"));

    std::cout << "LoopEngine tests passed\n";
    return 0;
}
