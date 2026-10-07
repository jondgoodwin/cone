/** Handling for reference types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef reference_h
#define reference_h

// Metadata for normalized reference type
typedef struct {
    LLVMTypeRef llvmtyperef;
    LLVMTypeRef structype;
    LLVMTypeRef ptrstructype;
} RefTypeInfo;

enum ManagedRefFields {
    RegionField,
    PermField,
    ValueField
};

// Reference node: used for reference type, allocation or borrow node
typedef struct {
    ITypeNodeHdr;
    INode *vtexp;     // Value/type expression
    INode *perm;      // Permission
    INode *region;    // Region
    RefTypeInfo *typeinfo; // normalized ref info
    uint16_t scope;   // Lifetime: its band, 0 global, 1 the caller's, 2+ a block
    Name *lifename;   // The lifetime a signature names on it, or NULL (lifetime.h)
    Name *bound;      // A virtual reference's bound, '&<Trait + 'a': what the referenced value's borrows outlive, or NULL (lifetime.h)
    // Written '+R-perm T' (parsePlus). As an allocation that spelling is
    // refused (allocateTypeCheck, ErrorPlusAlloc); as a single or virtual
    // reference type it is refused (refTypeCheck), except at a match pattern's
    // root, which keeps it until patterns are given their own
    // (castPatternMark clears it there). A managed reference type is written
    // 'R[perm, T]' (fnCallLowerManagedRef), and allocated 'new R[perm, T](...)'
    // (typeLitNewAllocate).
    uint16_t plusSpelled;
} RefNode;

// Create a new reference type whose info will be filled in afterwards
RefNode *newRefNode(uint16_t tag);

// Set while a signature's 'self' is checked: the one place '&new' is written
extern int refAllowNewPerm;

// Does this reference type carry the length of its target: a pointer and a
// count, where a reference to a type with a size is a pointer alone? The target
// decides, whatever the region (itypeLenBodyElem).
int refIsFat(RefNode *ref);

// Allocate normalized reference type info
void *refTypeInfoAlloc();

// Clone reference
INode *cloneRefNode(CloneState *cstate, RefNode *node);

// Create a new reference type whose info is known and analyzeable
RefNode *newRefNodeFull(uint16_t tag, INode *lexnode, INode *region, INode *perm, INode *vtype);

// Set the inferred value type of a reference
void refSetPermVtype(RefNode *refnode, INode *perm, INode *vtype);

// Set type infection flags based on the reference's type parameters
void refAdoptInfections(RefNode *refnode);

// Whether a reference may cross to another thread, as far as the reference
// itself says (refThreadBinds): what it points at is asked separately
typedef enum {
    RefCrosses,         // It may, if what it points at may
    RefCrossesAll,      // It may, whatever it points at: a function's code
    RefBindsBorrow,     // A borrowed reference
    RefBindsTraced,     // A reference into a traced region
    RefBindsPerm,       // An owner that may be aliased, whose permission is not RaceSafe
    RefBindsShared      // An owner that may be aliased, in a region not declaring ThreadSafe
} RefBinds;

RefBinds refThreadBinds(RefNode *ref);

// Create a new ArrayDerefNode from an ArrayRefNode
RefNode *newArrayDerefNodeFrom(RefNode *refnode);

// Serialize a reference type
void refPrint(RefNode *node);

// Name resolution of a reference node
void refNameRes(NameResState *pstate, RefNode *node);

// Refuse an owning reference's region that is not a struct declaring 'is RegionRef'.
// A slot naming something other than a type is given the error type.
void refRegionCheck(INode **regionp);

// Judge a managed reference type whose permission is a struct: it must be a
// lock permission, and its region one the lock fits (ir/types/permission.c)
void refLockCheck(RefNode *node);

// Check what a reference, pointer or slice points at: resolved, and laid out
// only where no layout is in flight, since a reference's size is its kind's.
// '*waiting' says whether its layout may be waiting (structTargetWait).
int refTargetTypeCheck(TypeCheckState *pstate, INode **targetp, int *waiting);

// Type check a reference node
void refTypeCheck(TypeCheckState *pstate, RefNode *name);

// Type check a virtual reference node
void refvirtTypeCheck(TypeCheckState *pstate, RefNode *node);

// Compare two reference signatures to see if they are equivalent
int refIsSame(RefNode *node1, RefNode *node2);

// Calculate hash for a structural reference type
size_t refHash(RefNode *node);

// Compare two reference signatures to see if they are equivalent at runtime
int refIsRunSame(RefNode *node1, RefNode *node2);

// Will from region coerce to a to region
TypeCompare regionMatches(INode *to, INode *from, SubtypeConstraint constraint);

// Would a reference held behind a readable reference, seen as 'to' in place of
// its own type 'from', be copied out by a read where its own type moves?
int refHeldMoveSeenAsCopy(INode *to, INode *from);

// Will from reference coerce to a to reference (we know they are not the same)
TypeCompare refMatches(RefNode *to, RefNode *from, SubtypeConstraint constraint);

// Will from reference coerce to a virtual reference (we know they are not the same)
TypeCompare refvirtMatchesRef(RefNode *to, RefNode *from, SubtypeConstraint constraint);

// Will from reference coerce to a virtual reference (we know they are not the same)
TypeCompare refvirtMatches(RefNode *to, RefNode *from, SubtypeConstraint constraint);

// Return a type that is the supertype of both type nodes, or NULL if none found
INode *refFindSuper(INode *type1, INode *type2);

#endif
