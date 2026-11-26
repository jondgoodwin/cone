# allocheap

`Alloc` over cnmalloc's heap, called directly: no C name in between. The heap is the module's own, made on the first
call from the `CNMALLOC` settings.

No shim serves this heap, so it runs cnmalloc's lock design, not the lock-free one that is `cn.lib`'s default on
Windows. Don't set `CNMALLOC=design=lockfree` for a program that uses `allocheap`: its small allocations crash.

**Platforms: Windows only** (`congo.toml` declares `os = ["windows"]`). It imports `cnmalloc`, which is Windows only, so on Linux its one test fails to link `[planned: Linux]` (cnmalloc's Linux layer is lane-queue row 550).
