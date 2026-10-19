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

// Does this struct's 'is' list name 'RegionRef', whether or not the struct has
// been name resolved yet? What tells 'Rc[mut, Node]' is a managed reference type.
int regionStructWritesRegionRef(StructNode *strnode);

// The region method of this name (alloc, init, aliasRef, dealiasRef, free), or NULL
// where the region declares none, or declares something else under the name
FnDclNode *regionMethod(INode *region, Name *name);

// Is a copy of a reference into this region another owner, counted by its
// 'aliasRef'? Without one, a copy is a move where the region is 'Move', and costs
// nothing where it is not.
int regionIsCounted(INode *region);

// Is this region ref 'Move': one owner per value, a copy of a reference a move,
// and every owner's going the value's death?
int regionIsMove(INode *region);

// Is a reference into this region an owner, whose going is the region's to
// hear of: through 'dealiasRef' where it has one, as the value's death where it is
// 'Move', and not at all where it has neither? Every RegionRef is.
int regionIsOwning(INode *region);

// Does an owner of a reference into this region going away do anything: ask
// the region's 'dealiasRef', or, for a 'Move' region, the value's death? For
// every other region ref it is nothing at all, and releasing such an owner
// generates nothing.
int regionReleaseActs(INode *region);

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
// trace's mode, 'fn mark(self: &uni R, slot *usize, perm u32, mode u32)'?
int regionMarkTakesContext(INode *region);

// Whether any struct of this compile declares 'Traced', noted as each struct is
// name resolved, before any type is checked. Until one does, no reference is
// traced, and an erased value (core's Erased) holds nothing a trace need find.
void regionTracedSeen(void);
int regionTracedAnywhere(void);

// May a trace pass through an owning reference into this region, to the value it
// holds: it is single-owner (Move), or declares 'visit'?
int regionCanPassTrace(INode *region);

// Is this a traced region declaring 'writeBarrier', which the compiler calls
// with each of its references stored into memory that is not a local?
int regionHasBarrier(INode *region);

// A type declaring 'Traced' that is not a region ref is refused
void regionTracedUseCheck(StructNode *node);

// Is this region slot's type a region ref declaring 'ThreadSafe', whose owners
// of one value several threads may hold at once, its counts changed
// atomically? Nothing asks yet: it is for the thread check to come, which
// judges a reference by its region as well as its permission.
int regionIsThreadSafe(INode *region);

// A type declaring 'ThreadSafe' that is not a region ref is refused
void regionThreadSafeUseCheck(StructNode *node);

// Is this region slot's type a traced region ref declaring 'ActorLocal': its
// references stay in one actor and are collected between that actor's messages?
int regionIsActorLocal(INode *region);

// A type declaring 'ActorLocal' that is not a traced region ref is refused
void regionActorLocalUseCheck(StructNode *node);

// Note, as type check meets them, the places where a traced reference may not
// be held, or must be found by a trace: an owning reference type, a global or
// static, a call of mem.writeRaw or mem.moveRaw (from a method of a type that
// declares no GcTraceElements), a local or a parameter. Each is judged by regionTracedCheckAll.
void regionTracedRefNote(RefNode *node);
void regionTracedGlobalNote(VarDclNode *var);
void regionTracedRootNote(VarDclNode *var);
void regionTracedRawCall(TypeCheckState *pstate, FnCallNode *call, FnDclNode *fndcl);

// Judge every noted place, once type check has finished and every type is laid
// out: the rules of where a traced reference may be held
void regionTracedCheckAll();

#endif
