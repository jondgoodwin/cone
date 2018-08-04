/** AST handling for variable declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef vardcl_h
#define vardcl_h

// Name declaration node (e.g., variable, fn implementation, or named type)
typedef struct VarDclAstNode {
	NamedAstHdr;				// 'vtype': type of this name's value
	INode *value;				// Starting value/declaration (NULL if not initialized)
	LLVMValueRef llvmvar;		// LLVM's handle for a declared variable (for generation)
    PermAstNode *perm;			// Permission type (often mut or imm)
    uint16_t scope;				// 0=global
	uint16_t index;				// index within this scope (e.g., parameter number)
} VarDclAstNode;

VarDclAstNode *newVarDclNode(Name *namesym, uint16_t asttype, INode *sig, PermAstNode *perm, INode *val);
void varDclPrint(VarDclAstNode *fn);
void varDclPass(PassState *pstate, VarDclAstNode *node);

#endif