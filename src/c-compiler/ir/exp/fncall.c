/** AST handling for function/method calls
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include "../../shared/memory.h"
#include "../../parser/lexer.h"
#include "../nametbl.h"
#include "../../shared/error.h"

#include <assert.h>
#include <string.h>

// Create a function call node
FnCallAstNode *newFnCallAstNode(AstNode *fn, int nnodes) {
	FnCallAstNode *node;
	newAstNode(node, FnCallAstNode, FnCallNode);
	node->objfn = fn;
    node->methprop = NULL;
	node->args = nnodes == 0? NULL : newNodes(nnodes);
	return node;
}

FnCallAstNode *newFnCallOp(AstNode *obj, char *op, int nnodes) {
    FnCallAstNode *node = newFnCallAstNode(obj, nnodes);
    node->methprop = newMemberUseNode(nametblFind(op, strlen(op)));
    return node;
}

// Serialize function call node
void fnCallPrint(FnCallAstNode *node) {
    AstNode **nodesp;
    uint32_t cnt;
    astPrintNode(node->objfn);
    if (node->methprop) {
        astFprint(".");
        astPrintNode((AstNode*)node->methprop);
    }
    if (node->args) {
        astFprint("(");
        for (nodesFor(node->args, cnt, nodesp)) {
            astPrintNode(*nodesp);
            if (cnt > 1)
                astFprint(", ");
        }
        astFprint(")");
    }
}

// For all function calls, go through all arguments to verify correct types,
// handle value copying, and fill in default arguments
void fnCallFinalizeArgs(FnCallAstNode *node) {
    AstNode *fnsig = typeGetVtype(node->objfn);
    AstNode **argsp;
    uint32_t cnt;
    AstNode **parmp = &nodesGet(((FnSigAstNode*)fnsig)->parms, 0);
    for (nodesFor(node->args, cnt, argsp)) {
        if (!typeCoerces(*parmp, argsp))
            errorMsgNode(*argsp, ErrorInvType, "Expression's type does not match declared parameter");
        else
            typeHandleCopy(argsp);
        parmp++;
    }

    // If we have too few arguments, use default values, if provided
    int argsunder = ((FnSigAstNode*)fnsig)->parms->used - node->args->used;
    if (argsunder > 0) {
        if (((VarDclAstNode*)*parmp)->value == NULL)
            errorMsgNode((AstNode*)node, ErrorFewArgs, "Function call requires more arguments than specified");
        else {
            while (argsunder--) {
                nodesAdd(&node->args, ((VarDclAstNode*)*parmp)->value);
                parmp++;
            }
        }
    }
}

// Find best property or method (across overloaded methods whose type matches argument types)
// Then lower the node to a function call (objfn+args) or property access (objfn+methprop) accordingly
void fnCallLowerMethod(FnCallAstNode *callnode) {
    AstNode *obj = callnode->objfn;
    AstNode *objtype = typeGetVtype(obj);
    if (objtype->asttype == RefType || objtype->asttype == PtrType)
        objtype = typeGetVtype(((PtrAstNode *)objtype)->pvtype);
    if (!isMethodType(objtype)) {
        errorMsgNode((AstNode*)callnode, ErrorNoMeth, "Object's type does not support methods or properties.");
        return;
    }

    // Do lookup. If node found, it must be an instance's method or property
    Name *methsym = callnode->methprop->namesym;
    if (methsym->namestr == '_'
        && !(obj->asttype==VarNameUseTag && ((NameUseAstNode*)obj)->dclnode->namesym == selfName)) {
        errorMsgNode((AstNode*)callnode, ErrorNotPublic, "May not access the private method/property `%s`.", &methsym->namestr);
    }
    NamedAstNode *foundnode = methnodesFind(&((MethodTypeAstNode*)objtype)->methprops, methsym);
    if (!foundnode
        || !(foundnode->asttype == FnDclTag || foundnode->asttype == VarDclTag)
        || !(foundnode->flags & FlagMethProp)) {
        errorMsgNode((AstNode*)callnode, ErrorNoMeth, "Object's type has no method or property named %s.", &methsym->namestr);
        return;
    }

    // Handle when methprop refers to a property
    if (foundnode->asttype == VarDclTag) {
        if (callnode->args != NULL)
            errorMsgNode((AstNode*)callnode, ErrorManyArgs, "May not provide arguments for a property access");

        derefAuto(&callnode->objfn);
        callnode->methprop->asttype = VarNameUseTag;
        callnode->methprop->dclnode = foundnode;
        callnode->vtype = callnode->methprop->vtype = foundnode->vtype;
        return;
    }

    // For a method call, make sure object is specified as first argument
    if (callnode->args == NULL) {
        callnode->args = newNodes(1);
    }
    nodesInsert(&callnode->args, callnode->objfn, 0);

    FnDclAstNode *bestmethod = methnodesFindBestMethod((FnDclAstNode *)foundnode, callnode->args);
    if (bestmethod == NULL) {
        errorMsgNode((AstNode*)callnode, ErrorNoMeth, "No method named %s matches the call's arguments.", &methsym->namestr);
        return;
    }

    // Re-purpose the method name use node as a reference to the method function itself
    NameUseAstNode *methodrefnode = callnode->methprop;
    methodrefnode->asttype = VarNameUseTag;
    methodrefnode->dclnode = (NamedAstNode*)bestmethod;
    methodrefnode->vtype = bestmethod->vtype;

    callnode->objfn = (AstNode*)methodrefnode;
    callnode->methprop = NULL;
    callnode->vtype = ((FnSigAstNode*)bestmethod->vtype)->rettype;

    // Handle copying of value arguments and default arguments
    fnCallFinalizeArgs(callnode);
}

// Analyze function/method call node
// Type check significantly lowers the node's contents from its parsed structure
// to a type-resolved structure suitable for generation. The lowering involves
// resolving syntactic sugar and resolving a method to a function.
// It also distinguishes between methods and properties.
void fnCallPass(PassState *pstate, FnCallAstNode *node) {
	AstNode **argsp;
	uint32_t cnt;

    // Traverse tree for all passes
    // Note: Name resolution for .methprop happens in typecheck pass
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp))
            nodeWalk(pstate, argsp);
    }
    nodeWalk(pstate, &node->objfn);

    switch (pstate->pass) {
    case TypeCheck:
    {
        // If objfn is a method/property, rewrite it as self.method
        if (node->objfn->asttype == VarNameUseTag
            && ((NameUseAstNode*)node->objfn)->dclnode->flags & FlagMethProp
            && ((NameUseAstNode*)node->objfn)->qualNames == NULL) {
            // Build a resolved 'self' node
            NameUseAstNode *selfnode = newNameUseNode(selfName);
            selfnode->asttype = VarNameUseTag;
            selfnode->dclnode = (NamedAstNode *)nodesGet(pstate->fnsig->parms, 0);
            selfnode->vtype = selfnode->dclnode->vtype;
            // Reuse existing fncallnode if we can
            if (node->methprop == NULL) {
                node->methprop = (NameUseAstNode *)node->objfn;
                node->objfn = (AstNode*)selfnode;
            }
            else {
                // Re-purpose objfn as self.method
                FnCallAstNode *fncall = newFnCallAstNode((AstNode *)selfnode, 0);
                fncall->methprop = (NameUseAstNode *)node->objfn;
                copyNodeLex(fncall, node->objfn); // Copy lexer info into injected node in case it has errors
                node->objfn = (AstNode*)fncall;
                nodeWalk(pstate, &node->objfn);
            }
        }

        // How to lower depends on the type of the objfn
        if (!isValueNode(node)) {
            errorMsgNode(node->objfn, ErrorNotTyped, "Expecting a typed node");
            return;
        }
        AstNode *objfntype = typeGetDerefType(node->objfn);

        // Objects (method types) are lowered to method calls via a name lookup
        if (isMethodType(objfntype)) {

            // Use '()' method call, when no method or property is specified
            if (node->methprop == NULL)
                node->methprop = newNameUseNode(nametblFind("()", 2));

            // Lower to a property access or function call
            fnCallLowerMethod(node);
        }

        else if (node->methprop)
            errorMsgNode((AstNode *)node, ErrorBadMeth, "Cannot do method or property on a value of this type");

        else if (objfntype->asttype != FnSigType)
            errorMsgNode((AstNode *)node, ErrorNotFn, "Cannot apply arguments to a non-function");

        // Handle a regular function call or implicit method call
        else {
            derefAuto(&node->objfn);

            // Capture return vtype
            node->vtype = ((FnSigAstNode*)objfntype)->rettype;

            // Error out if we have too many arguments
            int argsunder = ((FnSigAstNode*)objfntype)->parms->used - node->args->used;
            if (argsunder < 0) {
                errorMsgNode((AstNode*)node, ErrorManyArgs, "Too many arguments specified vs. function declaration");
                return;
            }

            // Type check arguments, handling copy and default arguments along the way
            fnCallFinalizeArgs(node);
        }
        break;
    }
    }
}