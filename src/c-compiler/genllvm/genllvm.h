/** Generator for LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef genllvm_h
#define genllvm_h

#include "../ast/ast.h"
#include "../coneopts.h"

#include <llvm-c/Core.h>

typedef struct genl_t {
	LLVMContextRef context;
	LLVMModuleRef module;
	LLVMValueRef fn;
	LLVMBuilderRef builder;

	char *srcname;
} genl_t;

void genllvm(ConeOptions *opt, PgmAstNode *pgmast);

#endif
