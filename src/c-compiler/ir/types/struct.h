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
    Nodes *traits;          // Every trait whose members were mixed in (NULL if none)
    Nodes *siblings;        // A field-like node per type-body 'use': its 'vtype' the sibling named, its 'fold' what the clause admits (NULL if none)
    Nodes *lifecycle;       // Unlowered copies of its 'final' and 'clone', set aside as its layout settles and before its methods are type checked, for an enrichment taken after that (NULL if none)
    NodeList fields;        // Ordered list of all fields
    Vtable *vtable;         // Pointer to vtable info (may be NULL)
    GenericInfo *genericinfo;     // Link to generic parms, etc (or NULL if not generic)
    uint32_t tagnbr;        // If a tagged struct, this is the number in the tag field
} StructNode;

// A variant whose tag value has not been settled yet. The parser writes it before
// looking for a value the author pinned, so that keeping a pinned value and
// assigning the next number in sequence are one test rather than a flag: a type
// has no spare flag bit, and nothing after parse needs to know which a value was.
#define TagUnassigned 0xFFFFFFFFu

typedef struct FieldDclNode FieldDclNode;

StructNode *newStructNode(Name *namesym);

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
void structFoldReceiver(StructNode *type, Name *name, INode **objp, INode *lexnode);

// Unwrap one hop: the declaration of the base this type names
StructNode *structBaseTraitDcl(StructNode *node);

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

// Type check an extension's copies of its base's variants, which the module
// walk reaches through the extension
void structEnumCheckCopies(TypeCheckState *pstate, StructNode *node);

// Resolve an enum that extends another now, so its copies of its base's variants
// exist -- from anywhere, a function body included. Returns 0 when it is already
// being resolved.
int structEnumDemandSet(NameResState *pstate, StructNode *node);

// Get bottom-most base trait for some trait/struct, or NULL if there is not one
StructNode *structGetBaseTrait(StructNode *node);

// Type check a struct type
void structTypeCheck(TypeCheckState *pstate, StructNode *name);

// Settle an enum's discriminant width from its variants' tag values, refusing a
// value too large for the integer type it declared
void structSetTagWidth(StructNode *node);

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
