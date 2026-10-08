# DolphinPS4 PS4 runtime

Copied unchanged from DolphinPS4 (`port/`, GPL-2.0-or-later; dlmalloc is MIT-0) at
https://github.com/iHaiDeeZ/DolphinPS4, verified on retail consoles:

- `ps4_runtime.cpp` – app heap (dlmalloc on system flexible memory, direct-memory overflow heap,
  per-thread arenas), `__wrap_malloc` & co., the libc malloc-replacement table targets,
  `__wrap_mmap` (large mappings from the system pool; executable mappings by the method
  `ps4_exec_probe()` found), `__wrap_pthread_once`, `__wrap_clock_gettime`,
  `__cxa_thread_atexit_impl`.
- `ps4_dlmalloc.c` + `third_party/dlmalloc/malloc.c` – the allocator.
- `ps4_crt1.S` – replacement crt1.o that fills in libc's malloc-replacement table, so Sony's
  system modules allocate from the app heap too (assembled by cmake/ps4-eden.cmake).
- `ps4_string.c` – SSE2 memcpy/memset/memcmp/memmove/bcmp.

Log lines keep DolphinPS4's "[dolphin]" prefix; they go to the boot trace (/data/switch-emu/logs/boot-trace.log).
