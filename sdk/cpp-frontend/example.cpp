// Drives an engine through one session: build with any C++17 compiler and run
//   example <engine> [--tcp] <game file>
#include "ecp_frontend.hpp"

#include <cstdio>

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: example <engine> [--tcp] <game file>\n");
        return 2;
    }
    ecp::Frontend::Options opt;
    opt.command = {argv[1]};
    opt.name = "cpp-frontend-example";
    opt.version = "1.0.0";
    bool tcp = argc > 3 && std::string(argv[2]) == "--tcp";
    if (tcp) opt.transport = ecp::Frontend::Transport::Tcp;
    const char *game = argv[tcp ? 3 : 2];

    ecp::Frontend fe;
    std::string err;
    if (!fe.start(opt, &err)) {
        std::fprintf(stderr, "start: %s\n", err.c_str());
        return 1;
    }
    std::printf("engine: %s %s, level %lld\n", fe.engine()["name"].str().c_str(),
                fe.engine()["version"].str().c_str(), (long long)fe.engine()["level"].integer());

    ecp::Json launch = ecp::Json::object();
    launch.set("content", game).set("game_id", "cpp-example").set("title", "Example");
    ecp::Result r = fe.request("launch", launch, 30000);
    if (!r.ok) {
        std::fprintf(stderr, "launch: %s %s\n", r.error.code.c_str(), r.error.message.c_str());
        return 1;
    }
    ecp::Event ev;
    if (!fe.wait_for("first_frame", &ev, 60000)) return 1;
    std::printf("first frame\n");

    if (fe.has("pause")) std::printf("pause: %s\n", fe.request("pause").ok ? "ok" : "failed");
    if (fe.has("states")) {
        ecp::Json slot = ecp::Json::object();
        slot.set("slot", 3);
        ecp::Result saved = fe.request("save_state", slot, 10000);
        std::printf("save_state: %s, thumbnail %lld bytes of PNG\n", saved.ok ? "ok" : saved.error.code.c_str(),
                    (long long)ecp::base64_decode(saved["thumbnail"]["png"].str()).size());
        std::printf("load_state: %s\n", fe.request("load_state", slot).ok ? "ok" : "failed");
        std::printf("delete_state: %s\n", fe.request("delete_state", slot).ok ? "ok" : "failed");
    }
    if (fe.has("pause")) fe.request("resume");
    if (fe.has("perf") && fe.wait_for("perf", &ev, 3000))
        std::printf("perf: %.1f fps, speed %.2f\n", ev.msg["fps"].num(), ev.msg["speed"].num());

    r = fe.request("stop");
    std::printf("stop: %s\n", r.ok ? "ok" : r.error.code.c_str());
    if (fe.wait_for("stopped", &ev, 5000)) std::printf("stopped: %s\n", ev.msg["reason"].str().c_str());
    int code = -1;
    std::printf("exited: %s\n", fe.wait_exit(5000, &code) ? "yes" : "no");
    fe.close();
    return 0;
}
