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

// Initialize common fields
void iNsTypeInit(INsTypeNode *type, int nodecnt) {
    nodelistInit(&type->nodelist, nodecnt);
    namespaceInit(&type->namespace, nodecnt);
    // type->subtypes = newNodes(0);
}

// Add a function or potentially overloaded method
// If method is overloaded, add it to the link chain of same named methods
void iNsTypeAddFn(INsTypeNode *type, FnDclNode *fnnode) {
    NodeList *mnodes = &type->nodelist;
    FnDclNode *lastmeth = (FnDclNode *)iNsTypeNodeFind(mnodes, fnnode->namesym);
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
    nodelistAdd(mnodes, (INode*)fnnode);
}

// Add a property
void iNsTypeAddProp(INsTypeNode *type,  VarDclNode *varnode) {
    NodeList *mnodes = &type->nodelist;
    FnDclNode *lastmeth = (FnDclNode *)iNsTypeNodeFind(mnodes, varnode->namesym);
    if (lastmeth && (lastmeth->tag != VarDclTag)) {
        errorMsgNode((INode*)varnode, ErrorDupName, "Duplicate name %s: Only methods can be overloaded.", &varnode->namesym->namestr);
        return;
    }
    nodelistAdd(mnodes, (INode*)varnode);
}

// Find method that best fits the passed arguments
FnDclNode *iNsTypeFindBestMethod(FnDclNode *firstmethod, Nodes *args) {
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
                !refAutoRefCheck(nodesGet(args, 0), ((IExpNode*)nodesGet(((FnSigNode*)methnode->vtype)->parms, 0))->vtype))
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