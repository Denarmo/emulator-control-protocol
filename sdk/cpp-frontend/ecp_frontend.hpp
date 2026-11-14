// Emulator Control Protocol 1: frontend-side client. Header-only C++17, no dependencies.
// MIT licence, see LICENSE in the repository root.
//
//     ecp::Frontend fe;
//     ecp::Frontend::Options opt;
//     opt.command = {"path/to/engine"};
//     std::string err;
//     if (!fe.start(opt, &err)) { ... }
//     ecp::Json launch = ecp::Json::object();
//     launch.set("content", "game.iso").set("game_id", "abc");
//     ecp::Result r = fe.request("launch", launch, 30000);
//     ecp::Event ev;
//     while (fe.next_event(&ev, 100)) { if (ev.type == "first_frame") ... }
//     fe.request("stop");
//     fe.close();
//
// Results arrive by id; everything else the engine sends (events, applet_request) goes to the
// event queue. Windows (Winsock, job objects) and POSIX (fork/exec, PR_SET_PDEATHSIG on Linux).
#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32")
#endif
#else
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace ecp {

// ---------------------------------------------------------------------------------------------
// JSON value

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>; // keeps the order

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Number), num_(v) {}
    Json(int64_t v) : type_(Type::Number), num_(double(v)) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char *s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(Array a) : type_(Type::Array), arr_(std::make_shared<Array>(std::move(a))) {}
    static Json object() { Json j; j.type_ = Type::Object; j.obj_ = std::make_shared<Object>(); return j; }
    static Json array() { return Json(Array{}); }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_bool() const { return type_ == Type::Bool; }

    std::string str(const std::string &fallback = "") const { return is_string() ? str_ : fallback; }
    double num(double fallback = 0) const { return is_number() ? num_ : fallback; }
    int64_t integer(int64_t fallback = 0) const { return is_number() ? int64_t(num_) : fallback; }
    bool boolean(bool fallback = false) const { return is_bool() ? bool_ : fallback; }

    // Object member, or a null value. Never throws.
    const Json &operator[](const std::string &key) const {
        if (is_object())
            for (auto &kv : *obj_) if (kv.first == key) return kv.second;
        return null_value();
    }
    const Json &operator[](size_t i) const { return is_array() && i < arr_->size() ? (*arr_)[i] : null_value(); }
    bool has(const std::string &key) const { return !(*this)[key].is_null(); }
    size_t size() const { return is_array() ? arr_->size() : is_object() ? obj_->size() : 0; }
    const Array &items() const { static const Array none; return is_array() ? *arr_ : none; }
    const Object &members() const { static const Object none; return is_object() ? *obj_ : none; }

    Json &set(const std::string &key, Json v) {
        if (!is_object()) *this = object();
        else if (obj_.use_count() > 1) obj_ = std::make_shared<Object>(*obj_);
        for (auto &kv : *obj_) if (kv.first == key) { kv.second = std::move(v); return *this; }
        obj_->emplace_back(key, std::move(v));
        return *this;
    }
    Json &push(Json v) {
        if (!is_array()) *this = array();
        else if (arr_.use_count() > 1) arr_ = std::make_shared<Array>(*arr_);
        arr_->push_back(std::move(v));
        return *this;
    }

    std::string dump() const { std::string out; write(out); return out; }

    static bool parse(const std::string &text, Json *out) {
        const char *p = text.data(), *end = p + text.size();
        if (!parse_value(p, end, *out, 0)) return false;
        skip_ws(p, end);
        return p == end;
    }

private:
    static const Json &null_value() { static const Json n; return n; }

    void write(std::string &out) const {
        switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(num_)) { out += "null"; break; }
            char buf[32];
            if (num_ == std::floor(num_) && std::fabs(num_) < 9e15) snprintf(buf, sizeof buf, "%lld", (long long)num_);
            else snprintf(buf, sizeof buf, "%.17g", num_);
            for (char *c = buf; *c; c++) if (*c == ',') *c = '.';
            out += buf;
            break;
        }
        case Type::String: quote(str_, out); break;
        case Type::Array: {
            out += '[';
            for (size_t i = 0; i < arr_->size(); i++) { if (i) out += ','; (*arr_)[i].write(out); }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            for (size_t i = 0; i < obj_->size(); i++) {
                if (i) out += ',';
                quote((*obj_)[i].first, out);
                out += ':';
                (*obj_)[i].second.write(out);
            }
            out += '}';
            break;
        }
        }
    }

    static void quote(const std::string &s, std::string &out) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += char(c);
            }
        }
        out += '"';
    }

    static void skip_ws(const char *&p, const char *end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }

    static void utf8(std::string &o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | cp >> 6); o += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += char(0xE0 | cp >> 12); o += char(0x80 | (cp >> 6 & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
        else { o += char(0xF0 | cp >> 18); o += char(0x80 | (cp >> 12 & 0x3F)); o += char(0x80 | (cp >> 6 & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    }

    static bool hex4(const char *&p, const char *end, unsigned &v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++, p++) {
            char c = *p;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static bool parse_string(const char *&p, const char *end, std::string &o) {
        p++; // opening quote
        while (p < end && *p != '"') {
            unsigned char c = (unsigned char)*p++;
            if (c < 0x20) return false;
            if (c != '\\') { o += char(c); continue; }
            if (p >= end) return false;
            switch (*p++) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                unsigned cp, lo;
                if (!hex4(p, end, cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    if (!hex4(p, end, lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                utf8(o, cp);
                break;
            }
            default: return false;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }

    // Locale-independent, like the C SDK.
    static bool parse_number(const char *&p, const char *end, double &out) {
        double v = 0, sign = 1;
        if (p < end && *p == '-') sign = -1, p++;
        if (p >= end || *p < '0' || *p > '9') return false;
        while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (p < end && *p == '.') {
            double scale = 0.1;
            if (++p >= end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') v += (*p++ - '0') * scale, scale *= 0.1;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            int esign = 1, e = 0;
            p++;
            if (p < end && (*p == '+' || *p == '-')) esign = *p++ == '-' ? -1 : 1;
            if (p >= end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') { if (e < 10000) e = e * 10 + (*p - '0'); p++; }
            v *= std::pow(10.0, esign * e);
        }
        out = sign * v;
        return true;
    }

    static bool parse_value(const char *&p, const char *end, Json &out, int depth) {
        skip_ws(p, end);
        if (p >= end || depth > 64) return false;
        char c = *p;
        if (c == '{') {
            out = object();
            p++;
            skip_ws(p, end);
            if (p < end && *p == '}') { p++; return true; }
            for (;;) {
                std::string key;
                Json v;
                skip_ws(p, end);
                if (p >= end || *p != '"' || !parse_string(p, end, key)) return false;
                skip_ws(p, end);
                if (p >= end || *p++ != ':') return false;
                if (!parse_value(p, end, v, depth + 1)) return false;
                out.obj_->emplace_back(std::move(key), std::move(v));
                skip_ws(p, end);
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (c == '[') {
            out = array();
            p++;
            skip_ws(p, end);
            if (p < end && *p == ']') { p++; return true; }
            for (;;) {
                Json v;
                if (!parse_value(p, end, v, depth + 1)) return false;
                out.arr_->push_back(std::move(v));
                skip_ws(p, end);
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (c == '"') {
            std::string s;
            if (!parse_string(p, end, s)) return false;
            out = Json(std::move(s));
            return true;
        }
        if (end - p >= 4 && !memcmp(p, "true", 4)) { p += 4; out = Json(true); return true; }
        if (end - p >= 5 && !memcmp(p, "false", 5)) { p += 5; out = Json(false); return true; }
        if (end - p >= 4 && !memcmp(p, "null", 4)) { p += 4; out = Json(); return true; }
        double d;
        if (!parse_number(p, end, d)) return false;
        out = Json(d);
        return true;
    }

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::shared_ptr<Array> arr_;
    std::shared_ptr<Object> obj_;
};

// ---------------------------------------------------------------------------------------------
// Messages

struct Error {
    std::string code, message, detail;
};

struct Result {
    bool ok = false;
    Error error; // code "timeout" or "closed" when no answer arrived
    Json msg;    // the whole result message, for its fields
    const Json &operator[](const std::string &key) const { return msg[key]; }
};

struct Event {
    std::string type;
    Json msg;
    std::chrono::steady_clock::time_point when;
};

inline std::vector<uint8_t> base64_decode(const std::string &s) {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;
        acc = acc << 6 | uint32_t(v);
        if ((bits += 6) >= 8) out.push_back(uint8_t(acc >> (bits -= 8)));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Frontend

class Frontend {
public:
    enum class Transport { Stdio, Tcp };

    struct Options {
        std::vector<std::string> command; // the engine and its own arguments
        Transport transport = Transport::Stdio;
        std::string name = "frontend", version = "0.0.0";
        int level = 2;                    // the highest level this frontend uses
        std::vector<std::string> capabilities; // empty: all the engine lists
        std::string log_path;             // the engine's standard error; empty: inherited
        int hello_timeout_ms = 10000;
    };

    Frontend() = default;
    Frontend(const Frontend &) = delete;
    Frontend &operator=(const Frontend &) = delete;
    ~Frontend() { close(0); }

    // Starts the engine, waits for its hello (and auth on TCP) and answers it.
    bool start(const Options &opt, std::string *err) {
        std::string dummy;
        if (!err) err = &dummy;
        opt_ = opt;
        std::vector<std::string> args = opt.command;
        if (args.empty()) { *err = "no engine command"; return false; }
        std::string token;
        if (opt.transport == Transport::Tcp) {
            token = random_token();
            int port = 0;
            if (!listen_loopback(&port, err)) return false;
            args.push_back("--ecp");
            args.push_back("tcp:127.0.0.1:" + std::to_string(port));
        } else {
            args.push_back("--ecp");
            args.push_back("stdio");
        }
        if (!spawn(args, token, err)) return false;
        if (opt.transport == Transport::Tcp && !accept_and_auth(token, err)) return false;
        open_ = true;
        reader_ = std::thread([this] { read_loop(); });

        Event hello;
        if (!wait_for("hello", &hello, opt.hello_timeout_ms)) {
            *err = "no hello within " + std::to_string(opt.hello_timeout_ms / 1000) + " s";
            return false;
        }
        engine_ = hello.msg;
        if (engine_["protocol"].integer() != 1) { *err = "the engine speaks another protocol"; return false; }
        Json caps = Json::array();
        for (auto &c : engine_["capabilities"].items()) {
            bool want = opt.capabilities.empty();
            for (auto &w : opt.capabilities) want = want || w == c.str();
            if (want) caps.push(c), caps_.push_back(c.str());
        }
        int level = int(engine_["level"].integer(1));
        Json mine = Json::object();
        mine.set("type", "hello").set("protocol", 1).set("frontend", opt.name).set("version", opt.version)
            .set("level", level < opt.level ? level : opt.level).set("capabilities", caps);
        return send(mine);
    }

    const Json &engine() const { return engine_; } // the engine's hello (its descriptor)
    bool has(const std::string &cap) const {
        for (auto &c : caps_) if (c == cap) return true;
        return false;
    }
    bool is_open() const { return open_; }

    // Sends a request and waits for its result.
    Result request(const std::string &type, Json fields = Json::object(), int timeout_ms = 5000) {
        Result r;
        int64_t id;
        {
            std::lock_guard<std::mutex> lk(mu_);
            id = ++next_id_;
        }
        fields.set("type", type).set("id", id);
        if (!send(fields)) { r.error.code = "closed"; return r; }
        std::unique_lock<std::mutex> lk(mu_);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!results_.count(id)) {
            if (!open_) { r.error.code = "closed"; return r; }
            if (cv_.wait_until(lk, deadline) == std::cv_status::timeout && !results_.count(id)) {
                r.error.code = "timeout";
                return r;
            }
        }
        r.msg = std::move(results_[id]);
        results_.erase(id);
        r.ok = r.msg["ok"].boolean();
        if (!r.ok) {
            const Json &e = r.msg["error"];
            r.error = {e["code"].str("internal"), e["message"].str(), e["detail"].str()};
        }
        return r;
    }

    // Answers a request the engine sent (applet_request).
    bool reply(int64_t id, Json fields = Json::object(), bool ok = true) {
        fields.set("type", "result").set("id", id).set("ok", ok);
        return send(fields);
    }

    // Pops the next event; false if none arrived within timeout_ms.
    bool next_event(Event *out, int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return !events_.empty() || !open_; }) ||
            events_.empty())
            return false;
        *out = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    // Waits for an event of one type, keeping the others queued in order.
    bool wait_for(const std::string &type, Event *out, int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu_);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            for (auto it = events_.begin(); it != events_.end(); ++it)
                if (it->type == type) {
                    *out = std::move(*it);
                    events_.erase(it);
                    return true;
                }
            if (!open_ || cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
                for (auto it = events_.begin(); it != events_.end(); ++it)
                    if (it->type == type) { *out = std::move(*it); events_.erase(it); return true; }
                return false;
            }
        }
    }

    // Closes the channel; the engine then saves and exits. Ends it after grace_ms.
    void close(int grace_ms = 5000) {
        shutdown_channel();
        if (!wait_exit(grace_ms, nullptr)) kill();
        if (reader_.joinable()) reader_.join(); // returns once the engine's end is closed
        release();
    }

    // True once the engine has exited, with its exit code.
    bool wait_exit(int timeout_ms, int *code) {
#ifdef _WIN32
        if (!proc_) return true;
        if (WaitForSingleObject(proc_, DWORD(timeout_ms)) != WAIT_OBJECT_0) return false;
        DWORD c = 0;
        GetExitCodeProcess(proc_, &c);
        if (code) *code = int(c);
        return true;
#else
        if (pid_ <= 0) { if (code) *code = exit_code_; return true; }
        for (int waited = 0;; waited += 10) {
            int status;
            pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == pid_) {
                exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                pid_ = -1;
                if (code) *code = exit_code_;
                return true;
            }
            if (waited >= timeout_ms) return false;
            usleep(10000);
        }
#endif
    }

private:
#ifdef _WIN32
    using Sock = SOCKET;
    static constexpr Sock kBad = INVALID_SOCKET;
    static void close_sock(Sock s) { closesocket(s); }
#else
    using Sock = int;
    static constexpr Sock kBad = -1;
    static void close_sock(Sock s) { ::close(s); }
#endif

    static std::string random_token() {
        std::random_device rd;
        static const char hex[] = "0123456789abcdef";
        std::string t;
        for (int i = 0; i < 32; i++) {
            unsigned b = rd() & 0xFF;
            t += hex[b >> 4];
            t += hex[b & 15];
        }
        return t;
    }

    bool listen_loopback(int *port, std::string *err) {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa)) { *err = "WSAStartup failed"; return false; }
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof a;
        if (listener_ == kBad || bind(listener_, (sockaddr *)&a, sizeof a) || listen(listener_, 1) ||
            getsockname(listener_, (sockaddr *)&a, &len)) {
            *err = "cannot listen on loopback";
            return false;
        }
        *port = ntohs(a.sin_port);
        return true;
    }

    bool accept_and_auth(const std::string &token, std::string *err) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener_, &set);
        timeval tv{10, 0};
        if (select(int(listener_) + 1, &set, nullptr, nullptr, &tv) <= 0) {
            *err = "the engine did not connect within 10 s";
            return false;
        }
        sock_ = accept(listener_, nullptr, nullptr);
        close_sock(listener_);
        listener_ = kBad;
        if (sock_ == kBad) { *err = "accept failed"; return false; }
        int one = 1;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        Json auth;
        if (!read_frame(&auth, 10000) || auth["type"].str() != "auth" || !same_token(auth["token"].str(), token)) {
            *err = "the engine's first frame was not the right token";
            close_sock(sock_);
            sock_ = kBad;
            return false;
        }
        return true;
    }

    static bool same_token(const std::string &a, const std::string &b) {
        if (a.size() != b.size()) return false;
        unsigned diff = 0;
        for (size_t i = 0; i < a.size(); i++) diff |= unsigned(a[i] ^ b[i]);
        return diff == 0;
    }

#ifdef _WIN32
    static std::wstring widen(const std::string &s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    }

    static void quote_arg(const std::wstring &a, std::wstring &cmd) {
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) { cmd += a; return; }
        cmd += L'"';
        size_t slashes = 0;
        for (wchar_t c : a) {
            if (c == L'\\') { slashes++; continue; }
            cmd.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            cmd += c;
        }
        cmd.append(slashes * 2, L'\\');
        cmd += L'"';
    }

    bool spawn(const std::vector<std::string> &args, const std::string &token, std::string *err) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE child_in = nullptr, child_out = nullptr, log = nullptr;
        bool stdio = opt_.transport == Transport::Stdio;
        if (stdio) {
            if (!CreatePipe(&child_in, &in_w_, &sa, 0) || !CreatePipe(&out_r_, &child_out, &sa, 0)) {
                *err = "CreatePipe failed";
                return false;
            }
            SetHandleInformation(in_w_, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(out_r_, HANDLE_FLAG_INHERIT, 0);
        }
        if (!opt_.log_path.empty())
            log = CreateFileW(widen(opt_.log_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdio ? child_in : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = stdio ? child_out : (log ? log : GetStdHandle(STD_OUTPUT_HANDLE));
        si.hStdError = log && log != INVALID_HANDLE_VALUE ? log : GetStdHandle(STD_ERROR_HANDLE);

        std::wstring cmd;
        for (size_t i = 0; i < args.size(); i++) { if (i) cmd += L' '; quote_arg(widen(args[i]), cmd); }
        // The environment: ours plus ECP_TOKEN on TCP.
        std::wstring env;
        wchar_t *block = GetEnvironmentStringsW();
        for (wchar_t *p = block; *p; p += wcslen(p) + 1)
            if (_wcsnicmp(p, L"ECP_TOKEN=", 10)) env.append(p).push_back(L'\0');
        FreeEnvironmentStringsW(block);
        if (!token.empty()) env.append(L"ECP_TOKEN=" + widen(token)).push_back(L'\0');
        env.push_back(L'\0');

        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_) SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                                 CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, &env[0], nullptr, &si, &pi);
        if (child_in) CloseHandle(child_in);
        if (child_out) CloseHandle(child_out);
        if (log && log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (!ok) { *err = "cannot start " + args[0]; return false; }
        if (job_) AssignProcessToJobObject(job_, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        return true;
    }
#else
    bool spawn(const std::vector<std::string> &args, const std::string &token, std::string *err) {
        bool stdio = opt_.transport == Transport::Stdio;
        int in[2] = {-1, -1}, out[2] = {-1, -1};
        if (stdio && (pipe(in) || pipe(out))) { *err = "pipe failed"; return false; }
        pid_t parent = getpid();
        pid_ = fork();
        if (pid_ < 0) { *err = "fork failed"; return false; }
        if (pid_ == 0) {
#ifdef __linux__
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) _exit(127);
#endif
            (void)parent;
            if (stdio) {
                dup2(in[0], 0);
                dup2(out[1], 1);
                ::close(in[0]), ::close(in[1]), ::close(out[0]), ::close(out[1]);
            }
            if (!opt_.log_path.empty()) {
                int fd = open(opt_.log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) { dup2(fd, 2); if (!stdio) dup2(fd, 1); ::close(fd); }
            }
            if (!token.empty()) setenv("ECP_TOKEN", token.c_str(), 1);
            std::vector<char *> argv;
            for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        if (stdio) {
            ::close(in[0]), ::close(out[1]);
            in_w_ = in[1], out_r_ = out[0];
            signal(SIGPIPE, SIG_IGN);
        }
        return true;
    }
#endif

    bool write_all(const void *data, size_t n) {
        const char *p = static_cast<const char *>(data);
        while (n) {
#ifdef _WIN32
            int k;
            if (sock_ != kBad) k = ::send(sock_, p, int(n), 0);
            else { DWORD w = 0; k = WriteFile(in_w_, p, DWORD(n), &w, nullptr) ? int(w) : -1; }
#else
            ssize_t k = sock_ != kBad ? ::send(sock_, p, n, MSG_NOSIGNAL) : ::write(in_w_, p, n);
            if (k < 0 && errno == EINTR) continue;
#endif
            if (k <= 0) return false;
            p += k, n -= size_t(k);
        }
        return true;
    }

    bool send(const Json &msg) {
        std::string body = msg.dump();
        uint32_t n = uint32_t(body.size());
        unsigned char hdr[4] = {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
        std::lock_guard<std::mutex> lk(send_mu_);
        return write_all(hdr, 4) && write_all(body.data(), body.size());
    }

    bool read_exact(void *dst, size_t n, int timeout_ms) {
        char *p = static_cast<char *>(dst);
        while (n) {
#ifdef _WIN32
            int k;
            if (sock_ != kBad) {
                if (timeout_ms >= 0) {
                    fd_set set;
                    FD_ZERO(&set);
                    FD_SET(sock_, &set);
                    timeval tv{timeout_ms / 1000, timeout_ms % 1000 * 1000};
                    if (select(0, &set, nullptr, nullptr, &tv) <= 0) return false;
                }
                k = recv(sock_, p, int(n), 0);
            } else {
                DWORD got = 0;
                k = ReadFile(out_r_, p, DWORD(n), &got, nullptr) ? int(got) : -1;
            }
#else
            int fd = sock_ != kBad ? sock_ : out_r_;
            if (timeout_ms >= 0) {
                pollfd pfd{fd, POLLIN, 0};
                if (poll(&pfd, 1, timeout_ms) <= 0) return false;
            }
            ssize_t k = ::read(fd, p, n);
            if (k < 0 && errno == EINTR) continue;
#endif
            if (k <= 0) return false;
            p += k, n -= size_t(k);
        }
        return true;
    }

    bool read_frame(Json *out, int timeout_ms) {
        unsigned char hdr[4];
        if (!read_exact(hdr, 4, timeout_ms)) return false;
        uint32_t n = uint32_t(hdr[0]) | uint32_t(hdr[1]) << 8 | uint32_t(hdr[2]) << 16 | uint32_t(hdr[3]) << 24;
        if (n > 16u * 1024 * 1024) return false;
        std::string body(n, '\0');
        if (n && !read_exact(&body[0], n, timeout_ms)) return false;
        return Json::parse(body, out);
    }

    void read_loop() {
        for (;;) {
            Json msg;
            if (!read_frame(&msg, -1)) break;
            if (!msg.is_object()) continue;
            std::lock_guard<std::mutex> lk(mu_);
            if (msg["type"].str() == "result" && msg["id"].is_number()) {
                results_[msg["id"].integer()] = std::move(msg);
            } else {
                Event ev{msg["type"].str(), std::move(msg), std::chrono::steady_clock::now()};
                events_.push_back(std::move(ev));
            }
            cv_.notify_all();
        }
        std::lock_guard<std::mutex> lk(mu_);
        open_ = false;
        cv_.notify_all();
    }

    void shutdown_channel() {
#ifdef _WIN32
        if (sock_ != kBad) shutdown(sock_, SD_BOTH);
        if (in_w_) { CloseHandle(in_w_); in_w_ = nullptr; }
        // A pending ReadFile on the pipe ends when the engine exits and its end closes.
#else
        if (sock_ != kBad) shutdown(sock_, SHUT_RDWR);
        if (in_w_ >= 0) { ::close(in_w_); in_w_ = -1; }
#endif
    }

    void kill() {
#ifdef _WIN32
        if (proc_) TerminateProcess(proc_, 1);
#else
        if (pid_ > 0) { ::kill(pid_, SIGKILL); wait_exit(1000, nullptr); }
#endif
    }

    void release() {
        if (sock_ != kBad) { close_sock(sock_); sock_ = kBad; }
        if (listener_ != kBad) { close_sock(listener_); listener_ = kBad; }
#ifdef _WIN32
        if (out_r_) { CloseHandle(out_r_); out_r_ = nullptr; }
        if (proc_) { CloseHandle(proc_); proc_ = nullptr; }
        if (job_) { CloseHandle(job_); job_ = nullptr; }
#else
        if (out_r_ >= 0) { ::close(out_r_); out_r_ = -1; }
#endif
    }

    Options opt_;
    Json engine_;
    std::vector<std::string> caps_;
    std::atomic<bool> open_{false};
    std::thread reader_;
    std::mutex mu_, send_mu_;
    std::condition_variable cv_;
    std::map<int64_t, Json> results_;
    std::deque<Event> events_;
    int64_t next_id_ = 0;
    Sock sock_ = kBad, listener_ = kBad;
#ifdef _WIN32
    HANDLE in_w_ = nullptr, out_r_ = nullptr, proc_ = nullptr, job_ = nullptr;
#else
    int in_w_ = -1, out_r_ = -1;
    pid_t pid_ = -1;
    int exit_code_ = -1;
#endif
};

} // namespace ecp
