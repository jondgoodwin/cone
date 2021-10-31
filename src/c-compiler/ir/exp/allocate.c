/** Handling for allocate expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Serialize allocate
void allocatePrint(RefNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->vtype);
    inodeFprint("->");
    inodePrintNode(node->vtexp);
    inodeFprint(")");
}

// Name resolution for questag: decide if Option type or fold into AllocNode
void allocateQuesNameRes(NameResState *pstate, FnCallNode **nodep) {
    FnCallNode *quesNode = *nodep;

    inodeNameRes(pstate, &quesNode->objfn);
    inodeNameRes(pstate, &nodesGet(quesNode->args, 0));

    quesNode->tag = FnCallTag;  // Treat as option type from here-onl
    INode *argnode = nodesGet(quesNode->args, 0);
    if (isTypeNode(argnode)) {
        // When arg is a type, then treat this as Option[T] type
    }
    else if (argnode->tag == AllocateTag) {
        RefNode *allocnode = (RefNode*)argnode;
        allocnode->flags |= FlagQues;
        allocnode->vtype = (INode*)quesNode; // in TypeCheck, this will be updated
        *((INode**)nodep) = argnode;
    }
    else {
        errorMsgNode((INode*)quesNode, ErrorInvType, "'?' is not valid here.");
    }
}

// Type check allocate node
void allocateTypeCheck(TypeCheckState *pstate, RefNode **nodep) {
    RefNode *node = *nodep;

    // The default permission type is 'uni'
    if (node->perm == unknownType)
        node->perm = newPermUseNode(uniPerm);

    // Ensure expression is a value usable for initializing allocated memory
    if (iexpTypeCheckAny(pstate, &node->vtexp) == 0)
        return;

    INode *vtype = ((IExpNode*)node->vtexp)->vtype;
    if (!itypeIsConcrete(vtype) || itypeIsZeroSize(vtype)) {
        errorMsgNode(node->vtexp, ErrorInvType, "May not allocate a value of abstract or zero-size type");
    }

    // Infer reference's value type based on initial value
    RefNode *reftype = newRefNodeFull(RefTag, (INode*)node, node->region, node->perm, vtype);
    if (node->flags & FlagQues) {
        // node->vtype already has an Option node (as FnCall). Fix up its parametric type
        FnCallNode *option = (FnCallNode *)node->vtype;
        INode **parm = &nodesGet(option->args, 0);
        *parm = (INode*)reftype;
    }
    else
        node->vtype = (INode *)reftype;
    inodeTypeCheckAny(pstate, &node->vtype);

    // Type check that ref region + permission's allocation functions are declared correctly
    regionAllocTypeCheck(itypeGetTypeDcl(node->region));
    permInitTypeCheck(itypeGetTypeDcl(node->perm));
}

// Perform data flow analysis on allocate node
void allocateFlow(FlowState *fstate, RefNode **nodep) {
    RefNode *node = *nodep;
    // For an allocated reference, we need to handle the copied value
    flowLoadValue(fstate, &node->vtexp);
    flowHandleMoveOrCopy(&node->vtexp);
}
