/** Handling for list nodes (e.g., type literals)
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Serialize a list node
void typeLitPrint(FnCallNode *node) {
    if (node->objfn)
        inodePrintNode(node->objfn);
    INode **nodesp;
    uint32_t cnt;
    inodeFprint("[");
    for (nodesFor(node->args, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt)
            inodeFprint(",");
    }
    inodeFprint("]");
}

// Check the type literal node (actually done by fncall)
void typeLitNameRes(NameResState *pstate, FnCallNode *arrlit) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->args, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
}

// Is the type literal actually a literal?
int typeLitIsLiteral(FnCallNode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->args, cnt, nodesp)) {
        INode *arg = *nodesp;
        if (arg->tag == NamedValTag)
            arg = ((NamedValNode*)arg)->val;
        if (!litIsLiteral(arg))
            return 0;
    }
    return 1;
}

// Type check a number literal
void typeLitNbrCheck(TypeCheckState *pstate, FnCallNode *nbrlit, INode *type) {

    if (nbrlit->args->used != 1) {
        errorMsgNode((INode*)nbrlit, ErrorBadArray, "Number literal requires one value");
        return;
    }

    INode *first = nodesGet(nbrlit->args, 0);
    if (!isExpNode(first)) {
        errorMsgNode((INode*)first, ErrorBadArray, "Literal value must be typed");
        return;
    }
    INode *firsttype = itypeGetTypeDcl(((IExpNode*)first)->vtype);
    if (firsttype->tag != IntNbrTag && firsttype->tag != UintNbrTag && firsttype->tag != FloatNbrTag) 
        errorMsgNode((INode*)first, ErrorBadArray, "May only create number literal from another number");
}

// Type check an array literal
void typeLitArrayCheck(TypeCheckState *pstate, FnCallNode *arrlit) {

    if (arrlit->args->used == 0) {
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Literal list may not be empty");
        return;
    }

    // Get element type from first element
    // Type of array literal is: array of elements whose type matches first value
    INode *first = nodesGet(arrlit->args, 0);
    if (!isExpNode(first)) {
        errorMsgNode((INode*)first, ErrorBadArray, "Array literal element must be a typed value");
        return;
    }
    INode *firsttype = ((IExpNode*)first)->vtype;
    ((ArrayNode*)arrlit->vtype)->elemtype = firsttype;

    // Ensure all elements are consistently typed (matching first element's type)
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->args, cnt, nodesp)) {
        if (!itypeIsSame(((IExpNode*)*nodesp)->vtype, firsttype))
            errorMsgNode((INode*)*nodesp, ErrorBadArray, "Inconsistent type of array literal value");
    }
}

// Return true if desired named property is found and swapped into place
int typeLitGetName(Nodes *args, uint32_t argi, Name *name) {
    uint32_t nargs = args->used;
    uint32_t i = argi;
    for (; i < nargs; i++) {
        NamedValNode *node = (NamedValNode*)nodesGet(args, i);
        if (node->tag == NamedValTag && ((NameUseNode*)node->name)->namesym == name) {
            nodesMove(args, argi, i);
            return 1;
        }
    }
    return 0;
}

// Type check a struct literal
// Verify correct name/type of each literal element against struct's properties
void typeLitStructCheck(TypeCheckState *pstate, FnCallNode *arrlit, StructNode *strnode) {
    INode **nodesp;
    uint32_t cnt;
    uint32_t argi = 0;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        VarDclNode *prop = (VarDclNode *)*nodesp;

        // If element has been specified, be sure it matches both name & type
        if (argi < arrlit->args->used) {
            INode **litval = &nodesGet(arrlit->args, argi);
            if ((*litval)->tag == NamedValTag && !typeLitGetName(arrlit->args, argi, prop->namesym)) {
                errorMsgNode((INode*)*litval, ErrorBadArray, "Cannot find struct property matching this name");
                ++argi;
                continue;
            }
            if (!iexpSameType(*nodesp, litval))
                errorMsgNode((INode*)*litval, ErrorBadArray, "Literal value's type does not match expected field's type");
        }
        // Append default value if no value specified
        else if (prop->value) {
            nodesAdd(&arrlit->args, prop->value);
        }
        else {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Not enough values specified on struct literal");
            break;
        }
        ++argi;
    }
    if (argi < arrlit->args->used)
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Too many values specified on struct literal");
}

// Check the list node
void typeLitTypeCheck(TypeCheckState *pstate, FnCallNode *arrlit) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->args, cnt, nodesp))
        inodeTypeCheck(pstate, nodesp);

    INode *littype = itypeGetTypeDcl(arrlit->vtype);
    if (littype->tag == ArrayTag)
        typeLitArrayCheck(pstate, arrlit);
    else if (littype->tag == StructTag)
        typeLitStructCheck(pstate, arrlit, (StructNode*)littype);
    else if (littype->tag == IntNbrTag || littype->tag == UintNbrTag || littype->tag == FloatNbrTag)
        typeLitNbrCheck(pstate, arrlit, littype);
    else
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Unknown type literal type for type checking");
}
