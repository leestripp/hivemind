// Regression test for the 2026-09-17 "attach button doesn't work" bug.
//
// Root cause: OmniNet's HttpClient formatted the request Content-Length
// via std::ostringstream insertion of size_t. The GTK client process has
// a global C++ locale with thousands separators (','), so a 1,261,963-byte
// upload went on the wire as "Content-Length: 1,261,963" — nginx rejected
// it (400 "client sent invalid Content-Length header"). Every upload
// >= 1000 bytes failed in the app while CLI probes (classic locale)
// succeeded, which made the bug look like a size limit.
//
// Fix (omninet bc2adc4): std::to_string for the wire value. This test
// poisons the global locale with a comma-grouping numpunct facet and runs
// a real upload through ApiClient against an in-process OmniNet server,
// asserting the header on the wire is plain digits and the upload
// succeeds. Without the fix the server sees "1,000,000" and the
// all-digits assertion fails.

#include "ApiClient.h"

#include <OmniNet.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <thread>

namespace {

// A numpunct that injects ',' every 3 digits — the poison that broke the
// client. Hermetic: does not depend on any OS locale being installed.
struct CommaPunct : std::numpunct<char> {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};

}  // namespace

int main() {
    using namespace OmniNet::HTTP;

    // Poison the C++ global locale (streams constructed afterwards group).
    std::locale::global(std::locale(std::locale::classic(), new CommaPunct));
    {
        std::ostringstream os;
        os << 1261963;
        assert(os.str() == "1,261,963" && "locale poison must be active");
    }

    bool cl_seen = false;
    bool cl_all_digits = false;
    size_t body_len = 0;
    int flaky_calls = 0;

    Server::Config cfg;
    cfg.address = "127.0.0.1";
    cfg.port = 18991;
    cfg.logRequests = false;
    Server server{cfg};
    server.post("/api/threads/:tid/attachments",
                [&](const Request& req, Response& res) {
                    if (auto cl = req.header("Content-Length")) {
                        cl_seen = true;
                        cl_all_digits =
                            !cl->empty() &&
                            std::all_of(cl->begin(), cl->end(),
                                        [](unsigned char c) { return std::isdigit(c) != 0; });
                    }
                    body_len = req.body().size();
                    res.setStatus(201);
                    res.setBody(R"({"name":"big.bin","size":1000000})");
                });
    server.get("/api/flaky", [&](const Request&, Response& res) {
        ++flaky_calls;
        if (flaky_calls < 3) {
            res.setStatus(400);
            res.setBody("temporary worker failure");
        } else {
            res.setStatus(200);
            res.setBody("video-bytes");
        }
    });

    std::jthread server_thread{[&server] { server.start(); }};
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    try {
        hive::ApiClient api{"http://127.0.0.1:18991"};
        const std::string bytes(1000000, 'x');  // >= 1000 triggers grouping
        auto meta = api.upload_attachment("thread-1", "big.bin", bytes);
        assert(meta.value("size", 0) == 1000000);
        assert(api.download("/api/flaky") == "video-bytes");
    } catch (const std::exception& e) {
        std::cerr << "upload threw: " << e.what() << std::endl;
        server.stop();
        return 1;
    }

    std::locale::global(std::locale::classic());

    assert(cl_seen && "server must see a Content-Length header");
    assert(cl_all_digits &&
           "Content-Length on the wire must be plain digits (locale leak!)");
    assert(body_len == 1000000);
    assert(flaky_calls == 3 && "transient downloads must retry then succeed");

    server.stop();   // lets the jthread join at scope exit
    std::cout << "test_upload_wire_format OK" << std::endl;
    return 0;
}
