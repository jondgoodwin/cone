/** Generator for LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef genllvm_h
#define genllvm_h

#include "../ir/ir.h"
#include "../coneopts.h"

#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/TargetMachine.h>

// An entry for each active loop block in current control flow stack
#define GenBlockStackMax 256
typedef struct {
    BlockNode *blocknode;
    LLVMBasicBlockRef blockbeg;
    LLVMBasicBlockRef blockend;
    LLVMValueRef *phis;
    LLVMBasicBlockRef *blocksFrom;
    uint32_t phiCnt;
    uint32_t tempmark;      // How many temporaries were waiting as the block began: a jump to it finalizes the rest
} GenBlockState;

// A temporary made and not yet finalized (TempNode): the slot its value was
// kept in. The generator keeps a stack of them; the end of a statement, of an
// 'if' or 'while' condition, or of the right operand of 'and' or 'or'
// finalizes those its part made, newest first (genlTempsEnd), and a jump
// finalizes every one made since the block it leaves to began (genlTempsJump).
typedef struct {
    LLVMValueRef slot;
    TempNode *temp;
    LLVMValueRef held;      // A lock's guard in a split method: its flag, whether it holds its lock yet (genlawait.c), else NULL
} GenTemp;

// A value made before a seam of a split method and used after it, an operand
// of a call or a literal whose later operand holds the seam (genlawait.c): it
// waits in a slot of its own, which the seam's record carries
typedef struct {
    LLVMValueRef slot;
    INode *type;            // Its Cone type
} GenFlight;

// What a seam's record holds, field by field (genlawait.c), in the order the
// values would die: the values in flight, the newest first; the statement's
// temporaries, the newest first; the variables, the last declared first, each
// with its drop flag after it where it has one
enum GenSeamFieldKind {
    GenSeamVar,             // a variable's value
    GenSeamFlag,            // a variable's drop flag
    GenSeamTemp,            // a temporary's value, from its slot ('at' from the function's first)
    GenSeamFlight,          // a value in flight, from its slot ('at' from the function's first)
    GenSeamAnswer           // the request's envelope a message returning a value answers, from the state's
                            // Answer slot, first in the record
};

typedef struct GenSeamField {
    uint8_t kind;           // GenSeamFieldKind
    uint32_t at;
    VarDclNode *var;        // GenSeamVar, GenSeamFlag
    TempNode *temp;         // GenSeamTemp
    INode *type;            // Its Cone type (NULL for a flag)
} GenSeamField;

// A seam of a split method, as the first half to generate it laid it out: its
// record and its second half, the same for every half generated after
typedef struct GenSeam {
    LLVMValueRef half;      // The second half: the method from just after the seam
    LLVMValueRef resume;    // Where it awaits a reply (a message's, an operation's), what the reply's dispatch
                            // calls: the record taken from the pending table, and the half called with it and the value
    LLVMValueRef drop;      // Where it parks a record, the record's drop function, '(record *u8)': its values
                            // finalized in death order, for the pending table to abandon it with
    LLVMTypeRef record;     // The record's struct type, or NULL when it is empty: no record parameter
    GenSeamField *fields;
    uint32_t nfields;
    uint32_t ntemps;        // How many temporaries and values in flight wait at the seam, as every half must find
    uint32_t nflights;
} GenSeam;

// The roots of the function being generated: each stack slot holding a value
// whose type holds a traced reference -- a local's, a parameter's, a birth's --
// and that type, in slot order. What its frame and root map are built from
// (genlRootFrame), once its body is.
typedef struct GenRoots {
    LLVMValueRef *slots;
    INode **types;
    uint32_t cnt;
    uint32_t max;
} GenRoots;

typedef struct GenState {
    LLVMTargetMachineRef machine;
    LLVMTargetDataRef datalayout;
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMValueRef fn;
    LLVMValueRef allocaPoint;
    LLVMBuilderRef builder;

    LLVMDIBuilderRef dibuilder;
    LLVMMetadataRef difile;

    LLVMTypeRef emptyStructType;

    ConeOptions *opt;
    ModuleNode *libroot;    // The package's root module in a library compile, else NULL
    ProgramNode *pgm;       // The program being generated, whose module order genlStitch reads
    LLVMValueRef stitch[2]; // The stitched init and final, once a call asks for one (genlStitchFn); else NULL
    int comdats;            // enum ComdatSupport, from the target's object format
    Nodes *symnodes;        // Every declaration given a global, which genlClaimSymbol searches for a clash
    INode *fnblock;
    FnDclNode *fndcl;       // The function being generated, whose returns genlFnDclReturn makes
    int exitzero;           // The function generated is a 'main' returning nothing, whose returns return i32 0
    int cabi;               // enum CAbiTarget, the C ABI a C-named function's values cross by (genlcabi.c)
    GenBlockState *blockstack;
    uint32_t blockstackcnt;
    GenTemp *temps;         // The temporaries waiting to be finalized, oldest first
    uint32_t tempcnt;
    uint32_t tempmax;
    uint32_t tempbase;      // How many were waiting as the function being generated began: a 'return' finalizes the rest

    // A split method (genlawait.c): its seams, or NULL for any other function;
    // the seam whose second half is being generated, NULL for the first half;
    // where that half resumes, where each field of its record goes there, and
    // the flags of the locks given back there, once its seam is generated
    Nodes *seams;
    AwaitNode *resumeat;
    LLVMBasicBlockRef resumeblk;
    LLVMValueRef *resumedest;
    LLVMValueRef *resumeheld;
    uint32_t resumeheldcnt;
    LLVMValueRef resumeslot;    // A future's seam: where the value awaited is, which the half's entry fills
    GenFlight *flights;     // The values in flight across a seam, oldest first
    uint32_t flightcnt;
    uint32_t flightmax;
    uint32_t flightbase;    // How many were in flight as the function being generated began
    // While what a message 'await' awaits is generated: how many values were
    // in flight as it began, which its seam's record carries, and the id its
    // envelope reserved in the pending table (NULL where the record is empty)
    uint32_t awaitflights;
    LLVMValueRef awaitid;

    // The type records this object has built (genlTypeRecord): each value type,
    // and its record's constant, in the same order
    INode **tyrectypes;
    LLVMValueRef *tyrecs;
    uint32_t tyreccnt;
    uint32_t tyrecmax;
    LLVMValueRef tyrecnothing; // The shared do-nothing finalizer a record's empty finalize slot points at
    LLVMValueRef tyrecuntraced; // The shared do-nothing trace a record of a type holding no traced reference points at

    GenRoots roots;         // The function being generated's roots, set aside around a nested one
    uint32_t rootmaps;      // How many root maps this object has built, which numbers them

    // On SPIR-V's Vulkan form (genlgpu.c): the kernels this object made, one
    // for each compute entry point; and the node each address computation
    // marked by genlGpuSite was made for, by its number
    struct GenlEntry *entries;
    uint32_t entrycnt;
    uint32_t entrymax;
    Nodes *gpusites;
} GenState;

// A compute entry point's kernel (genlgpu.c): the LLVM function a dispatch
// runs, the binding its error buffer takes, and its declaration
typedef struct GenlEntry {
    LLVMValueRef kernel;
    unsigned errbinding;
    FnDclNode *fndcl;
} GenlEntry;

// What the target's object file format does with COMDATs, which is how a
// symbol becomes individually discardable
enum ComdatSupport {
    ComdatNone,        // Mach-O has no COMDAT concept at all
    ComdatMergeOnly,   // WebAssembly lowers only the 'any' selection kind
    ComdatFull         // COFF and ELF lower both kinds
};

// The platform C ABI the target follows, which decides how a C-named
// function's structs cross to C (genlcabi.c)
enum CAbiTarget {
    CAbiOther,      // None built: structs cross as LLVM's first-class values
    CAbiWin64,      // x86_64 Windows
    CAbiSysV,       // x86_64 elsewhere (not built)
    CAbiWasm32      // wasm32 (not built)
};

// Different kinds of dispatch
enum FnCallDispatch {
    SimpleDispatch,  // Call function directly or indirectly
    VirtDispatch     // Lookup function in vtable, and then dispatch
};

// Setup LLVM generation, ensuring we know intended target
void genSetup(GenState *gen, ConeOptions *opt);
void genClose(GenState *gen);
void genpgm(GenState *gen, ProgramNode *pgm);
void genlFn(GenState *gen, FnDclNode *fnnode);
void genlComdat(GenState *gen, LLVMValueRef global);
// What this object file does with a declared symbol, which genlLinkage reads
typedef enum GenlDefinition {
    GenlDeclared,   // Only names it: some other object defines it
    GenlDefined,    // Defines it for itself alone
    GenlExported,   // Defines it for other objects too: a library's export
    GenlShared      // Defines it as every object using it does, and the linker
                    // keeps one copy: a generic's instance, or a vtable, in a
                    // described build ('linkonce_odr', 'comdat any')
} GenlDefinition;

// Set a declared symbol's linkage, storage class and calling convention from its
// node's facts (NULL for a vtable), and what this object does with it
void genlLinkage(LLVMValueRef global, INode *dclnode, GenlDefinition defined);
// What an object does with a vtable it builds: shared in a described build
GenlDefinition genlVtableDefinition(GenState *gen);
void genlGloVarName(GenState *gen, VarDclNode *glovar);
void genlGloVar(GenState *gen, VarDclNode *varnode);
void genlGloFnName(GenState *gen, FnDclNode *glofn);
// Whether a function whose symbol is 'symbol' is a 'main' returning nothing,
// which is generated returning i32 0 for the C runtime's exit status
int genlIsVoidMain(FnDclNode *fnnode, const char *symbol);
// The program's stitched init or final (InitAllIntrinsic or FinalAllIntrinsic),
// declared on the first call that asks for it; its body is built once every
// module is generated (genlStitch)
LLVMValueRef genlStitchFn(GenState *gen, int16_t intrinsic);

// genlstmt.c
LLVMBasicBlockRef genlInsertBlock(GenState *gen, char *name);
LLVMValueRef genlBlock(GenState *gen, BlockNode *blk);

// genlexpr.c
LLVMValueRef genlExpr(GenState *gen, INode *termnode);
// A variant's tag value as a constant of its discriminant's LLVM type
LLVMValueRef genlTagConst(LLVMTypeRef tagtype, StructNode *variant);
// Generate a function call, including special intrinsics (Internal version).
// 'selftype' is the Cone type of the first argument, which a virtual dispatch
// and the pointer intrinsics read; NULL for a call the compiler makes itself.
LLVMValueRef genlFnCallInternal(GenState *gen, int dispatch, INode *objfn, uint32_t fnargcnt, LLVMValueRef *fnargs, INode *selftype);
// Fill 'dest' in place when 'exp' is a construction by a declared 'init',
// 'new Point(1, 2)', returning 1; else 0, and the caller stores the value
int genlNewInto(GenState *gen, INode *exp, LLVMValueRef dest);
// A construction by a declared 'init' in two steps, as an allocation runs it:
// its arguments evaluated (the first slot left for 'self'), then, once its
// memory exists, the init called on it
LLVMValueRef *genlNewArgs(GenState *gen, FnCallNode *fncall);
void genlNewFill(GenState *gen, FnCallNode *fncall, LLVMValueRef *fnargs, LLVMValueRef dest);
// Fill the array at 'dest' in place from the literal of its contents, an
// element at a time, each store taking the write barrier where 'traced'
void genlArrayLitInto(GenState *gen, ArrayNode *lit, LLVMValueRef dest, int traced);
// The failures the compiler checks for at run time, each ending the program
// through the C runtime's entry for it (genlPanic)
typedef enum {
    PanicIndex,     // an index at or past the count: the index, the count
    PanicSlice,     // a range not within the count: its start, its end, the count
    PanicAlloc      // a region's 'alloc' answered null: the size asked for
} GenlPanicKind;
// End the program where a check the compiler inserted has failed, reporting
// the values the kind names and the source location of 'site'. Leaves the
// block terminated
void genlPanic(GenState *gen, INode *site, GenlPanicKind kind, LLVMValueRef *vals);
// Add an attribute that takes no value ('noreturn', 'cold') to a function
void genlFnAttr(GenState *gen, LLVMValueRef fn, char *name);
// The source file a node was written in, as a panic reports it: its name
// without its folders
char *genlSrcFileName(INode *node, size_t *len);
// A constant '&Array[u8]' slice of a source file's name, as genlSrcFileName gives it
LLVMValueRef genlSrcFileSlice(GenState *gen, char *text, size_t len);
// The address of the constant copy of a source file's name, made once a module
LLVMValueRef genlSrcFileText(GenState *gen, char *text, size_t len);
// A source file's id in this module, from 1, by the global holding its name
// (0 for a global that holds none); how many it has; and the name of each
int genlSrcFileId(GenState *gen, LLVMValueRef global);
int genlSrcFileCount(GenState *gen);
char *genlSrcFileAt(GenState *gen, int id);

// genlgpu.c: compute entry points, and a kernel's failed checks and slices
// Make the kernel for a compute entry point whose function was just generated
void genlComputeEntry(GenState *gen, FnDclNode *fnnode);
// Settle each kernel once the GPU pipeline has inlined everything into it:
// its failed checks recorded, its slices' elements reached by access chains
void genlGpuEntries(GenState *gen);
// Every signed remainder made a subtraction of the quotient's product, since
// a Vulkan driver computes SPIR-V's OpSRem unsigned
void genlGpuSignedRem(GenState *gen);
// Emit a Vulkan form's module, what LLVM cannot say patched in
void genlGpuOut(GenState *gen, char *objpath, char *asmpath);
// Each struct or array a function loads from or stores into a storage buffer
// whole, loaded or stored a scalar at a time
void genlGpuBufferAccess(GenState *gen, LLVMValueRef fn);
// Mark an address computation with the node it was made for (Vulkan form only)
void genlGpuSite(GenState *gen, LLVMValueRef inst, INode *site);
// A check the compiler inserted, failed, on a GPU target: the call a kernel
// records ('kind' a GenlPanicKind; 'value' the index or the range's end, or NULL)
void genlGpuFailCheck(GenState *gen, INode *site, int kind, LLVMValueRef value);
// Whether a function is core's 'panic' (conestd's 'cone_panic'); and a call to
// it on a GPU target, given its arguments
int genlIsConePanic(FnDclNode *fndcl);
LLVMValueRef genlGpuPanic(GenState *gen, LLVMValueRef *args);
// A call to a C library math function on a GPU target, as its LLVM intrinsic
// (GLSL.std.450's instruction), or NULL when the function is no such one
LLVMValueRef genlGpuMath(GenState *gen, FnDclNode *fndcl, LLVMValueRef *args, unsigned nargs);

// The node an instruction was made for, as genlGpuSite marked it, or NULL
INode *genlGpuSiteOf(GenState *gen, LLVMValueRef inst);

// genlgpusync.c: what a GPU's invocations share
// A workgroup barrier: every invocation of the workgroup waits there, and the
// workgroup's memory ('storage' 0) or the storage buffers' (1) is made visible
LLVMValueRef genlGpuBarrier(GenState *gen, int storage);
// The same barrier on the CPU: a call to the thread's barrier hook, conestd's
// 'cone_barrierHook', when it is set, handed its context and the kind
void genlCpuBarrier(GenState *gen, int storage);
// Mark an atomic instruction with the call to mem's intrinsic it was made for,
// unless that call is core's own (an Atomic method's), on the Vulkan form
void genlGpuAtomicSite(GenState *gen, LLVMValueRef inst, INode *call);
// The address space a global takes on a GPU target, 0 for the data layout's
unsigned genlGpuGlobalSpace(VarDclNode *glovar);
// Settle a function's atomic instructions on a GPU target, once the pipeline
// has given each pointer its space: scope and ordering; in a kernel ('kernel'
// its entry point, else NULL), one on memory no other invocation reaches is
// refused. Answers 0 once it has refused one
int genlGpuAtomics(GenState *gen, LLVMValueRef fn, FnDclNode *kernel);
// What LLVM's SPIR-V backend says wrongly about atomics, put right in the
// emitted module (a relaxed operation's semantics, a compare-and-swap's
// result); 'words' is reallocated, and its new count answered
size_t genlGpuSyncPatch(uint32_t **wordsp, size_t nwords);

// genllvm.c: the type a pointer is known to point to (an alloca's, a global's,
// an address computation's, a parameter's as its uses agree), or NULL
LLVMTypeRef genlGpuPointee(LLVMValueRef ptr);

// genlalloc.c
// Build an owning reference's allocation layout, once
void genlRefTypeSetup(GenState *gen, RefNode *reftype);
// Generate code that creates an allocated ref by allocating and initializing
LLVMValueRef genlallocref(GenState *gen, RefNode *allocatenode);
// Take the lock a guard's type names (FlagLockAcquire), in the header of the
// value 'ref' points at; answer 'ref', the guard. 'site' is the borrow, whose
// file and line a lock's acquiring method may ask for
LLVMValueRef genlLockAcquire(GenState *gen, LLVMValueRef ref, RefNode *guardtype, INode *site);
// Progressively dealias or drop all declared variables in nodes list
void genlDealiasNodes(GenState *gen, Nodes *nodes);
void genlDealiasNode(GenState *gen, INode *node);
// Drop flags (VarDropFlag): a variable's, made as it begins; its value
// arriving or leaving; code run only when it holds 'state' (DropFlagState),
// ended by genlDropFlagEnd; a marked name use's move
void genlDropFlagBegin(GenState *gen, VarDclNode *var, int state);
void genlDropFlagSet(GenState *gen, VarDclNode *var, int state);
LLVMBasicBlockRef genlDropFlagIf(GenState *gen, VarDclNode *var, int state);
void genlDropFlagEnd(GenState *gen, LLVMBasicBlockRef endblk);
void genlDropFlagUse(GenState *gen, INode *nameuse);
// Release an owning value: one owner of an owning reference goes away, through
// the region's 'dealiasRef', as the value's death for a 'Move' region, and as
// nothing for any other; each element of a tuple
void genlReleaseOwning(GenState *gen, LLVMValueRef val, INode *type);
// Finalize the value at 'valptr' where it sits, as its death would, without
// freeing its memory: the 'finalize' intrinsic, and a local's death
void genlFinalizeAt(GenState *gen, LLVMValueRef valptr, INode *vtype);
// The body of a drop the compiler gave a struct or an enum, built from its
// layout: an enum's dispatches on the tag to what the variant's death does
void genlTypeDrop(GenState *gen, FnDclNode *fnnode);
// A copied value at 'valptr': each counted reference its death releases
// gains 'amount' holders
void genlAliasHeld(GenState *gen, LLVMValueRef valptr, INode *type, long long amount);
// Release a hollowed variable's owning reference without the parts moved out
void genlHollowRelease(GenState *gen, HollowNode *hnode);
// Temporaries: one kept in its slot as it is made; those made since 'mark'
// finalized, newest first, and forgotten (the end of their statement); and
// finalized without being forgotten, before a jump out past them
LLVMValueRef genlTempKeep(GenState *gen, TempNode *temp, LLVMValueRef val);
// A temporary's death, where it is generated (a lock's guard only where its flag says it holds its lock)
void genlTempRelease(GenState *gen, GenTemp *entry);
void genlTempsEnd(GenState *gen, uint32_t mark);
void genlTempsJump(GenState *gen, uint32_t mark);
// A counted reference gains 'amount' owners, through its region's 'aliasRef'
void genlRegionAlias(GenState *gen, LLVMValueRef ref, long long amount, RefNode *refnode);
// The type record of 'vtype': a constant core's TypeRecord (the pointee of
// 'recptrtype', the '*TypeRecord' the caller was declared with) holding its size,
// its alignment, its finalizer and its trace, built once per object and type
LLVMValueRef genlTypeRecord(GenState *gen, INode *vtype, INode *recptrtype);
// Hand each traced reference the value of type 'vtype' at 'valptr' holds to its
// region's 'mark', with its permission and 'mode' where 'mark' takes them
void genlTraceAt(GenState *gen, LLVMValueRef valptr, INode *vtype, LLVMValueRef mode);
// A value of type 'vtype' was just stored at 'valptr', memory that is not a
// local: hand each traced reference it holds to its region's 'writeBarrier',
// where the region has one
void genlBarrierAt(GenState *gen, LLVMValueRef valptr, INode *vtype);
// Create an alloca (will be pushed to the entry point of the function.
LLVMValueRef genlAlloca(GenState *gen, LLVMTypeRef type, const char *name);

// genllvm.c: roots, the shadow stack
// A stack slot of the function being generated is a root when its type holds a
// traced reference: a local's, a parameter's
void genlRootNote(GenState *gen, LLVMValueRef slot, INode *vtype);
// A birth of a value holding a traced reference: stored into a root slot of
// its own for the site, so no collection finds it only in a register
void genlRootBirth(GenState *gen, LLVMValueRef val, INode *vtype);
// Set aside the roots of the function being generated, around generating
// another; and, once a function's body is generated, build its frame
void genlRootsSave(GenState *gen, GenRoots *saved);
void genlRootsRestore(GenState *gen, GenRoots *saved);
void genlRootFrame(GenState *gen);
// The type record of 'vtype', from core's TypeRecord struct itself
LLVMValueRef genlTypeRecordOf(GenState *gen, INode *vtype, StructNode *recnode);

// genlcabi.c: a C-named function's values, as the platform's C ABI passes them
// The CAbiTarget a target triple names
int genlCAbiTarget(const char *triple);
// Whether a function's values cross as C passes them: a C-named function a module owns
int genlIsCAbiFn(FnDclNode *fndcl);
// The LLVM function type of a declared function: its Cone signature, lowered
// to the C ABI for one that crosses to C
LLVMTypeRef genlFnDclType(GenState *gen, FnDclNode *fndcl);
// Mark a just-declared function's hidden result slot ('sret'), if it has one
void genlCAbiDeclare(GenState *gen, FnDclNode *fndcl, LLVMValueRef fn);
// Call a declared function with Cone argument values, returning its Cone value
LLVMValueRef genlFnDclCall(GenState *gen, FnDclNode *fndcl, LLVMValueRef fn, LLVMValueRef *args, uint32_t argcnt);
// A parameter's Cone value, in the prologue of the function being generated
LLVMValueRef genlFnDclParm(GenState *gen, FnDclNode *fndcl, VarDclNode *var);
// Return a Cone value from the function being generated ('fndcl', or NULL for
// one no declaration names)
void genlFnDclReturn(GenState *gen, FnDclNode *fndcl, LLVMValueRef retval);

// genlaggcopy.c: once the module is generated, every struct, array or tuple
// value too large to carry whole is moved into memory: copied with
// 'llvm.memcpy', passed by a pointer and returned into a slot the caller
// passes; a smaller one returned in registers, and one of more than 16 bytes
// passed by a pointer, to its caller's storage where generation lent it. Not
// run on a GPU target
void genlAggCopies(GenState *gen);
// Is an argument of this LLVM type passed to a Cone function as a pointer
// (never on a GPU target)?
int genlAggPassesByPtr(GenState *gen, LLVMTypeRef type);
// The marks generation leaves for genlAggCopies, each an empty node: a load
// of a place whose storage the call it is passed to may be handed in place
// of a copy (genlLendable), and the store of a parameter into its variable's
// slot where the body may read its caller's storage in place (genlParmVar)
#define GenlLendMark "cone.lend"
#define GenlParmHomeMark "cone.parmhome"
// Mark an instruction so
void genlMark(GenState *gen, LLVMValueRef inst, const char *mark);

// genllvm.c: a target machine for the options' triple, CPU and features
LLVMTargetMachineRef genlCreateMachine(ConeOptions *opt);

// genltype.c
// Generate a type value
LLVMTypeRef genlType(GenState *gen, INode *typ);
// The Cone type a reference, pointer or slice points at: what a load through it
// reads and what a GEP over it steps by. An LLVM pointer does not know this
// (opaque pointers), so a load, GEP or call is always typed from the Cone type.
INode *genlPointee(INode *type);
// The LLVM type of genlPointee
LLVMTypeRef genlPointeeType(GenState *gen, INode *type);
// The function type of a vtable slot holding a method, whose self is erased to *u8
LLVMTypeRef genlVtableSlotFnType(GenState *gen, FnDclNode *meth);
// Generate LLVM value corresponding to the size of a type
LLVMValueRef genlSizeof(GenState *gen, INode *vtype);
// Generate LLVM value corresponding to the alignment of a type
LLVMValueRef genlAlignof(GenState *gen, INode *vtype);
// Generate unsigned integer whose bits are same size as a pointer
LLVMTypeRef genlUsize(GenState *gen);
LLVMTypeRef genlEmptyStruct(GenState* gen);
// Generate a vtable type
void genlVtable(GenState *gen, Vtable *vtable);

// genlawait.c: a split method, an actor's message holding an 'await'
// The seam: what is awaited, the locks given back, the record built, and the
// return; what follows it generates into a block its second half resumes at
LLVMValueRef genlAwait(GenState *gen, AwaitNode *node);
// The envelope of the request a message 'await' sends: the seam laid out, its
// record given an id in the pending table, and the reply's node made
LLVMValueRef genlAwaitReply(GenState *gen, AwaitReplyNode *node);
// Each second half of a split method whose first half was just generated
void genlSplitHalves(GenState *gen, FnDclNode *fnnode);
// Generate each of 'nodes' into 'vals', in the order a seam gives them
// (awaitOrder): in a split method, a value made before a later node's seam is
// kept in flight across it (GenFlight), and a receiver or a borrow of a plain
// path before it is made after it
void genlExprsAcross(GenState *gen, Nodes *nodes, LLVMValueRef *vals);
// One value 'node' made, of 'type', kept in flight across a seam to come: its
// slot, or NULL for a constant. Once past the seam, genlKeptAcross reads it
// back and pops the flights to 'mark', gen->flightcnt before the keep
LLVMValueRef genlKeepAcross(GenState *gen, INode *node, INode *type, LLVMValueRef val);
LLVMValueRef genlKeptAcross(GenState *gen, LLVMValueRef slot, LLVMValueRef val, uint32_t mark);
// Whether a seam is generated inside 'node', in a split method
int genlHasSeam(GenState *gen, INode *node);
// A lock's guard's flag, made as a temporary guard is kept (genlTempKeep)
// in a split method, and code run only while it holds its lock, ended by
// genlDropFlagEnd
LLVMValueRef genlHeldBegin(GenState *gen);
LLVMBasicBlockRef genlHeldIf(GenState *gen, LLVMValueRef held);

#endif
