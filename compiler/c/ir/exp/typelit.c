/** Handling for type literals and constructions: a struct's value built from
 * its fields, which 'new Point(1., 2.)' lowers to when the struct's implicit
 * 'init' takes its arguments (typeLitNewCheck), or which an enum's variant
 * and an allocation's value write in brackets, 'Some[x]'; and a number's
 * conversion, 'u64.from(count)', which type check lowers into the same node
 * (fnCallNumberFrom)
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

// Is this struct a variant, built with its discriminant: an enum's, or a
// tagged trait's?
static int typeLitIsVariant(StructNode *strnode) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp))
        if ((*nodesp)->flags & IsTagField)
            return 1;
    return 0;
}

// Is this type-checked value one 'new' constructs: a construction, or a struct
// that is not a variant written in brackets?
int typeLitIsConstruction(INode *node) {
    if (node->flags & FlagNew)
        return 1;
    if (node->tag != TypeLitTag)
        return 0;
    INode *littype = itypeGetTypeDcl(((FnCallNode*)node)->vtype);
    return littype->tag == StructTag && !typeLitIsVariant((StructNode*)littype);
}

// Serialize a type literal: a construction, a variant's or an allocation's
// value, and a number's conversion, each as it was written
void typeLitPrint(FnCallNode *node) {
    int conversion = node->vtype && typeLitIsNbrType(itypeGetTypeDcl(node->vtype));
    int paren = conversion || (node->flags & FlagNew);
    if (node->flags & FlagNew)
        inodeFprint("new ");
    if (node->objfn)
        inodePrintNode(node->objfn);
    INode **nodesp;
    uint32_t cnt;
    inodeFprint(conversion ? ".from(" : paren ? "(" : "[");
    for (nodesFor(node->args, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt)
            inodeFprint(",");
    }
    inodeFprint(paren ? ")" : "]");
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

// May a value of fromtype be converted to Bool? A number may, and so may a
// reference or a pointer, which converts by asking whether it is non-null, as a
// condition asks
static int typeLitConvertsToBool(INode *fromtype) {
    switch (fromtype->tag) {
    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
    case RefTag:
    case PtrTag:
        return 1;
    default:
        return 0;
    }
}

// Type check the value a number's 'from' converts, its one argument, already
// type checked. Returns 0 when it does not convert.
int typeLitNbrFromCheck(FnCallNode *conv, INode *type) {
    INode *first = nodesGet(conv->args, 0);
    INode *firsttype = itypeGetTypeDcl(((IExpNode*)first)->vtype);

    // 'Bool.from(value)' accepts a reference or a pointer too. Every other
    // number type requires a number source: a pointer has no conversion to an
    // integer.
    if (type == (INode*)boolType) {
        if (!typeLitConvertsToBool(firsttype)) {
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
    else if (littype->tag == StructTag) {
        // A struct's value is constructed with 'new'. An enum's variant keeps
        // its brackets, 'Some[x]'; a '+' allocation's value, '+Rc-mut Node[1]',
        // is refused by allocateTypeCheck instead (ErrorPlusAlloc). Refused
        // here, at type check, so that a struct reached through an alias or a
        // type parameter is refused as one named directly. The literal is
        // still built, so nothing after it reports again.
        if (!(arrlit->flags & (FlagNew | FlagAllocValue)) && !typeLitIsVariant((StructNode*)littype)) {
            Name *written = isNameUseNode(arrlit->objfn) ? ((NameUseNode*)arrlit->objfn)->namesym : ((StructNode*)littype)->namesym;
            errorMsgNode((INode*)arrlit, ErrorStructBracket,
                "A struct's value is constructed with 'new', its init's arguments in parentheses: 'new %s(...)'.", &written->namestr);
        }
        typeLitStructCheck(pstate, arrlit, (StructNode*)littype);
    }
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

// Can a declared init take these arguments after its 'self'? Viability only,
// as fnSigViableCall decides it for a call: the count, the defaults, and each
// argument passable to its parameter.
static int typeLitInitViable(FnDclNode *init, Nodes *args) {
    if (init->genericinfo)
        return 0;
    FnSigNode *sig = (FnSigNode*)init->vtype;
    uint32_t nparms = sig->parms->used - 1;
    if (args->used > nparms)
        return 0;
    INode **parmp = &nodesGet(sig->parms, 1);
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(args, cnt, argsp)) {
        if (iexpMatches(argsp, ((IExpNode *)*parmp)->vtype, Coercion) == NoMatch)
            return 0;
        ++parmp;
    }
    for (uint32_t i = args->used; i < nparms; ++i) {
        if (((VarDclNode *)*parmp++)->value == NULL)
            return 0;
    }
    return 1;
}

// Can the struct's implicit init, which takes its fields in the order they are
// declared, take these values by position, a field left out taking its default?
static int typeLitImplicitViable(StructNode *strnode, Nodes *args) {
    uint32_t argi = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        if (argi < args->used) {
            if (iexpMatches(&nodesGet(args, argi), field->vtype, Coercion) == NoMatch)
                return 0;
        }
        else if (field->value == NULL)
            return 0;
        ++argi;
    }
    return argi >= args->used;
}

// Coerce a declared init's arguments to its parameters after 'self', and
// append the defaults of those left out, as fnCallFinalizeArgs does for a call
static void typeLitInitArgs(FnCallNode *node, FnSigNode *sig) {
    INode **parmp = &nodesGet(sig->parms, 1);
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(node->args, cnt, argsp)) {
        if (!iexpCoerce(argsp, ((IExpNode *)*parmp)->vtype))
            errorMsgNode(*argsp, ErrorInvType, "Expression's type does not match declared parameter");
        ++parmp;
    }
    for (uint32_t i = node->args->used + 1; i < sig->parms->used; ++i) {
        // 'srcFile()' and 'srcLine()' answer where the construction is
        INode *dflt = ((VarDclNode *)nodesGet(sig->parms, i))->value;
        if (intrinsicIsSrcCall(dflt))
            dflt = intrinsicSrcCallAt(dflt, (INode*)node);
        nodesAdd(&node->args, dflt);
    }
}

// 'new Rc[mut, Node](1)': an allocation in the region a managed reference type
// names, written out or through an alias of it ('new Node(1)' for 'alias Node
// = Gc[mut, NodeValue]'). The parentheses are the value's: they become the
// construction 'new NodeValue(1)', its init chosen as any value's is, which the
// allocation node holds as its value, so that generation runs an allocation's
// order: the init's arguments, 'alloc', the region's init, the permission's,
// the value's init in place, the destination (genlallocref). 'trynew' types
// the allocation as an Option of the reference, None when memory runs out.
static void typeLitNewAllocate(TypeCheckState *pstate, FnCallNode **nodep, RefNode *reftype, INode *option) {
    FnCallNode *node = *nodep;
    if (reftype->region == borrowRef) {
        errorMsgNode((INode*)node, ErrorNewType,
            "'new' allocates in a region, and a borrowed reference has none: it is made by borrowing a value, '&x'.");
        return;
    }
    if (reftype->tag == VirtRefTag) {
        errorMsgNode((INode*)node, ErrorNewType,
            "A virtual reference refers to a trait, which has no value to construct: allocate a type implementing it, as 'new Rc[mut, Rect](...)', and the reference coerces where the virtual one is wanted.");
        return;
    }

    FnCallNode *value = newFnCallNode(reftype->vtexp, 0);
    inodeLexCopy((INode*)value, (INode*)node);
    value->flags |= FlagNew;
    value->args = node->args;

    RefNode *alloc = newRefNode(AllocateTag);
    inodeLexCopy((INode*)alloc, (INode*)node);
    alloc->region = reftype->region;
    alloc->perm = reftype->perm;
    alloc->vtexp = (INode*)value;
    // Typed as written, so a rule about the reference type is judged once
    alloc->vtype = (INode*)reftype;
    if (option) {
        FnCallNode *opttype = newFnCallNode(option, 1);
        inodeLexCopy((INode*)opttype, (INode*)node);
        nodesAdd(&opttype->args, (INode*)reftype);
        alloc->flags |= FlagQues;
        alloc->vtype = (INode*)opttype;
    }
    *((INode**)nodep) = (INode*)alloc;
    allocateTypeCheck(pstate, (RefNode**)nodep);
}

// 'new Point(1, 2)': construct a struct's value by one of its inits, which the
// arguments select. Every struct has an implicit init taking its fields in
// declaration order (by name too, 'new Point(y: 2, x: 1)', and a field left out
// taking its default), and it may declare others, 'fn init(self &new, ...)',
// overloaded under the name 'init'. Exactly one of them must take the
// arguments; named ones are the implicit init's alone, since no call takes
// them. The implicit init is lowered to the struct's literal, whose fields are
// stored straight into wherever the value goes; a declared one to a call of it,
// which generation hands the memory the value goes into (genlNew).
void typeLitNewCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    // Already lowered to its declared init's call, and reached again
    if (nameUseNames(node->objfn, FnDclTag))
        return;
    node->vtype = errorType;    // until a value is known to come of it
    // 'trynew': the bound 'Option' name resolution left in methfld
    INode *option = NULL;
    if (node->flags & FlagTryNew) {
        option = node->methfld;
        node->methfld = NULL;
    }

    if (!isTypeNode(node->objfn)) {
        Name *written = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->namesym : NULL;
        errorMsgNode(node->objfn, ErrorNewType, "'new' constructs a value of a type, and %s is not one.",
            written ? &written->namestr : "this");
        return;
    }

    INode **argsp;
    uint32_t cnt;
    // A generic struct named without its type arguments, 'new Box(5i64)', has
    // them inferred from the values its fields are given, as its literal did:
    // the arguments are checked first, with no expectation, and the instance
    // they infer replaces the name
    int argschecked = 0;
    if (isNameUseNode(node->objfn) && nameUseNames(node->objfn, StructTag)
        && genericGetInfo(nameUseGetDcl((NameUseNode*)node->objfn)) != NULL) {
        for (nodesFor(node->args, cnt, argsp))
            inodeTypeCheck(pstate, argsp, unknownType);
        argschecked = 1;
        if (genericSubstitute(pstate, nodep))
            return;
        node = *nodep;
    }
    if (!itypeTypeCheck(pstate, &node->objfn))
        return;

    INode *typedcl = itypeGetTypeDcl(node->objfn);
    Name *written = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->namesym : NULL;
    if (typedcl->tag == RefTag || typedcl->tag == VirtRefTag) {
        typeLitNewAllocate(pstate, nodep, (RefNode*)typedcl, option);
        return;
    }
    if (option) {
        errorMsgNode((INode*)node, ErrorTryNewValue,
            "'trynew' is an allocation in a region, which may run out of memory, and %s is not a managed reference type: a value is constructed with 'new'.",
            written ? &written->namestr : "this type");
        return;
    }
    if (typeLitIsNbrType(typedcl)) {
        errorMsgNode((INode*)node, ErrorNewType,
            "A number is not constructed: it is written as a literal, or converted with its method, %s.from(value).",
            written ? &written->namestr : &((NbrNode*)typedcl)->namesym->namestr);
        return;
    }
    // An array's contents are its construction (contentsLowerArray), which
    // gives them after '<-'
    if (typedcl->tag == ArrayTag) {
        errorMsgNode((INode*)node, ErrorArrayContents,
            "An array is constructed with the values that fill it, after '<-': 'new Array[f32, 4] <- fill 0.0'.");
        return;
    }
    if (typedcl->tag != StructTag || (typedcl->flags & TraitType)) {
        errorMsgNode((INode*)node, ErrorNewType, "'new' constructs a struct's value, and %s is not a struct.",
            written ? &written->namestr : "this type");
        return;
    }
    StructNode *strnode = (StructNode*)typedcl;
    if (typeLitIsVariant(strnode)) {
        errorMsgNode((INode*)node, ErrorNewType,
            "%s is an enum's variant, which is constructed with its brackets for now: '%s[...]'.",
            &strnode->namesym->namestr, &strnode->namesym->namestr);
        return;
    }

    // The inits it declares, whose signatures are wanted before the arguments
    // are matched to them
    INode *inits = iNsTypeFindFnField((INsTypeNode*)strnode, initMethodName);
    if (inits && inits->tag != FnDclTag && inits->tag != FnOverloadDclTag)
        inits = NULL;
    if (inits)
        fnCallDemandCandidates(inits);

    // Each argument is checked against the field it fills when the implicit
    // init is the only one, as a literal's values are; otherwise with no
    // expectation, as an overload set's arguments are
    uint32_t argi = 0;
    int named = 0;
    for (nodesFor(node->args, cnt, argsp)) {
        INode *expect = unknownType;
        if (inits == NULL) {
            FieldDclNode *field = fnCallTypeLitField(strnode, node->args, argi);
            if (field)
                expect = field->vtype;
        }
        if (!argschecked)
            inodeTypeCheck(pstate, argsp, expect);
        if ((*argsp)->tag == NamedValTag)
            named = 1;
        ++argi;
    }
    int badarg = 0;
    for (nodesFor(node->args, cnt, argsp)) {
        if (!isExpNode(*argsp)) {
            errorMsgNode(*argsp, ErrorNotTyped, "Expected a typed expression.");
            badarg = 1;
        }
        else if (inodeIsError(*argsp))
            badarg = 1;
    }
    if (badarg)
        return;

    // A name none of the fields has is meant for an init the struct declares,
    // which takes its arguments by position, as every call does
    if (named && inits) {
        for (nodesFor(node->args, cnt, argsp)) {
            if ((*argsp)->tag != NamedValTag)
                continue;
            Name *name = ((NameUseNode*)((NamedValNode*)*argsp)->name)->namesym;
            int isfield = 0;
            INode **fieldp;
            uint32_t fcnt;
            for (nodelistFor(&strnode->fields, fcnt, fieldp))
                if (((FieldDclNode*)*fieldp)->namesym == name)
                    isfield = 1;
            if (!isfield) {
                namedValRefuseArgs(node->args, "a call of an init a struct declares");
                return;
            }
        }
    }

    // Select the one init that takes the arguments
    FnDclNode *selected = NULL;
    uint32_t viable = 0;
    if (inits && !named) {
        INode **candp;
        uint32_t ncand;
        if (inits->tag == FnDclTag) {
            candp = &inits;
            ncand = 1;
        }
        else {
            candp = &nodesGet(((FnOverloadDclNode*)inits)->overloads, 0);
            ncand = ((FnOverloadDclNode*)inits)->overloads->used;
        }
        while (ncand--) {
            FnDclNode *cand = (FnDclNode*)*candp++;
            if (fnDclIsInit(cand) && typeLitInitViable(cand, node->args)) {
                selected = cand;
                ++viable;
            }
        }
    }
    int implicit = inits == NULL || named || typeLitImplicitViable(strnode, node->args);
    viable += implicit;
    if (viable != 1) {
        errorMsgNode((INode*)node, ErrorInitNone, viable == 0
            ? "No init of %s takes these arguments: neither its fields, in the order they are declared, nor an init it declares."
            : "More than one init of %s takes these arguments, so the construction cannot choose between them.",
            &strnode->namesym->namestr);
        return;
    }

    // The implicit init: the struct's literal
    if (implicit) {
        node->tag = TypeLitTag;
        node->vtype = node->objfn;
        typeLitTypeCheck(pstate, node);
        return;
    }

    // A declared init, called with the memory to fill. One not declared 'pub'
    // is the type's own.
    if (inodeIsPrivate(inits) && !structSeesPrivate(pstate, (INode*)strnode)) {
        errorMsgNode((INode*)node, ErrorNotPublic,
            "May not construct %s with its private init: only the type's own methods may, unless it is declared 'pub fn init'.",
            &strnode->namesym->namestr);
        return;
    }
    INode *type = node->objfn;
    node->objfn = newNameUseFromDclNode((INode*)selected, (INode*)node);
    typeLitInitArgs(node, (FnSigNode*)selected->vtype);
    node->vtype = type;
}
