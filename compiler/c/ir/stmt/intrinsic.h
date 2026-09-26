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
    FinalAllIntrinsic,

    // Declared in Cone, in core, with '@intrinsic' (refintrinsic.html), and
    // defined by the registry in intrinsic.c in Cone's terms. Unlike the kinds
    // above, generation never asks the LLVM type of an argument what one of
    // these means: the type it acts on is the Cone type its node carries
    // ('typearg'), fixed when its instance was type checked.
    SizeofIntrinsic,        // sizeof[T]() usize
    AlignofIntrinsic,       // alignof[T]() usize
    NeedsFinalIntrinsic,    // needsFinal[T]() Bool
    FinalizeIntrinsic,      // finalize[T](p *T)
    SliceFromPartsIntrinsic,    // sliceFromParts[T](p *T, len usize) &[]T
    SliceFromPartsMutIntrinsic, // sliceFromPartsMut[T](p *T, len usize) &[]mut T
    ReadRawIntrinsic,       // readRaw[T](p *T) T
    WriteRawIntrinsic,      // writeRaw[T](p *T, value T)
    MoveRawIntrinsic,       // moveRaw[T](to *T, from *T, count usize)
    TypeRecordIntrinsic     // typeRecord[T]() *TypeRecord
};

// The first kind declared in Cone rather than built in C
#define FirstDeclaredIntrinsic SizeofIntrinsic

// An internal operation (e.g., add).
// Used as an alternative to FnDcl->value = Block within a function declaration.
typedef struct IntrinsicNode {
    INodeHdr;
    INode *typearg;         // A declared generic intrinsic's type parameter: a use of
                            // it in the template, the type argument in an instance.
                            // NULL for every other intrinsic
    int16_t intrinsicFn;
} IntrinsicNode;

// Set by '--intrinsic-fallback': every intrinsic declared with a body uses that
// body, even where the back end lowers it itself, so the bodies can be tested
// against the lowerings (refintrinsic.html, "Fallback bodies")
extern int intrinsicForceFallback;

IntrinsicNode *newIntrinsicNode(int16_t intrinsicFn);
INode *cloneIntrinsicNode(CloneState *cstate, IntrinsicNode *node);
void intrinsicPrint(IntrinsicNode *node);

// An '@intrinsic' declaration, once its signature and body are name resolved:
// check it against the registry, then give it its meaning or its fallback
void intrinsicDclNameRes(FnDclNode *fndcl);

// Type check a declared intrinsic's instance (or a non-generic one): the type
// it acts on must be of the class the registry names
void intrinsicDclTypeCheck(TypeCheckState *pstate, FnDclNode *fndcl);

// Whether a declared intrinsic's name use or instance is in play: a function
// whose value is an IntrinsicNode of a kind declared in Cone
int intrinsicIsDeclared(FnDclNode *fndcl);

// Whether a type is '*TypeRecord', a pointer to core's type record: the struct
// named TypeRecord that the core package declares, whose constants the
// compiler builds (genlTypeRecord). A template may still hold the '*' as a
// dereference, as it holds '*T' (cloneStarNode), so both are accepted.
int typeRecordIsPtr(INode *type);

#endif
