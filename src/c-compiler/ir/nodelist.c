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

// Add an INode to the end of a NodeList, growing it if full (changing its memory location)
void nodelistAdd(NodeList *mnodes, INode *node) {
    if (mnodes->used >= mnodes->avail)
        nodelistGrow(mnodes);
    mnodes->nodes[mnodes->used++] = node;
}

// Insert some list into another list, beginning at index
void nodeListInsertList(NodeList *mnodes, NodeList *fromnodes, uint32_t index) {
    while (mnodes->used + fromnodes->used >= mnodes->avail)
        nodelistGrow(mnodes);
    uint32_t amt = fromnodes->used;
    memmove(&mnodes->nodes[amt + index], &mnodes->nodes[index], (mnodes->used-index) * sizeof(INode **));
    memcpy(&mnodes->nodes[index], fromnodes->nodes, amt * sizeof(INode **));
    mnodes->used += fromnodes->used;
}