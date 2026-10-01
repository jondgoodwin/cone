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
} GenBlockState;

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
} GenState;

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
// A constant '&[]u8' slice of a source file's name, as genlSrcFileName gives it
LLVMValueRef genlSrcFileSlice(GenState *gen, char *text, size_t len);

// genlalloc.c
// Build an owning reference's allocation layout, once
void genlRefTypeSetup(GenState *gen, RefNode *reftype);
// Generate code that creates an allocated ref by allocating and initializing
LLVMValueRef genlallocref(GenState *gen, RefNode *allocatenode);
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

#endif
