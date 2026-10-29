/** Handling for the SIMD vector type, 'Vector[T, N]'
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <stdio.h>
#include <string.h>

// Create a new vector type node, its lane type and count filled in afterwards
VectorNode *newVectorNode() {
    VectorNode *vec;
    newNode(vec, VectorNode, VectorTag);
    vec->vtype = unknownType;
    vec->llvmtype = NULL;
    iNsTypeInit((INsTypeNode*)vec, 64);
    vec->dimens = newNodes(1);
    vec->elems = newNodes(1);
    vec->mask = NULL;
    return vec;
}

// A vector type made of a lane type already checked and a count
static VectorNode *vectorNewTyped(INode *lexnode, uint64_t lanes, INode *elemtype) {
    VectorNode *vec = newVectorNode();
    inodeLexCopy((INode*)vec, lexnode);
    nodesAdd(&vec->dimens, (INode*)newULitNode(lanes, (INode*)u64Type));
    nodesAdd(&vec->elems, elemtype);
    return vec;
}

INode *vectorElemType(INode *vec) {
    return nodesGet(((VectorNode*)vec)->elems, 0);
}

uint64_t vectorLanes(INode *vec) {
    INode *dim = nodesGet(((VectorNode*)vec)->dimens, 0);
    return dim->tag == ULitTag ? ((ULitNode*)dim)->uintlit : 0;
}

int vectorIsMask(INode *vec) {
    return itypeGetTypeDcl(vectorElemType(vec)) == (INode*)boolType;
}

INode *vectorMaskType(INode *vec) {
    return (INode*)((VectorNode*)vec)->mask;
}

// Clone a vector type. The copy has no methods and no mask of its own yet:
// those are made when it is checked, and the original's name its own type.
INode *cloneVectorNode(CloneState *cstate, VectorNode *node) {
    VectorNode *newnode = memAllocBlk(sizeof(VectorNode));
    memcpy(newnode, node, sizeof(VectorNode));
    iNsTypeInit((INsTypeNode*)newnode, 64);
    newnode->llvmtype = NULL;
    newnode->mask = NULL;
    newnode->flags &= ~(NameResolved | TypeChecked | TypeChecking);
    newnode->elems = cloneNodes(cstate, node->elems);
    newnode->dimens = cloneNodes(cstate, node->dimens);
    return (INode*)newnode;
}

// Serialize a vector type as it is written, 'Vector[f32, 4]'
void vectorPrint(VectorNode *node) {
    inodeFprint("Vector[");
    inodePrintNode(nodesGet(node->elems, 0));
    inodeFprint(", ");
    inodePrintNode(nodesGet(node->dimens, 0));
    inodeFprint("]");
}

// Name resolution of a vector type: its lane type and count
void vectorNameRes(NameResState *pstate, VectorNode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->elems, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
    for (nodesFor(node->dimens, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
}

// Lower the type 'Vector[T, N]' into the vector type node it names. 'Vector'
// is a name every module reaches (stdlibInit); it looks like a generic type
// with a number parameter, but nothing is instantiated: the call becomes the
// VectorNode here, so from name resolution on nothing sees how it was written,
// as an array type's is (arrayTypeLower). The lane type and count are checked
// when the type is (vectorTypeCheck). A lane count that is a generic's value
// parameter is the number the clone substitutes.
void vectorTypeLower(NameResState *pstate, INode **nodep) {
    FnCallNode *node = (FnCallNode*)*nodep;
    Nodes *args = node->args;
    INode *where = node->objfn;
    if ((node->flags & FlagRange) || args == NULL || args->used != 2) {
        errorMsgNode(where, ErrorVectorArgs,
            "A vector type names its lane type, then its number of lanes: 'Vector[T, N]', as 'Vector[f32, 4]'.");
        *((INode**)nodep) = newErrorNode(where);
        return;
    }
    INode *elemtype = nodesGet(args, 0);
    if (!isTypeNode(elemtype) && !inodeIsProvisionalType(elemtype)) {
        errorMsgNode(elemtype, ErrorVectorArgs,
            "A vector type's first argument is its lane type, and the number of lanes follows it: 'Vector[T, N]'.");
        *((INode**)nodep) = newErrorNode(where);
        return;
    }
    INode *size = nodesGet(args, 1);
    if (size->tag != ULitTag && !isNameUseNode(size) && genericMentionsValueParm(size)) {
        errorMsgNode(size, ErrorGenValueArith,
            "A vector's lane count is a number or a value parameter alone, 'Vector[T, N]'. Arithmetic over a value parameter in a type, like this one, is not supported.");
        *((INode**)nodep) = newErrorNode(where);
        return;
    }
    VectorNode *vec = newVectorNode();
    inodeLexCopy((INode*)vec, where);
    nodesAdd(&vec->dimens, size);
    nodesAdd(&vec->elems, elemtype);
    *((INode**)nodep) = (INode*)vec;
}

// ---- The methods a vector has ---------------------------------------------

// A use of a number type, as a signature names it
static INode *vecNbrUse(INode *nbr) {
    NameUseNode *use = newNameUseNode(((NbrNode*)nbr)->namesym);
    use->dclnode = nbr;
    return (INode*)use;
}

static FnSigNode *vecSig(INode *rettype, INode *self, INode *other) {
    FnSigNode *sig = newFnSigNode();
    sig->rettype = rettype;
    nodesAdd(&sig->parms, (INode*)newVarDclFull(nametblFind("a", 1), VarDclTag, self, newPermUseNode(immPerm), NULL));
    if (other)
        nodesAdd(&sig->parms, (INode*)newVarDclFull(nametblFind("b", 1), VarDclTag, other, newPermUseNode(immPerm), NULL));
    return sig;
}

static void vecAdd(VectorNode *vec, Name *name, FnSigNode *sig, int kind) {
    iNsTypeAddFn((INsTypeNode*)vec,
        newFnDclNode(name, FlagMethFld | FlagPub, (INode*)sig, (INode*)newIntrinsicNode((int16_t)kind)));
}

// Several declarations share an operator's name ('-' is unary and binary): each
// has a concrete name of its own, and joins the overload set the operator names
static void vecAddOverload(VectorNode *vec, char *concrete, Name *opsym, FnSigNode *sig, int kind) {
    FnDclNode *fn = newFnDclNode(nametblFind(concrete, strlen(concrete)), FlagMethFld, (INode*)sig,
        (INode*)newIntrinsicNode((int16_t)kind));
    fn->overloadsym = opsym;
    iNsTypeAddFn((INsTypeNode*)vec, fn);
}

// A lane-wise method, 'abs', 'min' with an argument, 'floor' and the rest: the
// number intrinsic of the same name (corenumber.c, nbrMathMethods), its node
// carrying the vector as its typearg, which is how one lowering
// (genlNumberIntrinsic) tells a lane's type for a number or a vector. When
// 'overloadsym' is given, several declarations share the method's name, as
// 'min' is both the reduction and the lane-wise minimum, told by their arguments.
static void vecAddLanewise(VectorNode *vec, char *name, Name *overloadsym, FnSigNode *sig, int kind) {
    IntrinsicNode *node = newIntrinsicNode((int16_t)kind);
    node->typearg = (INode*)vec;
    FnDclNode *fn;
    if (overloadsym) {
        fn = newFnDclNode(nametblFind(name, strlen(name)), FlagMethFld, (INode*)sig, (INode*)node);
        fn->overloadsym = overloadsym;
    }
    else
        fn = newFnDclNode(nametblFind(name, strlen(name)), FlagMethFld | FlagPub, (INode*)sig, (INode*)node);
    iNsTypeAddFn((INsTypeNode*)vec, fn);
}

// The operators and lane methods a vector's lane type allows, each an intrinsic
// generation expands on the whole vector. They mirror what the scalar type
// has (corenumber.c), lane by lane, and a mask has the logical ones. Mixed
// operands (a number beside a vector) are broadcast before selection
// (vectorBroadcastOperand), so each is declared for vectors alone.
static void vectorMethods(VectorNode *vec, INode *lane) {
    int ismask = lane == (INode*)boolType;
    int isfloat = lane->tag == FloatNbrTag;
    int issigned = lane->tag == IntNbrTag;
    INode *laneuse = vecNbrUse(lane);
    INode *self = (INode*)vec;
    INode *mask = (INode*)vec->mask;
    FnSigNode *unary = vecSig(self, self, NULL);
    FnSigNode *binary = vecSig(self, self, self);
    FnSigNode *compare = vecSig(mask, self, self);
    FnSigNode *reduce = vecSig(laneuse, self, NULL);
    FnSigNode *test = vecSig((INode*)boolType, self, NULL);

    if (!ismask) {
        vecAddOverload(vec, "_neg", minusName, unary, NegIntrinsic);
        vecAdd(vec, plusName, binary, AddIntrinsic);
        vecAddOverload(vec, "_sub", minusName, binary, SubIntrinsic);
        vecAdd(vec, multName, binary, MulIntrinsic);
        vecAdd(vec, divName, binary, isfloat ? DivIntrinsic : issigned ? SDivIntrinsic : DivIntrinsic);
        // A float lane's '%' is the scalar's: frem, the sign of the dividend
        vecAdd(vec, remName, binary, isfloat ? RemIntrinsic : issigned ? SRemIntrinsic : RemIntrinsic);
        vecAdd(vec, nametblFind("sum", 3), reduce, VecSumIntrinsic);
        vecAdd(vec, nametblFind("product", 7), reduce, VecProductIntrinsic);
        // 'min' and 'max' with no argument reduce the lanes to a number; with
        // one they are the lane-wise minimum and maximum, a vector. The two are
        // an overload set told by the arguments
        Name *minsym = nametblFind("min", 3);
        Name *maxsym = nametblFind("max", 3);
        vecAddLanewise(vec, "_min_reduce", minsym, reduce, issigned ? VecSMinIntrinsic : VecMinIntrinsic);
        vecAddLanewise(vec, "_min_lanes", minsym, binary, MinIntrinsic);
        vecAddLanewise(vec, "_max_reduce", maxsym, reduce, issigned ? VecSMaxIntrinsic : VecMaxIntrinsic);
        vecAddLanewise(vec, "_max_lanes", maxsym, binary, MaxIntrinsic);
        if (isfloat || issigned)
            vecAddLanewise(vec, "abs", NULL, unary, AbsIntrinsic);
    }
    if (isfloat) {
        vecAdd(vec, nametblFind("sqrt", 4), unary, SqrtIntrinsic);
        vecAdd(vec, nametblFind("sin", 3), unary, SinIntrinsic);
        vecAdd(vec, nametblFind("cos", 3), unary, CosIntrinsic);
        vecAddLanewise(vec, "floor", NULL, unary, FloorIntrinsic);
        vecAddLanewise(vec, "ceil", NULL, unary, CeilIntrinsic);
        vecAddLanewise(vec, "round", NULL, unary, RoundIntrinsic);
        // Named as a number's 'fma' is (corenumber.c), since named arguments are part of the surface
        FnSigNode *fma = newFnSigNode();
        fma->rettype = self;
        nodesAdd(&fma->parms, (INode*)newVarDclFull(nametblFind("self", 4), VarDclTag, self, newPermUseNode(immPerm), NULL));
        nodesAdd(&fma->parms, (INode*)newVarDclFull(nametblFind("y", 1), VarDclTag, self, newPermUseNode(immPerm), NULL));
        nodesAdd(&fma->parms, (INode*)newVarDclFull(nametblFind("z", 1), VarDclTag, self, newPermUseNode(immPerm), NULL));
        vecAddLanewise(vec, "fma", NULL, fma, FmaIntrinsic);
    }
    else {
        vecAdd(vec, nametblFind("~", 1), unary, NotIntrinsic);
        vecAdd(vec, andName, binary, AndIntrinsic);
        vecAdd(vec, orName, binary, OrIntrinsic);
        vecAdd(vec, xorName, binary, XorIntrinsic);
        if (!ismask) {
            vecAdd(vec, shlName, binary, ShlIntrinsic);
            vecAdd(vec, shrName, binary, issigned ? SShrIntrinsic : ShrIntrinsic);
        }
    }
    vecAdd(vec, eqName, compare, EqIntrinsic);
    vecAdd(vec, neName, compare, NeIntrinsic);
    if (!ismask) {
        vecAdd(vec, ltName, compare, issigned ? SLtIntrinsic : LtIntrinsic);
        vecAdd(vec, leName, compare, issigned ? SLeIntrinsic : LeIntrinsic);
        vecAdd(vec, gtName, compare, issigned ? SGtIntrinsic : GtIntrinsic);
        vecAdd(vec, geName, compare, issigned ? SGeIntrinsic : GeIntrinsic);
    }
    else {
        vecAdd(vec, nametblFind("any", 3), test, VecAnyIntrinsic);
        vecAdd(vec, nametblFind("all", 3), test, VecAllIntrinsic);
    }
}

// Type check a vector type: the lane type is an integer or float type (or bool,
// which makes the mask), the count a literal from 1 through VectorMaxLanes
void vectorTypeCheck(TypeCheckState *pstate, VectorNode *node) {
    int ok = 1;
    INode *dim = nodesGet(node->dimens, 0);
    if (dim->tag != ULitTag) {
        errorMsgNode(dim, ErrorVectorLanes, "A vector's number of lanes must be an integer literal.");
        ok = 0;
    }
    else if (((dim->flags & FlagLitNeg) && ((ULitNode*)dim)->uintlit != 0) || ((ULitNode*)dim)->uintlit == 0) {
        errorMsgNode(dim, ErrorVectorLanes, "A vector has at least one lane.");
        ok = 0;
    }
    else if (((ULitNode*)dim)->uintlit > VectorMaxLanes) {
        errorMsgNode(dim, ErrorVectorLanes, "A vector has at most %d lanes.", VectorMaxLanes);
        ok = 0;
    }
    INode **elemp = &nodesGet(node->elems, 0);
    int errsbefore = errors;
    if (itypeTypeCheck(pstate, elemp) == 0) {
        if (errors == errsbefore)
            errorMsgNode((INode*)node, ErrorVectorElem, "A vector's lane type must be a known type.");
        ok = 0;
    }
    INode *lane = ok || errors == errsbefore ? itypeGetTypeDcl(*elemp) : NULL;
    if (lane && lane->tag != IntNbrTag && lane->tag != UintNbrTag && lane->tag != FloatNbrTag) {
        errorMsgNode(*elemp, ErrorVectorElem,
            "A vector's lanes are integers or floats, and %s is not a number type.", itypeName(*elemp));
        ok = 0;
    }
    else if (lane == (INode*)charType) {
        errorMsgNode(*elemp, ErrorVectorElem, "A vector's lanes are integers or floats, and a char is a code point, not a number.");
        ok = 0;
    }
    if (!ok || lane == NULL)
        return;
    // A comparison gives the mask; the mask's own is itself
    if (lane == (INode*)boolType)
        node->mask = node;
    else {
        VectorNode *mask = vectorNewTyped((INode*)node, ((ULitNode*)dim)->uintlit, (INode*)boolType);
        node->mask = mask;
        vectorTypeCheck(pstate, mask);
    }
    vectorMethods(node, lane);
}

// ---- Type identity ----------------------------------------------------------

// Compare two vector types to see if they are equivalent
int vectorEqual(VectorNode *node1, VectorNode *node2) {
    if (!itypeIsSame(vectorElemType((INode*)node1), vectorElemType((INode*)node2)))
        return 0;
    return vectorLanes((INode*)node1) == vectorLanes((INode*)node2);
}

// Is from-type a subtype of to-type (we know they are not the same): a vector
// never converts to another; '.from' says so
TypeCompare vectorMatches(VectorNode *to, VectorNode *from, SubtypeConstraint constraint) {
    (void)constraint;
    return vectorEqual(to, from) ? EqMatch : NoMatch;
}

// ---- Operands, conversions and lanes -----------------------------------------

static int vectorIsOpName(Name *op) {
    return op == plusName || op == minusName || op == multName || op == divName || op == remName
        || op == andName || op == orName || op == xorName || op == shlName || op == shrName
        || op == eqName || op == neName || op == ltName || op == leName || op == gtName || op == geName;
}

// A type that is a number, for a broadcast (bool is the mask's lane)
static int vectorIsNbrType(INode *type) {
    return type->tag == IntNbrTag || type->tag == UintNbrTag || type->tag == FloatNbrTag;
}

// Build a conversion node: a type literal of the vector (or array) type whose
// one argument, or three for a select, generation turns into the value
FnCallNode *vectorNewConvert(INode *vectype, INode *arg, INode *lexnode) {
    FnCallNode *lit = newFnCallNode(vectype, 1);
    inodeLexCopy((INode*)lit, lexnode);
    lit->tag = TypeLitTag;
    lit->vtype = vectype;
    if (lit->args == NULL)
        lit->args = newNodes(1);
    nodesAdd(&lit->args, arg);
    return lit;
}

// The scalar operand of an operator made a vector of the vector operand's lane
// type: converted to the lane type as an argument of that type is, then
// broadcast. Answers 0, having reported, when it is no number of that type.
static int vectorBroadcast(INode **scalarp, INode *vectype, INode *lexnode) {
    INode *lane = itypeGetTypeDcl(vectorElemType(vectype));
    INode *scalartype = iexpGetDerefTypeDcl(*scalarp);
    if (!isExpNode(*scalarp) || (!vectorIsNbrType(scalartype) && scalartype != (INode*)boolType)) {
        errorMsgNode(lexnode, ErrorVectorMixed,
            "A vector's operator takes another vector of the same lanes or a number of the lane type, %s, to use in every lane.",
            itypeName(lane));
        return 0;
    }
    derefInject(scalarp);
    if (!iexpCoerce(scalarp, lane)) {
        errorMsgNode(*scalarp, ErrorVectorMixed,
            "This number is %s, and the vector's lanes are %s. Convert it, '%s.from(x)', or the vector, 'Vector[%s, %llu].from(v)', so both are the same.",
            itypeName(iexpGetTypeDcl(*scalarp)), itypeName(lane), itypeName(lane), itypeName(lane),
            (unsigned long long)vectorLanes(vectype));
        return 0;
    }
    FnCallNode *conv = vectorNewConvert(vectype, *scalarp, *scalarp);
    *scalarp = (INode*)conv;
    return 1;
}

// An operator beside a vector and a number: the number is broadcast to a vector
// of the lane type (a literal takes the lane type), so 'v * 2.0' and '2.0 * v'
// are 'v * [2.0, 2.0, ...]'. Answers 1 when the call is to be abandoned (the
// operand was refused); 0 when selection should go on.
int vectorBroadcastOperand(TypeCheckState *pstate, FnCallNode *callnode) {
    (void)pstate;
    // The lane-wise methods that take vectors, 'v.min(0.0)' and 'v.fma(2.0, w)',
    // broadcast a number among their arguments as an operator's operand is
    if (!(callnode->flags & FlagOperator) && callnode->args != NULL && callnode->methfld != NULL
        && isNameUseNode(callnode->methfld) && isExpNode(callnode->objfn) && !inodeIsError(callnode->objfn)
        && iexpGetDerefTypeDcl(callnode->objfn)->tag == VectorTag) {
        char *meth = &((NameUseNode*)callnode->methfld)->namesym->namestr;
        uint32_t nargs = callnode->args->used;
        if ((nargs == 1 && (strcmp(meth, "min") == 0 || strcmp(meth, "max") == 0)) || (nargs == 2 && strcmp(meth, "fma") == 0)) {
            INode *vectype = iexpGetDerefTypeDcl(callnode->objfn);
            for (uint32_t i = 0; i < nargs; ++i) {
                INode **argp = &nodesGet(callnode->args, i);
                if (!isExpNode(*argp) || inodeIsError(*argp))
                    continue;
                INode *argtype = iexpGetDerefTypeDcl(*argp);
                if (argtype->tag != VectorTag && (vectorIsNbrType(argtype) || argtype == (INode*)boolType)
                    && !vectorBroadcast(argp, vectype, (INode*)callnode)) {
                    callnode->vtype = errorType;
                    return 1;
                }
            }
        }
        return 0;
    }
    if (!(callnode->flags & FlagOperator) || callnode->args == NULL || callnode->args->used != 1
        || callnode->methfld == NULL || !isNameUseNode(callnode->methfld)
        || !vectorIsOpName(((NameUseNode*)callnode->methfld)->namesym))
        return 0;
    INode **leftp = &callnode->objfn;
    INode **rightp = &nodesGet(callnode->args, 0);
    if (!isExpNode(*leftp) || !isExpNode(*rightp) || inodeIsError(*leftp) || inodeIsError(*rightp))
        return 0;
    INode *lt = iexpGetDerefTypeDcl(*leftp);
    INode *rt = iexpGetDerefTypeDcl(*rightp);
    if (lt->tag == VectorTag && rt->tag != VectorTag && (vectorIsNbrType(rt) || rt == (INode*)boolType)) {
        if (!vectorBroadcast(rightp, lt, (INode*)callnode)) {
            callnode->vtype = errorType;
            return 1;
        }
    }
    else if (rt->tag == VectorTag && lt->tag != VectorTag && (vectorIsNbrType(lt) || lt == (INode*)boolType)) {
        if (!vectorBroadcast(leftp, rt, (INode*)callnode)) {
            callnode->vtype = errorType;
            return 1;
        }
    }
    return 0;
}

// Does the method name belong to a number rather than to a mask's logic?
static int vectorNumberMethod(Name *op) {
    char *s = &op->namestr;
    return op == plusName || op == minusName || op == multName || op == divName || op == remName
        || op == shlName || op == shrName || op == ltName || op == leName || op == gtName || op == geName
        || strcmp(s, "sqrt") == 0 || strcmp(s, "sin") == 0 || strcmp(s, "cos") == 0
        || strcmp(s, "sum") == 0 || strcmp(s, "product") == 0 || strcmp(s, "min") == 0 || strcmp(s, "max") == 0
        || strcmp(s, "abs") == 0 || strcmp(s, "floor") == 0 || strcmp(s, "ceil") == 0 || strcmp(s, "round") == 0
        || strcmp(s, "fma") == 0;
}

// A method the vector does not have. A mask is a vector of bool, whose lanes
// are logical, not numbers, so the arithmetic and the ordering are refused as
// such. Answers 1 when it reported.
int vectorMissingMember(FnCallNode *callnode, INode *vectype, Name *methsym) {
    if (vectorIsMask(vectype) && vectorNumberMethod(methsym)) {
        errorMsgNode((INode*)callnode, ErrorVectorMask,
            "`%s` is not a mask's: a mask, %s, holds true or false in each lane, not numbers. Its operators are `&`, `|`, `^`, `~`, `==` and `!=`, and `any()`, `all()` and `select(a, b)` use it. Convert it for numbers, 'Vector[u8, %llu].from(m)'.",
            &methsym->namestr, itypeName(vectype), (unsigned long long)vectorLanes(vectype));
        return 1;
    }
    return 0;
}

// A method of a vector that no candidate accepted: usually the other operand
// is a vector of other lanes
int vectorNoCandidate(FnCallNode *callnode, INode *vectype, Name *methsym) {
    if (callnode->args == NULL || callnode->args->used != 1)
        return 0;
    INode *arg = nodesGet(callnode->args, 0);
    if (!isExpNode(arg))
        return 0;
    INode *argtype = iexpGetDerefTypeDcl(arg);
    if (argtype->tag != VectorTag)
        return 0;
    if (vectorIsMask(vectype) != vectorIsMask(argtype) && !itypeIsSame(vectype, argtype)
        && vectorLanes(vectype) == vectorLanes(argtype)) {
        errorMsgNode((INode*)callnode, ErrorVectorMask,
            "A mask, %s, and a vector of numbers, %s, do not mix in `%s`: a mask is not a number.",
            itypeName(vectype), itypeName(argtype), &methsym->namestr);
        return 1;
    }
    char receiver[96];
    snprintf(receiver, sizeof(receiver), "%s", itypeName(vectype));
    errorMsgNode((INode*)callnode, ErrorVectorMixed,
        "`%s` takes two vectors of the same lanes, and these are %s and %s. Convert one lane by lane, '%s.from(v)'.",
        &methsym->namestr, receiver, itypeName(argtype), receiver);
    return 1;
}

// 'mask.select(a, b)': a where the mask is true, else b, lane by lane. A number
// beside a vector is broadcast as an operator's is. The call becomes a type
// literal of three arguments, mask, a and b, of the type of a and b.
int vectorSelectLower(TypeCheckState *pstate, FnCallNode *callnode) {
    (void)pstate;
    INode *masktype = iexpGetDerefTypeDcl(callnode->objfn);
    if (!vectorIsMask(masktype)) {
        errorMsgNode((INode*)callnode, ErrorVectorMask,
            "`select` is a mask's: 'm.select(a, b)' takes a where the mask is true and b where it is not. This is %s, not a mask; compare it first, 'v > w'.",
            itypeName(masktype));
        callnode->vtype = errorType;
        return 1;
    }
    if (callnode->args == NULL || callnode->args->used != 2) {
        errorMsgNode((INode*)callnode, ErrorVectorFrom,
            "`select` takes two values: 'm.select(a, b)', a where the mask is true, else b.");
        callnode->vtype = errorType;
        return 1;
    }
    INode **ap = &nodesGet(callnode->args, 0);
    INode **bp = &nodesGet(callnode->args, 1);
    if (!isExpNode(*ap) || !isExpNode(*bp) || inodeIsError(*ap) || inodeIsError(*bp)) {
        callnode->vtype = errorType;
        return 1;
    }
    INode *at = iexpGetDerefTypeDcl(*ap);
    INode *bt = iexpGetDerefTypeDcl(*bp);
    INode *vectype = at->tag == VectorTag ? at : bt->tag == VectorTag ? bt : NULL;
    if (vectype == NULL) {
        errorMsgNode((INode*)callnode, ErrorVectorFrom,
            "`select` chooses between vectors, and neither value is one; a number beside a vector is used in every lane.");
        callnode->vtype = errorType;
        return 1;
    }
    if (vectorLanes(vectype) != vectorLanes(masktype)) {
        errorMsgNode((INode*)callnode, ErrorVectorMixed,
            "The mask has %llu lanes and the vectors it chooses between have %llu.",
            (unsigned long long)vectorLanes(masktype), (unsigned long long)vectorLanes(vectype));
        callnode->vtype = errorType;
        return 1;
    }
    if (at->tag != VectorTag && !vectorBroadcast(ap, vectype, (INode*)callnode)) {
        callnode->vtype = errorType;
        return 1;
    }
    if (bt->tag != VectorTag && !vectorBroadcast(bp, vectype, (INode*)callnode)) {
        callnode->vtype = errorType;
        return 1;
    }
    if (!itypeIsSame(iexpGetTypeDcl(*ap), iexpGetTypeDcl(*bp))) {
        errorMsgNode((INode*)callnode, ErrorVectorMixed,
            "`select` chooses between two vectors of the same lanes, and these are %s and %s. Convert one lane by lane, '%s.from(v)'.",
            itypeName(iexpGetTypeDcl(*ap)), itypeName(iexpGetTypeDcl(*bp)), itypeName(iexpGetTypeDcl(*ap)));
        callnode->vtype = errorType;
        return 1;
    }
    derefInject(&callnode->objfn);
    derefInject(ap);
    derefInject(bp);
    INode *result = iexpGetTypeDcl(*ap);
    Nodes *args = newNodes(3);
    nodesAdd(&args, callnode->objfn);
    nodesAdd(&args, *ap);
    nodesAdd(&args, *bp);
    callnode->args = args;
    callnode->tag = TypeLitTag;
    callnode->objfn = result;
    callnode->methfld = NULL;
    callnode->vtype = result;
    return 1;
}

// 'Vector[f32, 4].from(x)' and 'Array[f32, 4].from(v)'. The target is a vector
// and x an array of the same lane type and size, free, or a vector of the same
// size, converted lane by lane as the lane type's own 'from' converts; or the
// target is an array and the source the vector of those lanes. The call becomes
// a type literal whose one argument generation converts (genlVectorConvert).
int vectorFromCall(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    if (!isTypeNode(node->objfn) || node->methfld == NULL || !isNameUseNode(node->methfld)
        || ((NameUseNode*)node->methfld)->namesym != fromName)
        return 0;
    INode *target = itypeGetTypeDcl(node->objfn);
    if (target->tag != VectorTag && !(target->tag == ArrayTag && ((ArrayNode*)target)->dimens->used == 1))
        return 0;
    node->vtype = node->objfn;
    if (node->args == NULL || node->args->used != 1) {
        errorMsgNode((INode*)node, ErrorVectorFrom, "%s.from converts one value: %s.from(value)", itypeName(target), itypeName(target));
        return 1;
    }
    if (namedValRefuseArgs(node->args, "a call"))
        return 1;
    INode **argp = &nodesGet(node->args, 0);
    inodeTypeCheckAny(pstate, argp);
    if (!isExpNode(*argp)) {
        errorMsgNode(*argp, ErrorNotTyped, "Expected a typed expression.");
        return 1;
    }
    if (inodeIsError(*argp))
        return 1;
    derefInject(argp);
    INode *source = iexpGetTypeDcl(*argp);
    int ok = 0;
    if (target->tag == VectorTag) {
        uint64_t lanes = vectorLanes(target);
        if (source->tag == ArrayTag && ((ArrayNode*)source)->dimens->used == 1) {
            INode *dim = nodesGet(((ArrayNode*)source)->dimens, 0);
            if (dim->tag == ULitTag && ((ULitNode*)dim)->uintlit == lanes
                && itypeIsSame(arrayElemType(source), vectorElemType(target)))
                ok = 1;
            else
                errorMsgNode(*argp, ErrorVectorFrom,
                    "A vector comes free from an array of its lane type and number of lanes, and this is %s; %s.from(a) wants Array[%s, %llu].",
                    itypeName(source), itypeName(target), itypeName(vectorElemType(target)), (unsigned long long)lanes);
        }
        else if (source->tag == VectorTag) {
            if (vectorLanes(source) == lanes)
                ok = 1;
            else
                errorMsgNode(*argp, ErrorVectorMixed,
                    "%s has %llu lanes and %s has %llu: a conversion keeps the number of lanes.",
                    itypeName(source), (unsigned long long)vectorLanes(source), itypeName(target), (unsigned long long)lanes);
        }
        else
            errorMsgNode(*argp, ErrorVectorFrom,
                "%s.from converts an array of its lanes or a vector of the same number of lanes; to put one number in every lane, construct with fill: 'Vector[%s, %llu]() <- fill x'.",
                itypeName(target), itypeName(vectorElemType(target)), (unsigned long long)lanes);
    }
    else {
        if (source->tag == VectorTag && vectorLanes(source) == arrayDim1(target)
            && itypeIsSame(vectorElemType(source), arrayElemType(target)))
            ok = 1;
        else
            errorMsgNode(*argp, ErrorVectorFrom,
                "Array[%s, %llu].from converts the vector of its elements and size, 'Vector[%s, %llu]'.",
                itypeName(arrayElemType(target)), (unsigned long long)arrayDim1(target),
                itypeName(arrayElemType(target)), (unsigned long long)arrayDim1(target));
    }
    if (!ok)
        return 1;
    node->methfld = NULL;
    node->tag = TypeLitTag;
    node->flags &= ~FlagIndex;
    return 1;
}

// 'v[i]': a lane of the vector, the index an unsigned integer, the lane type
// the node's. Checked at run time against the lanes, as an array's index is.
int vectorIndex(FnCallNode *node) {
    INode *objtype = iexpGetDerefTypeDcl(node->objfn);
    if (vectorIsMask(objtype)) {
        errorMsgNode((INode*)node, ErrorVectorIndex,
            "A mask's lanes are not indexed: test them together with any() and all(), or choose with select(a, b).");
        node->vtype = errorType;
        return 0;
    }
    if (node->flags & (FlagBorrow | FlagRange)) {
        errorMsgNode((INode*)node, ErrorVectorIndex,
            (node->flags & FlagRange)
                ? "A range does not slice a vector; convert it to an array first, 'Array[T, N].from(v)'."
                : "A lane of a vector is not borrowed: it lives in a register. Read it, 'v[i]', or write it, 'v[i] = x'.");
        node->vtype = errorType;
        return 0;
    }
    node->vtype = vectorElemType(objtype);
    return 1;
}
