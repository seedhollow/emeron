# emeron

A host-side Android inspector. It runs on your laptop, talks to devices over
adb, and needs nothing installed in the app under test.

Five tools in one dockable window:

| Panel | What it does | Source on the device |
|---|---|---|
| **Dashboard** | Live CPU (total and per-core), RAM, battery, thermal zones, plus per-app CPU and RSS | `/proc/stat`, `/proc/meminfo`, `dumpsys battery`, `/sys/class/thermal/*`, `/proc/<pid>/{stat,status}` |
| **Frame Time** | Frame-time timeline, jank percentiles, severity buckets, per-frame phase breakdown. Jank is judged as `dumpsys gfxinfo` judges it — by each frame's own deadline — across all of the app's windows | `dumpsys gfxinfo <pkg> framestats` |
| **Device Files** | File manager: multi-select, cut/copy/paste, duplicate, rename, new folder, delete, upload, download, drag-and-drop | `ls -lA`, `cp`/`mv`/`rm`/`mkdir`, `adb pull`, `adb push` |
| **Apps** | Every installed app; pick one to see version, SDK levels, installer, signing, permissions (grant/revoke runtime ones), components, APK files, storage, memory, compilation state and the raw dump. Open, force stop, save APK, clear data, uninstall | `pm list packages`, `dumpsys package`, `pm path`, `cmd appops`, `dumpsys meminfo`, `dumpsys diskstats` |
| **Screen** | The device's screen, live. Click to tap, hold to long-press, drag to swipe, wheel to scroll; Back/Home/Recents/Power/Volume; type into the phone; save screenshots | scrcpy server + FFmpeg (fast), or `screencap \| gzip -1` + `input` (compatible) |
| **Device Info** | Build, SoC, display, storage, network, tracing posture, and the full raw `getprop` | `getprop` + one batched probe |
| **Sensors** | Full sensor enumeration with ranges, rates, FIFO depth; best-effort live values | `dumpsys sensorservice` |
| **Logcat** | Device log scoped to the selected process, with host-side level/tag/text filtering | `adb logcat -v threadtime` |
| **Perfetto** | Build a trace config, capture, pull, then query the trace with SQL | `perfetto`, `trace_processor_shell` |
| **Crash Logs** | Reports emeron writes about its *own* crashes: signal, backtrace, and what the UI was doing | none -- host side, `<workspace>/crash-logs` |

Built with Dear ImGui (docking branch) + ImPlot on GLFW/OpenGL 3.
Runs on macOS, Linux and Windows.

---

## Build

Requires CMake 3.25+ and a C++20 compiler (Apple Clang 14+, GCC 11+,
MSVC 19.3x+). **No network access needed** — Dear ImGui, ImPlot and GLFW are
committed under [vendor/](vendor/), so a clone builds offline and builds exactly
the sources that were tested.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/emeron
```

To resolve the same pinned revisions over the network instead — which is what
you want when bumping a version — pass `-DEMERON_VENDORED=OFF`. The configure
summary prints which origin each dependency came from. See
[vendor/README.md](vendor/README.md) for versions, licenses and how to update
one; [vendor/MANIFEST.txt](vendor/MANIFEST.txt) has the exact commits.

The two *runtime* tools are deliberately not vendored: `adb` cannot be
redistributed under the Android SDK terms, and `trace_processor_shell` is a
~80 MB binary per platform. Both are covered under
[Runtime prerequisites](#runtime-prerequisites).

Tests:

```sh
ctest --test-dir build --output-on-failure
# or directly, for per-case output:
./build/tests/emeron_tests
```

Sanitizers (the build is warning-clean at `-Wall -Wextra -Wpedantic -Wconversion`
and clean under ASan + UBSan):

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DEMERON_ASAN=ON
cmake --build build-asan && ASAN_OPTIONS=detect_leaks=0 ./build-asan/tests/emeron_tests
```

`-DEMERON_TSAN=ON` builds with ThreadSanitizer instead; `-DEMERON_WERROR=ON`
turns warnings into errors for CI.

### Linux prerequisites

GLFW needs X11 or Wayland development headers:

```sh
# Debian/Ubuntu
sudo apt install build-essential cmake ninja-build libgl1-mesa-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libgtk-3-dev   # optional: native Save / Choose Folder dialogs
```

Without `libgtk-3-dev` (or `libdbus-1-dev`, which uses the XDG portal instead)
the build still succeeds; downloads then ask for a folder inside emeron. The
configure summary's `dialogs:` line says which you got.

## Runtime prerequisites

- **adb** — found via `--adb`, then `$EMERON_ADB`, then `PATH`, then
  `$ANDROID_HOME`/`$ANDROID_SDK_ROOT` and the usual per-OS SDK locations. The status bar says which one is
  in use.
- **trace_processor_shell** — only for the Perfetto *Explore* tab. Either form
  of Perfetto's install works: the native `trace_processor_shell`, or the
  `trace_processor` launcher from `curl -LO https://get.perfetto.dev/trace_processor`
  (which needs python3 and fetches the native binary on first use — emeron then
  uses that cached binary directly). Put it in `~/.local/bin`, `/usr/local/bin`
  or `PATH`, or pass `--trace-processor <path>`. Those directories are searched
  even when emeron is launched from Finder or an IDE, where `~/.zshrc`'s PATH
  does not apply. The Explore tab's path tooltip says which copy was used.

## Theme

Dark and light, switchable from **View → Theme** or with **Ctrl+T**. The choice
is written to `<workspace>/emeron.ini` immediately, so it survives a restart
however the app exits.

The two modes are not tints of each other. A pastel green that reads well on a
near-black panel has far too little contrast on near-white, so the light
variants of the semantic colours (`good`, `warning`, `bad`, `critical`, `muted`,
`accent`) are darker and more saturated rather than lighter. Those colours are
used as *text* in tables and status lines, so both palettes are held to WCAG AA
for normal text — 4.5:1 against their own window background — and a unit test
asserts it, which is what stops a hand-picked colour from quietly becoming
unreadable.

Panels never name a colour directly: they call `theme::palette()`,
`theme::budgetColor()` or `theme::frameTimeColor()` in
[src/app/Theme.h](src/app/Theme.h). That is why a mode switch reaches the whole
UI, ImPlot styling and the backbuffer clear included, from one place. A new
panel gets both themes for free as long as it does the same.

Traces, pulled files and the window layout live in `~/.emeron` by default
(`--workspace <dir>` to change it). The layout is ImGui's own `imgui.ini`, so
dragging panels around persists across runs; **View → Reset layout** restores
the default arrangement.

---

## Device Files

Works on the selection: click, Ctrl/Cmd-click, Shift-click, drag a box, or
Ctrl/Cmd+A. Right-click a row or empty space for the full menu.

| | |
|---|---|
| Cut / Copy / Paste | Ctrl/Cmd + X / C / V — paste into a folder from its row menu |
| Delete | Delete, or Ctrl/Cmd+Backspace — always confirmed |
| Rename / New folder | F2 / Ctrl/Cmd+Shift+N |
| Open / Up | Enter or double-click / Backspace or Alt+Up |
| Back / Forward | Alt+Left / Alt+Right |
| Go to path | Ctrl/Cmd+L |
| Reload | F5 |

**Drag and drop.** Drag rows onto a folder, the Up arrow or any breadcrumb to
move them; hold Alt/Option to copy instead. Drag files or folders from
Finder/Explorer onto any emeron window — including an undocked one — to upload
into the open folder, after a confirmation. Dragging *out* of emeron to the
desktop is not possible: GLFW can receive OS drops but cannot start one, so
**Download** (into `<workspace>/pulled`, with a *Show in Finder* link) is the
way out.

**Download** opens the system's own dialog: *Save As* for a single file, so it
can be renamed on the way out, or *Choose Folder* for a folder or several items.
It starts in the last folder you downloaded to, then `~/Downloads`. Afterwards
*Show in Finder* opens the folder with the file selected. *Download to
workspace* in the row menu skips the dialog and saves into `<workspace>/pulled`.
Replacing an existing file only happens through Save As, where the OS dialog has
already asked.

**Nothing is overwritten.** A name collision on paste, move, upload or download
becomes `name copy`, `name copy 2`, … with the extension kept. Rename is the
exception: you typed that exact name, so it refuses rather than renaming your
rename. Every batch is planned on the host before anything runs on the device,
which is also where moving a folder into itself is caught.

## Apps

The list comes from one batched adb call: `pm list packages` with version code,
installer and uid, its `-3` (installed by you) and `-d` (disabled) variants,
`ps` for which apps are running, and the per-app sizes in `dumpsys diskstats`.
Android refreshes those sizes about once a day, so they can lag behind.

Picking an app makes a second batched call: the full `dumpsys package <pkg>`,
the APK files and their sizes, the launcher activity, `cmd appops get`, and,
if the app is running, the App Summary of `dumpsys meminfo`. Everything the
package block prints is kept. Fields without their own row show up under
**Raw > Every field**, and lists without their own section under **Build**.
So a field added in a newer Android release still appears.

**Native code and Signature** read the APK files themselves, without
downloading them. An APK is a ZIP, and everything needed sits in a few small
places: the ZIP index at the end, the *APK Signing Block* just before it, and
the first 4 KB of each `lib/<abi>/*.so`. emeron fetches only those byte ranges
with `dd` (and `unzip -p` for compressed entries), base64-encoded through
`adb shell`, in three round trips for all of an app's APKs. For Chrome that is
about one second, against a 225 MB APK.

- **Native code** lists every `.so` with its ABI, size, whether it is stored
  compressed, and whether it is ready for **16 KB page size** (Android 15+).
  A 64-bit library is ready when its ELF `PT_LOAD` segments are aligned to
  16 KB and, if it is stored uncompressed, its data starts on a 16 KB boundary
  in the ZIP.
- **Signature** shows which schemes are present (v1 JAR, v2, v3, v3.1). For
  each signing certificate it shows the owner, issuer, validity, algorithm,
  key, and the **SHA-256 / SHA-1 fingerprints** that Firebase, Google Cloud
  and similar consoles ask for. A certificate named `CN=Android Debug` is
  flagged as the debug key. emeron displays certificates but does not verify
  signatures; `apksigner verify` does that.

Against Chrome on a real device, the fingerprints, key size and every library's
offset and alignment matched `apksigner verify --print-certs` and Python's
`zipfile`. `tests/data/make_synthetic_apk.py` builds the APK the unit tests
read.

Two limits come from Android rather than emeron:

- **No app names, only package names.** The human-readable label lives in the
  APK's resources, and nothing on a stock device prints it over adb.
- **Components are incomplete.** `dumpsys package` lists only activities,
  services and receivers that have an intent filter, plus every provider.
  Internal screens with no filter are not reported.

Package and permission names are checked against `[A-Za-z0-9._]` before they
are put into a shell command. Clear data and Uninstall ask first, and Uninstall
is offered only for apps you installed.

## Screen

Two capture engines, picked in the panel:

**Fast (scrcpy)** -- the default when the build has FFmpeg. emeron pushes
scrcpy's own server (`vendor/scrcpy`, v3.3.4, embedded in the binary) to
`/data/local/tmp`, starts it with `app_process`, and connects to it through an
`adb forward` tunnel. The server feeds the phone's hardware video encoder and
streams H.264; emeron decodes it with FFmpeg and draws it. Input goes back on a
control socket as real touch events -- down, move, up -- so a drag, a long
press or a fling behaves exactly as on the phone, and the wheel is a real
scroll. Measured on a phone over USB 2: **57 fps** at 704x1600, about 13 KB per
frame, against 7 fps for the compatible engine on the same phone. Frames are
only sent when the screen changes, so a still screen shows a low frame rate.

This is the one place emeron runs code on the device, and only for the
session: the server deletes its own jar as it starts, the tunnel is removed on
stop, and the server exits when its sockets close. If it cannot start -- an
unusual ROM, a missing encoder -- the panel says why (the server's own error)
and falls back to the compatible engine by itself.

**Compatible (screencap)** -- needs nothing but adb, and is what a build
without FFmpeg uses. Raw `screencap` frames, piped through `gzip -1` on the
phone, from a few persistent device-side loops read continuously.
`screencap` itself costs ~170 ms per frame on a mid-range phone, but runs in
parallel, so several loops help with diminishing returns: 1 stream ~3 fps,
3 ~7-12, 6 ~8-16. The **Streams** slider sets this, because each stream costs
the phone CPU that skews what is being profiled; default 3. Input is
`input tap|swipe|keyevent|text`, so a gesture is decided at release
(`classifyGesture()`) and played as one swipe. `input text` is ASCII only.

Either way, capture runs only while the Screen tab is visible; apps that block
screenshots (FLAG_SECURE: banking, DRM video) show black.

FFmpeg is optional and found through pkg-config (`brew install ffmpeg`;
`apt install libavcodec-dev libswscale-dev`); `-DEMERON_USE_FFMPEG=OFF` turns
it off. Inflate and the compressed PNG screenshots use **miniz** (`vendor/miniz`).

## Notifications

Toasts in the bottom-right corner (**ImGuiNotify**, `vendor/ImGuiNotify`) for
what is worth knowing while looking at something else: a phone connected,
disconnected, or waiting for the USB-debugging prompt; a file transfer or
download finished or failed (with *Show in Finder*); an action in Apps
succeeded or failed; an APK or screenshot saved; a Perfetto capture finished
(with *Open in Explore*); the Screen panel falling back to its compatible
engine; and, once per report, that emeron crashed last time (with *Open Crash
Logs*). Errors stay 10 s, the rest 4 s. Turn them off under **View >
Notifications** (saved in `emeron.ini`). `notify::post()` is safe from any
thread.

## Crash logs

If emeron itself crashes (segfault, abort, illegal instruction, ...), a handler
installed at startup writes `<workspace>/crash-logs/crash-<pid>-<signal>.txt`
before the process exits normally. The report includes the signal, the fault
address, a backtrace, and the last *breadcrumb*: a one-line note such as
`Device Files: opening /sdcard/DCIM` that risky code paths record through
`crash::setBreadcrumb()`. The next launch logs a warning when it finds
reports. The **Crash Logs** panel lists them, opens the newest one, and can
copy one for a bug report or delete them.

The POSIX handler sticks to async-signal-safe calls (`open`/`write`,
`backtrace_symbols_fd`) and runs on an alternate stack, so a stack overflow
still produces a report. Breadcrumbs are written to a small file
*before* anything goes wrong, so the handler only has to read that file and
never touches the logger or its lock. On Windows an unhandled-exception filter
does the same, using DbgHelp for symbol names.

## Icons

[Font Awesome 6 Free](https://fontawesome.com/search?o=r&s=solid) (Solid, 1,402
icons) is merged into the UI font, so any ImGui string can carry an icon:

```cpp
#include <IconsFontAwesome6.h>

if (ImGui::Button(ICON_FA_DOWNLOAD " Download")) { ... }
ImGui::TextColored(color, "%s", ICON_FA_TRIANGLE_EXCLAMATION);
```

Names follow the Font Awesome website with dashes as underscores
(`arrows-rotate` → `ICON_FA_ARROWS_ROTATE`). The font is compiled into the
binary, so there is nothing to ship beside it. Two details:

- The 47 icons that Font Awesome maps onto plain ASCII (`ICON_FA_PLUS` is `+`,
  `ICON_FA_A` is `A`) draw the text font's character. A merged font has one glyph
  per codepoint, and handing ASCII to Font Awesome would turn every digit in the
  UI into an icon.
- `tests/test_fonts.cpp` checks every `ICON_FA_*` has a glyph in the embedded
  font, so an upgrade of one without the other fails the build's tests instead
  of drawing empty boxes.

Panel tabs carry icons through ImGui's `###` operator (`Panel::windowTitle()`):
the title shows `<icon>  Dashboard`, but ImGui hashes only the part after `###`
and skips the marker, so the window keeps the identity of plain `Dashboard`.
Layouts saved before icons existed still apply, and DockBuilder and lookups use
`Panel::windowKey()`. Icons that depend on data — battery level, sensor type,
Device Info section — live in `src/ui/Icons.h`, with tests.

For checking UI changes without clicking around:

```sh
emeron --screenshot out.png                 # save the main window after 4 s, then quit
emeron --package com.example.app            # target an app from the start
emeron --show-panel Sensors --show-panel Log  # bring background tabs to the front
```

## VS Code

`.vscode/` is checked in and configured for the **C/C++ (ms-vscode.cpptools)** +
**CMake Tools** pair. Install the recommended extensions when prompted.

IntelliSense gets compile flags from three sources, first one that works wins:
CMake Tools (exact per-file flags), then `build/compile_commands.json`, then a
static `includePath` in `c_cpp_properties.json` so a fresh clone is navigable
before the first configure. That fallback matters here because emeron includes
its own headers rooted at `src/` and the ImGui/ImPlot/GLFW checkouts live under
`build/_deps/`.

If includes still show as unresolved: run **CMake: Configure** once, then
**C/C++: Reset IntelliSense Database**, and check the bottom-right status bar
shows a configuration rather than the default. Do not install clangd alongside
cpptools — they both claim IntelliSense and the result is unpredictable.

Debug targets (F5):

| Configuration | Binary |
|---|---|
| Debug: CMake Tools target | follows the status-bar target and variant |
| Debug: emeron (GUI) | `build/emeron` |
| Debug: emeron (GUI, no viewports) | single-window, no vsync, trace logging |
| Debug: unit tests | `build/tests/emeron_tests` |
| Debug: unit tests (ASan + UBSan) | `build-asan/tests/emeron_tests` |
| Debug: emeron (ASan + UBSan) | `build-asan/emeron` |
| Attach to running emeron | pick the pid |

Use the *no viewports* configuration when stepping through UI code: with
multi-viewport on, panels live in real OS windows that stop redrawing at a
breakpoint. The sanitizer configurations set `abort_on_error=1` so a fault traps
into the debugger instead of printing a report and exiting.

Tasks (Cmd/Ctrl+Shift+B is `cmake: build`): `cmake: configure`, `cmake: build`,
`cmake: configure (asan)`, `cmake: build (asan)`, `ctest`,
`run unit tests (verbose)`, `clean`.

On Windows, change `"cppdbg"` to `"cppvsdbg"` in `launch.json` and drop
`MIMode` — the Visual Studio debugger does not use the MI protocol.

---

## Architecture

```
vendor/      Committed third-party source: imgui (docking), implot, glfw, nfd,
             Font Awesome Free, IconFontCppHeaders
src/
  core/      Result, Subprocess, ThreadPool/Dispatcher, RingBuffer, HttpClient,
             IniFile, Log
  adb/       IAdbTransport -> AdbCliTransport, AdbClient, DeviceManager
  collect/   MetricSampler, FrameStats, LogcatReader, DeviceInfo, SensorReader,
             FileSystemBrowser, PerfettoCapture, TraceProcessor
  app/       Application (window, dockspace, frame loop), Theme (dark/light),
             AppContext
  ui/        Panel, PanelRegistry, panels/*
tests/       Unit tests for every parser and the process layer
```

`emeron_core` is a static library with no ImGui and no window, so every parser
is unit-testable headlessly. The GUI target depends on it, never the reverse.

### Threading

Three kinds of thread, and the rules between them are the thing to keep
straight when adding a panel:

1. **The UI thread** owns all panel state and must never block on adb.
2. **Collector threads** — one each for `DeviceManager`, `MetricSampler`,
   `FrameStatsCollector`, `SensorReader`, `LogcatReader`, plus one per Perfetto
   capture. Each
   publishes into a `Shared<T>`, which panels borrow under lock via
   `readSnapshot(fn)` — no per-frame copying.
3. **The `ThreadPool`** runs one-shot work (list packages, pull a file, run a
   query). Results come back through `Dispatcher`, which `Application` drains
   once per frame *before* any panel draws. So dispatcher callbacks run on the
   UI thread and need no locking of their own.

A panel that needs device data therefore looks like:

```cpp
context.pool.submit([this, &context] {
    auto result = context.devices.adb().something();      // blocking, worker thread
    context.dispatcher.post([this, result] { apply(result); });  // UI thread
});
```

### Error handling

Anything touching the outside world returns `em::Result<T>` (a `std::expected`
subset — see the comment in `core/Result.h` for why it is not `std::expected`).
Exceptions are for programmer errors only and never cross a module boundary.
`EM_TRY(name, expr)` propagates early.

### One batched probe per tick

Each `adb` invocation costs 20–60 ms of process setup. `AdbClient::shellBatch`
joins N commands into a single `adb shell` with a sentinel between them and
splits the output, so the dashboard's eight probes are one round trip rather
than eight. This is the difference between smooth plots and visible jitter —
preserve it when adding a metric.

---

## Known limits, stated plainly

**Sensors are enumerate-complete but not stream-capable.** Every sensor the HAL
exposes is listed with full metadata. Live *values* only appear for a sensor
some on-device app has already activated, and polling `dumpsys sensorservice`
tops out around 2 Hz. That is a limit of the dumpsys interface. Real gyroscope
streaming needs a small on-device agent (an APK or `app_process` script holding
a `SensorEventListener`) writing to a socket reached over `adb forward`;
`SensorReader.h` documents where that plugs in.

**The Perfetto Explore tab uses the batch transport.** Queries run
`trace_processor_shell -q <file> <trace>` and parse its tabular stdout, which is
accepted in comma-, tab- and pipe-delimited forms because the shell has used all
three across versions. The faster `--httpd` RPC transport is scaffolded —
daemon lifecycle, health checking and HTTP plumbing are all in place — but its
`/query` response is a protobuf-encoded `QueryResult`, and that decoder is the
one piece not written. `queryRpc` returns an explicit "unsupported" rather than
pretending. See `TraceProcessor.h`.

**Logcat filters at two levels, and only one of them is reversible.** The
host-side filters (pid set, level, tag, text) run at display time over the whole
retained buffer, so widening one immediately reveals context already captured.
The device-side ones cannot be undone from the host: `--pid` means other
processes never cross USB, and the `*:LEVEL` spec means lower-priority lines are
never sent. `logcat` also accepts only a single `--pid`, so a package running as
several processes is filtered host side only. One useful consequence of keeping
the buffer unfiltered: with scope set to the selected process and the app not
yet running, lines are still retained, so the app's own startup logs appear the
moment its pid resolves.

**`/data/data` is unreadable on production builds.** Expected: the shell user
has no access. Use `adb shell run-as <package>` for a debuggable app, or
`adb root` on a userdebug build.

**Per-app CPU can exceed 100%.** It is summed across cores, as `/proc` reports
it. Device-wide CPU is normalised to 0–100%.

## Where to extend

- **A native adb transport.** `IAdbTransport` is the only seam to adb.
  Implementing it against the smartsockets protocol on tcp/5037 removes a
  process spawn per call and gives real binary streams for file transfer.
  Nothing above it changes.
- **A new panel.** Subclass `Panel`, give it a stable `kId` (it is the
  `imgui.ini` key), register it in `Application::Impl::registerPanels`, and add
  it to `buildDefaultLayout`.
- **A new metric.** Add a `Probe` enum entry and its command in
  `MetricSampler`'s `buildProbeCommands`, a parser with a test, and a
  `RingBuffer` in `MetricHistory`.
- **Crash grouping in Logcat.** Fatal lines and native tombstones are shown
  and highlighted but not grouped. Collapsing a Java stack trace or a
  tombstone into one expandable entry would key off the `AndroidRuntime` /
  `DEBUG` tag runs in `LogcatReader`.
