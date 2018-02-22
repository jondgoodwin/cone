/** AST handling for variable declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef vardcl_h
#define vardcl_h

// Name declaration node (e.g., variable, fn implementation, or named type)
typedef struct NameDclAstNode {
	TypedAstHdr;				// 'vtype': type of this name's value
	PermAstNode *perm;			// Permission type (often mut or imm)
	Symbol *namesym;			// Pointer to the global symbol table entry
	char *guname;				// Globally unique name (typically mangled)
	AstNode *value;				// Starting value/declaration (NULL if not initialized)
	struct AstNode *prev;		// previous name this overrides in global symbol table
	LLVMValueRef llvmvar;		// LLVM's handle for a declared variable (for generation)
	uint16_t scope;				// 0=global
	uint16_t index;				// index within this scope (e.g., parameter number)
} NameDclAstNode;

NameDclAstNode *newNameDclNode(Symbol *namesym, uint16_t asttype, AstNode *sig, PermAstNode *perm, AstNode *val);
void newNameDclNodeStr(char *namestr, uint16_t asttype, AstNode *type);
int isNameDclNode(AstNode *node);
void nameDclPrint(NameDclAstNode *fn);
void nameDclPass(AstPass *pstate, NameDclAstNode *node);
void nameVtypeDclPass(AstPass *pstate, NameDclAstNode *name);

#endif