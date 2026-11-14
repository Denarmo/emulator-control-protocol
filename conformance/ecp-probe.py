#!/usr/bin/env python3
"""ecp-probe: a test frontend for Emulator Control Protocol engines (SPEC section 10).

Starts an engine, drives every capability it lists, checks the answers and timings, and prints
a pass/fail table. Python 3.8+, standard library only. MIT licence.

    python ecp-probe.py [options] -- <engine> [engine args]

Exit code 0 when no check failed.
"""
import argparse
import base64
import json
import os
import queue
import re
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA_DIR = os.path.normpath(os.path.join(HERE, "..", "schema"))

STANDARD_KEYS = {
    "video.internal_scale": "range", "video.aspect": "choice", "video.widescreen_patches": "toggle",
    "video.renderer": "choice", "video.texture_filter": "choice", "video.frame_skip": "range",
    "video.vsync": "toggle", "audio.volume": "range", "audio.latency_ms": "range",
    "speed.limit": "toggle", "speed.fast_forward_factor": "range", "system.language": "choice",
}


# ---------------------------------------------------------------------------------------------
# A small JSON Schema (2020-12) validator: the keywords the ECP schemas use.

class Schemas:
    def __init__(self, root):
        self.root = root
        self.docs = {}

    def load(self, path):
        path = os.path.normpath(path)
        if path not in self.docs:
            with open(path, encoding="utf-8") as f:
                self.docs[path] = json.load(f)
        return self.docs[path]

    def has(self, rel):
        return os.path.exists(os.path.join(self.root, rel))

    def validate(self, inst, rel, fragment=""):
        path = os.path.join(self.root, rel)
        node, path = self._resolve(fragment, path) if fragment else (self.load(path), path)
        errors = []
        self._check(inst, node, path, "$", errors)
        return errors

    def _resolve(self, ref, base):
        url, _, frag = ref.partition("#")
        path = os.path.normpath(os.path.join(os.path.dirname(base), url)) if url else base
        node = self.load(path)
        for part in [p for p in frag.split("/") if p]:
            node = node[part]
        return node, path

    @staticmethod
    def _is(inst, t):
        if t == "null": return inst is None
        if t == "boolean": return isinstance(inst, bool)
        if t == "object": return isinstance(inst, dict)
        if t == "array": return isinstance(inst, list)
        if t == "string": return isinstance(inst, str)
        if isinstance(inst, bool): return False
        if t == "number": return isinstance(inst, (int, float))
        if t == "integer": return isinstance(inst, int) or (isinstance(inst, float) and inst.is_integer())
        return False

    @staticmethod
    def _eq(a, b):
        return type(a) is type(b) and a == b if isinstance(a, bool) or isinstance(b, bool) else a == b

    def _check(self, x, s, base, at, errs):
        if s is True: return
        if s is False:
            errs.append(at + ": not allowed")
            return
        n = len(errs)
        if "$ref" in s:
            node, path = self._resolve(s["$ref"], base)
            self._check(x, node, path, at, errs)
        if "type" in s:
            types = s["type"] if isinstance(s["type"], list) else [s["type"]]
            if not any(self._is(x, t) for t in types):
                errs.append("%s: expected %s, got %s" % (at, "/".join(types), json.dumps(x)[:60]))
                return
        if "const" in s and not self._eq(x, s["const"]):
            errs.append("%s: must be %s" % (at, json.dumps(s["const"])))
        if "enum" in s and not any(self._eq(x, e) for e in s["enum"]):
            errs.append("%s: %s is not one of %s" % (at, json.dumps(x)[:60], s["enum"]))
        if isinstance(x, dict):
            for k in s.get("required", []):
                if k not in x: errs.append("%s: missing %s" % (at, k))
            props = s.get("properties", {})
            for k, v in x.items():
                if k in props:
                    self._check(v, props[k], base, at + "." + k, errs)
                elif "additionalProperties" in s:
                    self._check(v, s["additionalProperties"], base, at + "." + k, errs)
        if isinstance(x, list):
            prefix = s.get("prefixItems", [])
            for i, v in enumerate(x):
                if i < len(prefix): self._check(v, prefix[i], base, "%s[%d]" % (at, i), errs)
                elif "items" in s: self._check(v, s["items"], base, "%s[%d]" % (at, i), errs)
            if len(x) < s.get("minItems", 0): errs.append(at + ": too few items")
            if "maxItems" in s and len(x) > s["maxItems"]: errs.append(at + ": too many items")
            if s.get("uniqueItems") and len({json.dumps(v, sort_keys=True) for v in x}) != len(x):
                errs.append(at + ": items repeat")
        if isinstance(x, str):
            if "pattern" in s and not re.search(s["pattern"], x):
                errs.append("%s: %r does not match %s" % (at, x[:60], s["pattern"]))
            if len(x) < s.get("minLength", 0): errs.append(at + ": too short")
        if self._is(x, "number"):
            if "minimum" in s and x < s["minimum"]: errs.append("%s: below %s" % (at, s["minimum"]))
            if "maximum" in s and x > s["maximum"]: errs.append("%s: above %s" % (at, s["maximum"]))
            if "exclusiveMinimum" in s and x <= s["exclusiveMinimum"]:
                errs.append("%s: must be above %s" % (at, s["exclusiveMinimum"]))
        for sub in s.get("allOf", []):
            self._check(x, sub, base, at, errs)
        for key, need in (("anyOf", None), ("oneOf", 1)):
            if key in s:
                ok = sum(1 for sub in s[key] if not self._sub(x, sub, base, at))
                if (need is None and ok == 0) or (need == 1 and ok != 1):
                    errs.append("%s: %s does not match" % (at, key))
        if "if" in s:
            branch = "then" if not self._sub(x, s["if"], base, at) else "else"
            if branch in s: self._check(x, s[branch], base, at, errs)
        return len(errs) == n

    def _sub(self, x, s, base, at):
        errs = []
        self._check(x, s, base, at, errs)
        return errs


# ---------------------------------------------------------------------------------------------
# PNG reading, enough to check a picture and compare two of them.

def png_decode(data):
    """Returns (width, height, pixel bytes or None); raises ValueError if it is not a PNG."""
    if data[:8] != b"\x89PNG\r\n\x1a\n": raise ValueError("no PNG signature")
    pos, idat, ihdr = 8, b"", None
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        crc = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])[0]
        if zlib.crc32(kind + body) & 0xFFFFFFFF != crc: raise ValueError("bad CRC in " + kind.decode())
        if kind == b"IHDR": ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT": idat += body
        elif kind == b"IEND": break
        pos += 12 + length
    if not ihdr: raise ValueError("no IHDR")
    w, h, depth, ctype, _, _, interlace = ihdr
    raw = zlib.decompress(idat)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or interlace or not channels: return w, h, None
    bpp, stride = channels, w * channels
    if len(raw) != h * (stride + 1): raise ValueError("IDAT has the wrong size")
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, bytes(out)


def check_picture(pic, what):
    """Returns (problem or None, pixels)."""
    if not isinstance(pic, dict): return "%s is missing" % what, None
    try:
        w, h, pixels = png_decode(base64.b64decode(pic.get("png", ""), validate=True))
    except Exception as e:  # noqa: BLE001 - any decoding failure is the finding
        return "%s is not a valid PNG: %s" % (what, e), None
    if (w, h) != (pic.get("width"), pic.get("height")):
        return "%s is %dx%d but says %sx%s" % (what, w, h, pic.get("width"), pic.get("height")), None
    return None, pixels


# ---------------------------------------------------------------------------------------------
# Channel and session

class Closed(Exception):
    pass


class Session:
    def __init__(self, cmd, transport, schemas, log_dir, label, adb=None):
        self.cmd, self.transport, self.schemas, self.adb = cmd, transport, schemas, adb
        self.inbox = queue.Queue()
        self.events = []      # (time, msg) for every event, in order
        self.requests = {}    # id -> type, to validate results
        self.schema_errors = []
        self.applets = 0
        self.next_id = 0
        self.closed = False
        self.sock = self.proc = None
        self.token = secrets.token_hex(32)
        self.log_path = os.path.join(log_dir, "engine-%s.log" % label)
        self.log = open(self.log_path, "wb")
        self.send_lock = threading.Lock()

    # -- start-up
    def start(self):
        args = list(self.cmd)
        env = dict(os.environ)
        server = None
        if self.transport in ("tcp", "adb"):
            server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            server.bind(("127.0.0.1", 0))
            server.listen(1)
            server.settimeout(10)
            port = server.getsockname()[1]
            args += ["--ecp", "tcp:127.0.0.1:%d" % port]
            env["ECP_TOKEN"] = self.token
        else:
            args += ["--ecp", "stdio"]
        self.started = time.monotonic()
        if self.adb:
            # The engine's loopback port on the device reaches this listener through adb reverse.
            self.adb.run("reverse", "tcp:%d" % port, "tcp:%d" % port)
            self.adb.run("shell", "am", "start", "-n", self.adb.component, "-a", "io.github.ecp.action.PLAY",
                         "--ei", "io.github.ecp.extra.PORT", str(port), "--es", "io.github.ecp.extra.TOKEN", self.token,
                         "--es", "io.github.ecp.extra.SESSION", "ecp-probe", "-d", self.adb.data_uri)
            self.port = port
        else:
            stdio = self.transport == "stdio"
            self.proc = subprocess.Popen(args, stdin=subprocess.PIPE if stdio else subprocess.DEVNULL,
                                         stdout=subprocess.PIPE if stdio else self.log,
                                         stderr=self.log, env=env, bufsize=0)
        self.auth = None
        self.connected = self.started
        if server:
            try:
                self.sock, _ = server.accept()
            finally:
                server.close()
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.connected = time.monotonic()
            self.auth = self._read_frame()
        self.inbox_reader = threading.Thread(target=self._reader, daemon=True)
        self.inbox_reader.start()

    def _read_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf)) if self.sock else self.proc.stdout.read(n - len(buf))
            if not chunk: raise Closed()
            buf += chunk
        return buf

    def _read_frame(self):
        (n,) = struct.unpack("<I", self._read_exact(4))
        if n > 16 * 1024 * 1024: raise Closed()
        return json.loads(self._read_exact(n).decode("utf-8"))

    def _reader(self):
        try:
            while True:
                msg = self._read_frame()
                self._validate(msg)
                self.inbox.put((time.monotonic(), msg))
        except (Closed, OSError, ValueError):
            self.inbox.put((time.monotonic(), None))

    def _validate(self, msg):
        if not isinstance(msg, dict):
            self.schema_errors.append("a frame is not a JSON object")
            return
        t = msg.get("type")
        if t == "hello": rel = "messages/hello.engine.schema.json"
        elif t == "result":
            req = self.requests.get(msg.get("id"))
            rel = "results/%s.schema.json" % req if req and self.schemas.has("results/%s.schema.json" % req) \
                else "messages/result.schema.json"
        elif isinstance(t, str) and self.schemas.has("messages/%s.schema.json" % t): rel = "messages/%s.schema.json" % t
        else:
            self.schema_errors.append("unknown message type %r" % t)
            return
        for e in self.schemas.validate(msg, rel)[:3]:
            self.schema_errors.append("%s: %s" % (t if t != "result" else "result of %s" % self.requests.get(msg.get("id")), e))

    # -- messages
    def send(self, obj):
        data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        frame = struct.pack("<I", len(data)) + data
        with self.send_lock:
            if self.sock: self.sock.sendall(frame)
            else:
                self.proc.stdin.write(frame)
                self.proc.stdin.flush()

    def _next(self, timeout):
        """One message or None on timeout; handles applet requests; raises Closed."""
        if self.closed: raise Closed()
        try:
            when, msg = self.inbox.get(timeout=max(0, timeout))
        except queue.Empty:
            return None
        if msg is None:
            self.closed = True
            raise Closed()
        if msg.get("type") == "applet_request" and "id" in msg:
            self.applets += 1
            self.send({"type": "result", "id": msg["id"], "ok": True, "text": "ecp-probe"})
        elif "id" not in msg or msg.get("type") != "result":
            self.events.append((when, msg))
        return when, msg

    def request(self, type_, timeout=5.0, **fields):
        """Returns (result, ms)."""
        self.next_id += 1
        rid = self.next_id
        self.requests[rid] = type_
        msg = {"type": type_, "id": rid}
        msg.update(fields)
        sent = time.monotonic()
        self.send(msg)
        deadline = sent + timeout
        while True:
            got = self._next(deadline - time.monotonic())
            if got is None: return None, (time.monotonic() - sent) * 1000
            when, m = got
            if m.get("type") == "result" and m.get("id") == rid:
                return m, (when - sent) * 1000

    def wait_event(self, type_, timeout, since=None, pred=None):
        """Waits for an event received at or after `since` (a monotonic time). Returns (msg, time)."""
        since = 0 if since is None else since
        deadline = time.monotonic() + timeout
        seen = 0
        while True:
            for when, m in self.events[seen:]:
                if when >= since and m.get("type") == type_ and (pred is None or pred(m)):
                    return m, when
            seen = len(self.events)
            got = self._next(deadline - time.monotonic())
            if got is None and time.monotonic() >= deadline: return None, None

    def drain(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self._next(deadline - time.monotonic())

    def close_channel(self):
        try:
            if self.sock: self.sock.shutdown(socket.SHUT_RDWR)
            else: self.proc.stdin.close()
        except OSError:
            pass

    def wait_exit(self, timeout):
        """The exit code, or None if the engine is still running. On Android an engine finishes its
        activity rather than its process, so there the channel closing counts as the exit."""
        if self.adb:
            deadline = time.monotonic() + timeout
            try:
                while time.monotonic() < deadline: self._next(deadline - time.monotonic())
            except Closed:
                return 0
            return None
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        if self.adb:
            self.adb.run("shell", "am", "force-stop", self.adb.component.split("/")[0])
            if getattr(self, "port", None): self.adb.run("reverse", "--remove", "tcp:%d" % self.port)
        if self.sock: self.sock.close()
        self.log.close()


class Adb:
    """An Android engine reached with adb: `component` is package/activity."""

    def __init__(self, component, serial, content, log_dir, ext):
        self.component, self.serial = component, serial
        self.exe = shutil.which("adb") or os.path.join(os.environ.get("ANDROID_HOME", os.environ.get(
            "ANDROID_SDK_ROOT", "")), "platform-tools", "adb")
        if content:
            self.data_uri = content
        else:
            local = os.path.join(log_dir, "ecp-probe-game." + ext)
            with open(local, "wb") as f: f.write(b"ECP probe dummy content\n")
            remote = "/data/local/tmp/ecp-probe-game." + ext
            self.run("push", local, remote)
            self.run("shell", "chmod", "644", remote)
            self.data_uri = "file://" + remote

    def run(self, *args):
        cmd = [self.exe] + (["-s", self.serial] if self.serial else []) + list(args)
        return subprocess.run(cmd, capture_output=True, timeout=30)


# ---------------------------------------------------------------------------------------------
# Checks

class Report:
    def __init__(self):
        self.rows = []

    def add(self, section, name, status, detail="", ms=None):
        self.rows.append((section, name, status, detail, ms))
        return status == "PASS"

    def ok(self, section, name, cond, detail="", ms=None, warn=False):
        return self.add(section, name, "PASS" if cond else ("WARN" if warn else "FAIL"), detail, ms)

    def failed(self):
        return any(r[2] == "FAIL" for r in self.rows)

    def print(self, out=sys.stdout):
        w1 = max(len(r[0]) for r in self.rows)
        w2 = max(len(r[1]) for r in self.rows)
        for section, name, status, detail, ms in self.rows:
            t = "%7.0f ms" % ms if ms is not None else " " * 10
            out.write("%-*s  %-*s  %-4s %s  %s\n" % (w1, section, w2, name, status, t, detail))
        counts = {s: sum(1 for r in self.rows if r[2] == s) for s in ("PASS", "FAIL", "WARN", "SKIP")}
        out.write("\n%d passed, %d failed, %d warnings, %d skipped\n" % (
            counts["PASS"], counts["FAIL"], counts["WARN"], counts["SKIP"]))


def err_code(res):
    return (res or {}).get("error", {}).get("code") if isinstance((res or {}).get("error"), dict) else None


def result_ok(res):
    return isinstance(res, dict) and res.get("ok") is True


def describe_detail(res):
    if res is None: return "no answer"
    if res.get("ok"): return ""
    e = res.get("error") or {}
    return "error %s %s" % (e.get("code"), e.get("detail") or e.get("message") or "")


def run_oneshots(cmd, desc, schemas, rep):
    sec = "one-shot"
    for command in ("describe", "schema", "status"):
        if command == "schema" and desc.get("settings") != "schema":
            rep.add(sec, command, "SKIP", "descriptor has no settings: schema")
            continue
        try:
            p = subprocess.run(cmd + ["--ecp-oneshot", command], input=b"{}", capture_output=True, timeout=10)
            out = json.loads(p.stdout.decode("utf-8"))
        except Exception as e:  # noqa: BLE001
            rep.add(sec, command, "FAIL", "no JSON result: %s" % e)
            continue
        errs = schemas.validate(out, "oneshot.schema.json", "#/$defs/" + command)
        rep.ok(sec, command, p.returncode == 0 and not errs,
               "exit %d %s" % (p.returncode, "; ".join(errs[:2])) if p.returncode or errs else "")


def run_session(args, cmd, desc, schemas, rep, transport, log_dir, content, launch_settings):
    sec = "session/" + transport
    s = Session(cmd, transport, schemas, log_dir, transport, getattr(args, "adb_conn", None))
    try:
        try:
            s.start()
        except socket.timeout:
            rep.add(sec, "connects within 10 s", "FAIL", "the engine never connected")
            return
        if transport in ("tcp", "adb"):
            rep.ok(sec, "connects within 10 s", s.connected - s.started <= 10, ms=(s.connected - s.started) * 1000)
            a = s.auth or {}
            rep.ok(sec, "first frame is auth with the token", a.get("type") == "auth" and a.get("token") == s.token,
                   "" if a.get("type") == "auth" else "first frame: %s" % json.dumps(a)[:80])
            for e in schemas.validate(a, "messages/auth.schema.json")[:2]: s.schema_errors.append("auth: " + e)

        hello, when = s.wait_event("hello", 10)
        if not hello:
            rep.add(sec, "hello within 10 s", "FAIL", "no hello")
            return
        rep.ok(sec, "hello within 10 s", True, ms=(when - s.connected) * 1000)
        rep.ok(sec, "hello speaks protocol 1", hello.get("protocol") == 1, "protocol %s" % hello.get("protocol"))
        if desc:
            same = all(hello.get(k) == desc.get(k) for k in ("id", "level", "capabilities"))
            rep.ok(sec, "hello matches --ecp-describe", same, "" if same else "id, level or capabilities differ")
        caps = set(hello.get("capabilities") or [])
        level = hello.get("level", 1)
        s.send({"type": "hello", "protocol": 1, "frontend": "ecp-probe", "version": "1.0.0",
                "level": level, "capabilities": sorted(caps)})

        res, ms = s.request("ecp_probe_unknown_request")
        rep.ok(sec, "unknown request answered unsupported", err_code(res) == "unsupported", describe_detail(res), ms)

        launch = {"content": content, "system": args.system or (hello.get("systems") or ["unknown"])[0],
                  "game_id": "ecp-probe-" + secrets.token_hex(4), "title": "ECP probe", "locale": "en",
                  "fullscreen": False}
        if level >= 2 and launch_settings: launch["settings"] = launch_settings
        if "menu" in caps: launch["menu_chord"] = ["l3", "r3"]
        t0 = time.monotonic()
        res, ms = s.request("launch", timeout=30, **launch)
        if not rep.ok(sec, "launch", result_ok(res), describe_detail(res), ms):
            return
        ff, when = s.wait_event("first_frame", args.first_frame_timeout, since=t0)
        if not rep.ok(sec, "first_frame", ff is not None, "" if ff else "none in %d s" % args.first_frame_timeout,
                      (when - t0) * 1000 if ff else None):
            return
        running, _ = s.wait_event("running", 5, since=t0)
        rep.ok(sec, "running", running is not None, "" if running else "no running event")

        if level >= 2:
            level2(args, s, rep, sec, caps, transport, log_dir)

        t1 = time.monotonic()
        res, ms = s.request("stop", timeout=5)
        rep.ok(sec, "stop answered within 5 s", result_ok(res), describe_detail(res), ms)
        stopped, _ = s.wait_event("stopped", 5, since=t1)
        rep.ok(sec, "stopped (reason frontend)", stopped is not None and stopped.get("reason") == "frontend",
               "" if stopped else "no stopped event")
        rc = s.wait_exit(max(0.1, 5 - (time.monotonic() - t1)))
        rep.ok(sec, "exits within 5 s of stop", rc is not None, "exit code %s" % rc if rc is not None else "still running",
               (time.monotonic() - t1) * 1000)
    except Closed:
        rep.add(sec, "channel stays open", "FAIL", "the engine closed the channel early; see %s" % s.log_path)
    finally:
        if s.applets: rep.add(sec, "applet_request answered", "PASS", "%d" % s.applets)
        rep.ok(sec, "messages match the schemas", not s.schema_errors, "; ".join(s.schema_errors[:3]))
        s.kill()


def level2(args, s, rep, sec, caps, transport, log_dir):
    def has(c): return c in caps

    def skip(name, why="not listed"): rep.add(sec, name, "SKIP", why)

    # menu hand-over: the engine has to ask by itself (the test engine: --menu-after <s>)
    if has("menu") and args.menu_timeout > 0:
        t = time.monotonic()
        mr, when = s.wait_event("menu_request", args.menu_timeout, since=t)
        if rep.ok(sec, "menu_request", mr is not None, "" if mr else "none in %g s" % args.menu_timeout):
            problem, _ = check_picture(mr.get("frame"), "frame")
            rep.ok(sec, "menu_request frame", problem is None, problem or "")
            p, _ = s.wait_event("paused", 1, since=t, pred=lambda m: m.get("reason") == "menu")
            rep.ok(sec, "paused (reason menu) with it", p is not None)
            t2 = time.monotonic()
            res, ms = s.request("resume")
            ev, _ = s.wait_event("resumed", 1, since=t2)
            rep.ok(sec, "resume after the menu", result_ok(res) and ev is not None, describe_detail(res), ms)
    elif has("menu"):
        skip("menu_request", "pass --menu-timeout and make the engine ask")
    else:
        skip("menu")

    if has("perf"):
        t = time.monotonic()
        p, when = s.wait_event("perf", 2.5, since=t)
        if rep.ok(sec, "perf within 2.5 s", p is not None, "" if p else "no perf event"):
            t = time.monotonic()
            s.drain(3.2)
            n = sum(1 for w, m in s.events if w >= t and m.get("type") == "perf")
            rep.ok(sec, "perf about once a second", 2 <= n <= 5, "%d in 3.2 s" % n, warn=True)
    else:
        skip("perf")

    paused = False
    if has("pause"):
        t = time.monotonic()
        res, ms = s.request("pause", x_ecp_probe_unknown_field=True)
        ev, when = s.wait_event("paused", 1, since=t, pred=lambda m: m.get("reason") == "frontend")
        ev_ms = (when - t) * 1000 if ev else None
        rep.ok(sec, "pause within 500 ms", result_ok(res) and ms <= 500 and ev is not None and ev_ms <= 500,
               describe_detail(res) or ("no paused event" if not ev else "event %.0f ms" % ev_ms), ms)
        paused = result_ok(res)
    else:
        skip("pause")

    if has("states"):
        states(s, rep, sec, has, paused)
    else:
        skip("states")

    if has("pause"):
        t = time.monotonic()
        res, ms = s.request("resume")
        ev, _ = s.wait_event("resumed", 1, since=t)
        rep.ok(sec, "resume", result_ok(res) and ev is not None, describe_detail(res) or ("" if ev else "no resumed event"), ms)

    if has("settings"):
        settings(s, rep, sec)
    else:
        skip("settings")

    simple = [
        ("reset", "reset", {"hard": False}),
        ("rewind", "set_rewind", {"on": True}), ("rewind", "set_rewind", {"on": False}),
        ("slow_motion", "set_speed", {"factor": 0.5}), ("slow_motion", "set_speed", {"factor": 1.0}),
        ("volume", "set_volume", {"percent": 50, "mute": False}), ("volume", "set_volume", {"percent": 100}),
        ("hud", "hud_level", {"level": 1}), ("hud", "hud_level", {"level": 0}),
        ("notify", "notify", {"text": "ecp-probe", "seconds": 1}),
        ("cheats", "set_cheats", {"cheats": []}),
    ]
    done = set()
    for cap, req, fields in simple:
        if not has(cap):
            if cap not in done: skip(cap)
            done.add(cap)
            continue
        done.add(cap)
        res, ms = s.request(req, **fields)
        rep.ok(sec, "%s %s" % (req, json.dumps(fields, separators=(",", ":"))), result_ok(res), describe_detail(res), ms)

    if has("fast_forward"):
        res, ms = s.request("set_speed", fast_forward=True)
        rep.ok(sec, "set_speed fast_forward on", result_ok(res), describe_detail(res), ms)
        if has("perf") and result_ok(res):
            t = time.monotonic()
            s.drain(2.2)
            speeds = [m.get("speed", 0) for w, m in s.events if w >= t + 1.0 and m.get("type") == "perf"]
            rep.ok(sec, "perf shows fast-forward", bool(speeds) and max(speeds) > 1.3,
                   "speed %s" % (speeds[-1] if speeds else "?"), warn=True)
        res, ms = s.request("set_speed", fast_forward=False)
        rep.ok(sec, "set_speed fast_forward off", result_ok(res), describe_detail(res), ms)
    else:
        skip("fast_forward")

    if has("screenshot"):
        res, ms = s.request("screenshot")
        problem, _ = check_picture((res or {}).get("image"), "image") if result_ok(res) else (describe_detail(res), None)
        rep.ok(sec, "screenshot inline", problem is None, problem or "", ms)
        path = os.path.join(log_dir, "screenshot-%s.png" % transport)
        res, ms = s.request("screenshot", path=path) if transport != "adb" else (None, None)
        if transport == "adb":
            rep.add(sec, "screenshot to a file", "SKIP", "desktop only")
        elif result_ok(res) and "image" in res:
            rep.add(sec, "screenshot to a file", "PASS", "answered inline (allowed)", ms)
        else:
            try:
                with open(path, "rb") as f: png_decode(f.read())
                rep.ok(sec, "screenshot to a file", result_ok(res) and res.get("path") == path, describe_detail(res), ms)
            except (OSError, ValueError) as e:
                rep.add(sec, "screenshot to a file", "FAIL", describe_detail(res) or str(e), ms)
    else:
        skip("screenshot")

    if has("discs"):
        res, ms = s.request("list_discs")
        discs = (res or {}).get("discs")
        rep.ok(sec, "list_discs", result_ok(res) and isinstance(discs, list), describe_detail(res), ms)
        if discs:
            cur = res.get("current", discs[0].get("index", 0))
            res, ms = s.request("set_disc", index=cur)
            rep.ok(sec, "set_disc (the current one)", result_ok(res), describe_detail(res), ms)
    else:
        skip("discs")


def states(s, rep, sec, has, paused):
    shot_before = None
    if has("screenshot") and paused:
        res, _ = s.request("screenshot")
        _, shot_before = check_picture((res or {}).get("image"), "image")
    now = time.time()
    res, ms = s.request("save_state", timeout=10, slot=1)
    good = result_ok(res) and res.get("slot") == 1 and abs(res.get("time", 0) - now) < 86400
    rep.ok(sec, "save_state slot 1", good, describe_detail(res) or ("" if good else "slot or time wrong"), ms)
    if has("state_thumbnails") and result_ok(res):
        problem, _ = check_picture(res.get("thumbnail"), "thumbnail")
        rep.ok(sec, "save_state thumbnail", problem is None, problem or "")
    res, ms = s.request("list_states", timeout=10)
    slots = [st.get("slot") for st in (res or {}).get("states", [])]
    rep.ok(sec, "list_states has slot 1", result_ok(res) and 1 in slots, describe_detail(res) or "slots %s" % slots, ms)

    if has("pause") and paused:
        s.request("resume")
        s.drain(0.5)
        s.request("pause")
    res, ms = s.request("load_state", timeout=10, slot=1)
    rep.ok(sec, "load_state slot 1", result_ok(res), describe_detail(res), ms)
    if shot_before is not None:
        s.drain(0.2)
        shot, _ = s.request("screenshot")
        _, after = check_picture((shot or {}).get("image"), "image")
        rep.ok(sec, "load_state restores the picture", after == shot_before,
               "" if after == shot_before else "the screenshot after loading differs from the one before saving",
               warn=True)

    res, ms = s.request("delete_state", slot=1)
    rep.ok(sec, "delete_state slot 1", result_ok(res), describe_detail(res), ms)
    res, _ = s.request("list_states", timeout=10)
    rep.ok(sec, "list_states after delete", result_ok(res) and 1 not in [st.get("slot") for st in res.get("states", [])])
    res, ms = s.request("load_state", slot=1)
    rep.ok(sec, "load_state empty slot is slot_empty", err_code(res) == "slot_empty", describe_detail(res) or "it loaded", ms)


def settings(s, rep, sec):
    res, ms = s.request("get_schema")
    schema = (res or {}).get("settings")
    if not rep.ok(sec, "get_schema", result_ok(res) and isinstance(schema, list), describe_detail(res), ms):
        return
    wrong = [st["key"] for st in schema if st.get("key") in STANDARD_KEYS and st.get("type") != STANDARD_KEYS[st["key"]]]
    rep.ok(sec, "standard keys have standard types", not wrong, ", ".join(wrong))
    res, ms = s.request("get_settings")
    values = (res or {}).get("values")
    if not rep.ok(sec, "get_settings", result_ok(res) and isinstance(values, dict), describe_detail(res), ms):
        return
    missing = [st["key"] for st in schema if st.get("key") not in values]
    rep.ok(sec, "get_settings covers the schema", not missing, ", ".join(missing[:5]), warn=True)
    bad = []
    for st in schema:
        if st.get("key") not in values: continue
        r, _ = s.request("set_setting", key=st["key"], value=values[st["key"]])
        if not result_ok(r) or r.get("applied") not in ("now", "restart", "no"):
            bad.append("%s (%s)" % (st["key"], describe_detail(r) or r.get("applied")))
    rep.ok(sec, "set_setting each to its value", not bad, "; ".join(bad[:3]) or "%d settings" % len(schema))
    r, ms = s.request("set_setting", key="ecp_probe.no_such_key", value=1)
    rep.ok(sec, "set_setting unknown key fails", r is not None and r.get("ok") is False, describe_detail(r) or "it was accepted", ms)


def run_close_test(args, cmd, schemas, rep, transport, log_dir, content):
    sec = "session/" + transport
    s = Session(cmd, transport, schemas, log_dir, transport + "-close", getattr(args, "adb_conn", None))
    try:
        s.start()
        hello, _ = s.wait_event("hello", 10)
        if not hello: return
        s.send({"type": "hello", "protocol": 1, "frontend": "ecp-probe", "version": "1.0.0", "level": 1,
                "capabilities": []})
        res, _ = s.request("launch", timeout=30, content=content, game_id="ecp-probe-close")
        if not result_ok(res): return
        s.wait_event("first_frame", args.first_frame_timeout)
        t = time.monotonic()
        s.close_channel()
        rc = s.wait_exit(5)
        rep.ok(sec, "exits within 5 s when the channel closes", rc is not None,
               "exit code %s" % rc if rc is not None else "still running", (time.monotonic() - t) * 1000)
    except (Closed, socket.timeout):
        rep.add(sec, "exits within 5 s when the channel closes", "FAIL", "could not set up the session")
    finally:
        s.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 usage="%(prog)s [options] -- <engine> [engine args]")
    ap.add_argument("--transport", choices=["stdio", "tcp", "both"], default="stdio")
    ap.add_argument("--content", help="game to launch (default: a dummy file with the engine's first extension)")
    ap.add_argument("--system", help="system id for launch (default: the engine's first)")
    ap.add_argument("--menu-timeout", type=float, default=0,
                    help="seconds to wait for the engine's menu_request (0: skip)")
    ap.add_argument("--first-frame-timeout", type=float, default=60)
    ap.add_argument("--keep-logs", action="store_true", help="keep the engine logs and screenshots")
    ap.add_argument("--adb", metavar="PACKAGE/ACTIVITY",
                    help="test an Android engine on the device adb sees, through adb reverse; --content is then a "
                         "URI on the device (default: a dummy file pushed to /data/local/tmp)")
    ap.add_argument("--adb-serial", help="adb device serial")
    ap.add_argument("--adb-ext", default="bin", help="extension of the dummy file pushed with --adb")
    ap.add_argument("engine", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    cmd = args.engine[1:] if args.engine and args.engine[0] == "--" else list(args.engine)
    if not cmd and not args.adb: ap.error("no engine command given")

    schemas = Schemas(SCHEMA_DIR)
    rep = Report()
    log_dir = tempfile.mkdtemp(prefix="ecp-probe-")

    if args.adb:
        args.adb_conn = Adb(args.adb, args.adb_serial, args.content, log_dir, args.adb_ext)
        rep.add("describe", "--ecp-describe", "SKIP", "Android: the descriptor comes in hello")
        run_session(args, [], None, schemas, rep, "adb", log_dir, "intent", {})
        run_close_test(args, [], schemas, rep, "adb", log_dir, "intent")
        return finish(args, rep, log_dir)

    desc = None
    try:
        p = subprocess.run(cmd + ["--ecp-describe"], capture_output=True, timeout=10, stdin=subprocess.DEVNULL)
        desc = json.loads(p.stdout.decode("utf-8"))
        errs = schemas.validate(desc, "descriptor.schema.json")
        rep.ok("describe", "--ecp-describe", p.returncode == 0 and not errs, "; ".join(errs[:3]))
    except Exception as e:  # noqa: BLE001
        rep.add("describe", "--ecp-describe", "FAIL", "no descriptor: %s" % e)
    exe = shutil.which(cmd[0]) or cmd[0]
    side = os.path.join(os.path.dirname(os.path.abspath(exe)), "ecp.json")
    if os.path.exists(side):
        with open(side, encoding="utf-8") as f: on_disk = json.load(f)
        rep.ok("describe", "ecp.json beside the executable", on_disk == desc or desc is None,
               "" if on_disk == desc else "differs from --ecp-describe")
    else:
        rep.add("describe", "ecp.json beside the executable", "SKIP", "not present")

    launch_settings = {}
    if desc and "oneshot" in desc.get("capabilities", []):
        run_oneshots(cmd, desc, schemas, rep)
        try:
            p = subprocess.run(cmd + ["--ecp-oneshot", "schema"], input=b"{}", capture_output=True, timeout=10)
            launch_settings = {st["key"]: st["default"] for st in json.loads(p.stdout)["settings"] if "default" in st}
        except Exception:  # noqa: BLE001 - reported by run_oneshots
            pass

    content = args.content
    if not content:
        ext = ((desc or {}).get("extensions") or ["bin"])[0]
        content = os.path.join(log_dir, "ecp-probe-game." + ext)
        with open(content, "wb") as f: f.write(b"ECP probe dummy content\n")

    for transport in (["stdio", "tcp"] if args.transport == "both" else [args.transport]):
        run_session(args, cmd, desc, schemas, rep, transport, log_dir, content, launch_settings)
        run_close_test(args, cmd, schemas, rep, transport, log_dir, content)

    return finish(args, rep, log_dir)


def finish(args, rep, log_dir):
    rep.print()
    if args.keep_logs or rep.failed():
        print("engine logs: %s" % log_dir)
    else:
        shutil.rmtree(log_dir, ignore_errors=True)
    return 1 if rep.failed() else 0


if __name__ == "__main__":
    sys.exit(main())
