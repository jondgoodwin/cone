/** Namespace node helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"
#include "../shared/error.h"

#include <string.h>
#include <assert.h>

// Hook a node into global symbol table, such that its owner can withdraw it later
void namespaceHook(NamedAstNode *name, Symbol *namesym) {
	name->hooklink = name->owner->hooklinks; // Add to owner's hook list
	name->owner->hooklinks = (NamedAstNode*)name;
	name->prevname = namesym->node; // Latent unhooker
	namesym->node = (NamedAstNode*)name;
}

// Unhook all of an owners names from global symbol table (LIFO)
void namespaceUnhook(NamedAstNode *owner) {
	NamedAstNode *node = owner->hooklinks;
	while (node) {
		NamedAstNode *next = node->hooklink;
		node->namesym->node = node->prevname;
		node->hooklink = NULL;
		node = next;
	}
	owner->hooklinks = NULL;
}