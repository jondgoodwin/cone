/** AST handling for name uses
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef nameuse_h
#define nameuse_h

// Name use node - when populated, it refers to the applicable declaration for the name
typedef struct NameUseAstNode {
	TypedAstHdr;
	Symbol *namesym;			// Pointer to the global symbol table entry
	NameDclAstNode *dclnode;	// Declaration of this name (NULL until names are resolved)
} NameUseAstNode;

NameUseAstNode *newNameUseNode(Symbol *namesym);
NameUseAstNode *newMemberUseNode(Symbol *namesym);
void nameUsePrint(NameUseAstNode *name);
void nameUsePass(AstPass *pstate, NameUseAstNode *name);

#endif