/** Shared logic for namespace-based types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// Initialize methnodes metadata
void nodelistInit(NodeList *mnodes, uint32_t size) {
    mnodes->avail = size;
    mnodes->used = 0;
    mnodes->nodes = (INode **)memAllocBlk(size * sizeof(INode **));
}

// Double size, if full
void nodelistGrow(NodeList *mnodes) {
    INode **oldnodes;
    oldnodes = mnodes->nodes;
    mnodes->avail <<= 1;
    mnodes->nodes = (INode **)memAllocBlk(mnodes->avail * sizeof(INode **));
    memcpy(mnodes->nodes, oldnodes, mnodes->used * sizeof(INode **));
}

// Find the desired named node.
// Return the node, if found or NULL if not found
INamedNode *iNsTypeNodeFind(NodeList *mnodes, Name *name) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(mnodes, cnt, nodesp)) {
        if (isNamedNode(*nodesp)) {
            if (((INamedNode*)*nodesp)->namesym == name)
                return (INamedNode*)*nodesp;
        }
    }
    return NULL;
}

// Add an INode to the end of a NodeList, growing it if full (changing its memory location)
void nodelistAdd(NodeList *mnodes, INode *node) {
    if (mnodes->used >= mnodes->avail)
        nodelistGrow(mnodes);
    mnodes->nodes[mnodes->used++] = node;
}
