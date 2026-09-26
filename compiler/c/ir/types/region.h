/** region type handling. A region is a struct declaring 'is RegionRef'
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef region_h
#define region_h

// Is this region slot's type a struct declaring 'is RegionRef'? A borrowed
// reference's region answers no.
int regionIsRegionRef(INode *region);

// The region method of this name (alloc, init, alias, dealias, free), or NULL
// where the region declares none, or declares something else under the name
FnDclNode *regionMethod(INode *region, Name *name);

// Is a copy of a reference into this region another owner, counted by its
// 'alias'? A region without one has a single owner, and a copy is a move.
int regionIsCounted(INode *region);

// Is a reference into this region released when an owner goes away: through
// 'dealias' where the region has one, and as the value's death where it has a
// single owner? Every RegionRef is.
int regionIsOwning(INode *region);

// At the end of a struct's name resolution: a RegionRef without 'alias' is
// marked MoveType, as '@move' marks it
void regionNameRes(StructNode *node);

// Hold a struct declaring 'is RegionRef' to the shapes and set of its methods
void regionRefCheck(StructNode *node);

// At an allocation: the region can allocate
void regionAllocTypeCheck(INode *region);

#endif
