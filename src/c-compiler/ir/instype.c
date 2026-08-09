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
    type->dropfn = NULL;
    // type->subtypes = newNodes(0);
}

// Add a function or potentially overloaded method to dictionary
// The first method declared for a name binds directly to its FnDclNode.
// A second same-named method replaces that binding with an FnOverloadDclNode
// holding both, and later methods are appended to that node's candidates.
void iNsTypeAddFnDict(INsTypeNode *type, FnDclNode *fnnode) {
    INode *foundnode = namespaceAdd(&type->namespace, fnnode->namesym, (INode*)fnnode);
    if (foundnode == NULL)
        return;

    // Growing an existing overload set only requires the new declaration be a method
    if (foundnode->tag == FnOverloadDclTag && (fnnode->flags & FlagMethFld)) {
        nodesAdd(&((FnOverloadDclNode*)foundnode)->overloads, (INode*)fnnode);
        return;
    }

    // Otherwise, only two methods may be turned into a new overload set
    if (foundnode->tag != FnDclTag
        || !(foundnode->flags & FlagMethFld) || !(fnnode->flags & FlagMethFld)) {
        errorMsgNode((INode*)fnnode, ErrorDupName, "Duplicate name %s: Only methods can be overloaded.", &fnnode->namesym->namestr);
        return;
    }

    // Replace the single method binding with an overload set holding both methods
    FnOverloadDclNode *overloadnode = newFnOverloadDclNode(fnnode->namesym);
    inodeLexCopy((INode*)overloadnode, foundnode);
    overloadnode->flags |= FlagMethFld;
    nodesAdd(&overloadnode->overloads, foundnode);
    nodesAdd(&overloadnode->overloads, (INode*)fnnode);
    namespaceSet(&type->namespace, fnnode->namesym, (INode*)overloadnode);
}

// Add a function/method to type's dictionary and owned list
void iNsTypeAddFn(INsTypeNode *type, FnDclNode *fnnode) {
    NodeList *mnodes = &type->nodelist;
    nodelistAdd(mnodes, (INode*)fnnode);
    iNsTypeAddFnDict(type, fnnode);
}

// Find the named node (could be method or field)
// Return the node, if found or NULL if not found
INode *iNsTypeFindFnField(INsTypeNode *type, Name *name) {
    return namespaceFind(&type->namespace, name);
}

// Point 'candidatesp' at the ordered method candidates a namespace binding declares,
// returning how many there are. Returns 0 when the binding declares no method.
static uint32_t iNsTypeCandidates(INode **bindingp, INode ***candidatesp) {
    INode *binding = *bindingp;
    if (binding == NULL)
        return 0;
    if (binding->tag == FnDclTag) {
        *candidatesp = bindingp;
        return 1;
    }
    if (binding->tag == FnOverloadDclTag) {
        Nodes *overloads = ((FnOverloadDclNode*)binding)->overloads;
        if (overloads->used == 0)
            return 0;
        *candidatesp = &nodesGet(overloads, 0);
        return overloads->used;
    }
    return 0;
}

// Find method that best fits the passed arguments
// 'binding' is the namespace's binding for the method's name:
// either a single FnDclNode or an FnOverloadDclNode holding all candidates
FnDclNode *iNsTypeFindBestMethod(INode *binding, INode **self, Nodes *args) {
    INode **candidatep;
    uint32_t cnt = iNsTypeCandidates(&binding, &candidatep);

    // Look for best-fit method
    FnDclNode *bestmethod = NULL;
    int bestnbr = 0x7fffffff; // ridiculously high number    
    while (cnt--) {
        FnDclNode *methnode = (FnDclNode *)*candidatep++;
        int match;
        switch (match = fnSigMatchMethCall((FnSigNode *)methnode->vtype, self, args)) {
        case 0: continue;        // not an acceptable match
        case 1: return methnode;    // perfect match!
        default:                // imprecise match using conversions
            if (match < bestnbr) {
                // Remember this as best found so far
                bestnbr = match;
                bestmethod = methnode;
            }
        }
    }
    return bestmethod;
}

// Find the first pointer/reference method candidate that accepts the passed arguments
// 'binding' is the namespace's binding for the method's name.
// A unary method matches on arity alone. A binary method requires an acceptable
// second argument: pointer/reference parameters must be the same type as self,
// and any other parameter type must accept the coerced argument.
FnDclNode *iNsTypeFindPtrMethod(INode *binding, Nodes *args) {
    INode **candidatep;
    uint32_t cnt = iNsTypeCandidates(&binding, &candidatep);

    while (cnt--) {
        FnDclNode *methnode = (FnDclNode *)*candidatep++;
        Nodes *parms = ((FnSigNode *)methnode->vtype)->parms;
        if (parms->used != args->used)
            continue;
        // Unary method is an instant match
        // Binary methods need to ensure acceptable second argument
        if (args->used > 1) {
            INode *parm1type = iexpGetTypeDcl(nodesGet(parms, 1));
            INode *arg1type = iexpGetTypeDcl(nodesGet(args, 1));
            if (parm1type->tag == PtrTag || parm1type->tag == RefTag) {
                // When pointers are involved, we want to ensure they are the same type
                if (!itypeIsSame(arg1type, iexpGetTypeDcl(nodesGet(args, 0))))
                    continue;
            }
            else {
                if (!iexpCoerce(&nodesGet(args, 1), parm1type))
                    continue;
            }
        }
        return methnode;
    }
    return NULL;
}

// Find method whose method signature matches exactly (except for self)
// 'binding' is the namespace's binding for the method's name
// return NULL if none
FnDclNode *iNsTypeFindVrefMethod(INode *binding, FnDclNode *matchmeth) {
    INode **candidatep;
    uint32_t cnt = iNsTypeCandidates(&binding, &candidatep);

    // Look through all overloaded methods for a match
    while (cnt--) {
        FnDclNode *methnode = (FnDclNode *)*candidatep++;
        if (fnSigVrefEqual((FnSigNode*)methnode->vtype, (FnSigNode*)matchmeth->vtype))
            return methnode;
    }
    return NULL;
}
