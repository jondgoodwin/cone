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
    TypeRecordIntrinsic,    // typeRecord[T]() *TypeRecord
    HoldsTracedIntrinsic,   // holdsTraced[T]() Bool
    TraceIntrinsic,         // trace[T](p *T, mode u32)
    TraceRootsIntrinsic,    // traceRoots(mode u32)
    // The atomic operations: each takes its ordering as a constant MemOrder,
    // read at the call (intrinsicCallOrders), not from its instance
    AtomicLoadIntrinsic,    // atomicLoad[T](p *T, order MemOrder) T
    AtomicStoreIntrinsic,   // atomicStore[T](p *T, value T, order MemOrder)
    AtomicSwapIntrinsic,    // atomicSwap[T](p *T, value T, order MemOrder) T
    AtomicAddIntrinsic,     // atomicAdd[T](p *T, value T, order MemOrder) T
    AtomicSubIntrinsic,     // atomicSub[T](p *T, value T, order MemOrder) T
    AtomicAndIntrinsic,     // atomicAnd[T](p *T, value T, order MemOrder) T
    AtomicOrIntrinsic,      // atomicOr[T](p *T, value T, order MemOrder) T
    AtomicXorIntrinsic,     // atomicXor[T](p *T, value T, order MemOrder) T
    AtomicCompareSwapIntrinsic, // atomicCompareSwap[T](p *T, expected T, desired T, success MemOrder, failure MemOrder) T, Bool
    // Where the call is written: its source file's name, and its line. Written
    // as a parameter's default value, where each call taking the default is
    // (fnCallFinalizeArgs), which is how 'panic' reports its caller
    SrcFileIntrinsic,       // srcFile() &[]u8
    SrcLineIntrinsic,       // srcLine() u32
    // Whether this is a debug build ('conec --debug', or 'build: debug' in a
    // build description): a constant, so a branch on it folds away
    IsDebugBuildIntrinsic   // isDebugBuild() Bool
};

// A MemOrder, core's enum of the orderings an atomic operation promises, in the
// order of its variants (the C11 model's, weakest first)
typedef enum {
    OrderRelaxed,
    OrderAcquire,
    OrderRelease,
    OrderAcqRel,
    OrderSeqCst
} MemOrderKind;

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

// Core's TypeRecord struct, once the core package's declaration of
// 'mem.typeRecord' has been checked against the registry; NULL before, or in a
// compile whose core declares none. Generation builds the records of a
// function's roots from it (genlRootFrame), where no declaration names it.
StructNode *typeRecordStruct(void);

// Type check what is left of a declared intrinsic's instance, lowered or not:
// the type an atomic operation acts on must be of the class its entry names.
// Answers 0, having reported it, when it is not, so its fallback body is not
// checked at a type it was never written for. Reported at the outermost place
// that instantiated it, once per place.
int intrinsicClassCheck(FnDclNode *fndcl);

// Is this a type some atomic operation acts on: an integer of 8 to 64 bits,
// Bool or a raw pointer?
int intrinsicIsAtomicType(INode *type);

// Check a call to a declared intrinsic once its arguments are coerced and its
// defaults appended: each ordering it takes must be a constant MemOrder, and
// one the operation allows
void intrinsicCallCheck(FnCallNode *call, FnDclNode *fndcl);

// The atomic intrinsic a call's lowering expands, or NULL: the callee's
// instance holds an IntrinsicNode of an atomic kind
FnDclNode *intrinsicAtomicCallee(FnCallNode *call);

// The orderings a checked call to an atomic intrinsic was given, in the order
// of its parameters: one, or compareSwap's success and failure
void intrinsicCallOrders(FnCallNode *call, FnDclNode *fndcl, MemOrderKind *orders);

// Which of 'srcFile()' and 'srcLine()' a node calls, or 0: SrcFileIntrinsic or
// SrcLineIntrinsic, each answering where the call is written
int intrinsicSrcKind(INode *node);

// Is this a call to 'srcFile()' or 'srcLine()'?
#define intrinsicIsSrcCall(node) (intrinsicSrcKind(node) != 0)

// A copy of a call to 'srcFile()' or 'srcLine()' placed at 'site': a
// parameter's default value, taken by the call 'site', answers where that call is
INode *intrinsicSrcCallAt(INode *call, INode *site);

#endif
