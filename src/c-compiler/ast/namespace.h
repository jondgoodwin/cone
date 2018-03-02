/** Namespace structures and helper functions
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef namespace_h
#define namespace_h

// Namespace Ownert Node header, for named declarations and blocks
// - owner is the namespace node this name belongs to
// - hooklinks starts linked list of all names in this name's namespace
#define OwnerAstHdr \
	TypedAstHdr; \
	struct NamedAstNode *owner; \
	struct NamedAstNode *hooklinks

// Named Ast Node header, for variable and type declarations
// - owner is the namespace node this name belongs to
// - namesym points to the global symbol table entry (holds name string)
// - hooklinks starts linked list of all names in this name's namespace
// - hooklink links this name as part of owner's hooklinks
// - prevname points to named node this overrides in global symbol table
#define NamedAstHdr \
	OwnerAstHdr; \
	Symbol *namesym; \
	struct NamedAstNode *hooklink; \
	struct NamedAstNode *prevname

// Castable structure for all named AST nodes
typedef struct NamedAstNode {
	NamedAstHdr;
} NamedAstNode;

void namespaceHook(NamedAstNode *name, Symbol *namesym);
void namespaceUnhook(NamedAstNode *owner);

#endif