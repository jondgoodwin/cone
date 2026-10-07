/** Generic Type node handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef itype_h
#define itype_h

// Result from comparing subtype relationship between two types
typedef enum {
    NoMatch,           // Types are incompatible
    EqMatch,           // Types are the same (equivalent)
    CastSubtype,       // Subtype: From type can be recast to 'to' type (compile-time upcast)
    ConvSubtype,       // Subtype: From type can be converted to 'to' type (runtime upcast)
    ConvByMeth,        // Convert by using a method
    ConvBorrow,        // Convert by auto-borrowing
} TypeCompare;

// The constraint context for comparing a subtyping relationship
// This determines the severity of extra constraints,
// due to the absence of some coercion conversions
typedef enum {
    Monomorph,          // Monomorphization - no constraints
    Virtref,            // Virtual reference - relaxed
    Regref,             // Regular reference - ordered prefix
    Coercion,           // Most restrictive: no complex conversions
} SubtypeConstraint;

// Flag to indicate whether type match should inject coercion node
typedef enum {
    NoCoerce,           // Do not inject coercion node
    DoCoerce,           // Inject coercion node if type matches
} CoerceFlag;

typedef struct Name Name;
typedef struct INsTypeNode INsTypeNode;

// Named type node header (most types are named)
#define ITypeNodeHdr \
    IExpNodeHdr; \
    LLVMTypeRef llvmtype

// Named type node interface (most types are named)
// A named type needs to remember generated LLVM type ref for typenameuse nodes
typedef struct ITypeNode {
    ITypeNodeHdr;
} ITypeNode;

// Return node's type's declaration node
// (Note: only use after it has been type-checked)
INode *itypeGetTypeDcl(INode *node);

// Return node's type's declaration node (or vtexp if a ref or ptr)
INode *itypeGetDerefTypeDcl(INode *node);

// May a value of this type hold a borrowed reference: is it one, or an owning
// reference, pointer, array, tuple or struct (a variant included) reaching one?
// Remembered per struct once the struct is type checked.
int itypeCarriesBorrow(INode *type);

// Can a borrowed reference to 'want' (a type declaration) be stored somewhere
// in a value of this type: does it hold a borrowed reference to that type, or
// to a trait?
int itypeHoldsBorrowOf(INode *type, INode *want);

// How many writable borrows deep can a store into a value of this type reach,
// to store a borrow there, up to 'most'? A callee handed a '&mut H', 'H'
// holding a '&mut &R', may store through both: 2. 0 when it holds no writable
// borrow whose referent can hold a borrow.
int itypeWritableBorrowDepth(INode *type, int most);

// May a value of this type, dying, read a borrowed reference it holds: does
// it hold one where a 'final' method can reach it? Rust's drop check, with
// what a raw pointer reaches (a collection's elements) taken as finalized,
// never read, as Rust's '#[may_dangle]' collections promise.
int itypeDropReadsBorrow(INode *type);

// Does a value of this type hold a traced reference where it sits: a reference
// into a region declaring 'Traced', or a tuple, array, struct or enum holding
// one inline (not through another reference or a pointer)? Remembered per
// struct once the struct is type checked.
int itypeHoldsTraced(INode *type);

// Does a value of this type hold an atomic value where it sits: a struct
// declaring 'AtomicValue', or a tuple, array, struct or enum holding one inline
// (not through a reference or a pointer)? Remembered per struct once the
// struct is type checked.
int itypeHoldsAtomic(INode *type);

// Does a value of this type hold a borrowed reference (not a function's) where
// it sits, inline rather than through an owning reference or a pointer?
int itypeHoldsBorrow(INode *type);

// Is a value of this type bound to its thread: does it hold, anywhere it
// reaches through owning references, a reference refThreadBinds refuses, a raw
// pointer, or a reference to an open trait, and not by way of a type declaring
// 'Sendable'? What 'Sendable' is granted by (genericTypeIs). '*settled', if
// given, says whether a "not bound" is final: it is not where the answer
// leaned on a struct not yet type checked. Remembered per struct once final.
int itypeThreadBound(INode *type, int *settled);

// A borrow that lives for the whole program may cross threads where the thread
// check is asked to allow it (refStaticCrosses). How the program's lifetime is
// vouched for
typedef enum {
    StaticOff,          // Not allowed: every borrow binds its thread
    StaticNever,        // As StaticOff, where a diagnostic is not to offer the way (a behaviour's reply)
    StaticVouched,      // A generic's parameter bounded by ''static': the call is checked to hand it only global borrows
    StaticWritten       // A signature's own types: a borrow qualifies only where it is written ''static'
} StaticBorrow;

// The same, a borrow of the whole program let cross as 'how' says
int itypeThreadBoundHow(INode *type, int *settled, StaticBorrow how);

// What binds a value of this type to its thread: the reference, raw pointer
// or open trait found first, with 'path' set to where it sits in the type
// ('Job.data', 'Pair.0', 'List.items[]'), empty where the type is the culprit
// itself. NULL where the type is not bound.
INode *itypeThreadBoundWhy(INode *type, char *path, size_t size);
INode *itypeThreadBoundWhyHow(INode *type, char *path, size_t size, StaticBorrow how);

// Append a type to 'buf' as a diagnostic spells it: a reference as it is
// written ('&mut Point', 'Rc[imm, Point]', '*u64'), anything else by its name
void itypeSpellCat(char *buf, size_t size, INode *type, int depth);

// Look for named field/method in type
INode *iTypeFindFnField(INode *type, Name *name);

// Refuse a generic type named without its type arguments where a type is
// wanted, and return 1 if this is one
int itypeRefuseBareGeneric(INode *type);

// Refuse a name of a module or a module trait where a type is wanted, and
// return 1 if this is one
int itypeRefuseModule(INode *type);

// Type check node, expecting it to be a type. Give error and return 0, if not.
int itypeTypeCheck(TypeCheckState *pstate, INode **node);

// Return 1 if nominally (or structurally) identical, 0 otherwise.
// Nodes must both be types, but may be name use or declare nodes.
int itypeIsSame(INode *node1, INode *node2);

// Calculate hash for a type for use indexing the type table
size_t itypeHash(INode *type);

// Return 1 if nominally (or structurally) identical at runtime, 0 otherwise.
// Nodes must both be types, but may be name use or declare nodes.
int itypeIsRunSame(INode *node1, INode *node2);

// Is totype equivalent or a subtype of fromtype
TypeCompare itypeMatches(INode *totype, INode *fromtype, SubtypeConstraint constraint);

// Return a type that is the supertype of both type nodes, or NULL if none found
INode *itypeFindSuper(INode *type1, INode *type2);

// The type arguments a generic instance was instantiated with, or NULL when the
// declaration is not an instance of a generic
Nodes *itypeInstanceTypeArgs(INode *dclnode);

// Return true if type has a concrete and instantiable. 
// False for Opaque structs, traits, functions 
int itypeIsConcrete(INode *type);

// The element type of a dynamically sized body whose length a reference to it
// carries ('str': a byte), or NULL for any other type
INode *itypeLenBodyElem(INode *type);

// Is the size of a value of this type known at compile time ('Sized')? Is it
// known at compile time or carried by a reference to the type ('DynSized')?
// The two built-in markers the compiler grants (genericTypeIs).
int itypeIsSized(INode *type);
int itypeIsDynSized(INode *type);

// Why this type cannot report a size, as a sentence naming the cause and the
// remedy, or NULL where the type is sized and may be held by value.
//
// There are five causes and one diagnostic, ErrorNoSize, because everything
// except the wording would be identical between them and the remedy is what the
// author actually needs. See compiler/c/doc/phases/type-check.md, "Size".
//
// A size is missing infectiously as often as directly: the type a field names is
// usually unsized only because of something several levels below it. So *rootp
// is set to the declaration that actually lacks the size, which is what the
// sentence returned is about, and itypeNoSizeExplain names the hops between.
char *itypeNoSizeCause(INode *type, INode **rootp);

// Name the path from an unsized type to the declaration that is the cause, one
// uncounted frame per hop, each pointing at the field that carried it down.
// Says nothing where the type is its own cause -- the diagnostic already did.
void itypeNoSizeExplain(INode *type);

// A type's name, for a diagnostic. Types that have no name of their own
// describe themselves instead, since a diagnostic still has to call them
// something.
char *itypeName(INode *type);

// Return true if type has zero size (e.g., void, empty struct)
int itypeIsZeroSize(INode *type);

// Is this 'Never', the return type of a function that does not return?
int itypeIsNever(INode *type);

// Return true if type implements move semantics
int itypeIsMove(INode *type);

// Return true if this is a generic type
int itypeIsGenericType(INode *type);

// The region struct a managed reference type's head names ('Rc' in
// 'Rc[mut, Node]'), or NULL
INode *itypeManagedRefRegion(INode *call);

// Is this a managed reference type not yet lowered, 'Rc[mut, Node]'?
int itypeIsManagedRefType(INode *type);

// Return drop function (or NULL) for type
INode *itypeGetDropFnDcl(INode *type);

// Whether a value of this type does anything when it dies in place
int itypeNeedsFinal(INode *type);

#endif
