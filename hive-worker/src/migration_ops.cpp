// =============================================================================
// migration_ops — see migration_ops.h. Implementation notes:
//  * run_cmd forks/execs with a drained pipe (house rule: never leave a
//    child's stdout/stderr unread — a full pipe deadlocks the child).
//  * tar archives carry ".hive-agent/<Name>/..." members, so extraction
//    into the target workspace root re-creates the state dir in place.
// =============================================================================

#include "migration_ops.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "runner.h"

namespace hwmigration {
namespace {

// ─── base64 (standard alphabet, with padding) ───────────────────────────────

constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_enc_impl(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t b0 = static_cast<unsigned char>(in[i]);
        const uint32_t b1 = i + 1 < in.size()
                                ? static_cast<unsigned char>(in[i + 1]) : 0;
        const uint32_t b2 = i + 2 < in.size()
                                ? static_cast<unsigned char>(in[i + 2]) : 0;
        const uint32_t n = (b0 << 16) | (b1 << 8) | b2;
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kB64[n & 63] : '=';
    }
    return out;
}

int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string b64_dec_impl(const std::string& in) {
    std::string out;
    if (in.size() % 4 != 0)
        throw std::runtime_error("b64: bad length");
    out.reserve((in.size() / 4) * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = in[i + k];
            if (c == '=') { v[k] = 0; ++pad; }
            else {
                if (pad) throw std::runtime_error("b64: pad mid-quad");
                v[k] = b64_val(c);
                if (v[k] < 0) throw std::runtime_error("b64: bad char");
            }
        }
        const uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) |
                           (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        out += char((n >> 16) & 0xff);
        if (pad < 2) out += char((n >> 8) & 0xff);
        if (pad < 1) out += char(n & 0xff);
    }
    return out;
}

// ─── process runner (drained pipe) ──────────────────────────────────────────

/// Run argv, capturing stdout+stderr into `out`. Returns the exit code
/// (-1 on fork/pipe failure). Never leaves the pipe unread.
int run_cmd(const std::vector<std::string>& argv, std::string* out) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        std::vector<char*> av;
        av.reserve(argv.size() + 1);
        for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return -1; }
    if (out) {
        out->clear();
        std::array<char, 4096> buf;
        ssize_t n;
        while ((n = read(fds[0], buf.data(), buf.size())) > 0)
            out->append(buf.data(), size_t(n));
    } else {
        // Still drain — a full pipe would deadlock the child.
        std::array<char, 4096> buf;
        while (read(fds[0], buf.data(), buf.size()) > 0) {}
    }
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// ─── staging helpers ────────────────────────────────────────────────────────

std::mutex g_file_m;  // staging file ops (chunks arrive on the WS thread)

fs::path staging_dir(const Ctx& ctx, const std::string& job) {
    return ctx.staging_root / job;
}

// ─── ops ────────────────────────────────────────────────────────────────────

void do_export(const Ctx& ctx, const json& f) {
    const std::string job = f.value("job_id", "");
    const std::string agent = f.value("agent_id", "");
    const std::string name = f.value("name", "");
    const bool force = f.value("force", false);
    auto err = [&](const std::string& e) {
        ctx.send({{"op", "agent_export_error"},
                  {"job_id", job}, {"error", e}});
    };
    if (job.empty() || agent.empty() || name.empty())
        return err("missing job_id/agent_id/name");

    if (ctx.hooks.is_running && ctx.hooks.is_running(agent)) {
        if (!force)
            return err("agent is running on this worker; re-run with force");
        ctx.hooks.stop_agent(agent);
        for (int i = 0; i < 150 && ctx.hooks.is_running(agent); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (ctx.hooks.is_running(agent))
            return err("agent did not stop within 30s");
    }

    const fs::path home = ctx.ws / ".hive-agent" / name;
    if (!fs::is_directory(home))
        return err("no state dir for '" + name + "' on this worker");

    const fs::path dir = staging_dir(ctx, job);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path arc = dir / "export.tar.zst";
    if (!fs::exists(arc, ec)) {  // idempotent: reuse a prior archive
        std::string out;
        if (run_cmd({"tar", "--zstd", "-C", ctx.ws.string(), "-cf",
                     arc.string(), ".hive-agent/" + name},
                    &out) != 0)
            return err("tar failed: " + out);
    }
    ctx.send({{"op", "agent_export_ready"},
              {"job_id", job},
              {"path", arc.string()},
              {"size", fs::file_size(arc, ec)}});
}

void do_data_begin(const Ctx& ctx, const json& f) {
    const std::string job = f.value("job_id", "");
    const fs::path arc = staging_dir(ctx, job) / "export.tar.zst";
    std::error_code ec;
    if (!fs::exists(arc, ec)) {
        // Sweep re-drive without a fresh export: ask the hub to re-export.
        ctx.send({{"op", "agent_export_error"}, {"job_id", job},
                  {"error", "export archive missing; re-drive export"}});
        return;
    }
    std::ifstream in(arc, std::ios::binary);
    if (!in) {
        ctx.send({{"op", "agent_export_error"}, {"job_id", job},
                  {"error", "cannot read export archive"}});
        return;
    }
    constexpr std::streamsize kChunk = 192 * 1024;
    std::vector<char> buf(size_t{192} * 1024);
    uint64_t seq = 0;
    while (in) {
        in.read(buf.data(), kChunk);
        const auto n = in.gcount();
        const bool last = n < kChunk;  // short read = EOF
        std::string chunk(buf.data(), size_t(n));
        ctx.send({{"op", "agent_data_chunk"},
                  {"job_id", job},
                  {"seq", seq},
                  {"data_b64", b64_enc_impl(chunk)},
                  {"last", last}});
        ++seq;
        if (last) break;
    }
}

void do_data_chunk(const Ctx& ctx, const json& f) {
    const std::string job = f.value("job_id", "");
    const uint64_t seq = f.value("seq", uint64_t{0});
    const bool last = f.value("last", false);
    auto fail = [&](const std::string& e) {
        ctx.send({{"op", "agent_data_error"}, {"job_id", job}, {"error", e}});
    };
    std::string bytes;
    try {
        bytes = b64_dec_impl(f.value("data_b64", ""));
    } catch (const std::exception& e) {
        return fail(std::string("bad chunk: ") + e.what());
    }
    std::lock_guard<std::mutex> lk(g_file_m);
    const fs::path dir = staging_dir(ctx, job);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path imp = dir / "import.tar.zst";
    std::ofstream out(imp,
                      std::ios::binary | (seq == 0 ? std::ios::trunc
                                                   : std::ios::app));
    if (!out) return fail("cannot write staging file");
    out.write(bytes.data(), std::streamsize(bytes.size()));
    if (last)
        ctx.send({{"op", "agent_data_done"},
                  {"job_id", job}, {"path", imp.string()}});
}

hwfleet::AgentSpec spec_from_import(const json& f) {
    hwfleet::AgentSpec spec;
    spec.id = f.value("agent_id", "");
    spec.name = f.value("name", "");
    spec.model = f.value("model", "");
    spec.memory = f.value("memory", "");
    spec.cpus = f.value("cpus", 0.0);
    spec.type = f.value("type", "");
    spec.host_agent = f.value("target_type", "") == "host";
    // The hub replays the FULL spawn spec (llm, prompt, secrets) on its
    // routine reconcile after the roster flip — a partial spec here is
    // enough to provision the runtime and restore the state dir.
    return spec;
}

void do_import(const Ctx& ctx, const json& f) {
    const std::string job = f.value("job_id", "");
    auto fail = [&](const std::string& e) {
        ctx.send({{"op", "agent_import_error"}, {"job_id", job}, {"error", e}});
    };
    const fs::path dir = staging_dir(ctx, job);
    const fs::path imp = dir / "import.tar.zst";
    std::error_code ec;
    if (!fs::exists(imp, ec))
        return fail("import archive missing (transfer incomplete?)");

    hwfleet::AgentSpec spec = spec_from_import(f);
    const bool host = spec.host_agent;
    fs::path root = ctx.ws;
    if (host) {
        root = hwrunner::host_workspace(spec.name);
        if (!hwrunner::host_agent_provisioned(spec.name)) {
            // Same auto-provision path as Fleet::spawn (Phase 72).
            std::string out;
            const int rc = hwrunner::run_as_user(
                "root",
                {"/usr/local/sbin/hivex-provision",
                 hwrunner::host_user(spec.name), hwrunner::current_user(),
                 hwrunner::hive_agent_binary(), root.string()},
                "", &out);
            if (rc != 0 || !hwrunner::host_agent_provisioned(spec.name))
                return fail("host provisioning failed: " + out);
        }
        std::string out;
        if (hwrunner::run_as_user(
                hwrunner::host_user(spec.name),
                {"tar", "--zstd", "-xf", imp.string(), "-C", root.string()},
                "", &out) != 0)
            return fail("extract failed: " + out);
    } else {
        std::string out;
        if (run_cmd({"tar", "--zstd", "-xf", imp.string(), "-C",
                     root.string()},
                    &out) != 0)
            return fail("extract failed: " + out);
    }

    try {
        if (ctx.hooks.spawn_agent) ctx.hooks.spawn_agent(spec);
    } catch (const std::exception& e) {
        return fail(std::string("spawn failed: ") + e.what());
    }
    fs::remove_all(dir, ec);  // target staging dies with the job
    ctx.send({{"op", "agent_import_done"}, {"job_id", job}});
}

void do_cleanup(const Ctx& ctx, const json& f) {
    const std::string job = f.value("job_id", "");
    const std::string agent = f.value("agent_id", "");
    const std::string name = f.value("name", "");
    std::error_code ec;
    const std::string hx = ctx.hooks.host_user_of
                               ? ctx.hooks.host_user_of(agent) : "";
    if (hx.empty()) {
        // Container source: fleet teardown stops the brain and destroys
        // the container + its state dir.
        if (ctx.hooks.remove_agent) {
            try { ctx.hooks.remove_agent(agent); }
            catch (const std::exception& e) {
                std::cerr << "migration cleanup: remove " << agent
                          << " failed: " << e.what() << "\n";
            }
        }
    } else {
        // Host source: remove ONLY the state dir as the hivex user. The
        // unix account itself stays — a rollback respawns on it.
        hwrunner::run_as_user(hx,
                              {"rm", "-rf",
                               (ctx.ws / ".hive-agent" / name).string()},
                              "", nullptr);
    }
    fs::remove_all(staging_dir(ctx, job), ec);
    ctx.send({{"op", "agent_cleanup_done"}, {"job_id", job}});
}

void do_abort(const Ctx& ctx, const json& f) {
    std::error_code ec;
    fs::remove_all(staging_dir(ctx, f.value("job_id", "")), ec);
}

}  // namespace

void run_op(const json& frame, const Ctx& ctx) {
    const std::string op = frame.value("op", "");
    try {
        if (op == "agent_export") return do_export(ctx, frame);
        if (op == "agent_data_begin") return do_data_begin(ctx, frame);
        if (op == "agent_data_chunk") return do_data_chunk(ctx, frame);
        if (op == "agent_import") return do_import(ctx, frame);
        if (op == "agent_cleanup") return do_cleanup(ctx, frame);
        if (op == "agent_migrate_abort") return do_abort(ctx, frame);
        std::cerr << "migration_ops: unknown op '" << op << "'\n";
    } catch (const std::exception& e) {
        const std::string job = frame.value("job_id", "");
        std::cerr << "migration_ops: " << op << " (" << job
                  << ") threw: " << e.what() << "\n";
        if (op == "agent_export" || op == "agent_data_begin")
            ctx.send({{"op", "agent_export_error"},
                      {"job_id", job}, {"error", e.what()}});
        else if (op == "agent_data_chunk")
            ctx.send({{"op", "agent_data_error"},
                      {"job_id", job}, {"error", e.what()}});
        else if (op == "agent_import")
            ctx.send({{"op", "agent_import_error"},
                      {"job_id", job}, {"error", e.what()}});
        // cleanup/abort: no ack defined — the hub deadline handles it.
    }
}

std::string b64_encode(const std::string& in) { return b64_enc_impl(in); }
std::string b64_decode(const std::string& in) { return b64_dec_impl(in); }

}  // namespace hwmigration
