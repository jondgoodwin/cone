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

// Extract lval variable and overall permission from lval
INode *borrowGetVarPerm(INode *lval, INode **lvalperm) {
    switch (lval->tag) {

    // A variable or named function node
    case VarNameUseTag:
        lval = (INode *)((NameUseNode *)lval)->dclnode;
        // Fallthrough is expected here
    case FnDclTag:
    {
        if (lval->tag == VarDclTag)
            *lvalperm = ((VarDclNode *)lval)->perm;
        else
            *lvalperm = (INode*)opaqPerm; // Function
        return lval;
    }

    case DerefTag:
    {
        DerefNode *deref = (DerefNode*)lval;
        INode *lvalret = borrowGetVarPerm(deref->exp, lvalperm);
        INode *typ = iexpGetTypeDcl(deref->exp);
        assert(typ->tag == RefTag || typ->tag == ArrayRefTag);
        *lvalperm = ((RefNode*)typ)->perm;
        return lvalret;
    }

    // An array element (obj[2]) or field access (obj.prop)
    case ArrIndexTag:
    case StrFieldTag:
    {
        FnCallNode *element = (FnCallNode *)lval;
        INode *lvalvar = borrowGetVarPerm(element->objfn, lvalperm);
        if (lvalvar == NULL)
            return NULL;
        INode *lvaltype = iexpGetTypeDcl((INode*)lvalvar);
        if (lvaltype->tag == RefTag || lvaltype->tag == ArrayRefTag) {
            *lvalperm = ((RefNode*)lvaltype)->perm; // implicit deref 
        }
        /*
        if (lvalperm = permIsLocked(*lvalperm) && var_is_not_unlocked) {
            errorMsgNode(lval, ErrorNotLval, "Cannot borrow a substructure reference from a locked variable");
            return NULL
        } */
        if (element->methfld) {
            PermNode *methperm = (PermNode *)((VarDclNode*)((NameUseNode *)element->methfld)->dclnode)->perm;
            // Downgrade overall static permission if field is immutable
            if (methperm == immPerm)
                *lvalperm = (INode*)constPerm;
        }
        return lvalvar;
    }

    // One cannot borrow a reference from any other node
    default:
        errorMsgNode(lval, ErrorNotLval, "You can only borrow a reference from a variable, function, or dereference");
        return NULL;
    }
}

// Analyze borrow node
void borrowTypeCheck(TypeCheckState *pstate, BorrowNode **nodep) {
    BorrowNode *node = *nodep;
    inodeTypeCheck(pstate, &node->exp);

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
    if (lval->tag != StrLitTag) {
        // lval is the variable or variable sub-structure we want to get a reference to
        // From it, obtain variable we are borrowing from and actual/calculated permission
        INode *lvalvar = borrowGetVarPerm(lval, &lvalperm);
        if (lvalvar == NULL) {
            reftype->pvtype = (INode*)voidType;  // Just to avoid a compiler crash later
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
    RefNode *refnode = newRefNodeFull(voidType, newPermUseNode(mutPerm), type);
    inodeLexCopy((INode*)refnode, *node);
    BorrowNode *borrownode = newBorrowNode();
    inodeLexCopy((INode*)borrownode, *node);
    borrownode->exp = *node;
    borrownode->vtype = (INode*)refnode;
    *node = (INode*)borrownode;
}
