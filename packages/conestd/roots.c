/** roots - the traced references on the stack, for a collector to find
 * @file
 *
 * Every Cone function holding a traced reference on its stack links a frame
 * of them into one chain while it runs: its locals, parameters and births
 * whose types hold one (compiler/c/doc/phases/generation.md, "Roots"). The
 * compiler generates the frames, their maps and the pushes and pops; this is
 * the chain's head and the walk 'mem.traceRoots' calls.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include <stddef.h>
#include <stdint.h>

// Core's TypeRecord, laid out as the compiler builds it (genlTypeRecord)
typedef struct ConeTypeRecord {
    size_t size;
    size_t align;
    void (*finalize)(void *p);
    void (*trace)(void *p, uint32_t mode);
    uint32_t flags;
} ConeTypeRecord;

// A function's root map, a constant: how many roots, and each one's record
typedef struct ConeRootMap {
    size_t count;
    const ConeTypeRecord *rec[];
} ConeRootMap;

// A running function's frame: the frame of the function it was called by
// (or of the nearest one before it holding roots), its map, and the address
// of each root, in the map's order
typedef struct ConeFrame {
    struct ConeFrame *prev;
    const ConeRootMap *map;
    void *slot[];
} ConeFrame;

// The newest frame. One chain: Cone is single threaded, and a thread would
// need its own, thread-local
ConeFrame *cone_gcframes = NULL;

// Trace every root of every running function, newest frame first: each
// root's record's trace, which hands each traced reference the root holds to
// its region's 'mark', with 'mode'
void cone_traceRoots(uint32_t mode) {
    for (ConeFrame *f = cone_gcframes; f; f = f->prev)
        for (size_t i = 0; i < f->map->count; ++i)
            f->map->rec[i]->trace(f->slot[i], mode);
}
