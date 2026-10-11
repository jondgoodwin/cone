/** heapregion - the heap's region, the one every collection points to by default
 * @file
 *
 * A growable collection gets, grows and gives back its block through a region
 * (core's 'MemRegion': three functions handed the region itself). This is the
 * heap's, a static object that is always there, so a collection made without a
 * region points to it and is never null-checked: its functions are the C
 * library's 'malloc', 'realloc' and 'free', and nothing more. Cone cannot
 * define it, because a global may be initialised only by a literal and these
 * are function addresses.
 *
 * core's 'heapRegion()' answers its address, and 'cone_heapRegion' is what
 * makes the linker take this object out of conestd.lib: a program that
 * constructs a collection names it.
 *
 * The layout is core's MemRegion: three function pointers, in this order.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include <stddef.h>
#include <stdlib.h>

struct ConeRegion;

struct ConeRegion {
    void *(*alloc)(struct ConeRegion *r, size_t bytes, size_t align);
    void *(*resize)(struct ConeRegion *r, void *block, size_t oldBytes, size_t newBytes, size_t align);
    void (*release)(struct ConeRegion *r, void *block, size_t bytes);
};

static void *heapAlloc(struct ConeRegion *r, size_t bytes, size_t align) {
    (void)r; (void)align;
    return malloc(bytes);
}

static void *heapResize(struct ConeRegion *r, void *block, size_t oldBytes, size_t newBytes, size_t align) {
    (void)r; (void)oldBytes; (void)align;
    return realloc(block, newBytes);
}

static void heapRelease(struct ConeRegion *r, void *block, size_t bytes) {
    (void)r; (void)bytes;
    free(block);
}

// core's `extern mut cone_heapRegion MemRegion` is this object. A Cone module that
// is not C-named spells the symbol of any global, 'extern' ones too, '_C' and
// its path (compiler/c/ir/name.c, nameSymbol): here the module 'core' and the
// 15 characters of 'cone_heapRegion'. Rename one and the other fails to link.
struct ConeRegion _CNvC4core15cone_heapRegion = { heapAlloc, heapResize, heapRelease };
