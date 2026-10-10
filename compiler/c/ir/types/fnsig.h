/** Handling for function signature
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fnsig_h
#define fnsig_h

typedef struct FnCallNode FnCallNode;

// Function signature is a type that defines the parameters and return type for a function.
// A function signature is never named (although a ptr/ref to a fnsig may be named).
// The parameter declaration list represents a namespace of local variables.
typedef struct FnSigNode {
    INodeHdr;
    Nodes *parms;            // Declared parameter nodes w/ defaults (VarDclTag)
    INode *rettype;        // void, a single type or a type tuple
    LifeOrder *lifeorder;  // The order among its lifetimes its 'where' clause and its structs' give (lifetime.h), or NULL
    uint8_t lifenamed;     // A lifetime is named on one of its types (lifetime.h)
    uint8_t lifechecked;   // lifeSigCheck has settled 'lifenamed' and 'lifeorder'
    uint8_t lifestatic;    // Its order bounds a lifetime by ''static': a type parameter's bound (lifetime.h)
    Name *spelled;         // A signature written bare as a generic bound: the text it was written as, for messages; else NULL
} FnSigNode;

FnSigNode *newFnSigNode();

// Clone function signature
INode *cloneFnSigNode(CloneState *cstate, FnSigNode *node);

void fnSigPrint(FnSigNode *node);
// Name resolution of the function signature
void fnSigNameRes(NameResState *pstate, FnSigNode *sig);
void fnSigTypeCheck(TypeCheckState *pstate, FnSigNode *name);
int fnSigEqual(FnSigNode *node1, FnSigNode *node2);

// For virtual reference structural matches on two methods,
// compare two function signatures to see if they are equivalent,
// ignoring the first 'self' parameter (we know their types differ).
// 'node1' is the implementation and 'node2' the requirement. A non-NULL
// 'selftype' is the type meeting the requirement, which 'Self' in it stands
// for; NULL compares exactly, as a vtable slot must.
int fnSigVrefEqual(FnSigNode *node1, FnSigNode *node2, INode *selftype);

// Do two signatures declare the same parameter types (ignoring return type)?
// Used to detect two overload candidates that would accept the same arguments.
int fnSigParmsEqual(FnSigNode *node1, FnSigNode *node2);

// Return TypeCompare indicating whether from type matches the function signature
TypeCompare fnSigMatches(FnSigNode *to, FnSigNode *from, SubtypeConstraint constraint);

// Return true if type of from-exp matches totype
int fnSigCoerce(FnSigNode *totype, INode **fromexp);

// The trait that stands for 'fn(sig)' behind a virtual reference or an owner:
// one method '()' of that signature, 'self &mut' if 'mutself', else 'self &'.
// One per signature and kind.
StructNode *fnSigCallTrait(TypeCheckState *pstate, FnSigNode *sig, int mutself, INode *lexnode);

// The signature a type is a callable trait for (fnSigCallTrait), or NULL
FnSigNode *fnSigOfCallTrait(INode *type);

// Does a struct's '()' take the receiver the callable trait's kind allows?
int fnSigCallSelfFits(StructNode *trait, FnDclNode *meth);

// The permission a method's 'self' borrows with, or NULL when 'self' is not a borrow
INode *fnSigSelfBorrowPerm(FnDclNode *meth);

// Does a type's method ask for no stronger 'self' than the trait's method
// declares, so it may fill the trait's slot behind a virtual reference?
int fnSigVrefSelfFits(FnDclNode *traitmeth, FnDclNode *implmeth);

// Why 'impl' cannot be viewed as 'trait' behind a virtual reference because a
// method asks for a stronger 'self' than the trait's; NULL when it is not that
char *fnSigVrefSelfRefusal(StructNode *trait, StructNode *impl);

// Why a value of type 'from' is refused where the callable type 'to' is wanted,
// when that is the permission of its '()'; NULL when it is not that. '*code' is
// the error code to report it under.
char *fnSigCallRefusal(INode *from, INode *to, int *code);

// Can a call passing 'self' (NULL if none) and 'args' call this signature?
// This only decides viability, and never alters the call: no cast, borrow or
// default argument is inserted. Argument finalization does that after selection.
int fnSigViableCall(FnSigNode *to, INode **self, Nodes *args);

#endif
