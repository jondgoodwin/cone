/** Handling for intrinsic nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef intrinsic_h
#define intrinsic_h

// The various intrinsic functions supported by IntrinsicNode
enum IntrinsicFn {
    // Arithmetic
    NegIntrinsic,
    IsTrueIntrinsic,
    AddIntrinsic,
    SubIntrinsic,
    MulIntrinsic,
    DivIntrinsic,
    SDivIntrinsic,
    RemIntrinsic,
    SRemIntrinsic,
    IncrIntrinsic,
    DecrIntrinsic,
    IncrPostIntrinsic,
    DecrPostIntrinsic,
    DiffIntrinsic,  // subtract two pointers
    AddEqIntrinsic,
    SubEqIntrinsic,

    // Comparison
    EqIntrinsic,
    NeIntrinsic,
    LtIntrinsic,
    LeIntrinsic,
    GtIntrinsic,
    GeIntrinsic,
    SLtIntrinsic,
    SLeIntrinsic,
    SGtIntrinsic,
    SGeIntrinsic,

    // An enum's equivalence, which reads the discriminant. Distinct from
    // EqIntrinsic because both arrive on a struct-shaped LLVM value, where a
    // slice's equality compares two words and an enum's compares one field.
    TagEqIntrinsic,
    TagNeIntrinsic,

    // An enum whose variants carry fields declares its comparison and refuses
    // the call, so the author is told why rather than left to read the absence
    // of '==' as an oversight. Never generated: type check stops the call.
    NoEqIntrinsic,

    // Bitwise
    NotIntrinsic,
    AndIntrinsic,
    OrIntrinsic,
    XorIntrinsic,
    ShlIntrinsic,
    ShrIntrinsic,
    SShrIntrinsic,

    // Reference methods
    CountIntrinsic,

    // Intrinsic functions
    SqrtIntrinsic,
    SinIntrinsic,
    CosIntrinsic,

    // The program's stitched lifecycle (genlStitch): every module's 'init' in
    // dependency order, and every module's finalizer in exactly the reverse.
    // Functions of no parameters, not methods, so no first argument decides them
    InitAllIntrinsic,
    FinalAllIntrinsic
};

// An internal operation (e.g., add). 
// Used as an alternative to FnDcl->value = Block within a function declaration.
typedef struct IntrinsicNode {
    INodeHdr;
    int16_t intrinsicFn;
} IntrinsicNode;

IntrinsicNode *newIntrinsicNode(int16_t intrinsicFn);

#endif
