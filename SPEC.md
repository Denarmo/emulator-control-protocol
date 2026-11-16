# Emulator Control Protocol (ECP) 1

ECP lets a **frontend** (a launcher such as EmuDEN, ES-DE or Daijisho) start a game in a standalone
emulator app (the **engine**) and control it while it runs: pause, the frontend's own menu over the
game, save states, speed, video and audio settings, screenshots and performance figures. The engine
keeps emulating and drawing in its own window or activity; the frontend keeps the library, the
menus and the player's settings.

ECP works the same way on Windows, Linux, macOS and Android. It does not belong to any one frontend:
the names in it are neutral, and this document and the schemas are under the MIT licence (see
`LICENSE`). Engines that only want to be started with a game file need none of this. Frontends can
always start them the old way (level 0).

The key words MUST, SHOULD and MAY are used as in RFC 2119.

## 1. Levels and capabilities

| Level | An engine at this level |
|---|---|
| 0 | Takes a game on its command line (or in an Android intent) and ends when the player is done. No ECP. |
| 1 | Speaks ECP: hello, launch, lifecycle events, stop. |
| 2 | Level 1 plus the session controls in section 6, each behind a capability. |

The engine states its level and capabilities in its hello. The frontend never sends a request whose
capability the engine did not list. An engine MAY list any subset; a frontend MUST hide or grey out
what is missing.

Capabilities: `pause`, `menu` (menu hand-over), `states`, `state_thumbnails`, `reset`,
`fast_forward`, `slow_motion`, `rewind`, `settings`, `screenshot`, `perf`, `hud`, `notify`,
`volume`, `cheats`, `discs` (multi-disc swap), `applets`, `oneshot`, `configure` (the engine's
own setup, section 6.6).

## 2. Discovery

An engine describes itself with a **descriptor**, a JSON object (`schema/descriptor.schema.json`):

```json
{
  "ecp": 1,
  "id": "com.example.ps2emu",
  "name": "Example PS2",
  "version": "1.4.0",
  "level": 2,
  "systems": ["ps2"],
  "extensions": ["iso", "chd", "cso"],
  "capabilities": ["pause", "menu", "states", "state_thumbnails", "settings", "fast_forward"],
  "needs": ["bios"],
  "settings": "schema"
}
```

- `id` is the engine's stable id: its Android package name, or a reverse-DNS name on desktop.
- `systems` uses the system ids in `systems.md`: `ps2`, `gba`, `nds` and so on.
- `needs` lists what the player must provide before games run: `bios`, `keys`, `firmware`. It is
  empty when the engine needs nothing.
- `settings`: `"schema"` means the engine answers the `schema` one-shot (section 7) with its settings.

Where to find it:

- **Desktop:** `ecp.json` beside the executable, or `<executable> --ecp-describe`, which prints the
  descriptor and exits.
- **Android:** the engine declares an activity with this intent filter:

  ```xml
  <intent-filter>
      <action android:name="io.github.ecp.action.PLAY" />
      <category android:name="android.intent.category.DEFAULT" />
  </intent-filter>
  ```

  The descriptor goes in a `<meta-data android:name="io.github.ecp.descriptor"
  android:resource="@raw/ecp" />` on that activity. Frontends find engines with
  `PackageManager.queryIntentActivities`, and need a matching `<queries>` entry in their own
  manifest.

A frontend MAY also learn about engines from its own plugin catalogue (EmuDEN's plugin manifests
do). It then pins the package name and signing certificate digest, or the download checksum.

## 3. Starting a game

### Desktop

```
<engine> --ecp <transport> [--ecp-token-env ECP_TOKEN]
```

- `--ecp stdio`: the channel is the engine's standard input and output, and standard error is its
  log. This is the default, and the frontend starts the engine as its child.
- `--ecp tcp:127.0.0.1:<port>`: the engine connects to the frontend, which listens on loopback
  only. The token is in the environment variable named by `--ecp-token-env`, never on the command
  line. Without `--ecp-token-env` the variable is `ECP_TOKEN`.

The game arrives in `launch` (section 5), not on the command line. The frontend SHOULD tie the
engine's life to its own: a Windows job object with kill-on-close, or a Linux process group with
the parent-death signal.

### Android

The frontend listens on `127.0.0.1:<random port>` and starts the engine's PLAY activity with an
explicit intent:

| Extra / field | Type | Meaning |
|---|---|---|
| `data` | Uri | The game, as a `content://` URI with `FLAG_GRANT_READ_URI_PERMISSION`. An engine with all-files access MAY also accept a `file://` path in `io.github.ecp.extra.PATH`. |
| `io.github.ecp.extra.PORT` | int | The frontend's loopback port. |
| `io.github.ecp.extra.TOKEN` | String | The session token: 32 random bytes, hex. |
| `io.github.ecp.extra.SESSION` | String | The session id, for the engine's log. |
| `io.github.ecp.extra.MENU_INTENT` | PendingIntent | Brings the frontend's menu forward (section 6.2). |
| `io.github.ecp.extra.FULLSCREEN` | boolean | Immersive mode. Default true. |
| `io.github.ecp.extra.COVER` | Uri | Optional. An image of the frontend's screen as it was when it started the game, as a `content://` URI granted with the intent's `ClipData`. |
| `io.github.ecp.extra.COVER_COLOR` | int | Optional. The ARGB colour behind `COVER`. Default black. |

The change of app should not be seen. The frontend starts the activity with no window animation
(`FLAG_ACTIVITY_NO_ANIMATION`). The engine SHOULD give the activity no starting window
(`android:windowDisablePreview`) and no system bars, and SHOULD cover its screen from its first
draw until `first_frame` (section 5): with `COVER` scaled to fill it, on `COVER_COLOR`, or black
without them. It SHOULD also keep its own on-screen notifications off while a frontend drives it.

The frontend picks the display (`ActivityOptions.setLaunchDisplayId`), so on a dual-screen device
the game can run on one screen while the frontend stays on the other. An engine started without
`PORT` behaves as at level 0.

The engine opens a TCP connection to `127.0.0.1:PORT` within 10 s of being started. It also needs
`android.permission.INTERNET` for loopback, which it does not otherwise use.

## 4. Channel

- **Framing:** a 4-byte little-endian length, then that many bytes of UTF-8 JSON, one object per
  frame. A frame over 16 MiB is a protocol error. Both sides keep frames under 4 MiB unless they
  carry a picture.
- **Authentication (TCP only):** the engine's first frame is `{"type":"auth","token":"<token>"}`.
  The frontend closes any connection whose first frame is not the right token, compared in
  constant time. One connection per session.
- **Messages:** `{"type":"<name>","id":<integer, optional>, ...fields}`.
  - A **request** carries `id`. Its answer is
    `{"type":"result","id":<same>,"ok":true|false,"error":{...}, ...result fields}`. `error` is
    present only when `ok` is false.
  - Messages without `id` are **events**.
  - Unknown types and unknown fields MUST be ignored. A request of an unknown type is answered with
    `ok:false` and the code `unsupported`.
- **Errors:** `{"code":"<snake_case>","message":"<for the player>","detail":"<for the log>"}`. The
  codes are:
  - `unsupported`, `busy`, `protocol`, `internal`
  - `content_missing`, `content_invalid`
  - `bios_missing`, `firmware_missing`, `keys_missing`
  - `gpu_unsupported`, `out_of_memory`, `crashed`
  - `slot_empty`, `state_incompatible`
- **Hello:** the engine speaks first (after `auth` on TCP). It sends the descriptor fields plus
  `"type":"hello"` and `"protocol":1`. The frontend answers
  `{"type":"hello","protocol":1,"frontend":"<name>","version":"<semver>","level":<level it will use>,"capabilities":[...it will use]}`.
  `protocol` changes only for incompatible changes. If no hello arrives within 10 s, the frontend
  ends the engine.
- **Pictures** (frames, thumbnails, screenshots) are PNG, sent inline as
  `{"png":"<base64>","width":w,"height":h}`. On desktop the frontend MAY ask for a file instead,
  by passing `path` in the request.
- **End:** when the channel closes, the engine saves (battery saves, its settings) and exits on its
  own, within 5 s.

## 5. Session lifecycle (level 1)

**Frontend → engine**

- `launch` (request):
  - `content`: the path, or on Android `"intent"` for the intent's data
  - `system`, `game_id` (the frontend's stable id, for per-game data)
  - `title`, `locale`, `fullscreen`
  - `settings` (level 2: resolved values by key, section 6.4)
  - `menu_chord` (with `menu`, section 6.2)

  The result arrives once loading has begun. A failure carries a typed error: `bios_missing`,
  `content_invalid` and so on.
- `stop` (request): end the session, save, exit. The frontend ends the engine 5 s after the
  result.

**Engine → frontend (events)**

- `loading`: `progress` (0–1, optional), `text` (optional)
- `first_frame`: the first frame the game itself drew is on screen. A black or blank frame the
  emulator shows while it boots does not count. Play time counts from here.
- `running`
- `stopped`: `reason` (`user`, `frontend`, `error`) and `error` when `reason` is `error`. The
  engine exits after sending it.

## 6. Session controls (level 2)

Every request below needs the capability in its heading. `resume` is the exception: it is
valid with `pause` or with `menu`, because it ends the menu hand-over (6.2).

### 6.1 `pause`

- `pause` and `resume` are requests. `resume` puts the engine back in the foreground if it isn't.
  Buttons still held at that moment MUST be ignored until they are released.
- `paused` (event): `reason`, one of `frontend`, `menu`, `focus`, `applet`. It reports a pause
  the frontend can see. The engine does not report the short pauses it takes internally, for
  example while it writes or reads a state.
- `resumed` (event)

### 6.2 `menu`: the frontend's menu over the game

1. The player presses the menu combination. The frontend sends it in `launch` as
   `menu_chord: ["l3","r3"]` (RetroPad button names, section 8). The engine MAY also offer its own
   way in, such as a touch button.
2. The engine pauses, sends `paused` with `reason:"menu"` (unless it has already reported
   `paused`), then the `menu_request` event with `frame`, the paused picture
   (`{"png":...,"width":...,"height":...}`).
3. **Android:** the engine then sends `MENU_INTENT`. The engine is in the foreground, so Android
   lets this start the frontend's activity. **Desktop:** the frontend raises its own window.
4. The frontend draws its menu over `frame` and sends whatever the player chose: `save_state`,
   `set_setting` and so on.
5. To continue, the frontend sends `resume`. On Android it first starts the engine's PLAY activity
   again with `FLAG_ACTIVITY_REORDER_TO_FRONT` (the frontend is in the foreground, so this is
   allowed); on desktop the engine raises its window when it gets `resume`.

The engine MUST NOT show its own pause menu while a frontend with `menu` is attached. It MAY keep
its own quick buttons, such as an on-screen fast-forward.

### 6.3 `states`

The engine stores the states in its own data, per game and slot. It keys them by `game_id`, or by
its own per-game identity (such as a disc serial or CRC) when it has one. The slots used through
ECP MAY be the same ones the engine's own UI shows. Slot 0 is the frontend's resume slot: the
frontend saves to it when the player leaves and loads it when they come back, and does not
show it as a player slot. Slots 1–99 are the player's.

A state saved before `first_frame` MAY have no thumbnail.

- `save_state` (request): `slot`. The result has `slot`, `time` (Unix seconds) and `thumbnail`
  (picture, if `state_thumbnails`).
- `load_state` (request): `slot`. It can fail with `slot_empty` or `state_incompatible`.
- `list_states` (request): the result has `states: [{slot, time, thumbnail?}]`.
- `delete_state` (request): `slot`.

### 6.4 `settings`

The engine publishes its settings with the `schema` one-shot or the `get_schema` request; both
answer with `settings`, a list of setting objects: `key`, `label`, `help`, `type` (`toggle`, `choice`, `range`, `text`),
`choices` (`[[value,label]]`), `min`, `max`, `step`, `default`, `group`, and `applies` (`now`,
`restart`).

Engines SHOULD use these **standard keys** where they have the setting, so a frontend can put them
in its own menus without knowing the engine:

| Key | Type | Values |
|---|---|---|
| `video.internal_scale` | range | 1–8 (native × n) |
| `video.aspect` | choice | `auto`, `4:3`, `16:9`, `stretch` |
| `video.widescreen_patches` | toggle | |
| `video.renderer` | choice | engine-defined (`vulkan`, `opengl`, `software`, ...) |
| `video.texture_filter` | choice | `nearest`, `linear`, engine-defined |
| `video.frame_skip` | range | 0–3 |
| `video.vsync` | toggle | |
| `audio.volume` | range | 0–100 |
| `audio.latency_ms` | range | engine-defined |
| `speed.limit` | toggle | the frame limiter |
| `speed.fast_forward_factor` | range | 2–10 |
| `system.language` | choice | engine-defined |

Requests:

- `get_settings`: the result has `values`, the current values by key.
- `set_setting`: `key` and `value`. The result has `applied`, one of `now`, `restart` or `no`.

The engine stores no setting it got through `launch` or `set_setting`. The frontend owns the player's choices and sends
them again in `launch`. An engine's own settings screen MAY still exist for when it is used without
a frontend.

### 6.5 Other controls

| Capability | Request | Fields |
|---|---|---|
| `reset` | `reset` | `hard` (bool) |
| `fast_forward` | `set_speed` | `fast_forward` (bool), `factor` (optional, > 1) |
| `slow_motion` | `set_speed` | `factor` < 1 |
| `rewind` | `set_rewind` | `on` (bool) |
| `screenshot` | `screenshot` | result: `image` (picture), or `path` on desktop |
| `volume` | `set_volume` | `percent` 0–100, `mute` (bool) |
| `hud` | `hud_level` | `level` 0–3: off, fps, timing, full |
| `notify` | `notify` | `text`, `seconds`: a toast drawn by the engine |
| `cheats` | `set_cheats` | `cheats: [{code, enabled, description}]` |
| `discs` | `list_discs`, `set_disc` | `list_discs` result: `discs: [{index, label}]`, `current`; `set_disc`: `index` |

`set_speed` sets the whole speed state. `fast_forward:false`, or `factor:1` without
`fast_forward`, is normal speed. `fast_forward:true` without `factor` uses the engine's own
fast-forward speed (`speed.fast_forward_factor` if it has it). A request with neither field is
normal speed.

### 6.6 `configure`: the engine's own setup

A frontend that already has the player's BIOS files and game folders can set the engine up with
them, so the engine opened on its own (from the app list or the desktop) skips its first-run
setup and lists the same games with the same settings.

- `configure` (request): every field is optional.
  - `bios`: locations of files or folders holding the player's BIOS, firmware or key files. The
    engine imports the ones it recognises into its own storage on the same device and ignores
    the rest. It never sends them anywhere.
  - `game_folders`: locations of folders holding the player's games for the engine's systems.
    They replace the folders an earlier `configure` gave; folders the player added in the engine
    stay.
  - `settings`: values by key (6.4). The engine stores them as its own settings.

  The result has `bios` (BIOS files the engine has from what was sent), `game_folders` (folders
  it kept), `settings` (values it stored), `setup_done` (its first-run setup is complete) and
  `problems` (typed errors, such as `bios_missing` when none of the files was a usable BIOS).
- Unlike `launch` settings, what `configure` carries is kept. It only adds: it never deletes the
  player's saves, states, BIOS files or own folders.
- Locations: absolute paths on desktop. On Android, `content://` URIs: a folder is a document tree
  URI (`DocumentsContract.buildTreeDocumentUri`), a file a document URI. The frontend grants them
  with `Context.grantUriPermission` (read; for folders also `FLAG_GRANT_PREFIX_URI_PERMISSION` and
  `FLAG_GRANT_PERSISTABLE_URI_PERMISSION`), and the engine takes the persistable permission of the
  folders it keeps.
- In a session the frontend sends `configure` after hello and before `launch`, and waits for its
  result, so a BIOS it carries is in place for the game.
- **Outside a session (Android):** an engine with `configure` MAY declare a receiver for the
  action `io.github.ecp.action.CONFIGURE`. The frontend sends it an explicit broadcast whose
  String extra `io.github.ecp.extra.CONFIGURE` holds the request's fields as a JSON object, once
  the engine is installed and whenever the locations or settings change. There is no answer.
  Any app can send that broadcast, so an engine applies it only additively as above, and MAY
  ignore senders it does not know (on Android 14 and later, `BroadcastReceiver.getSentFromPackage`
  when the frontend shares its identity).

Events:

- `perf` (with `perf`): about once a second, `fps`, `target_fps`, `speed` (1.0 = full) and
  `frame_ms`. It MAY also carry `cpu_ms` and `gpu_ms`, and `shaders_building` (bool).
- `applet_request` (with `applets`, carries `id`): the engine needs the player for something, such
  as a text prompt (`kind:"text"`). The frontend answers with a `result`.

## 7. One-shot commands

`<engine> --ecp-oneshot <command>`. The request is one JSON object on standard input, the result
one JSON object on standard output, and the exit code is 0 when `ok`. There is no window. Android
has no equivalent: the descriptor and `get_schema` cover what a frontend needs.

| Command | Request | Result |
|---|---|---|
| `describe` | none | descriptor |
| `schema` | none | `settings` |
| `status` | none | `ready` (bool), `problems` (typed errors, e.g. `bios_missing`) |
| `install-bios`, `install-firmware`, `install-keys` | `source` | typed error or ok |
| `verify` | `content` | `title`, `serial`, `region`, or an error |

## 8. Names

The menu chord and input use RetroPad names: `a b x y l r l2 r2 l3 r3 select start up down left
right`. Positions follow the SNES layout (`b` is the bottom face button).

## 9. Rules for engines

- Nothing interactive before `launch`. On desktop that means no window. On Android the PLAY
  activity is on screen from the moment it starts, so it shows black (or a neutral loading
  screen) until the game boots, with no menus or prompts.
- Nothing kept from a session's settings (only `configure` is kept). No profile selector: one
  player.
- Saves and states live in the engine's own storage, per game (section 6.3).
- An engine that loses the channel during a session saves and exits; it never keeps running
  headless.
- Everything that changes the game (states, cheats, discs) happens only when the frontend asks.

## 10. Conformance

`conformance/` holds a test frontend (`ecp-probe`). It starts an engine, drives every capability
it lists, and checks the answers and timings: hello within 10 s, `first_frame`, pause (result and
`paused` event) within 500 ms, a state saved and loaded, `stop` within 5 s. An engine is ECP-conformant at the level it
claims when the probe passes.
