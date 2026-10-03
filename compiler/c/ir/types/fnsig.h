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

// Can a call passing 'self' (NULL if none) and 'args' call this signature?
// This only decides viability, and never alters the call: no cast, borrow or
// default argument is inserted. Argument finalization does that after selection.
int fnSigViableCall(FnSigNode *to, INode **self, Nodes *args);

#endif
