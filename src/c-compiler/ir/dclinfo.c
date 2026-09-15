/** Declaration facts
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

// Record that a declaration has joined the namespace of 'owner'
void dclInfoJoin(INode *node, INode *owner) {
    DclInfo *dclinfo = inodeGetDclInfo(node);
    if (dclinfo == NULL)
        return;
    dclinfo->owner = owner;

    uint16_t facts = 0;
    if (inodeIsPrivate(node))
        facts |= DclPrivate;
    // 'extern' and 'extern system' are fn/var flags; the same bits mean other things on a type
    if (node->tag == FnDclTag || node->tag == VarDclTag) {
        if (node->flags & FlagExtern)
            facts |= DclExternal | DclCName;
        if (node->flags & FlagSystem)
            facts |= DclSystemCC;
    }
    dclinfo->facts = facts;
}

// The nearest module enclosing a declaration, or NULL
ModuleNode *dclInfoGetModule(INode *node) {
    while (node && node->tag != ModuleTag)
        node = inodeGetOwner(node);
    return (ModuleNode*)node;
}

// Print the owner chain outermost first, '::'-separated. Returns whether anything was printed:
// a module that contributes no name to the chain (the root) and the compiler's unnamed
// pointer/reference types contribute nothing, as in nameOwnerChain (name.c).
static int dclInfoPrintChain(INode *owner) {
    if (owner == NULL)
        return 0;
    int printed = dclInfoPrintChain(inodeGetOwner(owner));
    if (owner->tag == ModuleTag && !(((ModuleNode*)owner)->dclinfo.facts & DclNamesChain))
        return printed;
    Name *name = isNamedNode(owner) ? inodeGetName(owner) : NULL;
    if (name == NULL)
        return printed;
    inodeFprint(printed ? "::%s" : "%s", &name->namestr);
    return 1;
}

// Serialize a declaration's owner chain and facts, for the IR dump
void dclInfoPrint(INode *node) {
    DclInfo *dclinfo = inodeGetDclInfo(node);
    if (dclinfo == NULL || dclinfo->owner == NULL)
        return;
    inodeFprint(" [");
    dclInfoPrintChain(dclinfo->owner);
    if (dclinfo->facts & DclPrivate)
        inodeFprint(" private");
    if (dclinfo->facts & DclExternal)
        inodeFprint(" extern");
    if (dclinfo->facts & DclCName)
        inodeFprint(" cname");
    if (dclinfo->facts & DclSystemCC)
        inodeFprint(" system");
    inodeFprint("]");
}
