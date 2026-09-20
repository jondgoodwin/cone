/** Handling for alias declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>

// Create an alias under a local spelling for what 'target' names
AliasDclNode *newAliasDclNode(Name *namesym, INode *target) {
    AliasDclNode *node;
    newNode(node, AliasDclNode, AliasDclTag);
    node->namesym = namesym;
    node->target = target;
    node->flags |= FlagPub | FlagMethFld;
    return node;
}

// Clone an alias. The original is mapped to the copy, so that a cloned method
// body whose name use was bound to the original is re-pointed at the copy, as
// a use of any other cloned declaration is.
INode *cloneAliasDclNode(CloneState *cstate, AliasDclNode *node) {
    AliasDclNode *newnode = memAllocBlk(sizeof(AliasDclNode));
    memcpy(newnode, node, sizeof(AliasDclNode));
    newnode->target = cloneNode(cstate, node->target);
    cloneDclSetMap((INode*)node, (INode*)newnode);
    return (INode*)newnode;
}

// Serialize an alias
void aliasDclPrint(AliasDclNode *node) {
    inodeFprint("use %s", &node->namesym->namestr);
    if (node->target) {
        inodeFprint(" = ");
        inodePrintNode(node->target);
    }
}

// The declaration at the end of a chain of aliases, or NULL for one not yet bound
INode *aliasDclResolve(INode *node) {
    while (node && node->tag == AliasDclTag)
        node = ((NameUseNode*)((AliasDclNode*)node)->target)->dclnode;
    return node;
}
