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
// 'alias'? Without one, a copy is a move where the region is 'Move', and costs
// nothing where it is not.
int regionIsCounted(INode *region);

// Is this region ref 'Move': one owner per value, a copy of a reference a move,
// and every owner's going the value's death?
int regionIsMove(INode *region);

// Is a reference into this region an owner, whose going is the region's to
// hear of: through 'dealias' where it has one, as the value's death where it is
// 'Move', and not at all where it has neither? Every RegionRef is.
int regionIsOwning(INode *region);

// Hold a struct declaring 'is RegionRef' to the shapes and set of its methods
void regionRefCheck(StructNode *node);

// At an allocation: the region can allocate
void regionAllocTypeCheck(INode *region);

// Does the region's 'alloc' ask for the value type's record, by taking
// 'ty *TypeRecord' after the size?
int regionAllocTakesRecord(INode *region);

// Is this region slot's type a region ref declaring 'Traced', whose references
// a trace hands to its 'mark'?
int regionIsTraced(INode *region);

// Does the traced region's 'mark' take each reference's permission and the
// trace's mode, 'fn mark(self &uni R, perm u32, mode u32)'?
int regionMarkTakesContext(INode *region);

// A type declaring 'Traced' that is not a region ref is refused
void regionTracedUseCheck(StructNode *node);

// Note, as type check meets them, the places where a traced reference may not
// be held: an owning reference type, a global or static, an instance of
// mem.writeRaw or mem.moveRaw. Each is judged by regionTracedCheckAll.
void regionTracedRefNote(RefNode *node);
void regionTracedGlobalNote(VarDclNode *var);
void regionTracedRawNote(FnDclNode *fndcl, int16_t intrinsic, INode *typearg);

// Judge every noted place, once type check has finished and every type is laid
// out: the rules of where a traced reference may be held
void regionTracedCheckAll();

#endif
