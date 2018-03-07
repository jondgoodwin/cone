/** Module and import node helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../ast/nametbl.h"
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
	mod->nodes = newNodes(64);
	mod->namednodes = newInodes(64);
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

void modAddNamedNode(ModuleAstNode *mod, NamedAstNode *node, Name *alias) {
	Name *name = alias ? alias : node->namesym;

	if (name->node) {
		errorMsgNode((AstNode *)node, ErrorDupName, "Global name is already defined. Only one allowed.");
		errorMsgNode((AstNode*)name->node, ErrorDupName, "This is the conflicting definition for that name.");
	}
	else {
		inodesAdd(&mod->namednodes, name, (AstNode *)node);
		nameHook((OwnerAstNode *)node->owner, node, name);
	}

}

// Unhook old module's names, hook new module's names
// (works equally well from parent to child or child to parent
void modHook(ModuleAstNode *oldmod, ModuleAstNode *newmod) {
	if (oldmod)
		nameUnhook((OwnerAstNode *)oldmod);
	if (newmod)
		inodesHook((OwnerAstNode*)newmod, newmod->namednodes);
}

// Check the module's AST
void modPass(PassState *pstate, ModuleAstNode *mod) {
	AstNode **nodesp;
	uint32_t cnt;

	// Switch name table over to new mod for name resolution
	if (pstate->pass == NameResolution)
		modHook((ModuleAstNode*)mod->owner, mod);

	// For global variables and functions, handle all their type info first
	for (nodesFor(mod->nodes, cnt, nodesp)) {
		if ((*nodesp)->asttype == VarNameDclNode) {
			NameDclAstNode *name = (NameDclAstNode*)*nodesp;
			astPass(pstate, (AstNode*)name->perm);
			astPass(pstate, name->vtype);
		}
	}

	// Now we can process the full node info
	if (errors == 0) {
		for (nodesFor(mod->nodes, cnt, nodesp)) {
			astPass(pstate, *nodesp);
		}
	}

	// Switch name table back to owner module
	if (pstate->pass == NameResolution)
		modHook(mod, (ModuleAstNode*)mod->owner);
}