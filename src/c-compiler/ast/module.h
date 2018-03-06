/** Namespace structures and helper functions
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef module_h
#define module_h

// Namespace Ownert Node header, for named declarations and blocks
// - owner is the namespace node this name belongs to
// - hooklinks starts linked list of all names in this name's namespace
#define OwnerAstHdr \
	TypedAstHdr; \
	struct NamedAstNode *owner; \
	struct NamedAstNode *hooklinks

// Named Ast Node header, for variable and type declarations
// - owner is the namespace node this name belongs to
// - namesym points to the global name table entry (holds name string)
// - hooklinks starts linked list of all names in this name's namespace
// - hooklink links this name as part of owner's hooklinks
// - prevname points to named node this overrides in global name table
#define NamedAstHdr \
	OwnerAstHdr; \
	Name *namesym; \
	struct NamedAstNode *hooklink; \
	struct NamedAstNode *prevname

// Castable structure for all named AST nodes
typedef struct NamedAstNode {
	NamedAstHdr;
} NamedAstNode;

// Module
typedef struct ModuleAstNode {
	NamedAstHdr;
	Nodes *nodes;
} ModuleAstNode;

ModuleAstNode *newModuleNode();
void modPrint(ModuleAstNode *mod);
void modPass(PassState *pstate, ModuleAstNode *mod);

void nameHook(NamedAstNode *name, Name *namesym);
void nameUnhook(NamedAstNode *owner);

#endif