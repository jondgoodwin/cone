/** Name and Member Use nodes.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// A list of module names (qualifiers)
typedef struct NameList {
    uint16_t avail;     // Max. number of names allocated for
    uint16_t used;      // Number of names stored in list
    ModuleNode *basemod;  // base module (root or current) holding qualifiers
    // Name* pointers for qualifiers follow, starting here
} NameList;

// Create a new name use node
NameUseNode *newNameUseNode(Name *namesym) {
    NameUseNode *name;
    newNode(name, NameUseNode, NameUseTag);
    name->qualNames = NULL;
    name->dclnode = NULL;
    name->namesym = namesym;
    return name;
}

NameUseNode *newMemberUseNode(Name *namesym) {
    NameUseNode *name;
    newNode(name, NameUseNode, MbrNameUseTag);
    name->qualNames = NULL;
    name->dclnode = NULL;
    name->namesym = namesym;
    return name;
}

// If a NameUseNode has module name qualifiers, it will first set basemod
// (either root module or the current module scope). This allocates an area
// for qualifiers to be added.
void nameUseBaseMod(NameUseNode *node, ModuleNode *basemod) {
    node->qualNames = (NameList *)memAllocBlk(sizeof(NameList) + 4 * sizeof(Name*));
    node->qualNames->avail = 4;
    node->qualNames->used = 0;
    node->qualNames->basemod = basemod;
}

// Add a module name qualifier to the end of the list
void nameUseAddQual(NameUseNode *node, Name *name) {
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

// Serialize a name use node
void nameUsePrint(NameUseNode *name) {
    if (name->qualNames) {
        // if root: inodeFprint("::");
        int16_t cnt = name->qualNames->used;
        Name **namep = (Name**)(name->qualNames + 1);
        while (cnt--)
            inodeFprint("%s::", &(*namep++)->namestr);
    }
    inodeFprint("%s", &name->namesym->namestr);
}

// Handle name resolution for name use references
// - Point to name declaration in other module or this one
// - If name is for a method or field, rewrite node as 'self.property'
// - If not method/field, re-tag it as either TypeNameUse or VarNameUse
void nameUseNameRes(NameResState *pstate, NameUseNode **namep) {
    NameUseNode *name = *namep;

    // For module-qualified names, look up name in that module
    if (name->qualNames) {
        // Do iterative look ups of module qualifiers beginning with basemod
        ModuleNode *mod = name->qualNames->basemod;
        int16_t cnt = name->qualNames->used;
        Name **namep = (Name**)(name->qualNames + 1);
        while (cnt--) {
            mod = (ModuleNode*)namespaceFind(&mod->namednodes, *namep++);
            if (mod == NULL || mod->tag != ModuleTag) {
                errorMsgNode((INode*)name, ErrorUnkName, "Module %s does not exist", &(*--namep)->namestr);
                return;
            }
        }
        name->dclnode = namespaceFind(&mod->namednodes, name->namesym);
    }
    else
        // For non-qualified names (current module), should already be hooked in global name table
        name->dclnode = (INamedNode*)name->namesym->node;

    if (!name->dclnode) {
        errorMsgNode((INode*)name, ErrorUnkName, "The name %s does not refer to a declared name", &name->namesym->namestr);
        return;
    }

    // If name is for a method or field, rewrite node as 'self.property'
    if (name->dclnode->tag == VarDclTag && name->dclnode->flags & FlagMethProp) {
        // Doing this rewrite ensures we reuse existing type check and gen code for
        // properly handling property access
        NameUseNode *selfnode = newNameUseNode(selfName);
        FnCallNode *fncall = newFnCallNode((INode *)selfnode, 0);
        fncall->methprop = name;
        copyNodeLex(fncall, name); // Copy lexer info into injected node in case it has errors
        *((FnCallNode**)namep) = fncall;
        inodeNameRes(pstate, (INode **)namep);
        return;
    }

    // Distinguish whether a name is for a variable/function name vs. type
    if (name->dclnode->tag == VarDclTag || name->dclnode->tag == FnDclTag)
        name->tag = VarNameUseTag;
    else
        name->tag = TypeNameUseTag;
}

// Handle type check for name use references
void nameUseTypeCheck(TypeCheckState *pstate, NameUseNode **namep) {
    NameUseNode *name = *namep;
    name->vtype = name->dclnode->vtype;
}
