/** Handling for type literals: a struct's value built from its fields,
 * 'Point[1., 2.]', and a number's conversion, 'u64.from(count)', which type
 * check lowers into the same node (fnCallNumberFrom)
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Is this a number type, the target of a conversion rather than a struct literal?
static int typeLitIsNbrType(INode *littype) {
    return littype->tag == IntNbrTag || littype->tag == UintNbrTag || littype->tag == FloatNbrTag;
}

// Serialize a type literal: a struct's as written, a number's conversion as
// the call it was written as
void typeLitPrint(FnCallNode *node) {
    int conversion = node->vtype && typeLitIsNbrType(itypeGetTypeDcl(node->vtype));
    if (node->objfn)
        inodePrintNode(node->objfn);
    INode **nodesp;
    uint32_t cnt;
    inodeFprint(conversion ? ".from(" : "[");
    for (nodesFor(node->args, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt)
            inodeFprint(",");
    }
    inodeFprint(conversion ? ")" : "]");
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

// Type check the value a number's 'from' converts, its one argument, already
// type checked. Returns 0 when it does not convert.
int typeLitNbrFromCheck(FnCallNode *conv, INode *type) {
    INode *first = nodesGet(conv->args, 0);
    INode *firsttype = itypeGetTypeDcl(((IExpNode*)first)->vtype);

    // 'Bool.from(value)' is the same conversion as 'value into Bool', so it
    // accepts whatever that accepts -- a reference or a pointer included, which
    // convert by asking whether they are non-null. Every other number type
    // requires a number source: a pointer has no conversion to an integer on
    // either path.
    if (type == (INode*)boolType) {
        if (!castConvertsToBool(firsttype)) {
            errorMsgNode((INode*)first, ErrorNbrFrom, "Bool.from converts a number, a reference or a pointer");
            return 0;
        }
        return 1;
    }

    if (!typeLitIsNbrType(firsttype)) {
        errorMsgNode((INode*)first, ErrorNbrFrom, "%s.from converts a number", &((NbrNode*)type)->namesym->namestr);
        return 0;
    }
    return 1;
}

// Return true if desired named field is found and swapped into place
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

// Reorder the literal's field values to the same order as the type's fields
// Also prevent the specification of a value for a private field outside the type's methods
int typeLitStructReorder(FnCallNode *arrlit, StructNode *strnode, int private) {

    int retcode = 1;
    INode **nodesp;
    uint32_t cnt;
    uint32_t argi = 0;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;

        // If field represents a discriminated tag, inject struct's discriminant nbr.
        // A negative one's 64 bits are cut to the discriminant's width where the
        // literal is generated.
        if (field->flags & IsTagField) {
            ULitNode *tagnbrnode = newULitNodeTC((uint64_t)strnode->tagnbr, field->vtype);
            nodesInsert(&arrlit->args, (INode*)tagnbrnode, argi++);
            continue;
        }

        // A field value has been specified...
        if (argi < arrlit->args->used) {
            // If we have a named value, insert the proper named value here where it belongs
            INode **litval = &nodesGet(arrlit->args, argi);
            if ((*litval)->tag == NamedValTag && !typeLitGetName(arrlit->args, argi, field->namesym)) {
                // Use default value for unmatched field, if the type defined one.
                // The default is the type's own value, not one the literal gives,
                // so leaving a private field to it is allowed from anywhere.
                if (field->value) {
                    nodesInsert(&arrlit->args, field->value, argi);
                    ++argi;
                    continue;
                }
                else {
                    errorMsgNode((INode*)arrlit, ErrorBadArray, "Cannot find named value matching the field %s", &field->namesym->namestr);
                    ++argi;
                    retcode = 0;
                    continue;
                }
            }
            // Don't allow the literal to give a value for a private field outside of the type's methods
            if (!private && inodeIsPrivate((INode*)field)) {
                errorMsgNode(*litval, ErrorNotTyped, "Only a method in the type may specify a value for the private field %s.", &field->namesym->namestr);
                retcode = 0;
            }
        }
        // Append default value if no value specified
        else if (field->value) {
            nodesAdd(&arrlit->args, field->value);
        }
        else {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Not enough values specified on type literal");
            while (cnt--)
                nodesAdd(&arrlit->args, (INode*)newULitNodeTC(0,field->vtype));  // Put in fake nodes to pretend we are ok
            return 0;
        }
        ++argi;
    }
    if (argi < arrlit->args->used) {
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Too many values specified on type literal");
        retcode = 0;
    }
    return retcode;
}

// Type check a struct literal
void typeLitStructCheck(TypeCheckState *pstate, FnCallNode *arrlit, StructNode *strnode) {

    // Ensure type has been type-checked, in case any rewriting/semantic analysis was needed
    itypeTypeCheck(pstate, &arrlit->vtype);

    // Reorder the literal's arguments to match the type's field order. A private
    // field is given a value by the type's own code, or by any code inside the
    // braces of the enum it belongs to (structEnumSeesPrivate).
    int private = (INode*)strnode == pstate->typenode || structEnumSeesPrivate(pstate, (INode*)strnode);
    if (typeLitStructReorder(arrlit, strnode, private) == 0)
        return;

    uint32_t cnt;
    INode **nodesp;
    uint32_t argi = 0;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        INode **litval = &nodesGet(arrlit->args, argi);
        // A value given by name is coerced inside its NamedValNode: the coercion
        // must see the value itself, so a string literal borrows as an lval and
        // an untyped number literal adopts the field's type, as by position.
        NamedValNode *named = (*litval)->tag == NamedValTag ? (NamedValNode*)*litval : NULL;
        if (named)
            litval = &named->val;
        // Coerce the value to the field's type rather than demanding an exact
        // match: a field takes a value on the same terms a variable initializer
        // does, including a union variant standing in for the union.
        if (!iexpCoerce(litval, field->vtype)) {
            errorMsgNode((INode*)*litval, ErrorBadArray, "Literal value's type does not match expected field's type");
        }
        if (named)
            named->vtype = ((IExpNode*)named->val)->vtype;
        ++argi;
    }
}

// Check the list node
// Note:  We get here from FnCallTypeCheck, which has already checked that all arguments are expressions
// Perform data flow analysis on a type literal's field values.
// A literal initializes its fields from expressions, so each value is moved or
// copied exactly as a function call's argument is. A value given by field name
// is wrapped in a NamedValNode, which is unwrapped here so the decision is made
// about the value itself.
void typeLitFlow(FlowState *fstate, FnCallNode **nodep) {
    INode **argsp;
    uint32_t cnt;
    uint16_t inflight = fstate->inflightcnt;
    for (nodesFor((*nodep)->args, cnt, argsp)) {
        INode **valp = (*argsp)->tag == NamedValTag ? &((NamedValNode *)*argsp)->val : argsp;
        flowLoadValue(fstate, valp);
        flowHandleMoveOrCopy(valp);
        flowGateOperand(fstate, *valp);
    }
    flowGateOperandsEnd(fstate, inflight);
}

void typeLitTypeCheck(TypeCheckState *pstate, FnCallNode *arrlit) {

    INode *littype = itypeGetTypeDcl(arrlit->vtype);
    if (!itypeIsConcrete(arrlit->vtype))
        errorMsgNode((INode*)arrlit, ErrorInvType, "Type must be concrete and instantiable.");
    else if (littype->tag == StructTag)
        typeLitStructCheck(pstate, arrlit, (StructNode*)littype);
    // A number is not built from brackets: its conversion is a method,
    // 'u64.from(count)'. Refused here, at type check, so that a number reached
    // through an alias or a type parameter is refused as one named directly.
    else if (typeLitIsNbrType(littype)) {
        Name *written = isNameUseNode(arrlit->objfn) ? ((NameUseNode*)arrlit->objfn)->namesym : ((NbrNode*)littype)->namesym;
        errorMsgNode((INode*)arrlit, ErrorNbrBracket,
            "A number type takes no '[...]': a conversion is its method, %s.from(value)", &written->namestr);
    }
    else  // ArrayTag is dispatched in a different way and should never get here
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Unknown type literal type for type checking");
}
