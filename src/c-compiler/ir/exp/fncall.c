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
	node->objfn = fn;
    node->methprop = NULL;
	node->args = nnodes == 0? NULL : newNodes(nnodes);
	return node;
}

FnCallNode *newFnCallOp(INode *obj, char *op, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    node->methprop = newMemberUseNode(nametblFind(op, strlen(op)));
    return node;
}

// Serialize function call node
void fnCallPrint(FnCallNode *node) {
    INode **nodesp;
    uint32_t cnt;
    inodePrintNode(node->objfn);
    if (node->methprop) {
        inodeFprint(".");
        inodePrintNode((INode*)node->methprop);
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
        if ((*argsp)->tag == StrLitTag && (parmtype->tag == RefTag || parmtype->tag == PtrTag)) {
            RefNode *addrtype = newRefNode();
            addrtype->perm = (INode*)immPerm;
            addrtype->alloc = voidType;
            addrtype->pvtype = (INode*)u8Type;
            AddrNode *addrnode = newAddrNode();
            addrnode->vtype = (INode*)addrtype;
            addrnode->exp = *argsp;
            *argsp = (INode*)addrnode;
        }

        if (!iexpCoerces(*parmp, argsp))
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

// Find best property or method (across overloaded methods whose type matches argument types)
// Then lower the node to a function call (objfn+args) or property access (objfn+methprop) accordingly
void fnCallLowerMethod(FnCallNode *callnode) {
    INode *obj = callnode->objfn;
    INode *methtype = iexpGetTypeDcl(obj);
    if (methtype->tag == RefTag || methtype->tag == PtrTag)
        methtype = iexpGetTypeDcl(((PtrNode *)methtype)->pvtype);
    if (!isMethodType(methtype)) {
        errorMsgNode((INode*)callnode, ErrorNoMeth, "Object's type does not support methods or properties.");
        return;
    }

    // Do lookup. If node found, it must be an instance's method or property
    Name *methsym = callnode->methprop->namesym;
    if (methsym->namestr == '_'
        && !(obj->tag==VarNameUseTag && ((NameUseNode*)obj)->dclnode->namesym == selfName)) {
        errorMsgNode((INode*)callnode, ErrorNotPublic, "May not access the private method/property `%s`.", &methsym->namestr);
    }
    INamedNode *foundnode = imethnodesFind(&((IMethodNode*)methtype)->methprops, methsym);
    if (!foundnode
        || !(foundnode->tag == FnDclTag || foundnode->tag == VarDclTag)
        || !(foundnode->flags & FlagMethProp)) {
        errorMsgNode((INode*)callnode, ErrorNoMeth, "Object's type has no method or property named %s.", &methsym->namestr);
        callnode->vtype = methtype; // Pretend on a vtype
        return;
    }

    // Handle when methprop refers to a property
    if (foundnode->tag == VarDclTag) {
        if (callnode->args != NULL)
            errorMsgNode((INode*)callnode, ErrorManyArgs, "May not provide arguments for a property access");

        derefAuto(&callnode->objfn);
        callnode->methprop->tag = VarNameUseTag;
        callnode->methprop->dclnode = foundnode;
        callnode->vtype = callnode->methprop->vtype = foundnode->vtype;
        callnode->tag = StrFieldTag;
        return;
    }

    // For a method call, make sure object is specified as first argument
    if (callnode->args == NULL) {
        callnode->args = newNodes(1);
    }
    nodesInsert(&callnode->args, callnode->objfn, 0);

    FnDclNode *bestmethod = imethnodesFindBestMethod((FnDclNode *)foundnode, callnode->args);
    if (bestmethod == NULL) {
        errorMsgNode((INode*)callnode, ErrorNoMeth, "No method named %s matches the call's arguments.", &methsym->namestr);
        callnode->vtype = ((ITypedNode*)obj)->vtype; // make up a vtype
        return;
    }

    // Do autoref or autoderef self, as necessary
    refAutoRef(&nodesGet(callnode->args, 0), ((ITypedNode*)nodesGet(((FnSigNode*)bestmethod->vtype)->parms, 0))->vtype);

    // Re-purpose method's name use node into objfn, so name refers to found method
    NameUseNode *methodrefnode = callnode->methprop;
    methodrefnode->tag = VarNameUseTag;
    methodrefnode->dclnode = (INamedNode*)bestmethod;
    methodrefnode->vtype = bestmethod->vtype;

    callnode->objfn = (INode*)methodrefnode;
    callnode->methprop = NULL;
    callnode->vtype = ((FnSigNode*)bestmethod->vtype)->rettype;

    // Handle copying of value arguments and default arguments
    fnCallFinalizeArgs(callnode);
}

// Name resolution on 'fncall' node of all varieties
// Note: this never name resolves .methprop, which is handled in type checking
void fnCallNameCheck(PassState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    INode **argsp;
    uint32_t cnt;

    // Name resolve objfn so we know what it is to vary subsequent processing
    inodeWalk(pstate, &node->objfn);

    // TBD: objfn is a macro

    // If objfn is a type, handle it as a type literal
    if (isTypeNode(node->objfn)) {
        if (!node->flags & FlagArrSlice) {
            errorMsgNode(node->objfn, ErrorBadTerm, "May not do a function call on a type");
            return;
        }
        node->tag = TypeLitTag;
        node->vtype = node->objfn;

        INode *typdcl = itypeGetTypeDcl(node->objfn);
        if (typdcl->tag == StructTag && (typdcl->flags & FlagStructPrivate) && typdcl != pstate->typenode) {
            errorMsgNode(node->objfn, ErrorNotTyped, "For types with private fields, literal can only be used by type's methods");
        }
    }

    // Name resolve arguments/statements
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp))
            inodeWalk(pstate, argsp);
    }
}

// Analyze function/method call node
// Type check significantly lowers the node's contents from its parsed structure
// to a type-resolved structure suitable for generation. The lowering involves
// resolving syntactic sugar and resolving a method to a function.
// It also distinguishes between methods and properties.
void fnCallPass(PassState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;

    // Name resolution
    if (pstate->pass == NameResolution) {
        fnCallNameCheck(pstate, nodep);
        return;
    }

    // Type check all of tree except methprop for now
    INode **argsp;
    uint32_t cnt;
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp))
            inodeWalk(pstate, argsp);
    }
    inodeWalk(pstate, &node->objfn);

    // If objfn is a method/property, rewrite it as self.method
    if (node->objfn->tag == VarNameUseTag
        && ((NameUseNode*)node->objfn)->dclnode->flags & FlagMethProp
        && ((NameUseNode*)node->objfn)->qualNames == NULL) {
        // Build a resolved 'self' node
        NameUseNode *selfnode = newNameUseNode(selfName);
        selfnode->tag = VarNameUseTag;
        selfnode->dclnode = (INamedNode *)nodesGet(pstate->fnsig->parms, 0);
        selfnode->vtype = selfnode->dclnode->vtype;
        // Reuse existing fncallnode if we can
        if (node->methprop == NULL) {
            node->methprop = (NameUseNode *)node->objfn;
            node->objfn = (INode*)selfnode;
        }
        else {
            // Re-purpose objfn as self.method
            FnCallNode *fncall = newFnCallNode((INode *)selfnode, 0);
            fncall->methprop = (NameUseNode *)node->objfn;
            copyNodeLex(fncall, node->objfn); // Copy lexer info into injected node in case it has errors
            node->objfn = (INode*)fncall;
            inodeWalk(pstate, &node->objfn);
        }
    }

    // How to lower depends on the type of the objfn
    if (!isExpNode(node)) {
        errorMsgNode(node->objfn, ErrorNotTyped, "Expecting a typed value or expression");
        return;
    }
    INode *objfntype = iexpGetTypeDcl(node->objfn);
    if (!((node->flags & FlagIndex)
        && (objfntype->tag == PtrTag || (objfntype->tag == RefTag && (objfntype->flags & FlagArrSlice)))))
        objfntype = iexpGetDerefTypeDcl(node->objfn); // Deref if not applying [] to ptr or slice

    // Objects (method types) are lowered to method calls via a name lookup
    if (isMethodType(objfntype)) {

        // Use '()' or '[]' method call, when no method or property is specified
        if (node->methprop == NULL)
            node->methprop = newNameUseNode(nametblFind(node->flags & FlagIndex? "[]" : "()", 2));

        // Lower to a property access or function call
        fnCallLowerMethod(node);
    }

    else if (node->methprop)
        errorMsgNode((INode *)node, ErrorBadMeth, "Cannot do method or property on a value of this type");

    // Handle a regular function call or implicit method call
    else if (objfntype->tag == FnSigTag) {
        derefAuto(&node->objfn);

        // Capture return vtype
        node->vtype = ((FnSigNode*)objfntype)->rettype;

        // Error out if we have too many arguments
        int argsunder = ((FnSigNode*)objfntype)->parms->used - node->args->used;
        if (argsunder < 0) {
            errorMsgNode((INode*)node, ErrorManyArgs, "Too many arguments specified vs. function declaration");
            return;
        }

        // Type check arguments, handling copy and default arguments along the way
        fnCallFinalizeArgs(node);
    }

    // Handle index/slice arguments on an array or array reference
    else if (objfntype->tag == ArrayTag || objfntype->tag == RefTag || objfntype->tag == PtrTag) {
        uint32_t nargs = node->args->used;
        if (nargs == 1) {
            INode *index = nodesGet(node->args, 0);
            INode *indextype = iexpGetTypeDcl(index);
            if (indextype->tag != UintNbrTag && index->tag != ULitTag)
                errorMsgNode((INode *)node, ErrorBadIndex, "Array index must be an unsigned integer");
            switch (objfntype->tag) {
            case ArrayTag:
                node->vtype = ((ArrayNode*)objfntype)->elemtype;
                break;
            case RefTag:
                node->vtype = ((RefNode*)objfntype)->pvtype;
                break;
            case PtrTag:
                node->vtype = ((PtrNode*)objfntype)->pvtype;
                break;
            default:
                assert(0 && "Invalid type for indexing");
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
