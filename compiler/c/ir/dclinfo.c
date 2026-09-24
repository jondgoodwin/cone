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

    // What the declaration's own '@c' and '@initpure' stated survive the join;
    // everything else is written here
    uint16_t facts = dclinfo->facts & (DclStated | DclInitPure);
    // The parser's flag, read once: this is where the bit inodeIsPrivate reads
    // is written. A module carries a visibility only where it has a parent to be
    // visible outside of -- a submodule, which its parent's subfolder drew. A
    // module that is a file of its own, and one an 'import' reached, has no
    // parent and so no visibility; a submodule's own 'pub mod' declaration is
    // what clears the bit this sets.
    if ((node->tag != ModuleTag || owner != NULL) && !(node->flags & FlagPub))
        facts |= DclPrivate;
    // 'extern' is a fn/var flag; the same bit means other things on a type. It
    // says only where the definition is: the symbol is spelled by the owning
    // module's naming, C or Cone, like any other declaration's [Jon 23 Sep]
    if (node->tag == FnDclTag || node->tag == VarDclTag) {
        if (node->flags & FlagExtern)
            facts |= DclExternal;
        // A C-named module names the functions and globals it owns directly. A
        // type's methods keep Cone names, as does a generic function, whose
        // instances need a name each
        if (owner != NULL && owner->tag == ModuleTag
            && (((ModuleNode*)owner)->dclinfo.facts & DclCName)
            && !(node->tag == FnDclTag && ((FnDclNode*)node)->genericinfo)) {
            facts |= DclCName;
            if (node->tag == FnDclTag)
                facts |= ((ModuleNode*)owner)->dclinfo.facts & DclSystemCC;
        }
    }
    dclinfo->facts = facts;
}

// The nearest module enclosing a declaration, or NULL
ModuleNode *dclInfoGetModule(INode *node) {
    while (node && node->tag != ModuleTag)
        node = inodeGetOwner(node);
    return (ModuleNode*)node;
}

// Print the owner chain outermost first, '.'-separated, as the source writes a path.
// Returns whether anything was printed:
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
    inodeFprint(printed ? ".%s" : "%s", &name->namestr);
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
        inodeFprint(dclinfo->cname ? " cname \"%s\"" : " cname", dclinfo->cname);
    if (dclinfo->facts & DclSystemCC)
        inodeFprint(" system");
    if (dclinfo->facts & DclExpandReached)
        inodeFprint(" expand-reached");
    inodeFprint("]");
}
