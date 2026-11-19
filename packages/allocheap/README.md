# allocheap

`Alloc` over cnmalloc's heap, called directly: no C name in between. The heap is the module's own, made on the first
call from the `CNMALLOC` settings.

**Platforms: Windows only** (`congo.toml` declares `os = ["windows"]`). It imports `cnmalloc`, which is Windows only, so on Linux its one test fails to link `[planned: Linux]` (cnmalloc's Linux layer is lane-queue row 550).
