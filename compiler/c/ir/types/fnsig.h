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
    uint8_t ccall;         // C's calling convention: the signature of a C-named function ('fn @c', a function of an '@c' module) and the type '&fn @c (...)' of a reference to one
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

// The same match, loosened as a subtype's signature is to its supertype's:
// 'impl' meets 'req' where each parameter after 'self' is the requirement's or a
// supertype of it, and the result its or a subtype of it, wherever the difference
// is a recast with no run-time conversion (the test a function reference's
// signature gets). Anything fnSigVrefEqual accepts, this accepts.
int fnSigVrefMeets(FnSigNode *impl, FnSigNode *req, INode *selftype);

// The same test for two signatures with no receiver: a function reference's, a
// plain function standing where a callable's '()' is wanted, a module's function
// where a module trait asks for one, a function made a 'So[fn(sig)]', a generic's
// 'fn(sig)' condition. Anything fnSigEqual accepts, this accepts. One rule, asked
// by every route a callable meets a signature through.
int fnSigMeets(FnSigNode *impl, FnSigNode *req);

// The same test for a method standing where a signature written without a
// receiver is wanted (a struct's '()' meeting a generic's 'fn(sig)' condition):
// its receiver is skipped, and its lifetimes, counted by position, are not asked.
int fnSigMeetsMethod(FnSigNode *impl, FnSigNode *req);

// What is the first parameter of the two signatures a test compares: none (a
// function's), a receiver in each that is not compared (a method meeting a
// method), or a receiver in the implementation alone
typedef enum { ReceiverNone, ReceiverBoth, ReceiverImpl } FnSigReceiver;

// Where a signature is refused for one reason alone, a parameter or the result held
// by value that is not one layout (a variant for its enum, or the reverse): the
// first such position, and the two types in it
typedef struct FnSigLayoutDiff {
    char position[100];     // "parameter 1 (`s`)", "result"
    INode *req;             // the type the requirement has there
    INode *impl;            // the type the implementation has there
} FnSigLayoutDiff;

// Is that the one reason 'impl' does not meet 'req'? 'receiver', 'checkc' and
// 'selftype' are as for the test it explains (fnSigVrefMeets, fnSigMeets); 'diff'
// is filled when it is. Every route that refuses a signature reports this cause
// under ErrorFnRefByValue, in its own words (fnSigLayoutSpell).
int fnSigLayoutDiff(FnSigNode *impl, FnSigNode *req, INode *selftype, FnSigReceiver receiver, int checkc,
        FnSigLayoutDiff *diff);

// The sentence that says why, the same on every route: "its parameter 1 (`s`) is
// `Shape` held by value, where {wanted} has `Shape.Circle`..." 'wanted' is what has
// the requirement's type ("the wanted one", "the trait"), 'through' what the call
// goes through ("the reference", "the trait")
void fnSigLayoutSpell(char *buf, size_t size, const FnSigLayoutDiff *diff, const char *wanted, const char *through);

// Do two signatures declare the same parameter types (ignoring return type)?
// Used to detect two overload candidates that would accept the same arguments.
int fnSigParmsEqual(FnSigNode *node1, FnSigNode *node2);

// Why a value of type 'from' is refused where the type 'to' is wanted, when the
// two are function references that differ only in the calling convention they
// are marked for (FnSigNode.ccall); NULL when it is not that. The reference is
// never converted: a Cone function and a C-named one are called differently.
// Also when they differ in a parameter or result held by value that is not one
// layout (a variant for its enum): '*code' is the error code to report it under.
char *fnSigConvRefusal(INode *from, INode *to, int *code);

// Return TypeCompare indicating whether from type matches the function signature
TypeCompare fnSigMatches(FnSigNode *to, FnSigNode *from);

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

// Why 'impl' cannot be viewed as 'trait' behind a virtual reference that may write
// because a field the trait requires has a narrower type in 'impl'; NULL when it is
// not that
char *fnSigVrefFieldRefusal(StructNode *trait, StructNode *impl);

// Why a value of type 'from' is refused where the callable type 'to' is wanted,
// when that is the permission of its '()'; NULL when it is not that. '*code' is
// the error code to report it under.
char *fnSigCallRefusal(INode *from, INode *to, int *code);

// Can a call passing 'self' (NULL if none) and 'args' call this signature?
// This only decides viability, and never alters the call: no cast, borrow or
// default argument is inserted. Argument finalization does that after selection.
int fnSigViableCall(FnSigNode *to, INode **self, Nodes *args);

#endif
