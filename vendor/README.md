# vendor/

Third-party source, committed so that `cmake` works with no network access and
a clone always builds the exact code that was tested.

[MANIFEST.txt](MANIFEST.txt) records the upstream repository, tag, commit and
license for each, plus what was pruned and why.

| | version | license |
|---|---|---|
| [dear imgui](https://github.com/ocornut/imgui) (docking branch) | v1.92.9b-docking | MIT |
| [implot](https://github.com/epezent/implot) | v1.0 | MIT |
| [glfw](https://github.com/glfw/glfw) | 3.4 | Zlib/libpng |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | v1.4.1 | Zlib |
| [Font Awesome Free](https://fontawesome.com) — Solid | 6.7.2 | Font: SIL OFL 1.1, icons: CC BY 4.0 |
| [IconFontCppHeaders](https://github.com/juliettef/IconFontCppHeaders) — `IconsFontAwesome6.h` | 2026-06-05 | Zlib |

All are permissively licensed and redistributable. Their license files are
kept alongside the source and must stay there.

Font Awesome Free's icons are CC BY 4.0, which asks for attribution. If emeron
is distributed as a binary, credit it somewhere a user can see -- an About box
or the release notes: "Icons by Font Awesome (https://fontawesome.com),
CC BY 4.0." 

## How the build picks these up

`cmake/Dependencies.cmake` prefers this directory and falls back to a pinned
`FetchContent` download if it is missing, so neither path is a dead end:

```sh
cmake -S . -B build                          # uses vendor/ (default)
cmake -S . -B build -DEMERON_VENDORED=OFF    # downloads the pinned tags instead
```

The configure summary prints which origin each dependency came from.

## Not vendored: adb and trace_processor_shell

These are *runtime* tools, not source, and deliberately absent:

- **adb** — the Android SDK terms restrict redistributing SDK components, and
  it is ~19 MB per platform. Anyone doing Android work already has it. emeron
  finds it via `--adb`, `$EMERON_ADB`, `$ANDROID_HOME`, or `PATH`.
- **trace_processor_shell** — Apache-2.0 and redistributable, but a separate
  ~80 MB binary for each of macOS/Linux/Windows and arm64/x64. That does not
  belong in git. It is optional; only the Perfetto *Explore* tab needs it.

## Updating a dependency

1. Check out the new upstream tag somewhere scratch.
2. Copy the same file set over the existing directory (see the `pruned` notes
   in MANIFEST.txt for what to leave out) and keep the license file.
3. Update the tag and commit in MANIFEST.txt, and the matching `*_TAG` in
   `cmake/Dependencies.cmake`, so a `-DEMERON_VENDORED=OFF` build resolves to
   the same revision.
4. Re-apply local patches. `grep -rn "EMERON PATCH" vendor/` lists them, and
   MANIFEST.txt says why each exists. Today it is only imgui_tables.cpp (NaN
   column weights). A `-DEMERON_VENDORED=OFF` build fetches stock upstream
   and does not get them.
5. Build and run the tests. For an imgui or implot bump, read their release
   notes for API breaks first -- the ImPlot v0.16 to v1.0 jump rewrote the
   per-item styling API and touched every plotting call site.
