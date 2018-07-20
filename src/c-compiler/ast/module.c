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
	mod->owner = NULL;
	mod->nodes = newNodes(64);
	namespaceInit(&mod->namednodes, 64);
	return mod;
}

// Add a newly parsed named node to the module:
// - We preserve all nodes for later semantic pass and serialization iteration
//     Name resolution will iterate over these even to pick up folder names/aliases
// - We hook all names in global name table at parse time to check for name dupes and
//     because permissions and allocators do not support forward references
// - We remember all public names for later resolution of qualified names
void modAddNode(ModuleAstNode *mod, AstNode *node) {

    // Add to regular ordered node list
    nodesAdd(&mod->nodes, node);

    // If it is a named node...
    if (isNamedNode(node)) {
        NamedAstNode *nnode = (NamedAstNode *)node;
        Name *name = nnode->namesym;

        // Hook into global name table (and add to namednodes), if not already there
        if (!name->node) {
            nametblHookNode(nnode);
            // Remember public names
            if (name->namestr != '_')
                namespaceSet(&mod->namednodes, name, nnode);
        }
        else {
            errorMsgNode((AstNode *)node, ErrorDupName, "Global name is already defined. Duplicates not allowed.");
            errorMsgNode((AstNode*)name->node, ErrorDupName, "This is the conflicting definition for that name.");
        }
    }
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

// Unhook old module's names, hook new module's names
// (works equally well from parent to child or child to parent
void modHook(ModuleAstNode *oldmod, ModuleAstNode *newmod) {
    if (oldmod)
        nametblHookPop();
    if (newmod) {
        AstNode **nodesp;
        uint32_t cnt;
        nametblHookPush();
        for (nodesFor(newmod->nodes, cnt, nodesp)) {
            if (isNamedNode(*nodesp))
                nametblHookNode((NamedAstNode *)*nodesp);
        }
    }
}

// Check the module's AST
void modPass(PassState *pstate, ModuleAstNode *mod) {
	ModuleAstNode *svmod = pstate->mod;
	pstate->mod = mod;
	AstNode **nodesp;
	uint32_t cnt;

	// Switch name table over to new mod for name resolution
	if (pstate->pass == NameResolution)
		modHook((ModuleAstNode*)mod->owner, mod);

	// For global variables and functions, handle all their type info first
	for (nodesFor(mod->nodes, cnt, nodesp)) {
        switch ((*nodesp)->asttype) {
        case VarDclTag:
        {
            VarDclAstNode * varnode = (VarDclAstNode*)*nodesp;
            astPass(pstate, (AstNode*)varnode->perm);
            astPass(pstate, varnode->vtype);
        }
        case FnDclTag:
        {
            FnDclAstNode * varnode = (FnDclAstNode*)*nodesp;
            astPass(pstate, varnode->vtype);
        }
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

	pstate->mod = svmod;
}