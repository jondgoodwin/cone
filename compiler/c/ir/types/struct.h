/** Handling for record-based types with fields
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef struct_h
#define struct_h

// Describes how some struct implements a virtual reference's vtable
typedef struct {
    INode *structdcl;          // struct that implements
    Nodes *methfld;            // specific methods and fields in same order as vtable
    Nodes *foldpaths;          // per slot: for a folded method, the fields (Nodes of FieldDclNode) its receiver is reached through; else NULL
    LLVMValueRef llvmvtablep;  // generates a pointer to the implemented vtable
} VtableImpl;

// Describes the virtual interface supported by some trait/struct.
// Its names are spelled at generation from 'trait' and each impl's struct (name.c).
typedef struct {
    INode *trait;              // the trait/struct whose virtual interface this is
    Nodes *methfld;            // list of public methods and then fields
    Nodes *impl;               // list of VtableImpl, for structs using this virtref
    LLVMTypeRef llvmvtable;    // for the vtable
    LLVMTypeRef llvmreftype;   // For the virtual reference, not the vtable
    LLVMValueRef llvmvtables;  // List of vtables
    LLVMValueRef llvmfnvtable; // A callable trait's vtable for a plain function: its one slot a stub calling it (NULL until a conversion needs it)
} Vtable;

// Field-containing types (e.g., struct, trait, etc.)
// - fields holds all owned fields, plus an enum's spliced into a variant
// - nodelist holds owned methods and static functions and variables
// - namespace is the dictionary of all owned and inherited named nodes
typedef struct StructNode {
    INsTypeNodeHdr;
    Name *namesym;
    DclInfo dclinfo;        // Owner and the facts that decide the linker symbol
    INode *basetrait;       // The abstraction this type is-a, or the enum a variant belongs to
    INode *extendsbase;     // The type expression an 'extends' names: a concrete base to enrich, or, on an enum, the enum whose variants join its set
    INode *extendsdcl;      // An enriched base's declaration, set once its members have been taken; NULL until then, and always NULL for an enum, which licenses no substitution
    Nodes *derived;         // If a closed, base trait, this lists all structs derived from it
    Nodes *traits;          // Every trait whose members were taken in (NULL if none)
    Nodes *siblings;        // A field-like node per type-body 'use': its 'vtype' the sibling named, its 'fold' what the clause admits (NULL if none)
    Nodes *condis;          // A generic type's 'is' entries with a condition, 'is Move if T is Move': a placeholder per entry, its 'vtype' the trait, its 'value' the condition; an instance meeting it takes the entry (NULL if none)
    Nodes *lifecycle;       // Unlowered copies of its 'final' and 'clone', set aside as its layout settles and before its methods are type checked, for an enrichment taken after that (NULL if none)
    NodeList fields;        // Ordered list of all fields
    Vtable *vtable;         // Pointer to vtable info (may be NULL)
    GenericInfo *genericinfo;     // Link to generic parms, etc (or NULL if not generic)
    LifeParms *lifeparms;   // The lifetimes it declares, apart from its type parameters (lifetime.h), or NULL
    int64_t tagnbr;         // If a tagged struct, the number in the tag field, read as 'tagstate' says
    DclSpans *spans;        // Where each member of its braces sits in its file, in the order parsed (dclspan.h); NULL for none
    uint8_t carriesborrow;  // itypeCarriesBorrow's remembered answer (CarriesBorrow*), once the type is checked
    uint8_t holdstraced;    // itypeHoldsTraced's remembered answer (HoldsTraced*), once the type is checked
    uint8_t lends;          // What its element borrows cost it (StructLends), from its 'is' list at name resolution
    uint8_t shapeinf;       // Whether the compiler finds it shape-changing (ShapeInfer), for a type that declares none of the three (shapeinfer.h)
    uint8_t holdsatomic;   // itypeHoldsAtomic's remembered answer (HoldsTraced*, read as "holds an atomic value"), once the type is checked
    uint8_t tagstate;       // Whether 'tagnbr' is settled yet, and whether it is below zero (TagState)
    uint8_t threadbound;    // itypeThreadBound's remembered answer (CarriesBorrow*, read as "bound to its thread"), once the type is checked
    uint8_t callmut;        // On a callable trait (fnSigCallTrait): its '()' takes 'self &mut', else 'self &'
    struct FnSigNode *callsig;     // The signature a callable trait stands for; NULL for every other type
} StructNode;

// What a borrow one of its methods returns costs a container (StructNode.lends),
// as it declares it with a marker trait [Jon 26 Sep; names provisional]. A
// type declaring none keeps its receiver loaned while the borrow is used, the
// Rust way (the loan walk's pwCall). A no-loan kind keeps only its lifetime:
// it may not be moved, replaced or ended while the borrow is used, and nothing
// else is frozen. A type is shape-changing when it declares 'ShapeChanging' or
// the compiler finds it so (shapeinfer.h); a borrow of one, reached through a
// shared path, freezes that path (loanFreezeShared). A change through another
// name is not seen yet (corelib.c says why).
enum StructLends {
    LendsLoaned,            // declares none of them
    LendsShapeChanging,     // 'ShapeChanging': its elements may move
    LendsNoLoanMut,         // 'NoLoanMut': any borrow it returns loans nothing (an arena)
    LendsNoLoanRead         // 'NoLoanRead': a read-only borrow it returns loans nothing
};

// What the compiler found of whether a type that declares none of the three is
// shape-changing (StructNode.shapeinf; shapeinfer.h): not settled yet, being
// settled (a method of the type, checked on the way, asked again), or the
// answer
enum ShapeInfer {
    ShapeUnknown,
    ShapeAsking,
    ShapeNo,
    ShapeYes
};

// What StructNode.holdstraced remembers of whether a value of the type holds a
// traced reference where it sits: not yet known, being asked, or the answer.
// StructNode.holdsatomic remembers the same of an atomic value.
enum HoldsTraced {
    HoldsTracedUnknown,
    HoldsTracedAsking,
    HoldsTracedNo,
    HoldsTracedYes
};

// What StructNode.carriesborrow remembers of whether a value of the type may
// hold a borrowed reference: not yet known, being asked (a cycle reached it
// again), or the answer
enum CarriesBorrow {
    CarriesBorrowUnknown,
    CarriesBorrowAsking,
    CarriesBorrowNo,
    CarriesBorrowYes
};

// What StructNode.tagstate says of a variant's 'tagnbr'.
//
// A tag value may be anything the widest integer an enum declares can hold, u64's
// or i64's, and 64 bits hold either but not both at once: 0xFFFFFFFFFFFFFFFF and
// -1 are the same bits. Which one the value is gets kept here, so the check against
// the enum's integer type and the check for a value already taken each read the
// number the author meant.
//
// TagUnassigned is what the parser writes before looking for a value the author
// pinned, so that keeping a pinned value and assigning the next number in sequence
// are one test; nothing after that needs to know which a value was.
enum TagState {
    TagUnassigned,      // Not settled yet: 'tagnbr' means nothing
    TagNonNeg,          // Zero or above: 'tagnbr's bits read unsigned
    TagNegative         // Below zero: 'tagnbr' reads signed
};

// Give a variant the tag value after 'prior's, or zero when it is the first
void structTagFollow(StructNode *variant, StructNode *prior);

// Do two variants hold the same tag value? Never, while either is unsettled.
int structTagSame(StructNode *a, StructNode *b);

// A variant's tag value in decimal, as its author reads it, written into 'buf'
// (at least 24 bytes), which is returned
char *structTagText(StructNode *variant, char *buf);

typedef struct FieldDclNode FieldDclNode;

StructNode *newStructNode(Name *namesym);

// Set what its element borrows cost it (StructNode.lends) from its 'is' list
void structLendsDeclared(StructNode *node);

// Clone struct
INode *cloneStructNode(CloneState *cstate, StructNode *node);

// Map each function, static and overload name of 'original' to the member of
// 'copy' of the same name, for the clones made while the map is in force
void structCloneMapMembers(StructNode *original, StructNode *copy);

// Add a field node to a struct type
void structAddField(StructNode *type, FieldDclNode *node);

void structPrint(StructNode *node);

// Name resolution of a struct type
void structNameRes(NameResState *pstate, StructNode *node);

// Resolve a type's declaration now, because another type's resolution needs its
// members complete. Returns 0 when the type is already being resolved.
int structNameResDemand(NameResState *pstate, StructNode *type);

// Rewrite the receiver of a call to a method 'type' holds by folding: '*objp'
// becomes the access to the field the name was folded through, recursing into
// that field's type where the name is folded there too. Nothing happens for a
// name the type declares itself.
// A name folded from a body the type lends ('use str via view') shifts the
// receiver by calling the lending method instead.
void structFoldReceiver(TypeCheckState *pstate, StructNode *type, Name *name, INode **objp, INode *lexnode);

// The body a type lends and folds in ('use str via view'), and the method it is
// lent through; NULL for a type that lends none
StructNode *structLentBody(StructNode *type);
Name *structLentVia(StructNode *type);

// '*objp', a value or a reference to a type that lends a body, becomes the
// borrow of the body that type's lending method gives
void structLendView(TypeCheckState *pstate, StructNode *type, INode **objp, INode *lexnode);

// Unwrap one hop: the declaration of the base this type names
StructNode *structBaseTraitDcl(StructNode *node);

// Does this type's 'is' list name this trait (once it has been taken in)?
int structDeclaresTrait(StructNode *node, StructNode *trait);

// The default methods of a trait cloned into a type that does not declare it
// (a closure literal's hidden struct, which fills the trait's one required
// method), each unless the type has the name already
void structInheritDefaults(StructNode *node, StructNode *trait);

// The concrete type at the bottom of this type's 'extends' chain: the type
// itself where it enriches nothing. Two types substitute for each other exactly
// where this answers the same declaration for both -- which is what the
// declarations licensed, chain and siblings included, and never a coincidence of
// shape between two types that named no base.
StructNode *structExtendsRoot(StructNode *node);

// Do two type declarations substitute for each other because one enriches the
// other, or because both enrich one base? Nothing else answers yes.
int structExtendsEquiv(INode *type1, INode *type2);

// The enum an 'extends' on an enum adds variants to, or NULL for anything else.
// The two are distinct types that do not substitute for each other in either
// direction, so nothing that answers substitution reads this.
StructNode *structEnumBaseDcl(StructNode *node);

// How many of this enum's variants are its copies of its base's: the first that
// many of its 'derived' list. No module holds them, so the extension's own type
// check and generation reach them.
uint32_t structEnumCopyCount(StructNode *node);

// The member of a generic base's instance standing for 'dcl', a member of the
// base's template that a bare name inside an extension's braces was bound to, as
// seen from 'where', the type whose function is being checked; NULL when 'where'
// extends no instance of that template
INode *structEnumBaseInstanceMember(INode *where, INode *dcl);

// Resolve an enum that extends another now, so its copies of its base's variants
// exist -- from anywhere, a function body included. Returns 0 when it is already
// being resolved.
int structEnumDemandSet(NameResState *pstate, StructNode *node);

// May the function being checked reach a private member of 'type' through any
// value because it is written in an extension of the enum 'type' belongs to?
// 'self' is granted apart from this, for every type.
int structEnumSeesPrivate(TypeCheckState *pstate, INode *type);

// May the code being checked reach a private member of 'type' through any
// value: because it is written in the module that declares the type, or in an
// extension of its enum (structEnumSeesPrivate)?
int structSeesPrivate(TypeCheckState *pstate, INode *type);

// Get bottom-most base trait for some trait/struct, or NULL if there is not one
StructNode *structGetBaseTrait(StructNode *node);

// Type check a struct type: its layout. Its members are checked once no layout
// is in flight.
void structTypeCheck(TypeCheckState *pstate, StructNode *name);

// An array type checked as a reference's target waits for its element's
// layout before it asks the element's size and takes its move and thread
// flags (arrayTypeCheck); a by-value use settles it at once (structTypeSettle)
void structArrayWait(TypeCheckState *pstate, INode *array);

// A layout of a type holding values by value begins, or ends. When the last one
// in flight ends, every variant still waiting is laid out and every waiting
// type's members are checked.
void structLayoutEnter(void);
void structLayoutExit(void);
// Is a layout in flight, so that no member may be checked yet?
int structLayoutInFlight(void);

// The layout of a struct, an array or a tuple begins or ends: counted as above,
// and kept on the stack a by-value cycle is named from (structLayoutCycle)
void structLayoutBegin(INode *type);
void structLayoutEnd(void);

// Is a layout of a type holding values by value in flight, one this check is
// inside of? A function's signature is held as a layout in flight for the
// members queue's sake (fnDclTypeCheck) but holds nothing by value, so it sets
// the stack's base aside while it is checked (structValueHold, structValueRelease).
int structValueInFlight(void);
uint32_t structValueHold(void);
void structValueRelease(uint32_t base);

// A by-value cycle through 'root', which a size question found still being laid
// out ('entry' is root itself, or, for an enum, its variant in flight), asked
// through the types 'hops' (the field's type down to root, root excluded). The
// cycle as the message names it, "contains B contains A by value", or NULL where
// root is not on the stack.
char *structLayoutCycle(INode *root, INode *entry, INode **hops, uint32_t nhops);

// A reference's target is checked. While a layout is in flight a reference does
// not demand its target's layout: what it points at is resolved -- a name to its
// declaration, a generic's instance made -- and each struct reached is laid out
// once no layout is in flight, as is every check that reads one. See
// compiler/c/doc/phases/type-check.md, "A reference does not demand its target".
void structTargetEnter(void);
void structTargetExit(void);
int structTargetDeferring(void);
uint32_t structTargetSuspend(void);
void structTargetResume(uint32_t depth);

// Work deferred until every layout in flight is done: 'fn' is called with a
// copy of the walk state, 'node' and 'extra'. A layout is laid out ahead of every
// check (structDeferLayout); a check runs once no layout is waiting either
// (structDeferCheck), and before any type's members.
typedef void (*StructDeferFn)(TypeCheckState *pstate, INode *node, void *extra);
void structDeferLayout(TypeCheckState *pstate, StructDeferFn fn, INode *node, void *extra);
void structDeferCheck(TypeCheckState *pstate, StructDeferFn fn, INode *node, void *extra);

// A struct reached as a reference's target, not yet begun: laid out later
void structTargetWait(TypeCheckState *pstate, INode *node);

// A use is about to read this type's layout: lay out now whatever a reference
// left waiting in it, and finish an array whose element's size was waiting
void structTypeSettle(TypeCheckState *pstate, INode *type);

// Settle an enum's discriminant width from its variants' tag values, refusing a
// value too large for the integer type it declared
void structSetTagWidth(StructNode *node);

// Give an enum whose variants are laid out its drop, when any variant has
// something to do as it dies; and is this function a drop the compiler gave a
// struct or an enum, whose body generation builds?
void structSetEnumDropFn(StructNode *node);
int structIsGeneratedDropFn(INode *fn);

// Type check an instance of a generic enum whose variants are already listed,
// leaving its discriminant's width to the caller
void structTypeCheckEnumInstance(TypeCheckState *pstate, StructNode *instance);

// Populate the vtable for this struct
void structMakeVtable(StructNode *node);

// Populate the vtable implementation info for a struct ref being coerced to some trait
TypeCompare structVirtRefMatches(StructNode *trait, StructNode *strnode);

// Will from-type coerce to to-struct (we know they are not the same)
// We can only do this for a same-sized trait supertype
TypeCompare structMatches(StructNode *to, INode *fromdcl, SubtypeConstraint constraint);

// Return a type that is the supertype of both type nodes, or NULL if none found
INode *structFindSuper(INode *type1, INode *type2);

// Return a type that is the supertype of both type nodes, or NULL if none found
// This is used by reference types, where same-sized is no longer a requirement
INode *structRefFindSuper(INode *type1, INode *type2);

#endif
