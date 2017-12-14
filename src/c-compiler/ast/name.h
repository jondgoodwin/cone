/** AST handling for names
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef name_h
#define name_h

// Name declaration node (e.g., variable, fn implementation, or named type)
typedef struct NameDclAstNode {
	TypedAstHdr;				// 'vtype': type of this name's value
	PermAstNode *perm;		// Permission type (often mut or imm)
	Symbol *namesym;			// Pointer to the global symbol table entry
	AstNode *value;				// Starting value/declaration (NULL if not initialized)
	struct NameDclAstNode *prev; // previous name this overrides in global symbol table
	LLVMValueRef llvmvar;		// LLVM's handle for a declared variable (for generation)
	uint16_t scope;				// 0=global
	uint16_t index;				// index within this scope (e.g., parameter number)
} NameDclAstNode;

// Name use node - when populated, it refers to the applicable declaration for the name
typedef struct NameUseAstNode {
	TypedAstHdr;
	Symbol *namesym;			// Pointer to the global symbol table entry
	NameDclAstNode *dclnode;	// Declaration of this name (NULL until names are resolved)
} NameUseAstNode;

NameUseAstNode *newNameUseNode(Symbol *namesym);
void nameUsePrint(int indent, NameUseAstNode *name, char *prefix);
void nameUsePass(AstPass *pstate, NameUseAstNode *name);

NameDclAstNode *newNameDclNode(Symbol *namesym, uint16_t asttype, AstNode *sig, PermAstNode *perm, AstNode *val);
void newNameDclNodeStr(char *namestr, uint16_t asttype, AstNode *type);
int isNameDclNode(AstNode *node);
void nameDclPrint(int indent, NameDclAstNode *fn, char *prefix);
void nameDclPass(AstPass *pstate, NameDclAstNode *node);

#endif