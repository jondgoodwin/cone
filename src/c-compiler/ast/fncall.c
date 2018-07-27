/** AST handling for function/method calls
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../ast/nametbl.h"
#include "../shared/error.h"

#include <assert.h>
#include <string.h>

// Create a function call node
FnCallAstNode *newFnCallAstNode(AstNode *fn, int nnodes) {
	FnCallAstNode *node;
	newAstNode(node, FnCallAstNode, FnCallNode);
	node->objfn = fn;
    node->methfield = NULL;
	node->args = nnodes == 0? NULL : newNodes(nnodes);
	return node;
}

FnCallAstNode *newFnCallOp(AstNode *obj, char *op, int nnodes) {
    FnCallAstNode *node = newFnCallAstNode(obj, nnodes);
    node->methfield = newMemberUseNode(nametblFind(op, strlen(op)));
    if (nnodes > 1)
        nodesAdd(&node->args, obj);
    return node;
}

// Serialize function call node
void fnCallPrint(FnCallAstNode *node) {
    AstNode **nodesp;
    uint32_t cnt;
    astPrintNode(node->objfn);
    if (node->methfield) {
        astFprint(".");
        astPrintNode((AstNode*)node->methfield);
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
void fnCallFinalizeArgs(FnCallAstNode *node, int docheck) {
    AstNode *fnsig = typeGetVtype(node->objfn);
    AstNode **argsp;
    uint32_t cnt;
    AstNode **parmp = &nodesGet(((FnSigAstNode*)fnsig)->parms, 0);
    for (nodesFor(node->args, cnt, argsp)) {
        if (docheck && !typeCoerces(*parmp, argsp))
            errorMsgNode(*argsp, ErrorInvType, "Expression's type does not match declared parameter");
        else
            typeHandleCopy(argsp);
        parmp++;
    }

    // If we have too few arguments, use default values, if provided
    int argsunder = ((FnSigAstNode*)fnsig)->parms->used - node->args->used;
    if (argsunder > 0) {
        if (docheck && ((VarDclAstNode*)*parmp)->value == NULL)
            errorMsgNode((AstNode*)node, ErrorFewArgs, "Function call requires more arguments than specified");
        else {
            while (argsunder--) {
                nodesAdd(&node->args, ((VarDclAstNode*)*parmp)->value);
                parmp++;
            }
        }
    }
}

// Find best field or method (across overloaded methods whose type matches argument types)
// Then lower the node to a function call (objfn+args) or field access (objfn+methfield) accordingly
void fnCallLowerMethod(FnCallAstNode *callnode) {
    AstNode *objtype = typeGetVtype(callnode->objfn);
    if (objtype->asttype == RefType || objtype->asttype == PtrType)
        objtype = typeGetVtype(((PtrAstNode *)objtype)->pvtype);
    if (!isMethodType(objtype)) {
        errorMsgNode((AstNode*)callnode, ErrorNoMeth, "Object's type does not support methods or fields.");
        return;
    }

    // Do lookup. If node found, it must be an instance's method or field
    Name *methsym = callnode->methfield->namesym;
    if (methsym->namestr == '_') {
        errorMsgNode((AstNode*)callnode, ErrorNotPublic, "May not access the private method/field `%s`.", &methsym->namestr);
        return;
    }
    NamedAstNode *foundnode = methnodesFind(&((MethodTypeAstNode*)objtype)->methfields, methsym);
    if (!foundnode
        || !(foundnode->asttype == FnDclTag || foundnode->asttype == VarDclTag)
        || !(foundnode->flags & FlagMethField)) {
        errorMsgNode((AstNode*)callnode, ErrorNoMeth, "Object's type has no method or field named %s.", &methsym->namestr);
        return;
    }

    // Handle when methfield refers to a field
    if (foundnode->asttype == VarDclTag) {
        if (callnode->args != NULL)
            errorMsgNode((AstNode*)callnode, ErrorManyArgs, "May not provide arguments for a field access");

        callnode->methfield->asttype = VarNameUseTag;
        callnode->methfield->dclnode = foundnode;
        callnode->vtype = callnode->methfield->vtype = foundnode->vtype;
        return;
    }

    // For a method call, make sure object is specified as first argument
    if (callnode->args == NULL) {
        callnode->args = newNodes(1);
        nodesAdd(&callnode->args, callnode->objfn);
    }

    FnDclAstNode *bestmethod = methnodesFindBestMethod((FnDclAstNode *)foundnode, callnode->args);
    if (bestmethod == NULL) {
        errorMsgNode((AstNode*)callnode, ErrorNoMeth, "No method named %s matches the call's arguments.", &methsym->namestr);
        return;
    }

    // Re-purpose the method ref node as a reference to the method function itself
    NameUseAstNode *methodrefnode = callnode->methfield;
    callnode->objfn = (AstNode*)methodrefnode;
    callnode->methfield = NULL;
    methodrefnode->asttype = VarNameUseTag;
    methodrefnode->dclnode = (NamedAstNode*)bestmethod;
    methodrefnode->vtype = bestmethod->vtype;
    callnode->vtype = ((FnSigAstNode*)bestmethod->vtype)->rettype;

    // Handle copying of value arguments and default arguments
    fnCallFinalizeArgs(callnode, 0);
}

// Analyze function/method call node
// Type check significantly lowers the node's contents from its parsed structure
// to a type-resolved structure suitable for generation. The lowering involves
// resolving syntactic sugar and resolving a method to a function.
// It also distinguishes between methods and fields.
void fnCallPass(PassState *pstate, FnCallAstNode *node) {
	AstNode **argsp;
	uint32_t cnt;

    // Traverse tree for all passes
    // Note: Name resolution for .methfield happens in typecheck pass
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp))
            astPass(pstate, *argsp);
    }
    astPass(pstate, node->objfn);

    switch (pstate->pass) {
    case TypeCheck:
    {
        derefAuto(&node->objfn);

        // Step 1a: Inject '()' method call, when no method provided for an method-typed object
        if (isMethodType(typeGetVtype(node->objfn)) && node->methfield == NULL) {
            node->methfield = newNameUseNode(nametblFind("()", 2));
            nodesInsert(&node->args, node->objfn, 0); // for method call, args must start with self
        }

        // Step 1b: If objfn name resolved to a method (and we are "in" a method), 
        // move objfn to methfield and put resolved self in objfn
        else if (node->objfn->flags & FlagMethField) {
        }

        // Step 2a: Lower to a field access or function call, if methfield is specified
        if (node->methfield) {
            fnCallLowerMethod(node);
        }

        // Step 2b: For non-method function call, auto-deref as needed and match args to parms
        // Append default arguments and handle borrow/copy against all arguments
        else {
            // Capture return vtype and ensure we are calling a function
            AstNode *fnsig = typeGetVtype(node->objfn);
            if (fnsig->asttype == FnSigType)
                node->vtype = ((FnSigAstNode*)fnsig)->rettype;
            else {
                errorMsgNode(node->objfn, ErrorNotFn, "Cannot call a value that is not a function");
                return;
            }

            // Error out if we have too many arguments
            int argsunder = ((FnSigAstNode*)fnsig)->parms->used - node->args->used;
            if (argsunder < 0) {
                errorMsgNode((AstNode*)node, ErrorManyArgs, "Too many arguments specified vs. function declaration");
                return;
            }

            // Type check arguments, handling copy and default arguments along the way
            fnCallFinalizeArgs(node, 1);
        }
        break;
    }
    }
}