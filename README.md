# KytyPS5 — Linux-First PS4/PS5 Emulator

A community fork of [KytyPS5](https://github.com/KytyPS5/KytyPS5) focused on **Linux-first stability, correctness, and performance**.

> ⚠️ **Early Development.** This emulator is in an early stage. Most PS4/PS5 titles do **not** run to completion. Do not expect playable framerates or full game compatibility. This fork prioritises architectural correctness and a maintainable Linux platform layer over quick hacks.

---

## What Changed in This Fork

This fork diverges from upstream with a strict engineering philosophy:

**Correctness > Stability > Compatibility > Performance > Cosmetics**

### Fixed

- **Virtual Memory Manager (Linux):** Replaced `pthread_mutex_t` with `std::shared_mutex` to allow concurrent read access during page-protection lookups, reducing lock contention on multi-threaded guests.
- **Race Condition Elimination:** Removed runtime `/proc/self/maps` parsing in favour of `MAP_FIXED_NOREPLACE` (Linux >= 4.17), making guest memory mapping atomic and race-free.
- **TLB Optimisation:** Added best-effort `madvise(MADV_HUGEPAGE)` for allocations >= 2 MiB to reduce TLB misses on large guest memory regions.

### Improved

- **Pipeline Cache Threading:** Replaced exclusive `Common::Mutex` with `std::shared_mutex` and a double-checked locking pattern so that pipeline cache lookups no longer block on unrelated pipeline compilations.
- **Linux Platform Layer:** Consolidated Linux-specific code paths, removed dead Windows-oriented branches from the Linux compilation unit, and enforced modern kernel requirements explicitly.
- **Build Hygiene:** Added a strict `.gitignore` covering build artefacts, shader caches, game files, secrets, and IDE temporaries.

### Linux

- **Kernel Requirement:** Linux >= 4.17 (for `MAP_FIXED_NOREPLACE`).
- **Display:** Native Wayland and X11/XWayland supported via SDL2 + Vulkan WSI.
- **Graphics:** Vulkan 1.3 required. Tested conceptually against RADV (AMD), NVK/NVIDIA, and ANV (Intel). No vendor-specific hacks are included; all paths are capability-gated.
- **Audio:** SDL2 audio backend (PipeWire / PulseAudio / ALSA handled by SDL).
- **Input:** SDL2 controller backend (DualShock 4, DualSense, Xbox, generic HID).

### Known Limitations

- **No CPU JIT:** Kyty uses native x86-64 execution with HLE trampolines. There is no instruction translator. This is by upstream design and has **not** been changed in this fork.
- **HLE Coverage:** Many PS4/PS5 kernel and library calls remain stubs or partial implementations. Games that depend on unimplemented syscalls will crash or hang.
- **GPU Accuracy:** The Vulkan renderer is functional for basic draws but does not yet cover all guest GPU state permutations.
- **No Game Compatibility Database:** There is no curated list of working games. Treat all titles as experimental.
- **No Online Features:** PSN, networking, and online services are not implemented.

---

## Build Instructions (Linux)

### Dependencies

| Package | Arch / CachyOS | Fedora | Ubuntu / Debian |
|---|---|---|---|
| Git | `git` | `git` | `git` |
| CMake >= 3.24 | `cmake` | `cmake` | `cmake` |
| Ninja | `ninja` | `ninja-build` | `ninja-build` |
| Clang >= 15 | `clang` | `clang` | `clang` |
| Qt 6 (Widgets, Multimedia) | `qt6-base` | `qt6-qtbase` | `qt6-base-dev` |
| Vulkan SDK / Headers | `vulkan-devel` | `vulkan-devel` | `libvulkan-dev` |
| SDL2 | `sdl2` | `SDL2-devel` | `libsdl2-dev` |
| glslang | `glslang` | `glslang-devel` | `glslang-dev` |
| SPIRV-Tools | `spirv-tools` | `spirv-tools` | `spirv-tools` |
| fmt | `fmt` | `fmt-devel` | `libfmt-dev` |
| xxHash | `xxhash` | `xxhash-devel` | `libxxhash-dev` |
| spdlog | `spdlog` | `spdlog-devel` | `libspdlog-dev` |
| nlohmann-json | `nlohmann-json` | `json-devel` | `nlohmann-json3-dev` |
| Tracy (optional) | `tracy` | — | — |

### Clone

```bash
git clone --recursive https://github.com/Bilazzzz/KytyPS5.git
cd KytyPS5
