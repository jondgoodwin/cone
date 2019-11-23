/** Handling for function/method calls
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>
#include <string.h>

// Create a function call node
FnCallNode *newFnCallNode(INode *fn, int nnodes) {
    FnCallNode *node;
    newNode(node, FnCallNode, FnCallTag);
    node->vtype = voidType;  // Will be overridden by return type
    node->objfn = fn;
    node->methfld = NULL;
    node->args = nnodes == 0? NULL : newNodes(nnodes);
    return node;
}

// Create new fncall node, prefilling method, self, and creating room for nnodes args
FnCallNode *newFnCallOpname(INode *obj, Name *opname, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    node->methfld = newMemberUseNode(opname);
    return node;
}

FnCallNode *newFnCallOp(INode *obj, char *op, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    node->methfld = newMemberUseNode(nametblFind(op, strlen(op)));
    return node;
}

// Serialize function call node
void fnCallPrint(FnCallNode *node) {
    INode **nodesp;
    uint32_t cnt;
    inodePrintNode(node->objfn);
    if (node->methfld) {
        inodeFprint(".");
        inodePrintNode((INode*)node->methfld);
    }
    if (node->args) {
        inodeFprint(node->tag==ArrIndexTag? "[" : "(");
        for (nodesFor(node->args, cnt, nodesp)) {
            inodePrintNode(*nodesp);
            if (cnt > 1)
                inodeFprint(", ");
        }
        inodeFprint(node->tag == ArrIndexTag ? "]" : ")");
    }
}

// Name resolution on 'fncall'
// - If node is indexing on a type, retag node as a typelit
// Note: this never name resolves .methfld, which is handled in type checking
void fnCallNameRes(NameResState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    INode **argsp;
    uint32_t cnt;

    // Name resolve objfn so we know what it is to vary subsequent processing
    inodeNameRes(pstate, &node->objfn);

    // TBD: objfn is a macro

    // If objfn is a type, handle it as a type literal
    if (isTypeNode(node->objfn)) {
        if (!node->flags & FlagIndex) {
            errorMsgNode(node->objfn, ErrorBadTerm, "May not do a function call on a type");
            return;
        }
        node->tag = TypeLitTag;
        node->vtype = node->objfn;
    }

    // Name resolve arguments/statements
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp))
            inodeNameRes(pstate, argsp);
    }
}

// For all function calls, go through all arguments to verify correct types,
// handle value copying, and fill in default arguments
void fnCallFinalizeArgs(FnCallNode *node) {
    INode *fnsig = iexpGetTypeDcl(node->objfn);
    INode **argsp;
    uint32_t cnt;
    INode **parmp = &nodesGet(((FnSigNode*)fnsig)->parms, 0);
    for (nodesFor(node->args, cnt, argsp)) {
        // Auto-convert string literal to borrowed reference
        INode *parmtype = iexpGetTypeDcl(*parmp);
        if ((*argsp)->tag == StringLitTag 
            && (parmtype->tag == ArrayRefTag || parmtype->tag == PtrTag)) {
            RefNode *addrtype = newRefNodeFull(voidType, newPermUseNode(immPerm), (INode*)u8Type);
            BorrowNode *borrownode = newBorrowNode();
            borrownode->vtype = (INode*)addrtype;
            borrownode->exp = *argsp;
            *argsp = (INode*)borrownode;
        }

        // Make sure the type matches (and coerce as needed)
        // (but not for vref as self)
        if (!iexpBiTypeInfer(&((IExpNode*)*parmp)->vtype, argsp) 
            && !(cnt == node->args->used && (node->flags & FlagVDisp)))
            errorMsgNode(*argsp, ErrorInvType, "Expression's type does not match declared parameter");
        parmp++;
    }

    // If we have too few arguments, use default values, if provided
    int argsunder = ((FnSigNode*)fnsig)->parms->used - node->args->used;
    if (argsunder > 0) {
        if (((VarDclNode*)*parmp)->value == NULL)
            errorMsgNode((INode*)node, ErrorFewArgs, "Function call requires more arguments than specified");
        else {
            while (argsunder--) {
                nodesAdd(&node->args, ((VarDclNode*)*parmp)->value);
                parmp++;
            }
        }
    }
}

Name *fnCallOpEqMethod(Name *opeqname) {
    if (opeqname == plusEqName) return plusName;
    if (opeqname == minusEqName) return minusName;
    if (opeqname == multEqName) return multName;
    if (opeqname == divEqName) return divName;
    if (opeqname == remEqName) return remName;
    if (opeqname == orEqName) return orName;
    if (opeqname == andEqName) return andName;
    if (opeqname == xorEqName) return xorName;
    if (opeqname == shlEqName) return shlName;
    if (opeqname == shrEqName) return shrName;
    return NULL;
}

// Find best field or method (across overloaded methods whose type matches argument types)
// Then lower the node to a function call (objfn+args) or field access (objfn+methfld) accordingly
void fnCallLowerMethod(FnCallNode *callnode) {
    INode *obj = callnode->objfn;
    Name *methsym = callnode->methfld->namesym;

    INode *methtype = iexpGetDerefTypeDcl(obj);
    if (!isMethodType(methtype)) {
        errorMsgNode((INode*)callnode, ErrorNoMeth, "Object's type does not support methods or fields.");
        return;
    }

    // Do lookup. If node found, it must be an instance's method or field
    if (methsym->namestr == '_'
        && !(obj->tag==VarNameUseTag && ((VarDclNode*)((NameUseNode*)obj)->dclnode)->namesym == selfName)) {
        errorMsgNode((INode*)callnode, ErrorNotPublic, "May not access the private method/field `%s`.", &methsym->namestr);
    }
    IExpNode *foundnode = (IExpNode*)iNsTypeFindFnField((INsTypeNode*)methtype, methsym);
    if (callnode->flags & FlagLvalOp) {
        if (foundnode)
            callnode->flags ^= FlagLvalOp;  // Turn off flag: found method handles the rest of it just fine
        else {
            foundnode = (IExpNode*)iNsTypeFindFnField((INsTypeNode*)methtype, fnCallOpEqMethod(methsym));
            // + cannot substitute for += unless it returns same type it takes
            FnSigNode *methsig = (FnSigNode *)foundnode->vtype;
            if (!itypeMatches(methsig->rettype, ((IExpNode*)nodesGet(methsig->parms, 0))->vtype)) {
                errorMsgNode((INode*)callnode, ErrorNoMeth, "Cannot find valid operator method that returns same type that it takes.");
            }
        }
    }
    if (!foundnode
        || !(foundnode->tag == FnDclTag || foundnode->tag == FieldDclTag)
        || !(foundnode->flags & FlagMethFld)) {
        errorMsgNode((INode*)callnode, ErrorNoMeth, "Object's type has no method or field named %s.", &methsym->namestr);
        callnode->vtype = methtype; // Pretend on a vtype
        return;
    }

    // Handle when methfld refers to a field
    if (foundnode->tag == FieldDclTag) {
        if (callnode->args != NULL)
            errorMsgNode((INode*)callnode, ErrorManyArgs, "May not provide arguments for a field access");

        derefAuto(&callnode->objfn);
        callnode->methfld->tag = VarNameUseTag;
        callnode->methfld->dclnode = (INode*)foundnode;
        callnode->vtype = callnode->methfld->vtype = foundnode->vtype;
        callnode->tag = FldAccessTag;
        return;
    }

    // For a method call, make sure object is specified as first argument
    if (callnode->args == NULL) {
        callnode->args = newNodes(1);
    }
    nodesInsert(&callnode->args, callnode->objfn, 0);

    FnDclNode *bestmethod = iNsTypeFindBestMethod((FnDclNode *)foundnode, callnode->args, callnode->flags & FlagVDisp);
    if (bestmethod == NULL) {
        errorMsgNode((INode*)callnode, ErrorNoMeth, "No method named %s matches the call's arguments.", &methsym->namestr);
        callnode->vtype = ((IExpNode*)obj)->vtype; // make up a vtype
        return;
    }

    // Do autoref or autoderef self, as necessary
    refAutoRef(&nodesGet(callnode->args, 0), ((IExpNode*)nodesGet(((FnSigNode*)bestmethod->vtype)->parms, 0))->vtype);

    // Re-purpose method's name use node into objfn, so name refers to found method
    NameUseNode *methodrefnode = callnode->methfld;
    methodrefnode->tag = VarNameUseTag;
    methodrefnode->dclnode = (INode*)bestmethod;
    methodrefnode->vtype = bestmethod->vtype;

    callnode->objfn = (INode*)methodrefnode;
    callnode->methfld = NULL;
    callnode->vtype = ((FnSigNode*)bestmethod->vtype)->rettype;

    // Handle copying of value arguments and default arguments
    fnCallFinalizeArgs(callnode);
}

// Find matching pointer or reference method (comparison or arithmetic)
// Lower the node to a function call (objfn+args)
void fnCallLowerPtrMethod(FnCallNode *callnode) {
    INode *obj = callnode->objfn;
    INode *objtype = iexpGetTypeDcl(obj);
    Name *methsym = callnode->methfld->namesym;

    // if 'self' is 'null', swap self and first argument (order is irrelevant for ==/!-)
    if (obj->tag == NullTag && callnode->args->used == 1) {
        if (methsym != eqName && methsym != neName) {
            errorMsgNode((INode*)callnode, ErrorNoMeth, "Method not supported for null, only equivalence.");
            callnode->vtype = (INode*)voidType; // make up a vtype
            return;
        }
        callnode->objfn = nodesGet(callnode->args, 0);
        nodesGet(callnode->args, 0) = obj;
        obj = callnode->objfn;
    }

    // Obtain the list of methods for the reference type
    INsTypeNode *methtype;
    switch (objtype->tag) {
    case PtrTag: methtype = ptrType; break;
    case RefTag: methtype = refType; break;
    case VirtRefTag: methtype = refType; break;
    case ArrayRefTag: methtype = arrayRefType; break;
    default: assert(0 && "Unknown reference type");
    }

    INode *foundnode = iNsTypeFindFnField(methtype, methsym);
    if (!foundnode) { // It can only be a method
        if (objtype->tag == RefTag || objtype->tag == VirtRefTag) {
            // Give references another crack at method via deref type's methods
            if (objtype->tag == VirtRefTag)
                callnode->flags |= FlagVDisp;
            fnCallLowerMethod(callnode);
            return;
        }
        errorMsgNode((INode*)callnode, ErrorNoMeth, "Object's type has no method named %s.", &methsym->namestr);
        callnode->vtype = (INode *)methtype; // Pretend on a vtype
        return;
    }

    // For a method call, make sure object is specified as first argument
    if (callnode->args == NULL) {
        callnode->args = newNodes(1);
    }
    nodesInsert(&callnode->args, callnode->objfn, 0);

    FnDclNode *bestmethod = NULL;
    Nodes *args = callnode->args;
    for (FnDclNode *methnode = (FnDclNode *)foundnode; methnode; methnode = methnode->nextnode) {
        Nodes *parms = ((FnSigNode *)methnode->vtype)->parms;
        if (parms->used != args->used)
            continue;
        // Unary method is an instant match
        // Binary methods need to ensure acceptable second argument
        if (args->used > 1) {
            INode *parm1type = iexpGetTypeDcl(nodesGet(parms, 1));
            INode *arg1type = iexpGetTypeDcl(nodesGet(args, 1));
            if (parm1type->tag == PtrTag || parm1type->tag == RefTag) {
                if (nodesGet(args, 1)->tag == NullTag && (methsym == eqName || methsym == neName))
                    ((IExpNode*)nodesGet(args, 1))->vtype = ((IExpNode*)nodesGet(args, 0))->vtype;
                // When pointers are involved, we want to ensure they are the same type
                else if (1 != itypeMatches(arg1type, iexpGetTypeDcl(nodesGet(args, 0))))
                    continue;
            }
            else {
                if (!iexpBiTypeInfer(&parm1type, &nodesGet(args, 1)))
                    continue;
            }
        }
        bestmethod = methnode;
        break;
    }
    if (bestmethod == NULL) {
        errorMsgNode((INode*)callnode, ErrorNoMeth, "No method's parameter types match the call's arguments.");
        callnode->vtype = ((IExpNode*)obj)->vtype; // make up a vtype
        return;
    }

    callnode->flags &= (uint16_t)0xFFFF - FlagLvalOp; // Ptrs implement +=,-=

    // Autoref self, as necessary
    INode **selfp = &nodesGet(callnode->args, 0);
    INode *selftype = iexpGetTypeDcl(*selfp);
    INode *totype = itypeGetTypeDcl(((IExpNode*)nodesGet(((FnSigNode*)bestmethod->vtype)->parms, 0))->vtype);
    if (selftype->tag != RefTag && totype->tag == RefTag) {
        RefNode *selfref = newRefNodeFull(voidType, newPermUseNode(mutPerm), selftype);
        borrowAuto(selfp, (INode *)selfref);
    }

    // Re-purpose method's name use node into objfn, so name refers to found method
    NameUseNode *methodrefnode = callnode->methfld;
    methodrefnode->tag = VarNameUseTag;
    methodrefnode->dclnode = (INode*)bestmethod;
    methodrefnode->vtype = bestmethod->vtype;

    callnode->objfn = (INode*)methodrefnode;
    callnode->methfld = NULL;
    callnode->vtype = ((FnSigNode*)bestmethod->vtype)->rettype;
    if (callnode->vtype->tag == PtrTag)
        callnode->vtype = selftype;  // Generic substitution for T
    return;
}

// Analyze function/method call node
// Type check significantly lowers the node's contents from its parsed structure
// to a type-resolved structure suitable for generation. The lowering involves
// resolving syntactic sugar and resolving a method to a function.
// It also distinguishes between methods and fields.
void fnCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;

    // Type check all of tree except methfld for now
    INode **argsp;
    uint32_t cnt;
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp))
            inodeTypeCheck(pstate, argsp);
    }
    inodeTypeCheck(pstate, &node->objfn);

    // If objfn is a method/field, rewrite it as self.method
    if (node->objfn->tag == VarNameUseTag
        && ((NameUseNode*)node->objfn)->dclnode->flags & FlagMethFld
        && ((NameUseNode*)node->objfn)->qualNames == NULL) {
        // Build a resolved 'self' node
        NameUseNode *selfnode = newNameUseNode(selfName);
        selfnode->tag = VarNameUseTag;
        selfnode->dclnode = nodesGet(pstate->fnsig->parms, 0);
        selfnode->vtype = ((VarDclNode*)selfnode->dclnode)->vtype;
        // Reuse existing fncallnode if we can
        if (node->methfld == NULL) {
            node->methfld = (NameUseNode *)node->objfn;
            node->objfn = (INode*)selfnode;
        }
        else {
            // Re-purpose objfn as self.method
            FnCallNode *fncall = newFnCallNode((INode *)selfnode, 0);
            fncall->methfld = (NameUseNode *)node->objfn;
            copyNodeLex(fncall, node->objfn); // Copy lexer info into injected node in case it has errors
            node->objfn = (INode*)fncall;
            inodeTypeCheck(pstate, &node->objfn);
        }
    }

    // How to lower depends on the type of the objfn and whether methfld is specified
    if (!isExpNode(node->objfn)) {
        errorMsgNode(node->objfn, ErrorNotTyped, "Expecting a typed value or expression");
        return;
    }
    INode *objfntype = iexpGetTypeDcl(node->objfn);
    // Since fncall supports auto-deref of objfn, let's get underlying type under a pointer or normal reference
    // (but not if fncall is part of a borrow or not indexing a pointer or slice)
    INode *objdereftype = objfntype;
    if (!(node->flags & FlagBorrow) 
        && !((node->flags & FlagIndex) && objfntype->tag == PtrTag))
        objdereftype = iexpGetDerefTypeDcl(node->objfn);

    // If object's deref-ed type supports methods, fill in a default method if unspecified: '()', '[]' or '&[]'
    if (isMethodType(objdereftype) && node->methfld == NULL)
        node->methfld = newNameUseNode(
            node->flags & FlagIndex ? (node->flags & FlagBorrow ? refIndexName : indexName) : parensName);

    // a) If method/field specified, handle it via name lookup in type and lower method call to function call
    if (node->methfld) {
        if (objfntype->tag == RefTag || objfntype->tag == VirtRefTag || objfntype->tag == PtrTag || objfntype->tag == ArrayRefTag)
            fnCallLowerPtrMethod(node); // Try ref/ptr specific methods first, otherwise will fallback to deref-ed method call
        else if (isMethodType(objdereftype))
            fnCallLowerMethod(node); // Lower to a field access or function call
        else {
            errorMsgNode((INode *)node, ErrorBadMeth, "Cannot do method or field on a value of this type");
        }
    }

    // b) Handle a regular function call
    else if (objdereftype->tag == FnSigTag) {
        derefAuto(&node->objfn);

        // Capture return vtype
        node->vtype = ((FnSigNode*)objdereftype)->rettype;

        // Error out if we have too many arguments
        int argsunder = ((FnSigNode*)objdereftype)->parms->used - node->args->used;
        if (argsunder < 0) {
            errorMsgNode((INode*)node, ErrorManyArgs, "Too many arguments specified vs. function declaration");
            return;
        }

        // Type check arguments, handling copy and default arguments along the way
        fnCallFinalizeArgs(node);
    }

    // c) Handle index/slice arguments on an array or array reference
    else if (objdereftype->tag == ArrayTag || objdereftype->tag == RefTag 
            || objdereftype->tag == ArrayRefTag || objdereftype->tag == PtrTag) {
        uint32_t nargs = node->args->used;
        if (nargs == 1) {
            INode **indexp = &nodesGet(node->args, 0);
            INode *indextype = iexpGetTypeDcl(*indexp);
            if (objdereftype->tag == PtrTag) {
                int match = 0;
                if (indextype->tag == UintNbrTag)
                    match = iexpBiTypeInfer((INode**)&usizeType, indexp);
                else if (indextype->tag == IntNbrTag)
                    match = iexpBiTypeInfer((INode**)&isizeType, indexp);
                if (!match)
                    errorMsgNode((INode *)node, ErrorBadIndex, "Pointer index must be an integer");
            }
            else {
                int match = 0;
                if (indextype->tag == UintNbrTag || (*indexp)->tag == ULitTag)
                    match = iexpBiTypeInfer((INode**)&usizeType, indexp);
                if (!match)
                    errorMsgNode((INode *)node, ErrorBadIndex, "Array index must be an unsigned integer");
            }
            switch (objdereftype->tag) {
            case ArrayTag:
                node->vtype = ((ArrayNode*)objdereftype)->elemtype;
                break;
            case RefTag:
            case ArrayRefTag:
                node->vtype = ((RefNode*)objdereftype)->pvtype;
                break;
            case PtrTag:
                node->vtype = ((PtrNode*)objdereftype)->pvtype;
                break;
            default:
                assert(0 && "Invalid type for indexing");
            }
            if (node->flags & FlagBorrow) {
                INode *objtype = iexpGetTypeDcl(node->objfn);
                assert(objtype->tag == RefTag || objtype->tag == ArrayRefTag);
                RefNode *refnode = newRefNodeFull(voidType, ((RefNode*)objtype)->perm, node->vtype);
                node->vtype = (INode*)refnode;
            }
            node->tag = ArrIndexTag;
        }
        else
            errorMsgNode((INode *)node, ErrorBadIndex, "Array indexing/slicing supports only 1-2 arguments");
    }

    else
        errorMsgNode((INode *)node, ErrorNotFn, "May not apply arguments to a value of this type");

}

// Do data flow analysis for fncall node (only real function calls)
void fnCallFlow(FlowState *fstate, FnCallNode **nodep) {
    // For += implemented via +, ensure self is a mutable lval
    if ((*nodep)->flags & FlagLvalOp) {
        uint16_t scope;
        INode *perm;
        INode *lval = assignLvalInfo(nodesGet((*nodep)->args, 0), &perm, &scope);
        if (!lval || !(MayWrite & permGetFlags(perm))) {
            errorMsgNode((INode*)*nodep, ErrorNoMut, "Can only operate on a valid and mutable lval to the left.");
        }
    }

    // Handle function call aliasing
    size_t svAliasPos = flowAliasPushNew(1); // Alias reference arguments
    FnCallNode *node = *nodep;
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(node->args, cnt, argsp)) {
        flowLoadValue(fstate, argsp);
        flowAliasReset();
    }
    flowAliasPop(svAliasPos);
}
