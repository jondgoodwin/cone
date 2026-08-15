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

// Bind a static function or method to its unique concrete name and, when it
// declares an overload name, to that name's separate FnOverloadDclNode.
// Two functions/methods declaring the same concrete name are a duplicate name error.
void iNsTypeAddFnDict(INsTypeNode *type, FnDclNode *fnnode) {
    if (namespaceAdd(&type->namespace, fnnode->namesym, (INode*)fnnode) != NULL)
        errorMsgNode((INode*)fnnode, ErrorDupName,
            "Duplicate name %s: every function/method needs its own name.", &fnnode->namesym->namestr);

    if (fnnode->overloadsym == NULL)
        return;

    // The overload name binds to its own node, holding every candidate declared for it
    INode *binding = namespaceFind(&type->namespace, fnnode->overloadsym);
    if (binding == NULL) {
        FnOverloadDclNode *overloadnode = newFnOverloadDclNode(fnnode->overloadsym);
        inodeLexCopy((INode*)overloadnode, (INode*)fnnode);
        fnOverloadDclAdd(overloadnode, fnnode);
        namespaceSet(&type->namespace, fnnode->overloadsym, (INode*)overloadnode);
        return;
    }
    if (binding->tag != FnOverloadDclTag) {
        errorMsgNode((INode*)fnnode, ErrorOverloadClash,
            "Overload name %s is already declared as something that is not an overload name.",
            &fnnode->overloadsym->namestr);
        return;
    }
    fnOverloadDclAdd((FnOverloadDclNode*)binding, fnnode);
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

// Find the one method candidate that accepts the call's receiver and arguments.
// Every candidate is tested, using a viability test that never alters the call.
// No candidate wins for being an exact rather than a coercible match, for needing
// fewer coercions, or for being declared first: the caller resolves an ambiguity
// by using a concrete name or by converting its arguments.
FnDclNode *iNsTypeFindMethod(INode *binding, INode **self, Nodes *args, enum OverloadMatch *status) {
    INode **candidatep;
    uint32_t cnt = iNsTypeCandidates(&binding, &candidatep);

    FnDclNode *found = NULL;
    *status = OverloadNone;
    while (cnt--) {
        FnDclNode *methnode = (FnDclNode *)*candidatep++;
        if (!fnSigViableCall((FnSigNode *)methnode->vtype, self, args))
            continue;
        if (found) {
            *status = OverloadAmbiguous;
            return NULL;
        }
        found = methnode;
        *status = OverloadUnique;
    }
    return found;
}

// Find the one pointer/reference method candidate that accepts the passed arguments.
// A unary method matches on arity alone. A binary method requires an acceptable
// second argument: pointer/reference parameters must be the same type as self,
// and any other parameter type must accept the argument through a permitted coercion.
// Nothing is inserted into the call: argument finalization happens after selection.
FnDclNode *iNsTypeFindPtrMethod(INode *binding, Nodes *args, enum OverloadMatch *status) {
    INode **candidatep;
    uint32_t cnt = iNsTypeCandidates(&binding, &candidatep);

    FnDclNode *found = NULL;
    *status = OverloadNone;
    while (cnt--) {
        FnDclNode *methnode = (FnDclNode *)*candidatep++;
        Nodes *parms = ((FnSigNode *)methnode->vtype)->parms;
        if (parms->used != args->used)
            continue;
        // Unary method matches on arity alone
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
                if (iexpMatches(&nodesGet(args, 1), parm1type, Coercion) == NoMatch)
                    continue;
            }
        }
        if (found) {
            *status = OverloadAmbiguous;
            return NULL;
        }
        found = methnode;
        *status = OverloadUnique;
    }
    return found;
}

// Find method whose method signature matches exactly (except for self)
// 'binding' is the namespace's binding for the method's name
// return NULL if none, or if more than one candidate matches
FnDclNode *iNsTypeFindVrefMethod(INode *binding, FnDclNode *matchmeth) {
    INode **candidatep;
    uint32_t cnt = iNsTypeCandidates(&binding, &candidatep);

    // Look through every candidate the name declares for the sole exact match
    FnDclNode *found = NULL;
    while (cnt--) {
        FnDclNode *methnode = (FnDclNode *)*candidatep++;
        if (!fnSigVrefEqual((FnSigNode*)methnode->vtype, (FnSigNode*)matchmeth->vtype))
            continue;
        if (found)
            return NULL;
        found = methnode;
    }
    return found;
}
