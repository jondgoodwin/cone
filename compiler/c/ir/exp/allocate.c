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
    inodeFprint("+(");
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

    quesNode->tag = FnCallTag;  // Treat as option type
    INode *argnode = nodesGet(quesNode->args, 0);
    // 'trynew T(...)': the construction carries the bound 'Option' to type
    // check, which types the allocation with it (typeLitNewCheck)
    if (argnode->tag == FnCallTag && (argnode->flags & FlagTryNew)) {
        ((FnCallNode*)argnode)->methfld = quesNode->objfn;
        *((INode**)nodep) = argnode;
    }
    else if (isTypeNode(argnode)) {
        // When arg is a type, then treat this as Option[T] type
    }
    else if (argnode->tag == AllocateTag) {
        RefNode *allocnode = (RefNode*)argnode;
        allocnode->flags |= FlagQues;
        allocnode->vtype = (INode*)quesNode; // in TypeCheck, this will be updated
        *((INode**)nodep) = argnode;
    }
    else if (inodeIsProvisionalType(argnode)) {
        // In a generic's template '?T' asks of a generic parameter, which is
        // not a type until substituted. Leave it the Option[T] type: the
        // instance's clone substitutes the argument, and the clone of a '&T'
        // or '(T, T)' argument decides that one again.
    }
    else {
        errorMsgNode((INode*)quesNode, ErrorInvType, "'?' is not valid here.");
    }
}

// Type check an allocation written with the retired '+', '+Rc-mut 5' or
// '?+Rc 5', which name resolution made of a '+' reference whose operand is a
// value: refused, naming the 'new' or 'trynew' form, with the value or the
// init's arguments in the parentheses (typeLitNewAllocate). It is still
// checked as an allocation, so nothing after it reports again.
void allocateTypeCheck(TypeCheckState *pstate, RefNode **nodep) {
    RefNode *node = *nodep;
    if (node->perm == unknownType)
        node->perm = newPermUseNode(uniPerm);

    // A struct's value in brackets, '+Rc-mut Node[1]', is refused here, as an
    // allocation written with '+', and not again as a bracket construction
    // (ErrorStructBracket)
    if (node->vtexp->tag == FnCallTag && (node->vtexp->flags & FlagIndex))
        node->vtexp->flags |= FlagAllocValue;
    if (iexpTypeCheckAny(pstate, &node->vtexp) == 0)
        return;

    INode *vtype = ((IExpNode*)node->vtexp)->vtype;
    char *perm = itypeName(node->perm);
    int uni = itypeGetTypeDcl(node->perm) == (INode*)uniPerm;
    // A type with no name of its own, an array, a tuple or a reference, is
    // written 'T' and said
    uint16_t vtag = itypeGetTypeDcl(vtype)->tag;
    int named = vtag != ArrayTag && vtag != TTupleTag && vtag != FnSigTag && vtag != RefTag
        && vtag != VirtRefTag && vtag != ArrayRefTag && vtag != PtrTag;
    errorMsgNode((INode*)node, ErrorPlusAlloc, typeLitIsConstruction(node->vtexp)
        ? "An allocation is written '%s %s[%s%s%s](...)', the init's arguments in the parentheses%s%s."
        : "An allocation is written '%s %s[%s%s%s](value)', the value in the parentheses%s%s.",
        (node->flags & FlagQues) ? "trynew" : "new", itypeName(node->region),
        uni ? "" : perm, uni ? "" : ", ", named ? itypeName(vtype) : "T",
        named ? "" : ", T ", named ? "" : itypeName(vtype));
    allocateValueCheck(pstate, nodep);
}

// Type check an allocation whose value is checked: the value a region can
// hold, and the reference's type
void allocateValueCheck(TypeCheckState *pstate, RefNode **nodep) {
    RefNode *node = *nodep;

    INode *vtype = ((IExpNode*)node->vtexp)->vtype;

    // A GPU has no allocator: a kernel's memory is its locals and the buffers
    // it is bound. Checked as an allocation all the same, so nothing after it
    // reports again.
    if (flowGpu)
        errorMsgNode((INode*)node, ErrorGpuUnavailable,
            "In GPU code nothing is allocated, '%s %s[...]': a GPU has no allocator. Keep the value in a local, or in a buffer the kernel is bound.",
            (node->flags & FlagQues) ? "trynew" : "new", itypeName(node->region));

    // The default permission type is 'uni', or 'imm' for a type declaring
    // Immutable
    if (node->perm == unknownType) {
        INode *immperm = refImmutableDefaultPerm(vtype);
        node->perm = immperm ? immperm : newPermUseNode(uniPerm);
    }

    if (!itypeIsConcrete(vtype) || itypeIsZeroSize(vtype)) {
        errorMsgNode(node->vtexp, ErrorInvType, "May not allocate a value of abstract or zero-size type");
    }

    // The reference's type: the one written after 'new', already checked
    // (typeLitNewAllocate), or, for the refused '+' spelling, inferred from
    // the value. A fallible one's is the parameter of the Option node it
    // already has.
    INode **reftypep = (node->flags & FlagQues) ? &nodesGet(((FnCallNode *)node->vtype)->args, 0) : &node->vtype;
    if ((*reftypep)->tag != RefTag)
        *reftypep = (INode*)newRefNodeFull(RefTag, (INode*)node, node->region, node->perm, vtype);
    inodeTypeCheckAny(pstate, &node->vtype);

    // The region can allocate (the shapes of its methods are checked at its
    // declaration), and the permission's init is declared correctly
    regionAllocTypeCheck(node->region);
    permInitTypeCheck(itypeGetTypeDcl(node->perm));
}

// Perform data flow analysis on allocate node
void allocateFlow(FlowState *fstate, RefNode **nodep) {
    RefNode *node = *nodep;
    // For an allocated reference, we need to handle the copied value
    flowLoadValue(fstate, &node->vtexp);
    flowHandleMoveOrCopy(&node->vtexp);
}
