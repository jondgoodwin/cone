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

    // An enum whose variants carry fields declares its '==' so that selection
    // finds it; type check then turns the call into one of the comparison the
    // compiler gave the enum (structSetEnumEqFn), or refuses it, naming the variant
    // that declares no '==' of its own for that comparison to call. Never
    // generated: type check replaces the call.
    NoEqIntrinsic,

    // An integer's or bool's 'hash': what core's Hash requires of every type that
    // is Hash, declared on each of them so that 'key.hash(h)' on a generic's key
    // reaches a method. Never generated: the call is rewritten at type check to
    // core's 'Hasher.writeU64' of the value's bits (fnCallHashNumber)
    HashNbrIntrinsic,

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
    NeedsFinalIntrinsic,    // needsFinal[T]() bool
    FinalizeIntrinsic,      // finalize[T](p *T)
    SliceFromPartsIntrinsic,    // sliceFromParts[T](p *T, len usize) &Array[T]
    SliceFromPartsMutIntrinsic, // sliceFromPartsMut[T](p *T, len usize) &mut Array[T]
    ReadRawIntrinsic,       // readRaw[T](p *T) T
    WriteRawIntrinsic,      // writeRaw[T](p *T, value T)
    MoveRawIntrinsic,       // moveRaw[T](to *T, from *T, count usize)
    TypeRecordIntrinsic,    // typeRecord[T]() *TypeRecord
    HoldsTracedIntrinsic,   // holdsTraced[T]() bool
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
    AtomicMinIntrinsic,     // atomicMin[T](p *T, value T, order MemOrder) T: signed for a signed T
    AtomicMaxIntrinsic,     // atomicMax[T](p *T, value T, order MemOrder) T: signed for a signed T
    AtomicCompareSwapIntrinsic, // atomicCompareSwap[T](p *T, expected T, desired T, success MemOrder, failure MemOrder) T, bool
    // Where the call is written: its source file's name, and its line. Written
    // as a parameter's default value, where each call taking the default is
    // (fnCallFinalizeArgs), which is how 'panic' reports its caller
    SrcFileIntrinsic,       // srcFile() &str
    SrcLineIntrinsic,       // srcLine() u32
    // Whether this is a debug build ('conec --debug', or 'build: debug' in a
    // build description): a constant, so only the side of an 'if' on it that
    // the build takes is generated (genlIf)
    IsDebugBuildIntrinsic,  // isDebugBuild() bool
    // TEMPORARY, a provisional mechanism whose final design is open: constants
    // of the build like isDebugBuild, the target's OS from its triple, and
    // what '-D' defined (intrinsicBuildConst)
    IsWindowsIntrinsic,     // isWindows() bool
    IsLinuxIntrinsic,       // isLinux() bool
    IsMacOSIntrinsic,       // isMacOS() bool
    IsWasmIntrinsic,        // isWasm() bool
    IsDefinedIntrinsic,     // isDefined(name &Array[u8]) bool
    DefinedIntIntrinsic,    // definedInt(name &Array[u8]) i64
    // An integer's bits. Each is also a method of every integer type
    // (corenumber.c), its node carrying that type as its typearg, which is how
    // 'x.leadingZeros()' reaches the same registry entry 'mem.leadingZeros(x)'
    // does. A count of the bits of 0 is defined: the width
    CountOnesIntrinsic,     // countOnes[T](x T) u32
    LeadingZerosIntrinsic,  // leadingZeros[T](x T) u32
    TrailingZerosIntrinsic, // trailingZeros[T](x T) u32
    // The amount taken modulo the width
    RotateLeftIntrinsic,    // rotateLeft[T](x T, n u32) T
    RotateRightIntrinsic,   // rotateRight[T](x T, n u32) T
    ShlMaskedIntrinsic,     // shlMasked[T](x T, n u32) T
    ShrMaskedIntrinsic,     // shrMasked[T](x T, n u32) T: arithmetic for a signed T
    // A GPU workgroup's barriers: every invocation of the workgroup waits there,
    // and what each wrote before it to the workgroup's memory, or to the storage
    // buffers, every other reads after it. The CPU runs a kernel one invocation
    // at a time, so there each is nothing (genllvm/genlgpusync.c)
    WorkgroupBarrierIntrinsic,  // workgroupBarrier()
    StorageBarrierIntrinsic,    // storageBarrier()
    // The 128-bit product of two 64-bit numbers, its two halves xored: the
    // mixing step of the wyhash family, one instruction on a 64-bit target
    MulFoldIntrinsic            // mulFold(a u64, b u64) u64
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

// Whether a type is core's Invocation, which a compute entry point may take:
// known by its name and its package, as the type record is
int invocationIsCore(INode *type);

// Whether a declaration is core's Hash trait, or core's Hasher: known by their
// names and their package. The compiler grants Hash to the integers and bool,
// gives them their 'hash' (nbrAddHashMethods) and supplies the 'hash' of a
// struct that declares Hash and writes none (structSupplyHash)
int coreIsHashTrait(INode *dcl);
int coreIsHasher(INode *dcl);

// Core's TypeRecord struct, once the core package's declaration of
// 'mem.typeRecord' has been checked against the registry; NULL before, or in a
// compile whose core declares none. Generation builds the records of a
// function's roots from it (genlRootFrame), where no declaration names it.
StructNode *typeRecordStruct(void);

// A function declaration, once name resolved: if it is core's 'mem.sliceEq',
// hold it to its signature and remember it for sliceEqFn
void sliceEqDclNameRes(FnDclNode *fndcl);

// Core's generic 'mem.sliceEq[T](a &Array[T], b &Array[T]) bool', which '==' and '!=' on
// two slices call (fnCallLowerSliceCompare); NULL before core's declaration is
// name resolved, or in a compile whose core declares none
FnDclNode *sliceEqFn(void);

// Type check what is left of a declared intrinsic's instance, lowered or not:
// the type an atomic operation acts on must be of the class its entry names.
// Answers 0, having reported it, when it is not, so its fallback body is not
// checked at a type it was never written for. Reported at the outermost place
// that instantiated it, once per place.
int intrinsicClassCheck(FnDclNode *fndcl);

// Is this a type some atomic operation acts on: an integer of 8 to 64 bits,
// bool or a raw pointer?
int intrinsicIsAtomicType(INode *type);

// Check a call to a declared intrinsic once its arguments are coerced and its
// defaults appended: each ordering it takes must be a constant MemOrder, and
// one the operation allows
void intrinsicCallCheck(FnCallNode *call, FnDclNode *fndcl);

// The atomic intrinsic a call's lowering expands, or NULL: the callee's
// instance holds an IntrinsicNode of an atomic kind
FnDclNode *intrinsicAtomicCallee(FnCallNode *call);

// Does a call expand the declared intrinsic of this kind (WriteRawIntrinsic...)?
int intrinsicCallIs(FnCallNode *call, int16_t kind);

// The orderings a checked call to an atomic intrinsic was given, in the order
// of its parameters: one, or compareSwap's success and failure
void intrinsicCallOrders(FnCallNode *call, FnDclNode *fndcl, MemOrderKind *orders);

// Which of 'srcFile()' and 'srcLine()' a node calls, or 0: SrcFileIntrinsic or
// SrcLineIntrinsic, each answering where the call is written
int intrinsicSrcKind(INode *node);

// Is this a call to 'srcFile()' or 'srcLine()'?
#define intrinsicIsSrcCall(node) (intrinsicSrcKind(node) != 0)

// Is this a parameter's default that answers where a call is: a call to
// 'srcFile()' or 'srcLine()', or one under the cast that makes the text
// 'srcFile()' gives the slice of bytes a parameter declares?
int intrinsicIsSrcDefault(INode *node);

// A copy of a call to 'srcFile()' or 'srcLine()' placed at 'site': a
// parameter's default value, taken by the call 'site', answers where that call is
INode *intrinsicSrcCallAt(INode *call, INode *site);

// TEMPORARY, a provisional mechanism whose final design is open. The options
// the build constants are read from: the build, the target triple and each
// '-D'. Set once by main; the triple is read only once generation's setup has
// filled in the host's
struct ConeOptions;
void intrinsicBuildSetup(struct ConeOptions *opt);

// Whether a node is a constant of the build, and its value: a call to
// isDebugBuild, isWindows, isLinux, isMacOS, isWasm, isDefined or definedInt
// (whose argument type check held to a string literal), or '!', 'and' or 'or'
// of such constants. Generation reads it to generate only the side of an 'if'
// the build takes (genlIf), and as the value of each call (genlFnCall)
int intrinsicBuildConst(INode *node, int64_t *value);

#endif
