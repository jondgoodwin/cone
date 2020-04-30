/** Handling for borrow expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a new borrow node
RefNode *newBorrowNode() {
    RefNode *node;
    newNode(node, RefNode, BorrowTag);
    node->vtype = (INode*)unknownType;
    return node;
}

// Insert automatic ref, if node is a variable
void borrowAuto(INode **node, INode* reftype) {
    RefNode *borrownode = newBorrowNode();
    borrownode->vtexp = *node;
    borrownode->vtype = reftype;
    *node = (INode*)borrownode;
}

// Inject a borrow mutable node on some node (expected to be an lval)
void borrowMutRef(INode **node, INode* type) {
    // Rather than borrow from a deref, just return the ptr node we are de-reffing
    if ((*node)->tag == DerefTag) {
        StarNode *derefnode = (StarNode *)*node;
        *node = derefnode->vtexp;
        return;
    }
    RefNode *refnode = newRefNodeFull(borrowRef, newPermUseNode(mutPerm), type);
    inodeLexCopy((INode*)refnode, *node);
    RefNode *borrownode = newBorrowNode();
    inodeLexCopy((INode*)borrownode, *node);
    borrownode->vtexp = *node;
    borrownode->vtype = (INode*)refnode;
    *node = (INode*)borrownode;
}

// Clone borrow
INode *cloneBorrowNode(CloneState *cstate, RefNode *node) {
    RefNode *newnode;
    newnode = memAllocBlk(sizeof(RefNode));
    memcpy(newnode, node, sizeof(RefNode));
    newnode->vtexp = cloneNode(cstate, node->vtexp);
    return (INode *)newnode;
}

// Serialize borrow node
void borrowPrint(RefNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->vtype);
    inodeFprint("->");
    inodePrintNode(node->vtexp);
    inodeFprint(")");
}

// Name resolution of borrow node
void borrowNameRes(NameResState *pstate, RefNode **nodep) {
    RefNode *node = *nodep;
    inodeNameRes(pstate, &node->vtexp);
}

// Analyze borrow node
void borrowTypeCheck(TypeCheckState *pstate, RefNode **nodep) {
    RefNode *node = *nodep;
    if (iexpTypeCheckAny(pstate, &node->vtexp) == 0 || iexpIsLval(node->vtexp) == 0)
        return;

    // Auto-deref the exp, if we are borrowing a reference to a reference's field or indexed value
    INode *exptype = iexpGetTypeDcl(node->vtexp);
    if ((node->flags & FlagSuffix) && (exptype->tag == RefTag || exptype->tag == PtrTag || exptype->tag == ArrayRefTag)) {
        StarNode *deref = newStarNode(DerefTag);
        deref->vtexp = node->vtexp;
        if (exptype->tag == ArrayRefTag)
            deref->vtype = (INode*)newArrayDerefNodeFrom((RefNode*)exptype);
        else
            deref->vtype = ((RefNode*)exptype)->vtexp;  // assumes StarNode has field in same place
        node->vtexp = (INode*)deref;
    }

    // Setup lval, perm and scope info as if we were borrowing from a global constant literal.
    // If not, extract this info from expression nodes
    uint16_t scope = 0;  // global
    uint16_t tag = RefTag;
    INode *lval = node->vtexp;
    INode *lvalperm = (INode*)immPerm;
    scope = 0;  // Global
    if (lval->tag != StringLitTag) {
        // lval is the variable or variable sub-structure we want to get a reference to
        // From it, obtain variable we are borrowing from and actual/calculated permission
        INode *lvalvar = iexpGetLvalInfo(lval, &lvalperm, &scope);
        if (lvalvar == NULL) {
            node->vtype = (INode*)newRefNodeFull(node->region, node->perm, (INode*)unknownType); // To avoid a crash later
            return;
        }
        // Set lifetime of reference to borrowed variable's lifetime
        if (lvalvar->tag == VarDclTag)
            scope = ((VarDclNode*)lvalvar)->scope;
    }
    INode *lvaltype = ((IExpNode*)lval)->vtype;

    // The reference's value type is currently unknown (NULL).
    // Let's infer this value type from the lval we are borrowing from
    INode *refvtype;
    if (lvaltype->tag == ArrayTag) {
        // Borrowing from a fixed size array creates an array reference
        tag = ArrayRefTag;
        refvtype = ((ArrayNode*)lvaltype)->elemtype;
    }
    else if (lvaltype->tag == ArrayDerefTag) {
        tag = ArrayRefTag;
        refvtype = ((RefNode*)lvaltype)->vtexp;
    }
    else
        refvtype = lvaltype;

    // Ensure requested/inferred permission matches lval's permission
    INode *refperm = node->perm;
    if (refperm == NULL)
        refperm = itypeIsConcrete(refvtype) ? (INode*)constPerm : (INode*)opaqPerm;
    if (!permMatches(refperm, lvalperm))
        errorMsgNode((INode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");

    RefNode *reftype = newRefNodeFull(borrowRef, refperm, refvtype);
    inodeLexCopy((INode*)reftype, (INode*)node);
    reftype->scope = scope;
    reftype->tag = tag;
    node->vtype = (INode *)reftype;
}

// Perform data flow analysis on addr node
void borrowFlow(FlowState *fstate, RefNode **nodep) {
    RefNode *node = *nodep;
    RefNode *reftype = (RefNode *)node->vtype;
    // Borrowed reference:  Deactivate source variable if necessary
}
