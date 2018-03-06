/** Namespace node helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/nametbl.h"
#include "../shared/error.h"

#include <string.h>
#include <assert.h>

// Create a new Module node
ModuleAstNode *newModuleNode() {
	ModuleAstNode *mod;
	newAstNode(mod, ModuleAstNode, ModuleNode);
	mod->namesym = NULL;
	mod->hooklinks = NULL;
	mod->hooklink = NULL;
	mod->prevname = NULL;
	mod->owner = NULL;
	mod->nodes = newNodes(16);
	return mod;
}

// Serialize the AST for a module
void modPrint(ModuleAstNode *mod) {
	AstNode **nodesp;
	uint32_t cnt;

	if (mod->namesym)
		astFprint("module %s\n", &mod->namesym->namestr);
	else
		astFprint("AST for program %s\n", mod->lexer->url);
	astPrintIncr();
	for (nodesFor(mod->nodes, cnt, nodesp)) {
		astPrintIndent();
		astPrintNode(*nodesp);
		astPrintNL();
	}
	astPrintDecr();
}

// Check the module's AST
void modPass(PassState *pstate, ModuleAstNode *mod) {
	AstNode **nodesp;
	uint32_t cnt;

	if (pstate->pass == NameResolution && mod->owner)
		nameHook((NamedAstNode *)mod, mod->namesym);

	// For global variables and functions, handle all their type info first
	for (nodesFor(mod->nodes, cnt, nodesp)) {
		// Hook global vars/fns in global name table (alloc/perm already there)
		if (pstate->pass == NameResolution) {
			switch ((*nodesp)->asttype) {
			case VarNameDclNode:
			case VtypeNameDclNode:
				nameHook((NamedAstNode*)*nodesp, ((NamedAstNode*)*nodesp)->namesym);
				break;
			}
		}
		if ((*nodesp)->asttype == VarNameDclNode) {
			NameDclAstNode *name = (NameDclAstNode*)*nodesp;
			astPass(pstate, (AstNode*)name->perm);
			astPass(pstate, name->vtype);
		}
	}
	if (errors)
		return;

	// Now we can process the full node info
	for (nodesFor(mod->nodes, cnt, nodesp)) {
		astPass(pstate, *nodesp);
	}

	if (pstate->pass == NameResolution)
		nameUnhook((NamedAstNode*)mod);
}