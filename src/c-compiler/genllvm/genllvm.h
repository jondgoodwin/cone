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
#include <llvm-c/ExecutionEngine.h>

typedef struct GenState {
	LLVMTargetDataRef datalayout;
	LLVMContextRef context;
	LLVMModuleRef module;
	LLVMValueRef fn;
	LLVMBuilderRef builder;
	LLVMBasicBlockRef whilebeg;
	LLVMBasicBlockRef whileend;

	char *srcname;
} GenState;

void genllvm(ConeOptions *opt, ModuleNode *mod);
void genlFn(GenState *gen, FnDclNode *fnnode);
void genlGloVarName(GenState *gen, VarDclNode *glovar);
void genlGloFnName(GenState *gen, FnDclNode *glofn);

// genlstmt.c
LLVMBasicBlockRef genlInsertBlock(GenState *gen, char *name);
LLVMValueRef genlBlock(GenState *gen, BlockNode *blk);

// genlexpr.c
LLVMValueRef genlExpr(GenState *gen, INode *termnode);

// genlalloc.c
// Generate code that creates an allocated ref by allocating and initializing
LLVMValueRef genlallocref(GenState *gen, AddrNode *addrnode);
// Progressively dealias or drop all declared variables in nodes list
void genlDealiasNodes(GenState *gen, Nodes *nodes);
// Add to the counter of an rc allocated reference
void genlRcCounter(GenState *gen, LLVMValueRef ref, long long amount);
// Is value's type an Rc allocated ref we might copy (and increment refcount)?
int genlDoAliasRc(INode *rval);

// genltype.c
// Generate a type value
LLVMTypeRef genlType(GenState *gen, INode *typ);
// Generate LLVM value corresponding to the size of a type
LLVMValueRef genlSizeof(GenState *gen, INode *vtype);

#endif
