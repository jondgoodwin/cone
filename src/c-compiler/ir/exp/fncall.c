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
    node->vtype = unknownType;  // Will be overridden by return type
    node->objfn = fn;
    node->methfld = NULL;
    node->args = nnodes == 0? NULL : newNodes(nnodes);
    return node;
}

// Create new fncall node, prefilling method, self, and creating room for nnodes args
// These three are the only way an operator application is built, so each marks the
// node FlagOperator: a member access by name builds the same shape and must stay
// distinguishable from it.
FnCallNode *newFnCallOpname(INode *obj, Name *opname, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    node->flags |= FlagOperator;
    node->methfld = (INode*)newMemberUseNode(opname);
    return node;
}

FnCallNode *newFnCallOp(INode *obj, char *op, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    node->flags |= FlagOperator;
    node->methfld = (INode*)newMemberUseNode(nametblFind(op, strlen(op)));
    return node;
}

FnCallNode *newFnCallOpnameLower(INode *oldnode, INode *obj, Name *opname, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    node->flags |= FlagOperator;
    inodeLexCopy((INode*)node, oldnode);
    node->methfld = (INode*)newMemberUseNode(opname);
    inodeLexCopy((INode*)node->methfld, oldnode);
    return node;
}

FnCallNode *newFnCallLower(INode *oldnode, INode *obj, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    inodeLexCopy((INode*)node, oldnode);
    return node;
}

// Clone fncall
INode *cloneFnCallNode(CloneState *cstate, FnCallNode *node) {
    FnCallNode *newnode;
    newnode = memAllocBlk(sizeof(FnCallNode));
    memcpy(newnode, node, sizeof(FnCallNode));
    newnode->objfn = cloneNode(cstate, node->objfn);
    if (node->args)
        newnode->args = cloneNodes(cstate, node->args);
    newnode->methfld = cloneNode(cstate, node->methfld);
    return (INode *)newnode;
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

    // Name resolve arguments/statements
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp))
            inodeNameRes(pstate, argsp);
    }

    // Lower "<-" (append) on a vtuple to a block that appends each tuple element separately
    if (node->methfld && ((NameUseNode *)node->methfld)->namesym == lessDashName
        && node->args > 0 && nodesGet(node->args, 0)->tag == VTupleTag) {

        // Create block and start it with a variable that mutably borrows address of append receiver
        INode *lval = node->objfn;
        BlockNode *blk = newBlockNode();
        inodeLexCopy((INode*)blk, (INode*)node);
        borrowMutRef(&lval, unknownType, (INode*)mutPerm);
        INode *lvalvar = newNameUseAndDcl(&blk->stmts, lval, pstate->scope + 1);

        // Use dereferenced name as receiver for sequence of appends
        StarNode *starlval = newStarNode(DerefTag);
        starlval->vtexp = lvalvar;

        // Now create sequence of appends, one for each element of tuple
        INode **nodesp;
        uint32_t cnt;
        TupleNode *tuple = (TupleNode *)nodesGet(node->args, 0);
        for (nodesFor(tuple->elems, cnt, nodesp)) {
            if (cnt == tuple->elems->used) {
                node->objfn = (INode*)starlval;
                nodesGet(node->args, 0) = *nodesp;
            }
            else {
                node = newFnCallOpnameLower((INode*)*nodep, (INode*)starlval, lessDashName, 2);
                node->flags |= FlagOpAssgn | FlagLvalOp;
                nodesAdd(&node->args, *nodesp);
            }
            nodesAdd(&blk->stmts, (INode*)node);
        }
        *nodep = (FnCallNode*)blk;  // Replace fncall with constructed block (casting badly to satisfy type check)
        return;
    }
}

// We have an object that is an array, arrayref, ptr, or reference to an array
// We won't arrive here if a method or field was specified
// We can indexing into array or borrow reference to the indexed element
void fnCallArrIndex(FnCallNode *node) {
    if (!(node->flags & FlagIndex)) {
        errorMsgNode((INode *)node, ErrorBadIndex, "Indexing not supported on a value of this type.");
        return;
    }

    // Correct number of indices?
    INode *objtype = iexpGetTypeDcl(node->objfn);
    uint32_t nexpected = objtype->tag == ArrayTag ? ((ArrayNode*)objtype)->dimens->used : 1;
    uint32_t nargs = node->args? node->args->used : 0;
    if (nargs != nexpected) {
        errorMsgNode((INode *)node, ErrorBadIndex, "Incorrect number of indexing arguments");
        return;
    }
    // Ensure all indices are integers
    INode **indexp;
    uint32_t cnt;
    for (nodesFor(node->args, cnt, indexp)) {
        INode *indextype = iexpGetTypeDcl(*indexp);
        if (objtype->tag == PtrTag) {
            // Pointer supports signed or unsigned integer index
            int match = NoMatch;
            if (indextype->tag == UintNbrTag)
                match = iexpCoerce(indexp, (INode*)usizeType);
            else if (indextype->tag == IntNbrTag)
                match = iexpCoerce(indexp, (INode*)isizeType);
            if (!match)
                errorMsgNode((INode *)node, ErrorBadIndex, "Pointer index must be an integer");
        }
        else {
            // All other array types only support unsigned (positive) integer indexing
            int match = NoMatch;
            if (indextype->tag == UintNbrTag || (*indexp)->tag == ULitTag)
                match = iexpCoerce(indexp, (INode*)usizeType);
            if (!match)
                errorMsgNode((INode *)node, ErrorBadIndex, "Array index must be an unsigned integer");
        }
    }

    // Capture the element type returned
    switch (objtype->tag) {
    case ArrayTag:
        node->vtype = arrayElemType(objtype);
        break;
    case RefTag: {
        INode *vtype = ((RefNode *)objtype)->vtexp;
        if (vtype->tag == ArrayTag)
            node->vtype = arrayElemType(vtype);
        else if (vtype->tag == ArrayDerefTag)
            node->vtype = ((RefNode*)vtype)->vtexp;
        else
            assert(0 && "Illegal type to index a reference to");
        break;
    }
    case ArrayRefTag:
        node->vtype = ((RefNode*)objtype)->vtexp;
        break;
    case PtrTag:
        node->vtype = ((StarNode*)objtype)->vtexp;
        break;
    default:
        assert(0 && "Invalid type for indexing");
    }

    // If we are borrowing a reference to indexed element, fix up type
    if (node->flags & FlagBorrow) {
        assert(objtype->tag == RefTag || objtype->tag == ArrayRefTag);
        RefNode *refnode = newRefNodeFull(RefTag, (INode*)node, borrowRef, ((RefNode*)objtype)->perm, node->vtype);
        // An element of what a borrow points at lives exactly as long as the borrow
        // does, so it inherits its lifetime. borrowTypeCheck sets the scope on the
        // receiver it built; newRefNode defaults to 0, which means global, so
        // leaving it would let '&mut a[1]' on a local be returned from a function
        // while '&mut (p.x)' on the same local is refused.
        refnode->scope = ((RefNode*)objtype)->scope;
        node->vtype = (INode*)refnode;
    }
    node->tag = ArrIndexTag;
}

// At this point, we have a properly-lowered function call. objfn could be:
// - nameuse to a function dcl
// - an indirect ref/ptr to a function
// - a de-reffed ref/ptr to a function
// From the function's signature, we want to pick up the return type
// and ensure that all arguments are specified and coerced to the right types
void fnCallFinalizeArgs(FnCallNode *node) {
    FnSigNode *fnsig = (FnSigNode*)iexpGetDerefTypeDcl(node->objfn);
    assert(fnsig->tag == FnSigTag);

    // Establish the return type of the function call (or error if not what was expected)
    if (node->vtype != unknownType && !itypeIsSame(fnsig->rettype, node->vtype)) {
        errorMsgNode((INode*)node, ErrorNoMeth, "Type of call's returned value does not match what is expected");
    }
    node->vtype = fnsig->rettype;

    // Ensure we have enough arguments, based on how many expected
    int argsunder = fnsig->parms->used - node->args->used;
    if (argsunder < 0) {
        errorMsgNode((INode*)node, ErrorManyArgs, "Too many arguments specified vs. function declaration");
        return;
    }

    // Coerce provided arguments to expected types
    INode **argsp;
    uint32_t cnt;
    INode **parmp = &nodesGet(fnsig->parms, 0);
    for (nodesFor(node->args, cnt, argsp)) {
        // Make sure the type matches (and coerce as needed)
        // (but not for vref as self)
        if (!iexpCoerce(argsp, ((IExpNode*)*parmp)->vtype)
            && !(cnt == node->args->used && (node->flags & FlagVDisp)))
            errorMsgNode(*argsp, ErrorInvType, "Expression's type does not match declared parameter");
        parmp++;
    }

    // If we have too few arguments, use default values, if provided
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

// objfn is a function or a pointer to one. Make sure it is called correctly.
void fnCallFnSigTypeCheck(TypeCheckState *pstate, FnCallNode *node) {
    if ((node->flags & FlagIndex) || node->methfld != NULL) {
        errorMsgNode((INode*)node->objfn, ErrorNoMeth, "A function may not be called using indexing or a method.");
        return;
    }
    fnCallFinalizeArgs(node);
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

// Lower integer field index for tuple
int fnCallLowerIntField(FnCallNode *callnode) {
    if (callnode->methfld == NULL || callnode->methfld->tag != ULitTag || callnode->args != NULL)
        return 0;
    TupleNode* ttuple = (TupleNode*)((IExpNode*)callnode->objfn)->vtype;
    uint64_t index = ((ULitNode*)callnode->methfld)->uintlit;
    if (index >= (uint64_t)ttuple->elems->used)
        return 0;
    callnode->vtype = nodesGet(ttuple->elems, index);
    callnode->tag = FldAccessTag;
    return 1;
}

// Report why the name the caller used selected no single candidate.
// 'kind' names what the name declares, for a call ("function") or a method call ("method").
static void fnCallNoCandidate(INode *callnode, enum OverloadMatch status, Name *namesym, char *kind) {
    if (status == OverloadAmbiguous)
        errorMsgNode(callnode, ErrorAmbigCandidate,
            "More than one %s declared by `%s` accepts these arguments. Call a concrete name or convert the arguments.",
            kind, &namesym->namestr);
    else
        errorMsgNode(callnode, ErrorNoCandidate,
            "No %s declared by `%s` accepts the call's arguments.", kind, &namesym->namestr);
}

// Find the one field or method that accepts the call's receiver and arguments,
// then lower the node to a function call (objfn+args) or field access (objfn+methfld).
// A receiver held through a reference or pointer is dereferenced where the selected
// method declared 'self' by value; nothing is borrowed on the receiver's behalf, and
// an operator written on a pointer does not reach through at all.
// Returns 1 when lowered, 0 when the receiver's type supports no methods at all
// (so the caller may try another way), and -1 when a diagnostic was reported.
int fnCallLowerMethod(FnCallNode *callnode) {
    INode *obj = callnode->objfn;
    assert(callnode->methfld->tag == MbrNameUseTag);
    NameUseNode *methfld = (NameUseNode*)callnode->methfld;
    Name *methsym = methfld->namesym;

    INode *objdereftype = iexpGetDerefTypeDcl(obj);
    if (!isMethodType(objdereftype)) {
        return 0;
    }

    // Visibility is checked against the spelling the caller actually used.
    // A public overload name may therefore select a private concrete candidate.
    if (methsym->namestr == '_'
        && !(obj->tag==VarNameUseTag && ((VarDclNode*)((NameUseNode*)obj)->dclnode)->namesym == selfName)) {
        errorMsgNode((INode*)callnode, ErrorNotPublic, "May not access the private method/field `%s`.", &methsym->namestr);
    }
    INode *foundnode = iNsTypeFindFnField((INsTypeNode*)objdereftype, methsym);
    if (!foundnode
        || !(foundnode->tag == FnDclTag || foundnode->tag == FnOverloadDclTag || foundnode->tag == FieldDclTag)
        || !(foundnode->flags & FlagMethFld)) {
        errorMsgNode((INode*)callnode, ErrorNoMbr, "Method or field `%s` not found.", &methsym->namestr);
        return -1;
    }

    // Handle when methfld refers to a field
    if (foundnode->tag == FieldDclTag) {
        if (callnode->args != NULL)
            errorMsgNode((INode*)callnode, ErrorManyArgs, "May not provide arguments for a field access");

        derefInject(&callnode->objfn);  // automatically deref any reference/ptr, if needed
        methfld->tag = MbrNameUseTag;
        methfld->dclnode = foundnode;
        callnode->vtype = methfld->vtype = ((IExpNode*)foundnode)->vtype;
        callnode->tag = FldAccessTag;
        return 1;
    }

    // Test every candidate the name declares, without altering the call
    enum OverloadMatch status;
    FnDclNode *selected = iNsTypeFindMethod(foundnode, &callnode->objfn, callnode->args, &status);

    // A receiver held through a reference or a pointer still satisfies a method that
    // declared 'self' by value, so dereference it and select again. That is a load and
    // a copy -- the same adjustment a field access already makes -- and nothing more:
    // no reference is manufactured, so a method wanting 'self &' or 'self &mut' stays
    // out of the reach of a value or a pointer. The retry runs only when no candidate
    // matched at all, so an ambiguity among the candidates the receiver already fits is
    // reported as such, and a set holding both a value and a reference candidate still
    // selects the reference one for a reference receiver.
    //
    // An operator written on a pointer is the one thing the retry does not reach.
    // An operation on a pointer is an operation on the pointer, and the dereference
    // has to be written, because the alternative is two lines that look alike doing
    // different things: a pointer declares its own '+', so 'p + 2' offsets it, while
    // it declares no '*', so 'p * 2' would quietly become '(*p) * 2'. It is an error
    // again, as it was in C. Only a pointer narrows. A reference's comparisons are
    // its own identity operators, which refType declares and fnCallLowerPtrMethod
    // selects before ever arriving here, and its arithmetic reaching the value's is
    // by design.
    int opOnPointer = (callnode->flags & FlagOperator) && iexpGetTypeDcl(obj)->tag == PtrTag;
    if (selected == NULL && status == OverloadNone && !opOnPointer && derefInject(&callnode->objfn)) {
        selected = iNsTypeFindMethod(foundnode, &callnode->objfn, callnode->args, &status);
        if (selected == NULL)
            callnode->objfn = obj;  // a failed retry leaves the call as it was found
    }

    if (selected == NULL) {
        fnCallNoCandidate((INode*)callnode, status, methsym, "method");
        return -1;
    }

    // For a method call, make sure object is specified as first argument
    if (callnode->args == NULL) {
        callnode->args = newNodes(1);
    }
    nodesInsert(&callnode->args, callnode->objfn, 0);

    // Re-purpose method's name use node into objfn, so name refers to selected method
    NameUseNode *methodrefnode = (NameUseNode*)callnode->methfld;
    methodrefnode->tag = VarNameUseTag;
    methodrefnode->namesym = selected->namesym;
    methodrefnode->dclnode = (INode*)selected;
    methodrefnode->vtype = selected->vtype;

    callnode->objfn = (INode*)methodrefnode;
    callnode->methfld = NULL;
    callnode->vtype = ((FnSigNode*)selected->vtype)->rettype;

    // Handle copying of value arguments and default arguments
    fnCallFinalizeArgs(callnode);
    return 1;
}

// We have a reference or pointer, and a method to find (comparison or arithmetic)
// If found, lower the node to a function call (objfn+args)
// Otherwise try again against the type it points to
int fnCallLowerPtrMethod(FnCallNode *callnode, INsTypeNode *methtype) {
    INode *obj = callnode->objfn;
    INode *objtype = iexpGetTypeDcl(obj);
    assert(callnode->methfld->tag == MbrNameUseTag);
    NameUseNode *methfld = (NameUseNode*)callnode->methfld;
    Name *methsym = methfld->namesym;

    INode *foundnode = iNsTypeFindFnField(methtype, methsym);
    if (!foundnode)
        return 0;

    // For a method call, make sure object is specified as first argument
    if (callnode->args == NULL) {
        callnode->args = newNodes(1);
    }
    nodesInsert(&callnode->args, callnode->objfn, 0);

    enum OverloadMatch status;
    FnDclNode *selected = iNsTypeFindPtrMethod(foundnode, callnode->args, &status);
    if (selected == NULL) {
        fnCallNoCandidate((INode*)callnode, status, methsym, "method");
        callnode->vtype = ((IExpNode*)obj)->vtype; // make up a vtype
        return 1;
    }

    // Re-purpose method's name use node into objfn, so name refers to selected method
    INode **selfp = &nodesGet(callnode->args, 0);
    INode *selftype = iexpGetTypeDcl(*selfp);
    NameUseNode *methodrefnode = (NameUseNode*)callnode->methfld;
    methodrefnode->tag = VarNameUseTag;
    methodrefnode->namesym = selected->namesym;
    methodrefnode->dclnode = (INode*)selected;
    methodrefnode->vtype = selected->vtype;
    callnode->objfn = (INode*)methodrefnode;
    callnode->methfld = NULL;

    // Now that exactly one candidate is selected, coerce the argument once.
    // These compiler-declared signatures are generic over the pointer/reference's
    // value type, so only a concretely typed parameter takes part in coercion.
    Nodes *parms = ((FnSigNode *)selected->vtype)->parms;
    if (parms->used > 1) {
        INode *parm1type = iexpGetTypeDcl(nodesGet(parms, 1));
        if (parm1type->tag != PtrTag && parm1type->tag != RefTag && parm1type->tag != ArrayRefTag
            && !iexpCoerce(&nodesGet(callnode->args, 1), parm1type))
            errorMsgNode(nodesGet(callnode->args, 1), ErrorInvType,
                "Expression's type does not match declared parameter");
    }

    callnode->vtype = ((FnSigNode*)selected->vtype)->rettype;
    if (callnode->vtype->tag == PtrTag) {
        INode *t_type = selftype->tag == RefTag? ((RefNode *)selftype)->vtexp : selftype;
        callnode->vtype = t_type;  // Generic substitution for T
    }
    return 1;
}

// objfn names an overload set. Select the one candidate that accepts the call's
// arguments, rewrite the call to that concrete function, then finalize its arguments.
void fnCallLowerOverloadFn(FnCallNode *node) {
    NameUseNode *fnuse = (NameUseNode*)node->objfn;
    FnOverloadDclNode *overloadnode = (FnOverloadDclNode*)fnuse->dclnode;

    if ((node->flags & FlagIndex) || node->methfld != NULL) {
        errorMsgNode((INode*)node->objfn, ErrorNoMeth, "A function may not be called using indexing or a method.");
        return;
    }

    // Test every candidate the overload name declares, without altering the call
    enum OverloadMatch status;
    FnDclNode *selected = iNsTypeFindMethod((INode*)overloadnode, NULL, node->args, &status);
    if (selected == NULL) {
        fnCallNoCandidate((INode*)node, status, overloadnode->namesym, "function");
        return;
    }

    // Rewrite the callee to the selected concrete declaration, so nothing downstream
    // ever sees the overload node, then insert coercions and defaults exactly once
    fnuse->namesym = selected->namesym;
    fnuse->dclnode = (INode*)selected;
    fnuse->vtype = selected->vtype;
    fnCallFinalizeArgs(node);
}

// Lower opassign method for method-based types
void fnCallOpAssgn(FnCallNode **nodep) {
    FnCallNode *callnode = *nodep;
    INode *objtype = iexpGetTypeDcl(callnode->objfn);
    assert(callnode->methfld->tag == MbrNameUseTag);
    NameUseNode *methfld = (NameUseNode*)callnode->methfld;
    Name *methsym = methfld->namesym;

    // Change first argument to &mut obj
    borrowMutRef(&callnode->objfn, objtype, newPermUseNode(mutPerm));

    // Lower to op-assign, if method supported by type
    if (iNsTypeFindFnField((INsTypeNode*)objtype, methsym)) {
        fnCallLowerMethod(callnode);
        return;
    }

    // Let's try rewriting to: {imm tmp = lval; *tmp = *tmp + expr}
    VarDclNode *tmpvar = newVarDclFull(tempName, VarDclTag, ((IExpNode*)callnode->objfn)->vtype, (INode*)immPerm, callnode->objfn);
    NameUseNode *tmpname = newNameUseNode(tempName);
    tmpname->vtype = tmpvar->vtype;
    tmpname->tag = VarNameUseTag;
    tmpname->dclnode = (INode *)tmpvar;
    INode *derefvar = (INode *)tmpname;
    derefInject(&derefvar);
    callnode->objfn = derefvar;
    methfld->namesym = fnCallOpEqMethod(methsym);
    if (fnCallLowerMethod(callnode) == 0) {
        errorMsgNode((INode*)callnode, ErrorNoMeth,
            "No method/field named %s found that matches the call's arguments.",
            &methsym->namestr);
        return;
    }
    INode *dereflval = (INode *)tmpname;
    derefInject(&dereflval);
    AssignNode *tmpassgn = newAssignNode(NormalAssign, dereflval, (INode*)callnode);
    BlockNode *blk = newBlockNode();
    blk->vtype = callnode->vtype;
    nodesAdd(&blk->stmts, (INode*)tmpvar);
    nodesAdd(&blk->stmts, (INode*)tmpassgn);
    *((INode**)nodep) = (INode*)blk;
}

// Perform type check on function/method call node
// This should only be run once on a node, as it mutably lowers the node to another form:
// - If a generic/macro, it instantiates, then type checks instantiated nodes
// - If a type literal, it dispatches it to typelit for handlings
// - If a field access, it turns it into a FldAccess node
// - If an array index, it turns it into an ArrIndex node
// - A method call is resolved by lookup and lowered to a function call
// - A function call coerces and injects arguments as needed
void fnCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;

    // If we have a true macro, go handle it elsewhere
    // Note: Macros don't want us to type check arguments until after substitution
    //
    // Only when the macro name is what is being called. An operator or method
    // application builds the same node shape as a call -- 'TWO + 1' is
    // objfn 'TWO', methfld '+', one argument -- so methfld is what tells the two
    // apart. With it set, the name is the receiver a value is expected of, and
    // it expands below like the name in any other value position; the operator
    // is then applied to what it expanded to.
    if (node->objfn->tag == MacroNameTag && node->methfld == NULL) {
        macroCallTypeCheck(pstate, nodep);
        return;
    }

    // Type check arguments (methfld is handled later)
    int usesTypeArgs = 0;
    INode **argsp;
    uint32_t cnt;
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp)) {
            inodeTypeCheckAny(pstate, argsp);
            if (isTypeNode(*argsp))
                usesTypeArgs = 1;
        }
    }

    // Perform generic substitution (if requested) and quit if that finishes processing
    if (genericSubstitute(pstate, nodep))
        return;

    // An overload name has no value of its own, so it is only legal here, naming what
    // is called. Skipping the ordinary name-use check leaves that check free to reject
    // the overload name everywhere else.
    int calleeIsOverload = node->objfn->tag == VarNameUseTag
        && ((NameUseNode*)node->objfn)->dclnode->tag == FnOverloadDclTag;
    if (!calleeIsOverload)
        inodeTypeCheckAny(pstate, &node->objfn);

    // A callee already reported as bad -- a generic that could not be
    // instantiated, say -- leaves nothing to call. The call inherits the mark
    // rather than earning a second diagnostic saying its callee is not callable.
    if (inodeIsError(node->objfn)) {
        node->vtype = errorType;
        return;
    }

    // All arguments must now be expressions
    int badarg = 0;
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp))
            if (!isExpNode(*argsp)) {
                errorMsgNode(*argsp, ErrorNotTyped, "Expected a typed expression.");
                badarg = 1;
            }
    }
    if (badarg) {
        node->vtype = errorType;
        return;
    }

    // If objfn is a type, handle it as a constructor or initializer
    if (isTypeNode(node->objfn)) {
        // Handle type constructor, e.g.:  Point[1., 2.]
        if (node->flags & FlagIndex) {
            node->tag = TypeLitTag;
            node->vtype = node->objfn;
            typeLitTypeCheck(pstate, *nodep);
            return;
        }
        if (node->methfld != NULL || node->objfn->tag != TypeNameUseTag) {
            errorMsgNode(node->objfn, ErrorBadTerm, "May not do a function call on a type");
            node->vtype = errorType;
            return;
        }

        // Initializer:  Change nameuse to refer to type's 'init' function
        NameUseNode *nameuse = (NameUseNode *)node->objfn;
        //nameUseAddQual(nameuse, nameuse->namesym);
        nameuse->namesym = initMethodName;
        Namespace *namespace = &((StructNode*)nameuse->dclnode)->namespace;
        nameuse->dclnode = namespaceFind(namespace, nameuse->namesym);
        if (nameuse->dclnode == NULL || nameuse->dclnode->tag != FnDclTag) {
            errorMsgNode(node->objfn, ErrorBadTerm, "Does not refer to a valid type initializer");
            node->vtype = errorType;
            return;
        }
        nameuse->tag = VarNameUseTag;
        nameuse->vtype = ((FnDclNode*)nameuse->dclnode)->vtype;
    }
    
    if (!isExpNode(node->objfn)) {
        errorMsgNode(node->objfn, ErrorNotTyped, "Expected a typed expression.");
        node->vtype = errorType;
        return;
    }

    // If objfn is the name of a method/field, rewrite to: self.method
    if (node->objfn->tag == VarNameUseTag
        && ((NameUseNode*)node->objfn)->dclnode->flags & FlagMethFld
        && ((NameUseNode*)node->objfn)->qualNames == NULL) {
        // Build a resolved 'self' node
        NameUseNode *selfnode = newNameUseNode(selfName);
        selfnode->tag = VarNameUseTag;
        selfnode->dclnode = nodesGet(((FnSigNode*)pstate->fn->vtype)->parms, 0);
        selfnode->vtype = ((VarDclNode*)selfnode->dclnode)->vtype;
        // Reuse existing fncallnode if we can
        if (node->methfld == NULL) {
            node->methfld = node->objfn;
            node->methfld->tag = MbrNameUseTag;
            node->objfn = (INode*)selfnode;
        }
        else {
            // Re-purpose objfn as self.method
            FnCallNode *fncall = newFnCallNode((INode *)selfnode, 0);
            fncall->methfld = node->objfn;
            fncall->methfld->tag = MbrNameUseTag;
            copyNodeLex(fncall, node->objfn); // Copy lexer info into injected node in case it has errors
            node->objfn = (INode*)fncall;
            inodeTypeCheckAny(pstate, &node->objfn);
        }
    }

    // A call whose callee names an overload set selects its one viable candidate
    if (node->objfn->tag == VarNameUseTag
        && ((NameUseNode*)node->objfn)->dclnode->tag == FnOverloadDclTag) {
        fnCallLowerOverloadFn(node);
        return;
    }

    // Handle when method operator requires an lval
    // This is true for ++, --, <- and operator-equals (+=)
    INode *objtype = iexpGetTypeDcl(node->objfn);
    if (node->flags & FlagLvalOp) {
        // Lower opassign for method-based types with extra logic
        if (isMethodType(objtype) && (node->flags & FlagOpAssgn)) {
            fnCallOpAssgn(nodep);
            return;
        }

        // Turn objfn into &mut objfn
        borrowMutRef(&node->objfn, objtype, newPermUseNode(mutPerm));
        objtype = iexpGetTypeDcl(node->objfn);
    }

    // Dispatch for correct handling based on the type of the object
    switch (objtype->tag) {
    // Pure function call
    case FnSigTag:
        fnCallFnSigTypeCheck(pstate, node); break;

    // Types expecting method call or field access
    case StructTag:
    case IntNbrTag:
    case UintNbrTag:
    case FloatNbrTag:
        // Fill in empty methfld with '()', '[]' or '&[]' based on parser flags
        if (node->methfld == NULL)
            node->methfld = (INode*)newMemberUseNode(
                node->flags & FlagIndex ? (node->flags & FlagBorrow ? refIndexName : indexName) : parensName);
        // Lower to a field access or function call
        if (fnCallLowerMethod(node) == 0) {
            errorMsgNode((INode*)node, ErrorNoMeth,
                "No method/field named %s found that matches the call's arguments.",
                &((NameUseNode*)node->methfld)->namesym->namestr);
        }
        break;

    // Tuple type
    case TTupleTag:
        if (fnCallLowerIntField(node) == 0)
            errorMsgNode((INode*)node, ErrorNoMeth, "Invalid expression on a tuple");
        break;

    // Array type
    case ArrayTag:
        if (node->flags & FlagIndex)
            fnCallArrIndex(node);  // indexing or borrowed ref to index
        else
            errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on an array.");
        break;

    // Array reference
    case ArrayRefTag:
        if (node->flags & FlagIndex)
            fnCallArrIndex(node);
        else if (node->methfld && fnCallLowerPtrMethod(node, arrayRefType))
            ;
        else
            errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on an array ref.");
        break;

    // Regular reference
    case RefTag: {
        INode *objdereftype = itypeGetTypeDcl(((RefNode *)objtype)->vtexp);

        // Handle calling a function-by-ref (only callable using parens)
        if (objdereftype->tag == FnSigTag && !node->methfld) {
            if ((node->flags & FlagIndex))
                errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on a function reference.");
            else
                fnCallFnSigTypeCheck(pstate, node);
        }

        // Handle indexing an array
        else if ((node->flags & FlagIndex) && (objdereftype->tag == ArrayTag || objdereftype->tag == ArrayDerefTag))
            fnCallArrIndex(node);

        // Handle method call to some other type
        else {
            // Fill in empty methfld with '()', '[]' or '&[]' based on parser flags
            if (node->methfld == NULL) {
                Name *methname = node->flags & FlagIndex ? (node->flags & FlagBorrow ? refIndexName : indexName) : parensName;
                node->methfld = (INode*)newMemberUseNode(methname);
            }
            if (fnCallLowerPtrMethod(node, refType) == 0) {
                // Lower to a field access or function call, dereferencing the receiver
                // where that is what the selected method wants. fnCallLowerMethod cannot
                // answer 0 here, having already been told the deref type supports methods.
                if (isMethodType(objdereftype))
                    fnCallLowerMethod(node);
                else if (objdereftype->tag == PtrTag)
                    fnCallLowerPtrMethod(node, ptrType);
                else
                    errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on a reference.");
            }
        }
        break;
    }

    // Virtual reference
    case VirtRefTag: {
        if (node->methfld) {
            if (fnCallLowerPtrMethod(node, refType) == 0) {
                node->flags |= FlagVDisp;
                fnCallLowerMethod(node);
            }
        }
        else
            errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on a virtual reference.");
        break;
    }

    // Pointer type
    case PtrTag: {
        INode *objdereftype = ((StarNode *)objtype)->vtexp;
        if (node->flags & FlagIndex)
            fnCallArrIndex(node);
        else if (node->methfld) {
            // A pointer's own operators first, then the value type's fields and named
            // methods, reaching a value receiver by dereferencing the pointer. An
            // operator the pointer does not declare stops here rather than reaching
            // the value's; fnCallLowerMethod is what refuses it.
            if (fnCallLowerPtrMethod(node, ptrType) == 0 && fnCallLowerMethod(node) == 0)
                errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on a pointer.");
        }
        else if (objdereftype->tag == FnSigTag)
            fnCallFnSigTypeCheck(pstate, node);
        else
            errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on a pointer.");
        break;
    }

    default:
        errorMsgNode((INode*)node->objfn, ErrorNoMeth, "This type does not support calls or field access.");
        node->vtype = errorType;
    }
}

// Do data flow analysis for fncall node (only real function calls)
void fnCallFlow(FlowState *fstate, FnCallNode **nodep) {
    // Handle function call aliasing
    FnCallNode *node = *nodep;
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(node->args, cnt, argsp)) {
        flowLoadValue(fstate, argsp);
        flowHandleMoveOrCopy(argsp);  // Argument values are moved or copied
    }
}

// Perform data flow analysis on array index node
void fnCallArrIndexFlow(FlowState *fstate, FnCallNode **node) {
    flowLoadValue(fstate, &(*node)->objfn);
    flowLoadValue(fstate, &nodesGet((*node)->args, 0));
}

// Perform data flow analysis on field access node
void fnCallFldAccessFlow(FlowState *fstate, FnCallNode **node) {
    flowLoadValue(fstate, &(*node)->objfn);
}

