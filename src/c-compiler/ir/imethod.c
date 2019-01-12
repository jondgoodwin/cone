/** Shared logic for method-based types
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
void imethnodesInit(IMethNodes *mnodes, uint32_t size) {
    mnodes->avail = size;
    mnodes->used = 0;
    mnodes->nodes = (INode **)memAllocBlk(size * sizeof(INode **));
}

// Double size, if full
void methnodesGrow(IMethNodes *mnodes) {
    INode **oldnodes;
    oldnodes = mnodes->nodes;
    mnodes->avail <<= 1;
    mnodes->nodes = (INode **)memAllocBlk(mnodes->avail * sizeof(INode **));
    memcpy(mnodes->nodes, oldnodes, mnodes->used * sizeof(INode **));
}

// Find the desired named node.
// Return the node, if found or NULL if not found
INamedNode *imethnodesFind(IMethNodes *mnodes, Name *name) {
    INode **nodesp;
    uint32_t cnt;
    for (imethnodesFor(mnodes, cnt, nodesp)) {
        if (isNamedNode(*nodesp)) {
            if (((INamedNode*)*nodesp)->namesym == name)
                return (INamedNode*)*nodesp;
        }
    }
    return NULL;
}

// Add an INode to the end of a IMethNodes, growing it if full (changing its memory location)
void imethnodesAdd(IMethNodes *mnodes, INode *node) {
    if (mnodes->used >= mnodes->avail)
        methnodesGrow(mnodes);
    mnodes->nodes[mnodes->used++] = node;
}

// Add a function or potentially overloaded method
// If method is overloaded, add it to the link chain of same named methods
void imethnodesAddFn(IMethNodes *mnodes, FnDclNode *fnnode) {
    FnDclNode *lastmeth = (FnDclNode *)imethnodesFind(mnodes, fnnode->namesym);
    if (lastmeth && (lastmeth->tag != FnDclTag
        || !(lastmeth->flags & FlagMethProp) || !(fnnode->flags & FlagMethProp))) {
        errorMsgNode((INode*)fnnode, ErrorDupName, "Duplicate name %s: Only methods can be overloaded.", &fnnode->namesym->namestr);
        return;
    }
    if (lastmeth) {
        while (lastmeth->nextnode)
            lastmeth = lastmeth->nextnode;
        lastmeth->nextnode = fnnode;
    }
    imethnodesAdd(mnodes, (INode*)fnnode);
}

// Add a property
void imethnodesAddProp(IMethNodes *mnodes,  VarDclNode *varnode) {
    FnDclNode *lastmeth = (FnDclNode *)imethnodesFind(mnodes, varnode->namesym);
    if (lastmeth && (lastmeth->tag != VarDclTag)) {
        errorMsgNode((INode*)varnode, ErrorDupName, "Duplicate name %s: Only methods can be overloaded.", &varnode->namesym->namestr);
        return;
    }
    imethnodesAdd(mnodes, (INode*)varnode);
}

// Find method that best fits the passed arguments
FnDclNode *imethnodesFindBestMethod(FnDclNode *firstmethod, Nodes *args) {
    // Look for best-fit method
    FnDclNode *bestmethod = NULL;
    int bestnbr = 0x7fffffff; // ridiculously high number    
    for (FnDclNode *methnode = (FnDclNode *)firstmethod; methnode; methnode = methnode->nextnode) {
        int match;
        switch (match = fnSigMatchMethCall((FnSigNode *)methnode->vtype, args)) {
        case 0: continue;        // not an acceptable match
        case 1: return methnode;    // perfect match!
        default:                // imprecise match using conversions
            // If this will auto-ref, make sure the ref perm will match
            if (match >= 100 && 
                !refAutoRefCheck(nodesGet(args, 0), ((ITypedNode*)nodesGet(((FnSigNode*)methnode->vtype)->parms, 0))->vtype))
                continue;
            if (match < bestnbr) {
                // Remember this as best found so far
                bestnbr = match;
                bestmethod = methnode;
            }
        }
    }
    return bestmethod;
}