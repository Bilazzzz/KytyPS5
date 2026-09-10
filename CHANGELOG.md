# Changelog

All notable changes to this fork will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-09-10

First tagged release of the Bilazzzz/KytyPS5 Linux-first fork.

### Fixed

- **Virtual Memory Race Condition (Linux):** Removed runtime `/proc/self/maps` parsing
  from `sysLinuxVirtual.cpp`. All fixed-address mappings now use `MAP_FIXED_NOREPLACE`
  (Linux >= 4.17), eliminating a TOCTOU race where a concurrent thread could claim an
  address between the availability check and the `mmap` call.
- **Lock Contention in Virtual Memory Manager:** Replaced global `pthread_mutex_t` with
  `std::shared_mutex`. Read-heavy operations (`SysVirtualProtect` with `old_mode` query)
  now use `std::shared_lock`, allowing concurrent readers. Write operations
  (`SysVirtualAlloc`, `SysVirtualFree`) use `std::unique_lock`.
- **Pipeline Cache Stutter:** Replaced exclusive `Common::Mutex` in `PipelineCache` with
  `std::shared_mutex` and a double-checked locking pattern. Pipeline cache hits no longer
  block behind unrelated pipeline compilations. Compilation itself occurs outside any
  lock; only the final cache insertion takes an exclusive lock.

### Improved

- **Transparent Huge Pages:** Added best-effort `madvise(MADV_HUGEPAGE)` for anonymous
  mappings >= 2 MiB in `sysLinuxVirtual.cpp`. If the kernel denies the hint, execution
  continues normally. This reduces TLB pressure for large guest RAM regions.
- **Linux Platform Layer:** Enforced explicit `MAP_FIXED_NOREPLACE` requirement via
  compile-time `#error` on kernels that lack it. Removed dead `is_mapped()` helper and
  associated `/proc/self/maps` I/O from the hot path.
- **Build Hygiene:** Added comprehensive `.gitignore` covering build artefacts, shader
  caches, game files, secrets, IDE temporaries, and profiler output.

### Added

- **CI Workflow:** GitHub Actions workflow for Linux (Ubuntu 24.04) build validation
  using Clang, Ninja, CMake, Qt 6, and Vulkan headers.
- **Release Workflow:** GitHub Actions workflow for tagged releases producing a
  `KytyPS5-<version>-Linux-x86_64.tar.xz` archive with SHA256 checksums.
- **Documentation:** Rewritten `README.md` with accurate build instructions, dependency
  tables for Arch/Fedora/Ubuntu, Vulkan requirements, and an honest compatibility matrix.

### Known Limitations

- No CPU JIT exists. Kyty uses native x86-64 execution with HLE trampolines. This is
  upstream design and has not been changed.
- Many PS4/PS5 kernel calls and library functions remain stubs or partial
  implementations. Games depending on unimplemented syscalls will crash or hang.
- The Vulkan renderer covers basic draw paths but does not handle all guest GPU state
  permutations.
- No asynchronous pipeline compilation worker pool. Shader stutter is mitigated only by
  driver-side Vulkan pipeline caches and the in-memory `PipelineCache`.
- No game compatibility database. All titles should be treated as experimental.
- Networking, PSN, trophies, and online features are not implemented.
- Test coverage is minimal. Many subsystems lack automated regression tests.
- No reproducible performance benchmarks are published for this release.

[0.1.0]: https://github.com/Bilazzzz/KytyPS5/releases/tag/v0.1.0
