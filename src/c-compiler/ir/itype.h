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

// Look for named field/method in type
INode *iTypeFindFnField(INode *type, Name *name);

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

// Add type mangle info to buffer
char *itypeMangle(char *bufp, INode *vtype);

// Return true if type has a concrete and instantiable. 
// False for Opaque structs, traits, functions 
int itypeIsConcrete(INode *type);

// Why this type cannot report a size, as a sentence naming the cause and the
// remedy, or NULL where the type is sized and may be held by value.
//
// There are five causes and one diagnostic, ErrorNoSize, because everything
// except the wording would be identical between them and the remedy is what the
// author actually needs. See design/phases/type-check.md, "Size".
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

// Return true if type implements move semantics
int itypeIsMove(INode *type);

// Return true if this is a generic type
int itypeIsGenericType(INode *type);

// Return drop function (or NULL) for type
INode *itypeGetDropFnDcl(INode *type);

#endif
