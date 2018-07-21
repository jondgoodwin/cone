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
	AstNode *value;				// Block or intrinsic code nodes (NULL if no code)
	LLVMValueRef llvmvar;		// LLVM's handle for a declared variable (for generation)
    struct FnDclAstNode *nextnode;     // Link to next overloaded method with the same name (or NULL)
} FnDclAstNode;

FnDclAstNode *newFnDclNode(Name *namesym, uint16_t asttype, AstNode *sig, AstNode *val);
void fnDclPrint(FnDclAstNode *fn);
void fnDclPass(PassState *pstate, FnDclAstNode *node);

#endif