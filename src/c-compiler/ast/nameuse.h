/** AST handling for name uses
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef nameuse_h
#define nameuse_h

typedef struct NameList NameList;

// Name use node - when populated, it refers to the applicable declaration for the name
typedef struct NameUseAstNode {
	TypedAstHdr;
	Name *namesym;			// Pointer to the global name table entry
    NamedAstNode *dclnode;	// Node that declares this name (NULL until names are resolved)
    NameList *qualNames;    // Pointer to list of module qualifiers (NULL if none)
} NameUseAstNode;

NameUseAstNode *newNameUseNode(Name *name);
void nameUseBaseMod(NameUseAstNode *node, ModuleAstNode *basemod);
void nameUseAddQual(NameUseAstNode *node, Name *name);
NameUseAstNode *newMemberUseNode(Name *namesym);
void nameUsePrint(NameUseAstNode *name);
void nameUsePass(PassState *pstate, NameUseAstNode *name);

#endif