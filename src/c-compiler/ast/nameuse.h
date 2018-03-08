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
	Name *namesym;			// Pointer to the global name table entry
	ModuleAstNode *mod;		// Module this name belongs to
	NameDclAstNode *dclnode;	// Declaration of this name (NULL until names are resolved)
} NameUseAstNode;

NameUseAstNode *newNameUseNode(Name *namesym);
NameUseAstNode *newMemberUseNode(Name *namesym);
void nameUsePrint(NameUseAstNode *name);
void nameUsePass(PassState *pstate, NameUseAstNode *name);

#endif