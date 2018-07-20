/** AST handling for function/method declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fndcl_h
#define fndcl_h

// Name declaration node (e.g., variable, fn implementation, or named type)
typedef struct FnDclAstNode {
	NamedAstHdr;				// 'vtype': type of this name's value
	PermAstNode *perm;			// Permission type (often mut or imm)
	AstNode *value;				// Starting value/declaration (NULL if not initialized)
	LLVMValueRef llvmvar;		// LLVM's handle for a declared variable (for generation)
	uint16_t scope;				// 0=global
	uint16_t index;				// index within this scope (e.g., parameter number)
} FnDclAstNode;

FnDclAstNode *newFnDclNode(Name *namesym, uint16_t asttype, AstNode *sig, PermAstNode *perm, AstNode *val);
void fnDclPrint(FnDclAstNode *fn);
void fnDclPass(PassState *pstate, FnDclAstNode *node);

#endif