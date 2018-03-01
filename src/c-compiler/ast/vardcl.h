/** AST handling for variable declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef vardcl_h
#define vardcl_h

// Name declaration node (e.g., variable, fn implementation, or named type)
typedef struct NameDclAstNode {
	NamedAstHdr;				// 'vtype': type of this name's value
	PermAstNode *perm;			// Permission type (often mut or imm)
	AstNode *value;				// Starting value/declaration (NULL if not initialized)
	LLVMValueRef llvmvar;		// LLVM's handle for a declared variable (for generation)
	uint16_t scope;				// 0=global
	uint16_t index;				// index within this scope (e.g., parameter number)
} NameDclAstNode;

void nameDclHook(NamedAstNode *name, Symbol *namesym);
void nameDclUnhook(NamedAstNode *owner);

NameDclAstNode *newNameDclNode(Symbol *namesym, uint16_t asttype, AstNode *sig, PermAstNode *perm, AstNode *val);
void newNameDclNodeStr(char *namestr, uint16_t asttype, AstNode *type);
int isNameDclNode(AstNode *node);
void nameDclPrint(NameDclAstNode *fn);
void nameDclPass(PassState *pstate, NameDclAstNode *node);
void nameVtypeDclPass(PassState *pstate, NameDclAstNode *name);

#endif