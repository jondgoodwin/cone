/** Allocator types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef alloc_h
#define alloc_h

typedef struct AddrNode AddrNode;
typedef struct PtrNode PtrNode;

void allocAllocate(AddrNode *anode, RefNode *reftype);

#endif