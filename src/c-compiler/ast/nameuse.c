/** Name and Field Use nodes.
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

// A list of module names (qualifiers)
typedef struct NameList {
    uint16_t avail;     // Max. number of names allocated for
    uint16_t used;      // Number of names stored in list
    ModuleAstNode *basemod;  // base module (root or current) holding qualifiers
    // Name* pointers for qualifiers follow, starting here
} NameList;

// Create a new name use node
NameUseAstNode *newNameUseNode(Name *namesym) {
	NameUseAstNode *name;
	newAstNode(name, NameUseAstNode, NameUseTag);
	name->qualNames = NULL;
	name->dclnode = NULL;
    name->namesym = namesym;
	return name;
}

NameUseAstNode *newMemberUseNode(Name *namesym) {
	NameUseAstNode *name;
	newAstNode(name, NameUseAstNode, MbrNameUseTag);
    name->qualNames = NULL;
    name->dclnode = NULL;
    name->namesym = namesym;
	return name;
}

// If a NameUseAstNode has module name qualifiers, it will first set basemod
// (either root module or the current module scope). This allocates an area
// for qualifiers to be added.
void nameUseBaseMod(NameUseAstNode *node, ModuleAstNode *basemod) {
    node->qualNames = (NameList *)memAllocBlk(sizeof(NameList) + 4 * sizeof(Name*));
    node->qualNames->avail = 4;
    node->qualNames->used = 0;
    node->qualNames->basemod = basemod;
}

// Add a module name qualifier to the end of the list
void nameUseAddQual(NameUseAstNode *node, Name *name) {
    int16_t used = node->qualNames->used;
    if (used + 1 >= node->qualNames->avail) {
        NameList *oldlist = node->qualNames;
        uint16_t newavail = oldlist->avail << 1;
        node->qualNames = (NameList *)memAllocBlk(sizeof(NameList) + newavail * sizeof(Name*));
        node->qualNames->avail = newavail;
        node->qualNames->used = used;
        node->qualNames->basemod = oldlist->basemod;
        Name **oldp = (Name**)(oldlist + 1);
        Name **newp = (Name**)(node->qualNames + 1);
        int16_t cnt = used;
        while (cnt--)
            *newp++ = *oldp++;
    }
    Name **namep = (Name**)&(node->qualNames + 1)[used];
    *namep = name;
    ++node->qualNames->used;
}

// Serialize the AST for a name use
void nameUsePrint(NameUseAstNode *name) {
    if (name->qualNames) {
        // if root: astFprint("::");
        int16_t cnt = name->qualNames->used;
        Name **namep = (Name**)(name->qualNames + 1);
        while (cnt--)
            astFprint("%s::", &(*namep++)->namestr);
    }
	astFprint("%s", &name->namesym->namestr);
}

// Check the name use's AST
void nameUsePass(PassState *pstate, NameUseAstNode *name) {
	// During name resolution, point to name declaration and copy over needed fields
	switch (pstate->pass) {
	case NameResolution:
        if (name->qualNames) {
            // Do iterative look ups of module qualifiers beginning with basemod
            ModuleAstNode *mod = name->qualNames->basemod;
            int16_t cnt = name->qualNames->used;
            Name **namep = (Name**)(name->qualNames + 1);
            while (cnt--) {
                mod = (ModuleAstNode*)namespaceFind(&mod->namednodes, *namep++);
                if (mod==NULL || mod->asttype!=ModuleNode) {
                    errorMsgNode((AstNode*)name, ErrorUnkName, "Module %s does not exist", (*--namep)->namestr);
                    return;
                }
            }
            name->dclnode = namespaceFind(&mod->namednodes, name->namesym);
        }
        else
            // For current module, should already be hooked in global name table
			name->dclnode = (NamedAstNode*)name->namesym->node;

        if (name->dclnode) {
            if (name->dclnode->asttype == VarNameDclNode)
                name->asttype = VarNameUseTag;
            else
                name->asttype = TypeNameUseTag;
        }
        else
			errorMsgNode((AstNode*)name, ErrorUnkName, "The name %s does not refer to a declared name", &name->namesym->namestr);
		break;
	case TypeCheck:
		name->vtype = name->dclnode->vtype;
		break;
	}
}
