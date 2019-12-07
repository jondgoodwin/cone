/** Handling for borrow expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a new borrow node
BorrowNode *newBorrowNode() {
    BorrowNode *node;
    newNode(node, BorrowNode, BorrowTag);
    return node;
}

// Insert automatic ref, if node is a variable
void borrowAuto(INode **node, INode* reftype) {
    BorrowNode *borrownode = newBorrowNode();
    borrownode->exp = *node;
    borrownode->vtype = reftype;
    *node = (INode*)borrownode;
}

// Inject a borrow mutable node on some node (expected to be an lval)
void borrowMutRef(INode **node, INode* type) {
    // Rather than borrow from a deref, just return the ptr node we are de-reffing
    if ((*node)->tag == DerefTag) {
        DerefNode *derefnode = (DerefNode *)*node;
        *node = derefnode->exp;
        return;
    }
    RefNode *refnode = newRefNodeFull(borrowRef, newPermUseNode(mutPerm), type);
    inodeLexCopy((INode*)refnode, *node);
    BorrowNode *borrownode = newBorrowNode();
    inodeLexCopy((INode*)borrownode, *node);
    borrownode->exp = *node;
    borrownode->vtype = (INode*)refnode;
    *node = (INode*)borrownode;
}

// Clone borrow
INode *cloneBorrowNode(CloneState *cstate, BorrowNode *node) {
    BorrowNode *newnode;
    newnode = memAllocBlk(sizeof(BorrowNode));
    memcpy(newnode, node, sizeof(BorrowNode));
    newnode->exp = cloneNode(cstate, node->exp);
    return (INode *)newnode;
}

// Serialize borrow node
void borrowPrint(BorrowNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->vtype);
    inodeFprint("->");
    inodePrintNode(node->exp);
    inodeFprint(")");
}

// Name resolution of borrow node
void borrowNameRes(NameResState *pstate, BorrowNode **nodep) {
    BorrowNode *node = *nodep;
    inodeNameRes(pstate, &node->exp);
}

// Analyze borrow node
void borrowTypeCheck(TypeCheckState *pstate, BorrowNode **nodep) {
    BorrowNode *node = *nodep;
    if (iexpTypeCheck(pstate, &node->exp) == 0 || iexpIsLval(node->exp) == 0)
        return;

    // Auto-deref the exp, if we are borrowing a reference to a reference's field or indexed value
    INode *exptype = iexpGetTypeDcl(node->exp);
    if ((node->flags & FlagSuffix) && (exptype->tag == RefTag || exptype->tag == PtrTag || exptype->tag == ArrayRefTag)) {
        DerefNode *deref = newDerefNode();
        deref->exp = node->exp;
        if (exptype->tag == ArrayRefTag)
            deref->vtype = (INode*)newArrayDerefNodeFrom((RefNode*)exptype);
        else
            deref->vtype = ((RefNode*)exptype)->pvtype;  // assumes PtrNode has field in same place
        node->exp = (INode*)deref;
    }

    // Setup lval, perm and scope info as if we were borrowing from a global constant literal.
    // If not, extract this info from expression nodes
    RefNode *reftype = (RefNode *)node->vtype;
    INode *lval = node->exp;
    INode *lvalperm = (INode*)immPerm;
    reftype->scope = 0;  // Global
    if (lval->tag != StringLitTag) {
        // lval is the variable or variable sub-structure we want to get a reference to
        // From it, obtain variable we are borrowing from and actual/calculated permission
        INode *lvalvar = flowLvalInfo(lval, &lvalperm, &reftype->scope);
        if (lvalvar == NULL) {
            reftype->pvtype = (INode*)unknownType;  // Just to avoid a compiler crash later
            return;
        }
        // Set lifetime of reference to borrowed variable's lifetime
        if (lvalvar->tag == VarDclTag)
            reftype->scope = ((VarDclNode*)lvalvar)->scope;
    }
    INode *lvaltype = ((IExpNode*)lval)->vtype;

    // The reference's value type is currently unknown (NULL).
    // Let's infer this value type from the lval we are borrowing from
    INode *refvtype;
    if (lvaltype->tag == ArrayTag) {
        // Borrowing from a fixed size array creates an array reference
        reftype->tag = ArrayRefTag;
        refvtype = ((ArrayNode*)lvaltype)->elemtype;
    }
    else if (lvaltype->tag == ArrayDerefTag) {
        reftype->tag = ArrayRefTag;
        refvtype = ((RefNode*)lvaltype)->pvtype;
    }
    else
        refvtype = lvaltype;

    // Ensure requested/inferred permission matches lval's permission
    INode *refperm = reftype->perm;
    if (refperm == NULL)
        refperm = itypeIsConcrete(refvtype) ? (INode*)constPerm : (INode*)opaqPerm;
    if (!permMatches(refperm, lvalperm))
        errorMsgNode((INode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");

    refSetPermVtype(reftype, refperm, refvtype);
}

// Perform data flow analysis on addr node
void borrowFlow(FlowState *fstate, BorrowNode **nodep) {
    BorrowNode *node = *nodep;
    RefNode *reftype = (RefNode *)node->vtype;
    // Borrowed reference:  Deactivate source variable if necessary
}
