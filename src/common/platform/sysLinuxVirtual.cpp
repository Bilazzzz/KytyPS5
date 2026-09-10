#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
// #error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include "common/assert.h"
#include "common/platform/sysVirtual.h"
#include "common/virtualMemory.h"

#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

// IWYU pragma: no_include <asm/mman-common.h>
// IWYU pragma: no_include <asm/mman.h>
// IWYU pragma: no_include <bits/pthread_types.h>
// IWYU pragma: no_include <linux/mman.h>

// MAP_FIXED_NOREPLACE is mandatory for safe, race-free memory mapping.
// It was introduced in Linux kernel 4.17 (2018). We require it for correctness.
#if !defined(MAP_FIXED_NOREPLACE) && KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#error "MAP_FIXED_NOREPLACE is required for safe memory emulation. Please use Linux kernel >= 4.17."
#endif

namespace Common {

	// Using std::shared_mutex allows concurrent reads (e.g., page protection checks)
	// while serializing writes (allocations/deallocations).
	static std::shared_mutex            g_virtual_mutex {};
	static std::map<uintptr_t, size_t>* g_allocs   = nullptr;
	static std::map<uintptr_t, int>*    g_protects = nullptr;

	void SysVirtualInit() {
		g_allocs   = new std::map<uintptr_t, size_t>;
		g_protects = new std::map<uintptr_t, int>;
	}

	static int get_protection_flag(VirtualMemory::Mode mode) {
		int protect = PROT_NONE;
		switch (mode) {
			case VirtualMemory::Mode::Read: protect = PROT_READ; break;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: protect = PROT_READ | PROT_WRITE; break; // NOLINT
			case VirtualMemory::Mode::Execute: protect = PROT_EXEC; break;
			case VirtualMemory::Mode::ExecuteRead: protect = PROT_EXEC | PROT_READ; break; // NOLINT
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite:
				protect = PROT_EXEC | PROT_WRITE | PROT_READ;
				break; // NOLINT
			case VirtualMemory::Mode::NoAccess:
			default: protect = PROT_NONE; break;
		}
		return protect;
	}

	static VirtualMemory::Mode get_protection_flag(int mode) {
		switch (mode) {
			case PROT_NONE: return VirtualMemory::Mode::NoAccess;
			case PROT_READ: return VirtualMemory::Mode::Read;
			case PROT_WRITE: return VirtualMemory::Mode::Write;
			case PROT_READ | PROT_WRITE: return VirtualMemory::Mode::ReadWrite; // NOLINT
			case PROT_EXEC: return VirtualMemory::Mode::Execute;
			case PROT_EXEC | PROT_WRITE: return VirtualMemory::Mode::ExecuteWrite; // NOLINT
			case PROT_EXEC | PROT_READ: return VirtualMemory::Mode::ExecuteRead;   // NOLINT
			case PROT_EXEC | PROT_WRITE | PROT_READ:
				return VirtualMemory::Mode::ExecuteReadWrite; // NOLINT
			default: return VirtualMemory::Mode::NoAccess;
		}
	}

	// Keep automatic mappings inside the guest and GPU-addressable low window.
	static constexpr uintptr_t LOW_ARENA_LIMIT = 0x000000FC00000000ULL; // libc mspace window ceiling
	static constexpr uintptr_t LOW_ARENA_FLOOR = 0x000000A000000000ULL; // 640 GiB
	static constexpr uintptr_t LOW_ARENA_GRAIN = 0x0000000000010000ULL; // 64 KiB

	static_assert(LOW_ARENA_LIMIT <= 0x0000010000000000ULL,
				  "arena must stay inside the GPU page tracker's 1<<40 window");
	static_assert(LOW_ARENA_FLOOR < LOW_ARENA_LIMIT, "arena floor must sit below its ceiling");

	static std::atomic<uintptr_t> g_low_arena_next {LOW_ARENA_LIMIT};

	// Caller holds g_virtual_mutex.
	static void record_alloc(uintptr_t addr, size_t size) {
		auto next = g_allocs->upper_bound(addr);
		if (next != g_allocs->begin()) {
			auto       it         = std::prev(next);
			const auto alloc_addr = it->first;
			const auto alloc_end  = alloc_addr + it->second;
			if (alloc_addr <= addr && addr + size <= alloc_end) {
				g_allocs->erase(it);
				if (alloc_addr < addr) {
					(*g_allocs)[alloc_addr] = addr - alloc_addr;
				}
				if (addr + size < alloc_end) {
					(*g_allocs)[addr + size] = alloc_end - (addr + size);
				}
			}
		}
		(*g_allocs)[addr] = size;
	}

	static uintptr_t align_up_to(uintptr_t addr, uint64_t alignment) {
		return (addr + alignment - 1) & ~(alignment - 1);
	}

	// Freed arena addresses are not reused while GPU caches remain keyed by address.
	static void* map_anonymous(uintptr_t addr, size_t size, int protect, int flags) {
		if (addr != 0) {
			void* ptr = mmap(reinterpret_cast<void*>(addr), size, protect, flags, -1, 0); // NOLINT
			// Performance: Enable Transparent Huge Pages for large allocations to reduce TLB misses
			if (ptr != MAP_FAILED && size >= 2 * 1024 * 1024) {
				::madvise(ptr, size, MADV_HUGEPAGE);
			}
			return ptr;
		}

		const auto step = align_up_to(size, LOW_ARENA_GRAIN);
		for (int attempt = 0; attempt < 256; attempt++) {
			const auto top = g_low_arena_next.fetch_sub(step, std::memory_order_relaxed);
			if (top < step || top - step < LOW_ARENA_FLOOR) {
				break;
			}
			const auto hint = (top - step) & ~(LOW_ARENA_GRAIN - 1);
			void* ptr = mmap(reinterpret_cast<void*>(hint), size, protect, flags | MAP_FIXED_NOREPLACE,
							 -1, 0); // NOLINT
			if (ptr != MAP_FAILED) {
				// Performance: Enable Transparent Huge Pages for large allocations
				if (size >= 2 * 1024 * 1024) {
					::madvise(ptr, size, MADV_HUGEPAGE);
				}
				return ptr;
			}
		}

		void* ptr = mmap(nullptr, size, protect, flags, -1, 0); // NOLINT
		if (ptr != MAP_FAILED && size >= 2 * 1024 * 1024) {
			::madvise(ptr, size, MADV_HUGEPAGE);
		}
		return ptr;
	}

	uint64_t SysVirtualAlloc(uint64_t address, uint64_t size, VirtualMemory::Mode mode) {
		EXIT_IF(g_allocs == nullptr);

		auto addr = static_cast<uintptr_t>(address);

		int protect = get_protection_flag(mode);

		void* ptr = map_anonymous(addr, size, protect, MAP_PRIVATE | MAP_ANON);

		auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

		if (ptr != MAP_FAILED) {
			std::unique_lock lock(g_virtual_mutex);
			record_alloc(ret_addr, size);
			uintptr_t page_start = ret_addr >> 12u;
			uintptr_t page_end   = (ret_addr + size - 1) >> 12u;
			for (uintptr_t page = page_start; page <= page_end; page++) {
				(*g_protects)[page] = protect;
			}
		}

		return ret_addr;
	}

	static uintptr_t align_up(uintptr_t addr, uint64_t alignment) {
		return (addr + alignment - 1) & ~(alignment - 1);
	}

	uint64_t SysVirtualAllocAligned(uint64_t address, uint64_t size, VirtualMemory::Mode mode,
									uint64_t alignment) {
		if (alignment == 0) {
			return 0;
		}

		EXIT_IF(g_allocs == nullptr);

		auto addr    = static_cast<uintptr_t>(address);
		int  protect = get_protection_flag(mode);

		void* ptr = map_anonymous(addr, size, protect, MAP_PRIVATE | MAP_ANON);

		auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

		if (ptr != MAP_FAILED && ((ret_addr & (alignment - 1)) != 0)) {
			munmap(ptr, size);

			ptr =
			map_anonymous(addr, size + alignment, protect, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE);
			ret_addr = reinterpret_cast<uintptr_t>(ptr);
			if (ptr != MAP_FAILED) {
				#if defined(__APPLE__)
				// Carve the aligned subrange out of the live mapping with MAP_FIXED (in-place
				// replacement) and trim the slack; never munmap the whole range first, or a
				// concurrent host mapping (dyld, Rosetta, Metal) could claim the hole and be
				// destroyed by the MAP_FIXED. Other platforms keep the original path below.
				auto aligned_addr = align_up(ret_addr, alignment);
				// NOLINTNEXTLINE
				void* fixed = mmap(reinterpret_cast<void*>(aligned_addr), size, protect,
								   MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
				if (fixed == MAP_FAILED) {
					munmap(ptr, size + alignment);
					ret_addr = 0;
					ptr      = MAP_FAILED;
				} else {
					if (aligned_addr > ret_addr) {
						munmap(reinterpret_cast<void*>(ret_addr), aligned_addr - ret_addr);
					}
					const uintptr_t tail_start = aligned_addr + size;
					const uintptr_t resv_end   = ret_addr + size + alignment;
					if (resv_end > tail_start) {
						munmap(reinterpret_cast<void*>(tail_start), resv_end - tail_start);
					}
					ptr      = fixed;
					ret_addr = aligned_addr;
				}
				#else
				munmap(ptr, size + alignment);
				auto aligned_addr = align_up(ret_addr, alignment);
				// NOLINTNEXTLINE
				ptr = mmap(reinterpret_cast<void*>(aligned_addr), size, protect,
						   MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON, -1, 0);
				ret_addr = reinterpret_cast<uintptr_t>(ptr);
				if (ptr != MAP_FAILED && ((ret_addr & (alignment - 1)) != 0)) {
					munmap(ptr, size);
					ret_addr = 0;
					ptr      = MAP_FAILED;
				}
				#endif
			}
		}

		if (ptr == MAP_FAILED) {
			return SysVirtualAllocAligned(address, size, mode, alignment << 1u);
		}

		std::unique_lock lock(g_virtual_mutex);
		record_alloc(ret_addr, size);
		uintptr_t page_start = ret_addr >> 12u;
		uintptr_t page_end   = (ret_addr + size - 1) >> 12u;
		for (uintptr_t page = page_start; page <= page_end; page++) {
			(*g_protects)[page] = protect;
		}

		return ret_addr;
									}

									bool SysVirtualAllocFixed(uint64_t address, uint64_t size, VirtualMemory::Mode mode) {
										EXIT_IF(g_allocs == nullptr);

										auto addr    = static_cast<uintptr_t>(address);
										int  protect = get_protection_flag(mode);

										// MAP_FIXED_NOREPLACE guarantees atomicity and prevents race conditions
										// where another thread could claim the address between a check and the mapping.
										// NOLINTNEXTLINE
										void* ptr = mmap(reinterpret_cast<void*>(addr), size, protect,
														 MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON, -1, 0);

										auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

										if (ptr != MAP_FAILED && ret_addr != addr) {
											munmap(ptr, size);
											ret_addr = 0;
											ptr      = MAP_FAILED;
										}

										if (ptr != MAP_FAILED) {
											std::unique_lock lock(g_virtual_mutex);
											record_alloc(ret_addr, size);
											uintptr_t page_start = ret_addr >> 12u;
											uintptr_t page_end   = (ret_addr + size - 1) >> 12u;
											for (uintptr_t page = page_start; page <= page_end; page++) {
												(*g_protects)[page] = protect;
											}

											return true;
										}

										return false;
									}

									bool SysVirtualCommit(uint64_t address, uint64_t size, VirtualMemory::Mode mode) {
										return SysVirtualProtect(address, size, mode);
									}

									uint64_t SysVirtualReserve(uint64_t address, uint64_t size) {
										return SysVirtualReserveAligned(address, size, 1);
									}

									uint64_t SysVirtualReserveAligned(uint64_t address, uint64_t size, uint64_t alignment) {
										if (alignment == 0) {
											return 0;
										}

										EXIT_IF(g_allocs == nullptr);

										auto addr = static_cast<uintptr_t>(address);

										void* ptr = map_anonymous(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE);

										auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

										if (ptr != MAP_FAILED && ((ret_addr & (alignment - 1)) != 0)) {
											munmap(ptr, size);

											ptr      = map_anonymous(addr, size + alignment, PROT_NONE,
																	 MAP_PRIVATE | MAP_ANON | MAP_NORESERVE);
											ret_addr = reinterpret_cast<uintptr_t>(ptr);
											if (ptr != MAP_FAILED) {
												#if defined(__APPLE__)
												// Carve the aligned subrange out of the live reservation with MAP_FIXED (an
												// in-place replacement), then trim the slack. The range must never be
												// returned to the OS in between: another thread (dyld, Rosetta, Metal,
												// malloc) could claim the hole, and the subsequent MAP_FIXED would silently
												// destroy its mapping. Other platforms keep the original path below.
												auto aligned_addr = align_up(ret_addr, alignment);
												// NOLINTNEXTLINE
												void* fixed = mmap(reinterpret_cast<void*>(aligned_addr), size, PROT_NONE,
																   MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
												if (fixed == MAP_FAILED) {
													munmap(ptr, size + alignment);
													ret_addr = 0;
													ptr      = MAP_FAILED;
												} else {
													if (aligned_addr > ret_addr) {
														munmap(reinterpret_cast<void*>(ret_addr), aligned_addr - ret_addr);
													}
													const uintptr_t tail_start = aligned_addr + size;
													const uintptr_t resv_end   = ret_addr + size + alignment;
													if (resv_end > tail_start) {
														munmap(reinterpret_cast<void*>(tail_start), resv_end - tail_start);
													}
													ptr      = fixed;
													ret_addr = aligned_addr;
												}
												#else
												munmap(ptr, size + alignment);
												auto aligned_addr = align_up(ret_addr, alignment);
												// NOLINTNEXTLINE
												ptr = mmap(reinterpret_cast<void*>(aligned_addr), size, PROT_NONE,
														   MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
												ret_addr = reinterpret_cast<uintptr_t>(ptr);
												if (ptr != MAP_FAILED && ((ret_addr & (alignment - 1)) != 0)) {
													munmap(ptr, size);
													ret_addr = 0;
													ptr      = MAP_FAILED;
												}
												#endif
											}
										}

										if (ptr == MAP_FAILED) {
											return SysVirtualReserveAligned(address, size, alignment << 1u);
										}

										std::unique_lock lock(g_virtual_mutex);
										record_alloc(ret_addr, size);

										return ret_addr;
									}

									bool SysVirtualReserveFixed(uint64_t address, uint64_t size) {
										EXIT_IF(g_allocs == nullptr);

										auto addr = static_cast<uintptr_t>(address);

										// NOLINTNEXTLINE
										void* ptr = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
														 MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);

										auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

										if (ptr != MAP_FAILED && ret_addr != addr) {
											munmap(ptr, size);
											ret_addr = 0;
											ptr      = MAP_FAILED;
										}

										if (ptr != MAP_FAILED) {
											std::unique_lock lock(g_virtual_mutex);
											record_alloc(ret_addr, size);

											return true;
										}

										return false;
									}

									bool SysVirtualDecommit(uint64_t address, uint64_t size) {
										// Drop physical pages while preserving the reservation.
										if (!SysVirtualProtect(address, size, VirtualMemory::Mode::NoAccess)) {
											return false;
										}

										if (size != 0) {
											#if defined(__APPLE__)
											constexpr int RECLAIM_ADVICE = MADV_FREE;
											#else
											constexpr int RECLAIM_ADVICE = MADV_DONTNEED;
											#endif
											const auto page_size = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
											if (page_size != 0) {
												// Do not discard pages outside the requested range.
												const auto begin = (static_cast<uintptr_t>(address) + page_size - 1) & ~(page_size - 1);
												const auto end   = (static_cast<uintptr_t>(address) + size) & ~(page_size - 1);
												if (end > begin) {
													::madvise(reinterpret_cast<void*>(begin), end - begin, RECLAIM_ADVICE);
												}
											}
										}

										return true;
									}

									bool SysVirtualFree(uint64_t address) {
										EXIT_IF(g_allocs == nullptr);
										size_t size = 0;

										auto addr = static_cast<uintptr_t>(address & ~static_cast<uint64_t>(0xfffu));

										{
											std::unique_lock lock(g_virtual_mutex);
											if (auto s = g_allocs->find(addr); s != g_allocs->end()) {
												size = s->second;
												g_allocs->erase(s);
											}
										}

										if (size == 0) {
											return false;
										}

										if (munmap(reinterpret_cast<void*>(addr), size) == 0) {
											uintptr_t page_start = addr >> 12u;
											uintptr_t page_end   = (addr + size - 1) >> 12u;
											std::unique_lock lock(g_virtual_mutex);
											for (uintptr_t page = page_start; page <= page_end; page++) {
												g_protects->erase(page);
											}
											return true;
										}

										return false;
									}

									bool SysVirtualFreeRange(uint64_t address, uint64_t size) {
										EXIT_IF(g_allocs == nullptr);
										if (size == 0 || (address & 0xfffu) != 0 || (size & 0xfffu) != 0) {
											return false;
										}

										const auto addr = static_cast<uintptr_t>(address);
										const auto end  = addr + size;
										if (end < addr) {
											return false;
										}

										std::unique_lock lock(g_virtual_mutex);
										auto next = g_allocs->upper_bound(addr);
										if (next == g_allocs->begin()) {
											return false;
										}

										// A reservation may have been split into several adjacent records.
										auto       first      = std::prev(next);
										const auto alloc_addr = first->first;
										if (addr < alloc_addr || alloc_addr + first->second <= addr) {
											return false;
										}

										auto      last   = first;
										uintptr_t cursor = alloc_addr + first->second;
										while (cursor < end) {
											auto following = std::next(last);
											if (following == g_allocs->end() || following->first != cursor) {
												return false;
											}
											last   = following;
											cursor = following->first + following->second;
										}
										const auto alloc_end = cursor;

										if (munmap(reinterpret_cast<void*>(addr), size) != 0) {
											return false;
										}

										g_allocs->erase(first, std::next(last));
										if (alloc_addr < addr) {
											(*g_allocs)[alloc_addr] = addr - alloc_addr;
										}
										if (end < alloc_end) {
											(*g_allocs)[end] = alloc_end - end;
										}
										for (uintptr_t page = addr >> 12u; page <= (end - 1u) >> 12u; page++) {
											g_protects->erase(page);
										}
										return true;
									}

									bool SysVirtualProtect(uint64_t address, uint64_t size, VirtualMemory::Mode mode,
														   VirtualMemory::Mode* old_mode) {
										auto addr = static_cast<uintptr_t>(address);

										// Use shared_lock for reading old_mode to allow concurrent protection changes
										if (old_mode != nullptr) {
											std::shared_lock lock(g_virtual_mutex);
											if (auto s = g_protects->find(addr >> 12u); s != g_protects->end()) {
												*old_mode = get_protection_flag(s->second);
											} else {
												*old_mode = VirtualMemory::Mode::NoAccess;
											}
										}

										uintptr_t page_start = addr >> 12u;
										uintptr_t page_end   = (addr + size - 1) >> 12u;
										if (mprotect(reinterpret_cast<void*>(page_start << 12u), (page_end - page_start + 1) << 12u,
											get_protection_flag(mode)) == 0) {
											std::unique_lock lock(g_virtual_mutex);
										for (uintptr_t page = page_start; page <= page_end; page++) {
											(*g_protects)[page] = get_protection_flag(mode);
										}
										return true;
											}

											return false;
														   }

														   bool SysVirtualFlushInstructionCache(uint64_t /*address*/, uint64_t /*size*/) {
															   return true;
														   }

} // namespace Common

#endif
