/** Handling for closure literals
 *
 * A closure is written 'fn (u f32) [ribs, mut n = 0] f32 { ... }', or in the
 * short form 'x => x * 2'. Type check makes it a value of a struct the compiler
 * writes (the "hidden struct"): the state list's entries and the variables the
 * body borrows are its fields, and its one method, '()', is the body.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef closure_h
#define closure_h

// A closure literal, from the parser until type check lowers it to the
// construction of its hidden struct
typedef struct ClosureNode {
    IExpNodeHdr;
    FnSigNode *sig;             // Its parameters; a parameter's type is unknownType where not written. The return type is unknownType where not written
    Nodes *state;               // The state list: a VarDclNode for each entry, its value the initializer (a use of the same name where none is written)
    INode *body;                // A block
    Nodes *captures;            // Name resolution: the variables of the code around that the body names (VarDclNodes), each borrowed
    VarDclNode *outerself;      // Name resolution: the 'self' of the method it is written in, where the body names a member bare
    struct ClosureNode *outer;  // Name resolution: the closure literal this one is written inside
    uint16_t outerscope;        // Name resolution: the block scope it is written at
    uint8_t isarrow;            // Written in the short form, 'x => x * 2'
} ClosureNode;

// What the hidden struct holds for one variable
typedef struct ClosureCap {
    VarDclNode *dcl;            // The state entry, or the variable of the code around that is borrowed
    FieldDclNode *field;        // The field of the hidden struct standing for it
    uint8_t state;              // A state entry (the field holds the value), else a borrow of the variable
} ClosureCap;

// On the hidden struct's '()' (FnDclNode.closure)
typedef struct ClosureInfo {
    StructNode *strct;          // The hidden struct
    ClosureCap *caps;
    uint32_t ncaps;
    VarDclNode *selfparm;       // The '()''s own 'self'
    VarDclNode *outerself;      // The 'self' of the method the closure is written in, or NULL
    uint8_t retinfer;           // Its return type is read off its paths
    uint8_t retset;             // ... and one has set it
    uint8_t expanded;           // Written in a body an importer expands (an inline function, a generic's instance): every object that makes the closure defines it
    ClosureNode *lit;           // The literal, for messages
    Name *method;               // The name of its one method: '()', or the trait's method it was given to fill
    int errbase;                // The error count when its struct began to be checked: a failure since is its body's, reported there
} ClosureInfo;

ClosureNode *newClosureNode();
INode *cloneClosureNode(CloneState *cstate, ClosureNode *node);
void closurePrint(ClosureNode *node);

// Name resolution of a literal, and of a name used inside one
void closureNameRes(NameResState *pstate, ClosureNode *node);
void closureNoteUse(NameResState *pstate, NameUseNode *name);

// Type check of a literal: it becomes the construction of its hidden struct
void closureTypeCheck(TypeCheckState *pstate, ClosureNode **nodep, INode *expected);

// The closure a hidden struct, or one of its members, belongs to; else NULL
ClosureInfo *closureOfStruct(INode *node);
ClosureInfo *closureOfDcl(INode *dcl);

// A name use inside a closure's '()' that names a variable the closure holds:
// rewritten to the access of its field. Answers whether it was.
int closureUse(TypeCheckState *pstate, NameUseNode **namep);

// The struct that holds a reference to a plain function and calls it, so that an
// owner of a callable can own a function (the caller makes its value from the
// reference, whose type is 'reftype')
StructNode *closureFnHolder(TypeCheckState *pstate, FnSigNode *sig, INode *reftype, INode *lexnode);

// The 'self' a bare member name inside a closure is reached through: the
// method the closure is written in has the receiver, not the closure's '()'
INode *closureSelfParm(FnDclNode *fn);

// Reading the return type off a closure's paths (fndcl.c, return.c)
void closureImplicitReturn(FnDclNode *fn);
void closureReturnTypeCheck(TypeCheckState *tstate, BreakRetNode *retnode);

// The signature a closure given as an argument is to fit, set by the call that
// knows it (a parameter of function-reference type, a generic parameter bound
// by a signature); NULL elsewhere
extern FnSigNode *closureHint;

// With the hint, the name of the trait method the literal fills when it is given
// where a trait with one method is wanted; NULL for '()'
extern Name *closureMethod;

// What a literal given where 'trait' is wanted must be: the signature of the
// trait's one method and its name; NULL when the trait is not one with exactly
// one method and no field ('*count' says how many it has)
FnSigNode *closureTraitSig(StructNode *trait, Name **method, uint32_t *count);

// Whether a closure's one method takes 'self &mut'
int closureMethodMutates(ClosureInfo *info);

// Set while the return type of a closure is being read off its paths, so that
// the 'if' that finds its branches disagree says so in the closure's words
extern int closureInferring;

// In GPU code, a closure (a reference to its hidden struct) coerced to a
// virtual reference: refused, answering whether it was
int closureGpuVirtRefused(INode *from, INode *totypedcl);

// Whether a node is a closure literal, still to be lowered
#define closureIsLiteral(node) ((node)->tag == ClosureTag)

// Whether a closure literal's parameters take their types from the signature
// its position gives: some parameter is written without one
int closureNeedsSig(ClosureNode *node);
uint32_t closureParmCount(ClosureNode *node);

#endif
