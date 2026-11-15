#include "TtsTagLedger.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("TTS tag ledger ignores stale and duplicate completions") {
    hv::TtsTagLedger ledger;
    ledger.add(10);
    ledger.add(11);
    REQUIRE(ledger.size() == 2);
    REQUIRE_FALSE(ledger.complete(9));   // old epoch: no count consumed
    REQUIRE_FALSE(ledger.complete(10));  // one current tag still pending
    REQUIRE(ledger.complete(11));
    REQUIRE(ledger.size() == 0);
    REQUIRE_FALSE(ledger.complete(11));  // duplicate: no second empty edge
}

TEST_CASE("TTS stop clears tags so they cannot complete a later turn") {
    hv::TtsTagLedger ledger;
    ledger.add(20);
    ledger.clear();
    ledger.add(21);
    REQUIRE_FALSE(ledger.complete(20));
    REQUIRE(ledger.size() == 1);
    REQUIRE(ledger.complete(21));
}
