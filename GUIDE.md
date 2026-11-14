# Adding ECP to an emulator

This guide is for emulator developers. It covers the changes ECP needs in an existing emulator:
the main loop, mapping your pause, save-state and settings code to ECP requests, the Android
manifest, the menu hand-over, and testing with `ecp-probe`. [SPEC.md](SPEC.md) is the reference.
Here the SDKs do the protocol work and you connect them to code you already have.

| You have | Use |
|---|---|
| A desktop emulator in C or C++ | `sdk/c` (`ecp.h`, `ecp.c`): add the two files to your build |
| An Android app with a Java/Kotlin activity and a native core | `sdk/android` (`EcpEngine`) in the activity, and JNI calls into the core you already have |
| An Android app whose core runs everything | `sdk/c` inside the native code, with the port and token passed down through JNI |
| A frontend | `sdk/cpp-frontend` (header-only C++17) or `sdk/android` (`EcpFrontend`) |

PSX2's [ECP.md](https://github.com/Denarmo/PSX2/blob/ecp/ECP.md) is a worked example: a
PCSX2-based Android emulator at level 2, with each request mapped to its core and the reason for
every capability it leaves out.

Start at level 1 (launch, lifecycle, stop) and add level 2 controls one capability at a time.
List a capability only when its requests work, because the frontend greys out anything you leave
out. `examples/test-engine` implements every capability except `applets`. Read it next to this
guide.

## 1. The descriptor

Write the descriptor first (SPEC section 2). The frontend reads it before it starts you, and your
hello sends it again:

```json
{
  "ecp": 1,
  "id": "com.izzy2lost.psx2",
  "name": "PSX2",
  "version": "1.0.0",
  "level": 2,
  "systems": ["ps2"],
  "extensions": ["iso", "chd", "cso", "bin"],
  "capabilities": ["pause", "menu", "states", "state_thumbnails", "settings", "fast_forward", "perf"],
  "needs": ["bios"],
  "settings": "schema"
}
```

`systems` takes ids from [schema/systems.md](schema/systems.md). `id` is your Android package name, or
a reverse-DNS name on desktop. `schema/descriptor.schema.json` validates the file.

## 2. Desktop engines (sdk/c)

### 2.1 Start-up

`ecp_config_parse()` takes the ECP options out of `argv` before your own parser sees them.
Without `--ecp`, carry on as you do today (level 0).

```c
#include "ecp.h"

static const char *const systems[] = {"ps2", NULL};
static const char *const exts[] = {"iso", "chd", "cso", NULL};
static const char *const caps[] = {"pause", "menu", "states", "state_thumbnails", "settings", "perf", NULL};
static const char *const needs[] = {"bios", NULL};
static const ecp_descriptor desc = {"org.example.ps2emu", "Example PS2", "1.4.0", 2,
                                    systems, exts, caps, needs, 1};

int main(int argc, char **argv) {
    ecp_config cfg;
    if (ecp_config_parse(&cfg, &argc, argv)) { fprintf(stderr, "%s\n", cfg.error); return 2; }
    if (cfg.describe) return ecp_print_descriptor(&desc);      /* --ecp-describe */
    if (cfg.oneshot[0]) return run_oneshot(cfg.oneshot);       /* section 7, optional */
    if (cfg.transport == ECP_NONE) return run_like_before(argc, argv);

    ecp_handlers h = {0};
    h.launch = on_launch, h.stop = on_stop, h.pause = on_pause, h.resume = on_resume;
    h.save_state = on_save_state, h.load_state = on_load_state;
    h.list_states = on_list_states, h.delete_state = on_delete_state;
    h.get_schema = on_get_schema, h.get_settings = on_get_settings, h.set_setting = on_set_setting;
    h.closed = on_closed;
    char err[256];
    ecp_conn *c = ecp_open(&cfg, &desc, &h, app, err, sizeof err);
    if (!c) { fprintf(stderr, "ecp: %s\n", err); return 1; }
    ...
}
```

Rules from SPEC section 9 that affect start-up:

- **Nothing interactive before `launch`.** On desktop that means no window: initialise what you
  need, but create the window when the game arrives. (On Android the activity is already on
  screen; see 3.1.)
- **No profile picker or first-run wizard.** If something is missing, the frontend learns it from
  `launch`'s error (`bios_missing`, `keys_missing`) or from the `status` one-shot.
- **Standard output belongs to the channel.** On stdio, `ecp_open()` moves your `stdout` to
  `stderr`, so stray `printf()`s go to the log and cannot corrupt the channel. Do not write to
  the channel's handles yourself.

Ship `ecp.json` next to the executable. The test engine's CMake file writes it from
`--ecp-describe` after each build.

### 2.2 The main loop

`ecp_poll()` never blocks longer than the timeout you give it, so call it once per frame:

```c
while (!app->quit) {
    if (ecp_poll(c, 0) < 0) break;   /* channel gone: save and exit */
    run_one_frame();
    present();
}
ecp_close(c);
```

If your frame loop sleeps, poll with the time left until the next frame:
`ecp_poll(c, ms_until_next_frame)`. Requests are then handled as soon as they arrive. The
handlers run on the thread that calls `ecp_poll()`. If your UI thread and emulation thread are
separate, poll from the UI thread (or a thread of its own) and hand work to the emulation thread
as you already do for your own menus.

### 2.3 Answering from another thread

Every request gets exactly one answer. It does not have to be immediate: keep `r->id` and answer
when the work is done. All `ecp_send*` / `ecp_reply*` functions are thread-safe.

Put blocking emulator calls (a state save can take seconds) on your emulation thread or an
executor, never inside the handler that the poll loop runs. Keep `stop` on a path of its own:
answer it at once and shut down, even while an earlier request is still working. The frontend
ends the engine 5 s after `stop`, whatever is still queued.

```c
static void on_save_state(void *user, ecp_conn *c, const ecp_request *r) {
    int slot = (int)ecp_json_int(ecp_json_get(r->msg, "slot"), -1);
    if (slot < 0 || slot > 99) { ecp_reply_error(c, r->id, "protocol", NULL, "slot 0-99"); return; }
    queue_on_cpu_thread(save_job, c, r->id, slot);   /* r->msg is only valid during the call */
}

static void save_job(ecp_conn *c, int64_t id, int slot) {   /* on the emulation thread */
    if (!save_state_to(state_path(game_id, slot))) {
        ecp_reply_error(c, id, "internal", "The state could not be saved.", NULL);
        return;
    }
    ecp_writer w;
    ecp_msg_result(&w, id);                      /* {"type":"result","id":..,"ok":true */
    ecp_w_kint(&w, "slot", slot);
    ecp_w_kint(&w, "time", (int64_t)time(NULL));
    ecp_w_picture_rgba(&w, "thumbnail", fb->pixels, fb->w, fb->h, fb->stride, 0);
    ecp_send(c, &w);
}
```

### 2.4 Mapping your code to the requests

Most emulators already have every piece. The work is mostly wiring:

| Request | Usually maps to | Notes |
|---|---|---|
| `launch` | your "boot game" path | Answer once loading has begun, then send `loading`, `first_frame` and `running`. `first_frame` is the first frame the game itself drew, not the black frame your renderer shows while it boots: hook the point where your own loading screen goes away. Refuse with a typed error: `bios_missing`, `content_invalid`, `gpu_unsupported`. `content` is the path on desktop; on Android it is `"intent"`. |
| `stop` | "shut down VM" + save SRAM | Answer, send `stopped` (`reason:"frontend"`), exit. The frontend kills you 5 s later. Keep `stop` off the queue of slow requests (see 2.3). |
| `pause` / `resume` | your pause toggle | Send `paused` (`frontend`) / `resumed`. Do not report your internal pauses, such as the one your core takes around a state save or load: `paused` is for pauses the frontend can see. `resume` also ends the menu hand-over, so it must work when you list `menu` without `pause`. On `resume`, ignore buttons still held until they are released (see 3.4). |
| `save_state` / `load_state` | your slot functions | ECP slots are 0-99. Slot 0 is the frontend's resume slot: it saves there when the player leaves and loads it when they come back. Key states by `game_id`, or by your own per-game identity (disc serial, CRC) if you already have one. The slots may be the ones your own UI shows, so PCSX2-style `<serial>.<crc>.NN` states work as they are. `load_state` on an empty slot fails with `slot_empty`; a state from another version fails with `state_incompatible`. A state saved before `first_frame` may have no thumbnail. |
| `list_states` / `delete_state` | a directory listing / `remove()` | Thumbnails are optional and need `state_thumbnails`. |
| `get_schema`, `get_settings`, `set_setting` | your settings layer | See 2.5. |
| `set_speed` | fast-forward / turbo toggle, frame limiter | Each request sets the whole speed state: `fast_forward:true` (with an optional `factor` > 1) is fast-forward, `factor` < 1 is slow motion, and `fast_forward:false`, `factor:1` or neither field is normal speed. |
| `screenshot` | your screenshot code | Answer with `image` (a picture), or write a PNG to `path` if the request has one. |
| `reset` | soft/hard reset | `hard` tells which. |
| `set_volume`, `hud_level`, `notify` | output volume, FPS overlay, OSD message | |
| `list_discs` / `set_disc` | multi-disc / m3u swap | |

The library answers `unsupported` for anything without a handler, or whose capability your
descriptor does not list. A partly wired engine therefore still behaves correctly.

`perf` is an event you send about once a second while the game runs, if the frontend took `perf`:

```c
ecp_perf p = {fps, 60.0, fps / 60.0, frame_ms, cpu_ms, gpu_ms, shaders_building};
if (ecp_frontend_has(c, "perf")) ecp_send_perf(c, &p);
```

### 2.5 Settings

The frontend owns the player's settings. It sends resolved values in `launch.settings`, and sends
changes with `set_setting`. **Never write them to your config files.** An overlay that exists only
in memory works well. An emulator with layered settings (PCSX2's `LayeredSettingsInterface`, for
example) can add one more layer that the ECP session fills and drops at the end.

- If your core applies per-game settings while it boots (PCSX2's game settings and patches do),
  it can overwrite what `launch` sent. Apply the launch settings again after `first_frame` where
  they differ.
- Use the standard keys (`video.internal_scale`, `video.aspect`, `audio.volume`, `speed.limit`, and
  the rest in SPEC 6.4) for settings you already have. The frontend can then show them in its own
  menus without knowing your engine. `ecp-probe` checks their types.
- Publish everything else under your own prefix (`gs.blending_accuracy`, `ee.cycle_rate`).
- `set_setting` answers `applied`: `now`, `restart` (takes effect next launch), or `no`.
- Validate values. Refuse an unknown key or a bad value with `ok:false`; the test engine uses
  `unsupported` and `protocol`.

The test engine keeps a table of `{key, label, type, min, max, choices, default, applies}`. It
writes `get_schema`, the `schema` one-shot and validation from that one table. Copy that pattern.

### 2.6 The menu hand-over (`menu`)

When the frontend takes `menu`, the player's menu button opens the frontend's menu, not yours:

1. `launch` carries `menu_chord`, for example `["l3","r3"]` in RetroPad names. Detect it in your
   input layer, where your own hotkeys are handled. A touch button of your own may also open it.
2. Pause, grab the current frame, and call
   `ecp_send_menu_request(c, pixels, w, h, stride, flags)`. It sends `paused` (`menu`), unless
   you have already reported a pause, then `menu_request` with the picture. Pass `ECP_PNG_BGRA`
   for BGRA framebuffers. Large frames are halved until they fit.

   Grab the game's picture, not the window: the frame at its internal resolution, corrected to
   the game's display aspect (a 640x448 PS2 frame shown at 4:3 becomes 640x480), then scaled
   down if needed. The frontend draws its menu over this picture, so a stretched or letterboxed
   frame looks wrong behind it.
3. Do nothing else. The frontend raises its window, draws its menu over the frame and sends the
   player's choices (`save_state`, `set_setting`, and so on) while you stay paused.
4. On `resume`, raise your window and continue.

While a frontend with `menu` is attached (`ecp_frontend_has(c, "menu")`), **your own pause menu
must not open**: not from the hotkey, not on Escape or Back, not on focus loss. Quick buttons such
as an on-screen fast-forward may stay.

### 2.7 The end of the session

When the channel closes, the `closed` handler runs and `ecp_poll()` returns -1. Save what you
always save (battery saves, memory cards), then exit within 5 s. Never keep running headless.

## 3. Android engines (sdk/android)

### 3.1 Manifest and build

Add a PLAY activity. Keep it separate from your launcher activity, so the library screen and
first-run flow never appear in a frontend session:

```xml
<uses-permission android:name="android.permission.INTERNET" /> <!-- loopback only; the library adds it -->

<activity
    android:name=".EcpPlayActivity"
    android:exported="true"
    android:launchMode="singleTask"
    android:configChanges="orientation|screenSize|screenLayout|keyboard|keyboardHidden|navigation">
    <intent-filter>
        <action android:name="io.github.ecp.action.PLAY" />
        <category android:name="android.intent.category.DEFAULT" />
    </intent-filter>
    <meta-data android:name="io.github.ecp.descriptor" android:resource="@raw/ecp" />
</activity>
```

Put the descriptor in `res/raw/ecp.json`. `singleTask` matters: when the frontend brings the game
back on `resume`, it must reach the running activity, not a new one.

SPEC 9's "nothing interactive before `launch`" cannot mean "no window" on Android, because the
activity is on screen as soon as the frontend starts it. Show black, or a neutral loading screen,
until the game boots. No menus, prompts or first-run screens.

**Adding the SDK.** `sdk/android/ecp` is a plain Android library module with no dependencies
beyond the Kotlin standard library. Vendor it: copy the directory into your project (as PSX2
does with `ecp-sdk/`, or as a git subtree), then:

```kotlin
// settings.gradle.kts
include(":ecp")
project(":ecp").projectDir = file("ecp-sdk")

// app/build.gradle.kts
dependencies { implementation(project(":ecp")) }
```

The module's build file works with either toolchain, so you don't need to edit it:

- **AGP 9 or later** (built-in Kotlin): nothing else. This includes Java-only apps, because AGP 9
  compiles the module's Kotlin itself.
- **AGP 8.x**: the build needs the Kotlin Android plugin on its classpath. Declare it once in the
  root `build.gradle.kts` and the module applies it: `id("org.jetbrains.kotlin.android") version "2.2.20" apply false`.

From Java the API reads without Kotlin's defaults or `Companion`:

```java
EcpEngine.Session session = EcpEngine.Session.from(getIntent());
EcpEngine engine = new EcpEngine(EcpEngine.descriptor(this, R.raw.ecp), callbacks);
engine.connect(session);
// in a handler
int slot = r.getInt("slot");
r.ok(new JSONObject().put("slot", slot).put("time", now).put("thumbnail", Ecp.picture(frame)));
engine.stopped("error", "crashed", "The emulator stopped.", "GS thread: device lost");
```

A Java class implements `EcpEngine.Callbacks` and overrides only the handlers it needs; the
others are default methods. That needs the module compiled by Kotlin 2.2 or later: AGP 9's
built-in Kotlin is, and with AGP 8 use plugin version 2.2 or newer.

### 3.2 A Java/Kotlin activity and a native core (the PSX2 pattern)

Most Android emulators, PSX2 among them, have a game activity in Java or Kotlin that calls a
native core through JNI (`NativeApp.pause()`, `saveStateToSlot()`, `runVMThread()`). ECP fits
without touching the core's threading:

- **`EcpPlayActivity` extends your game activity.** It reuses all of its surface, input and
  lifecycle code. The base class gets a few overridable hooks with neutral defaults, so its own
  behaviour does not change: "launched for one game", "a frontend owns the menu", "menu
  requested", "apply session settings before the VM starts", "the VM thread ended". PSX2's
  `MainActivity` already has hooks of this shape.
- **Kotlin speaks ECP; the core does the work.** `EcpEngine` runs the socket on its own thread.
  By default it runs request handlers one at a time, in order, on a background thread of its
  own, so a handler may call a JNI function that blocks for seconds (a state save). `stop` and
  `onClosed` run on a separate control thread, so the end of a session never waits behind a slow
  request. Your `handleStop` may therefore run while another handler is still busy: make
  shutdown safe against that. Handlers that only touch UI state can run on the main thread
  instead: `EcpEngine(descriptor, callbacks, EcpEngine.MAIN_THREAD)`. Answer from wherever the
  work finishes.
- **Settings go into the core in memory only**, through one JNI call such as
  `ecpSetIniValue(section, key, value)`. They are never saved to the app's preferences.

```kotlin
class EcpPlayActivity : MainActivity(), EcpEngine.Callbacks {
    private var engine: EcpEngine? = null
    @Volatile private var resumedAt = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val session = EcpEngine.Session.from(intent) ?: return   // no PORT: level 0
        EcpEngine.applyFullscreen(this, session.fullscreen)
        engine = EcpEngine(EcpEngine.descriptor(this, R.raw.ecp), this).also { it.connect(session) }
    }

    override fun isLaunchedForGame() = engine != null
    override fun isFrontendMenuAttached() = engine?.frontendHas("menu") == true

    override fun handleLaunch(r: EcpEngine.Request) {
        if (!biosInstalled()) return r.error("bios_missing", "Add a PS2 BIOS in PSX2 first.")
        r.msg.optJSONObject("settings")?.let(::applySessionSettings)
        val fd = contentResolver.openFileDescriptor(intent.data ?: return r.error("content_missing"), "r")
            ?: return r.error("content_missing", "The game could not be opened.")
        bootGame("/proc/self/fd/${fd.detachFd()}")   // or your openContentUri() helper
        r.ok()
        engine?.loading(0.0)
    }

    override fun handlePause(r: EcpEngine.Request) {
        NativeApp.pause()
        r.ok()
        engine?.paused("frontend")
    }

    override fun handleResume(r: EcpEngine.Request) {
        resumedAt = SystemClock.uptimeMillis()   // see "held buttons" below
        NativeApp.resetKeyStatus()
        NativeApp.resume()
        r.ok()
        engine?.resumed()
    }

    // Runs on the SDK's request thread, so it may block until the core has written the state.
    // No paused/resumed around it: that pause is internal.
    override fun handleSaveState(r: EcpEngine.Request) {
        val slot = r.int("slot")
        if (!NativeApp.saveStateToSlot(slot)) return r.error("internal", "The state could not be saved.")
        r.ok(JSONObject().put("slot", slot).put("time", System.currentTimeMillis() / 1000)
            .put("thumbnail", Ecp.picture(captureFrame())))
    }

    override fun handleStop(r: EcpEngine.Request) {   // on the control thread
        r.ok()
        engine?.stopped("frontend")
        runOnUiThread { shutdownVmAndFinish() }
    }

    override fun onClosed(reason: String?) = runOnUiThread { shutdownVmAndFinish() }   // saves memory cards
}
```

**Held buttons.** SPEC 6.1 says buttons still held when the game resumes are ignored until they
are released. A list of held keys goes stale while the frontend has the input, so compare times
instead. Drop every key event that went down before the resume, and clear the core's pad state:

```kotlin
override fun dispatchKeyEvent(event: KeyEvent): Boolean {
    if (event.downTime < resumedAt) return true   // pressed before the game resumed: drop it
    return super.dispatchKeyEvent(event)
}
```

Do the same when the menu opens, so the chord's keys never reach the game.

Send `firstFrame()` and `running()` once the core presents its first frame. Hook the place where
you hide your loading spinner. Send `perf(...)` once a second from your existing FPS counter.

`handleX` names are used instead of `onPause`/`onResume`/`onStop` so that an activity can
implement `EcpEngine.Callbacks` without clashing with its lifecycle methods.

### 3.3 The game file

`launch.content` is `"intent"`. The game is the activity's `data` URI, a `content://` URI the
frontend granted you read access to. Open it with `ContentResolver.openFileDescriptor()` and pass
the fd to the core, as most cores already do for SAF documents. With all-files access you may
also accept the plain path in `io.github.ecp.extra.PATH`.

### 3.4 Menu hand-over on Android

The order matters, because Android lets an app start another app's activity only while the
starting app is in the foreground:

```kotlin
override fun onFrontendMenuRequested(source: String) {   // your menu hotkey, Back, a touch button
    val e = engine ?: return
    NativeApp.pause()
    e.requestMenu(this, captureFrame())   // paused(menu) if not yet reported, menu_request, MENU_INTENT
}
```

`captureFrame()` should return the game's frame at its internal resolution and display aspect
(2.6), not a copy of the window.

`requestMenu()` fires the frontend's `MENU_INTENT` PendingIntent with the sender's
background-activity-start opt-in (`ActivityOptions.setPendingIntentBackgroundActivityStartMode`,
which Android 14 and later require). Call it while your activity is still in the foreground,
before any dialog or `moveTaskToBack()`.

While the frontend's menu is up, your activity is stopped and the core stays paused. Do not treat
this `onPause()` as a focus pause, because you are already paused for the menu. To continue, the
frontend starts your PLAY activity again with `FLAG_ACTIVITY_REORDER_TO_FRONT`, then sends
`resume`. The new intent has no `PORT`. Recognise it so it does not start a second session:

```kotlin
override fun onNewIntent(intent: Intent) {
    super.onNewIntent(intent)
    if (EcpEngine.isBringBack(intent, engine?.session)) return   // the frontend bringing us back
    setIntent(intent)
}
```

When the player leaves through the system (home, recents) rather than the menu, pause and send
`paused` with `reason:"focus"`. Send `resumed` when you come back. The test engine's
`onPause()`/`onResume()` show this.

### 3.5 Native-only cores

If the protocol has to live in native code, call `EcpEngine.Session.from(intent)` in Java or
Kotlin, pass `port`, `token` and `id` down through JNI, and use `sdk/c`:

```c
ecp_config cfg;
ecp_config_tcp(&cfg, port, token, session_id);
ecp_conn *c = ecp_open(&cfg, &desc, &handlers, app, err, sizeof err);   /* from a worker thread */
```

The MENU_INTENT still has to be sent from Java (`EcpEngine.sendMenuIntent(context, pendingIntent)`),
so call up into Java after `ecp_send_menu_request()`.

## 4. Testing with ecp-probe

`conformance/ecp-probe.py` is a test frontend that needs only the Python 3 standard library. It
starts your engine, runs the SPEC section 10 checks for every capability you list, validates every
message against `schema/`, and prints a table. An engine conforms at its level when the probe
passes.

```sh
# desktop, over stdio (the default), TCP, or both
python conformance/ecp-probe.py -- ./my-engine
python conformance/ecp-probe.py --transport both --content game.iso -- ./my-engine --my-flag

# the menu hand-over needs the engine to ask: give it time and trigger the chord yourself
python conformance/ecp-probe.py --menu-timeout 30 -- ./my-engine

# Android, on the device adb sees: adb reverse + am start with the session extras
python conformance/ecp-probe.py --adb com.izzy2lost.psx2/.EcpPlayActivity \
    --content content://... --first-frame-timeout 120 --menu-timeout 30
```

To test the menu chord on a device without touching it, press the chord through adb while the
probe waits for `menu_request`. For L3+R3: `adb shell input keycombination KEYCODE_BUTTON_THUMBL KEYCODE_BUTTON_THUMBR`
(Android 13 and later). adb cannot pass `MENU_INTENT`, so the frontend's menu does not appear,
but `paused` and `menu_request` still arrive and are checked.

Without `--content`, the probe launches a dummy file, which a real emulator rightly refuses with
`content_invalid`. Pass a real game. With `--adb`, pass a URI the app can read, or let the probe
push a dummy file to `/data/local/tmp`. The probe saves states under a fresh `game_id` and
deletes them again, so it never touches the player's states.

A typical run:

```
session/stdio  pause within 500 ms                 PASS       0 ms  event 0 ms
session/stdio  save_state slot 1                   PASS      16 ms
session/stdio  load_state restores the picture     PASS
session/stdio  load_state empty slot is slot_empty PASS       0 ms  error slot_empty
...
111 passed, 0 failed, 0 warnings, 0 skipped
```

`WARN` rows do not fail the run, but look into them. Fast-forward not visible in `perf` is
usually a frame limiter that stayed on. A screenshot after `load_state` that differs from the one
before `save_state` is expected from an engine that redraws only when a frame runs: while paused
it still shows the old frame. On failure the probe
prints the directory with your engine's log.

For frontend developers, `examples/test-engine` is the other half. It runs on desktop
(`test-engine --ecp stdio --menu-after 5`, `--fail bios_missing` to test error screens) and as an
APK (`sdk/android`: `gradlew assembleRelease`), where a tap or L3+R3 asks for the menu.

## 5. Checklists

### Level 0 (no ECP)

- [ ] The game can be passed on the command line (desktop) or as the intent's `data` (Android).
- [ ] The emulator exits, or finishes the activity, when the player quits the game.

### Level 1

- [ ] Descriptor written: `ecp.json` beside the executable and `--ecp-describe` (desktop), or
      `res/raw/ecp.json` plus the PLAY intent filter and meta-data (Android).
- [ ] `--ecp stdio` and `--ecp tcp:127.0.0.1:<port>` (token from `ECP_TOKEN`) on desktop; the
      PORT/TOKEN extras on Android, with a connection within 10 s.
- [ ] Nothing interactive before `launch`: no window on desktop; black or a neutral loading screen
      on Android; no wizard or profile picker.
- [ ] `launch`: answered once loading begins; typed errors for missing BIOS, keys or firmware and
      for bad content.
- [ ] `loading` (optional), `first_frame` when the game's own first frame is up, `running`.
- [ ] `stop`: answer, `stopped` (`frontend`), exit within 5 s, never queued behind a slow request.
- [ ] Channel lost: save battery saves and exit within 5 s; never run headless.
- [ ] Unknown requests answered `unsupported` (the SDKs do this).
- [ ] `ecp-probe` passes.

### Level 2, per capability

- [ ] `pause`: `pause`/`resume` within 500 ms with `paused`/`resumed`; held buttons ignored after
      `resume`; `paused` with `focus` when the player leaves through the system; internal pauses
      (state save/load) not reported.
- [ ] `menu`: `menu_chord` detected; `paused` (`menu`) + `menu_request` with the frame at the
      game's aspect; Android sends MENU_INTENT while in the foreground; your own pause menu never
      opens; `resume` works without `pause` and raises the game.
- [ ] `states`: slots 0-99 by `game_id`; `slot_empty` and `state_incompatible`; `list_states`
      and `delete_state`. `state_thumbnails`: a picture with each state.
- [ ] `settings`: schema with the standard keys where they apply; `launch.settings` applied
      before boot (and again after `first_frame` if per-game settings overwrite them);
      `set_setting` answers `applied`; nothing written to your config.
- [ ] `fast_forward` / `slow_motion`: `set_speed`; `perf.speed` follows.
- [ ] `perf`: about once a second while running.
- [ ] `screenshot`, `reset`, `rewind`, `volume`, `hud`, `notify`, `cheats`, `discs`: each answers
      as SPEC 6.5 says.
- [ ] `oneshot` (desktop): `describe`, `schema`, `status`; exit code 0 when `ok`.
- [ ] `ecp-probe` passes with every capability listed.
