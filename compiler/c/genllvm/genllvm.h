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
    int exitzero;           // The function generated is a 'main' returning nothing, whose returns return i32 0
    GenBlockState *blockstack;
    uint32_t blockstackcnt;

    // The type records this object has built (genlTypeRecord): each value type,
    // and its record's constant, in the same order
    INode **tyrectypes;
    LLVMValueRef *tyrecs;
    uint32_t tyreccnt;
    uint32_t tyrecmax;
    LLVMValueRef tyrecnothing; // The shared do-nothing function a record's empty slots point at
} GenState;

// What the target's object file format does with COMDATs, which is how a
// symbol becomes individually discardable
enum ComdatSupport {
    ComdatNone,        // Mach-O has no COMDAT concept at all
    ComdatMergeOnly,   // WebAssembly lowers only the 'any' selection kind
    ComdatFull         // COFF and ELF lower both kinds
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
// Generate a function call, including special intrinsics (Internal version).
// 'selftype' is the Cone type of the first argument, which a virtual dispatch
// and the pointer intrinsics read; NULL for a call the compiler makes itself.
LLVMValueRef genlFnCallInternal(GenState *gen, int dispatch, INode *objfn, uint32_t fnargcnt, LLVMValueRef *fnargs, INode *selftype);
// Generate a panic
void genlPanic(GenState *gen);

// genlalloc.c
// Build usable metadata about a reference 
void genlRefTypeSetup(GenState *gen, RefNode *reftype);
// Generate code that creates an allocated ref by allocating and initializing
LLVMValueRef genlallocref(GenState *gen, RefNode *allocatenode);
// Progressively dealias or drop all declared variables in nodes list
void genlDealiasNodes(GenState *gen, Nodes *nodes);
// Release an owning value: one owner of an owning reference goes away, through
// the region's 'dealias', as the value's death for a 'Move' region, and as
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
// A counted reference gains 'amount' owners, through its region's 'alias'
void genlRegionAlias(GenState *gen, LLVMValueRef ref, long long amount, RefNode *refnode);
// The type record of 'vtype': a constant core's TypeRecord (the pointee of
// 'recptrtype', the '*TypeRecord' the caller was declared with) holding its size,
// its alignment, its finalizer and its trace, built once per object and type
LLVMValueRef genlTypeRecord(GenState *gen, INode *vtype, INode *recptrtype);
// Create an alloca (will be pushed to the entry point of the function.
LLVMValueRef genlAlloca(GenState *gen, LLVMTypeRef type, const char *name);

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
