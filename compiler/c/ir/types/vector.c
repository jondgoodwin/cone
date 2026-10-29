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

// A signature of the vector, and of a second parameter when there is one, which
// is named 'b' as the operators' are, or 'other' where the second is a method's
// argument that can be written by name (min, max: as a number's are)
static FnSigNode *vecSigNamed(INode *rettype, INode *self, INode *other, char *othername) {
    FnSigNode *sig = newFnSigNode();
    sig->rettype = rettype;
    nodesAdd(&sig->parms, (INode*)newVarDclFull(nametblFind("a", 1), VarDclTag, self, newPermUseNode(immPerm), NULL));
    if (other)
        nodesAdd(&sig->parms, (INode*)newVarDclFull(nametblFind(othername, strlen(othername)), VarDclTag, other, newPermUseNode(immPerm), NULL));
    return sig;
}

static FnSigNode *vecSig(INode *rettype, INode *self, INode *other) {
    return vecSigNamed(rettype, self, other, "b");
}

// A slice of the lane type, '&[T]' or '&mut [T]', for a parameter of load and store
static INode *vecSliceType(TypeCheckState *pstate, INode *lane, PermNode *perm) {
    INode *slice = (INode*)newRefNodeFull(ArrayRefTag, NULL, borrowRef, newPermUseNode(perm), vecNbrUse(lane));
    itypeTypeCheck(pstate, &slice);
    return slice;
}

// 'load' and 'store': the lanes at s[at], s[at+1] ... of a slice of the lane type,
// bounds checked once for all of them. 'load' belongs to the vector type
// ('Vector[f32, 4].load(s, at)'), 'store' to the value. Not for a mask, whose
// lanes are bits in a register and have no memory form of their own.
static void vectorSliceMethods(TypeCheckState *pstate, VectorNode *vec, INode *lane) {
    FnSigNode *loadsig = newFnSigNode();
    loadsig->rettype = (INode*)vec;
    nodesAdd(&loadsig->parms, (INode*)newVarDclFull(nametblFind("s", 1), VarDclTag, vecSliceType(pstate, lane, roPerm), newPermUseNode(immPerm), NULL));
    nodesAdd(&loadsig->parms, (INode*)newVarDclFull(nametblFind("at", 2), VarDclTag, (INode*)usizeType, newPermUseNode(immPerm), NULL));
    IntrinsicNode *loadnode = newIntrinsicNode(VecLoadIntrinsic);
    loadnode->typearg = (INode*)vec;
    FnDclNode *loadfn = newFnDclNode(nametblFind("load", 4), FlagPub, (INode*)loadsig, (INode*)loadnode);
    // Named directly by a call, which would check a declaration that has no body
    // to check: its signature is checked, here
    loadfn->flags |= TypeChecked;
    iNsTypeAddFn((INsTypeNode*)vec, loadfn);

    FnSigNode *storesig = newFnSigNode();
    storesig->rettype = (INode*)newVoidNode();
    nodesAdd(&storesig->parms, (INode*)newVarDclFull(nametblFind("self", 4), VarDclTag, (INode*)vec, newPermUseNode(immPerm), NULL));
    nodesAdd(&storesig->parms, (INode*)newVarDclFull(nametblFind("s", 1), VarDclTag, vecSliceType(pstate, lane, mutPerm), newPermUseNode(immPerm), NULL));
    nodesAdd(&storesig->parms, (INode*)newVarDclFull(nametblFind("at", 2), VarDclTag, (INode*)usizeType, newPermUseNode(immPerm), NULL));
    IntrinsicNode *storenode = newIntrinsicNode(VecStoreIntrinsic);
    storenode->typearg = (INode*)vec;
    iNsTypeAddFn((INsTypeNode*)vec, newFnDclNode(nametblFind("store", 5), FlagMethFld | FlagPub, (INode*)storesig, (INode*)storenode));
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
static void vectorMethods(TypeCheckState *pstate, VectorNode *vec, INode *lane) {
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
        // (the argument is named 'other', as a number's min and max name theirs)
        vecAddLanewise(vec, "_min_lanes", minsym, vecSigNamed(self, self, self, "other"), MinIntrinsic);
        vecAddLanewise(vec, "_max_reduce", maxsym, reduce, issigned ? VecSMaxIntrinsic : VecMaxIntrinsic);
        vecAddLanewise(vec, "_max_lanes", maxsym, vecSigNamed(self, self, self, "other"), MaxIntrinsic);
        vectorSliceMethods(pstate, vec, lane);
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
    vectorMethods(pstate, node, lane);
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
    if (vectorIsMask(vectype) && strcmp(&methsym->namestr, "store") == 0) {
        errorMsgNode((INode*)callnode, ErrorVectorSlice,
            "A mask has no `store`: its lanes are bits, with no form in memory. Convert it for numbers, 'Vector[u8, %llu].from(m)', and store those.",
            (unsigned long long)vectorLanes(vectype));
        return 1;
    }
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

// Is this an index of a mask? A mask's lane is read, a bool, and is no place.
int vectorIsMaskLane(INode *node) {
    if (node->tag != ArrIndexTag)
        return 0;
    FnCallNode *call = (FnCallNode*)node;
    if (!isExpNode(call->objfn) || inodeIsError(call->objfn))
        return 0;
    INode *objtype = iexpGetDerefTypeDcl(call->objfn);
    return objtype->tag == VectorTag && vectorIsMask(objtype);
}

// ---- Picking lanes: 'v.(3, 2, 1, 0)', 'v.x', 'a.join(b)' ----------------------

// A vector type of 'lanes' lanes of an already checked lane type, checked
static VectorNode *vectorMakeType(TypeCheckState *pstate, INode *lexnode, uint64_t lanes, INode *elemtype) {
    VectorNode *vec = vectorNewTyped(lexnode, lanes, itypeGetTypeDcl(elemtype));
    vectorTypeCheck(pstate, vec);
    return vec;
}

// What a vector type literal made by type check is, told by its arguments: a
// conversion has one argument; a pick has the vector, then the lane positions
// as literals; a join has two vectors; a select has the mask and two vectors
VectorLitKind vectorLitKind(FnCallNode *lit) {
    if (lit->args->used == 1)
        return VecLitConvert;
    if (nodesGet(lit->args, 1)->tag == ULitTag)
        return VecLitPick;
    return lit->args->used == 2 ? VecLitJoin : VecLitSelect;
}

// The lane a name in a field list or after a '.' stands for: x y z w are lanes 0
// to 3. Answers -1 for any other name.
static int vectorLaneNamed(Name *name) {
    char *s = &name->namestr;
    if (s[0] == '\0' || s[1] != '\0')
        return -1;
    switch (s[0]) {
    case 'x': return 0;
    case 'y': return 1;
    case 'z': return 2;
    case 'w': return 3;
    default: return -1;
    }
}

// A lane position for a name written in a vector's field list or after a '.':
// a number, or one of the names x y z w for a vector of up to four lanes.
// Answers 0, having reported, when the vector has no such lane.
static int vectorLanePosition(INode *vectype, INode *name, uint32_t *at) {
    uint64_t lanes = vectorLanes(vectype);
    if (name->tag == ULitTag) {
        uint64_t pos = ((ULitNode*)name)->uintlit;
        if (pos >= lanes) {
            errorMsgNode(name, ErrorVectorPick,
                "%s has %llu lane%s, numbered 0 to %llu; there is no lane %llu.", itypeName(vectype),
                (unsigned long long)lanes, lanes == 1 ? "" : "s", (unsigned long long)lanes - 1, (unsigned long long)pos);
            return 0;
        }
        *at = (uint32_t)pos;
        return 1;
    }
    Name *namesym = ((NameUseNode*)name)->namesym;
    int named = vectorLaneNamed(namesym);
    if (named < 0) {
        errorMsgNode(name, ErrorVectorPick,
            "`%s` is not one of a vector's lanes: number the lanes, 'v.(0, 2)', or for a vector of up to four lanes name them x, y, z, w.",
            &namesym->namestr);
        return 0;
    }
    if (lanes > 4) {
        errorMsgNode(name, ErrorVectorPick,
            "The lane names x, y, z, w are for vectors of up to four lanes, and %s has %llu: number the lane, 'v[%d]' or 'v.(%d, ...)'.",
            itypeName(vectype), (unsigned long long)lanes, named, named);
        return 0;
    }
    if ((uint64_t)named >= lanes) {
        errorMsgNode(name, ErrorVectorPick,
            "%s has %llu lane%s, named %s; there is no lane `%s`.", itypeName(vectype), (unsigned long long)lanes,
            lanes == 1 ? "" : "s", lanes == 1 ? "x" : lanes == 2 ? "x, y" : lanes == 3 ? "x, y, z" : "x, y, z, w", &namesym->namestr);
        return 0;
    }
    *at = (uint32_t)named;
    return 1;
}

// 'v.x': a lane named by one of x y z w, written as the field of a struct is. It
// becomes 'v[0]' (or 1, 2, 3), so it is a place, read and written like any lane.
// Answers 0 when the call is no such name on a vector, and 1 when it has been
// rewritten as the index (to be checked as one) or refused.
int vectorLaneName(FnCallNode *node) {
    if ((node->flags & (FlagIndex | FlagOperator | FlagRange)) || node->args != NULL || node->methfld == NULL
        || !isNameUseNode(node->methfld) || !isExpNode(node->objfn) || inodeIsError(node->objfn))
        return 0;
    INode *objtype = iexpGetDerefTypeDcl(node->objfn);
    if (objtype->tag != VectorTag)
        return 0;
    Name *name = ((NameUseNode*)node->methfld)->namesym;
    if (vectorLaneNamed(name) < 0)
        return 0;
    uint32_t at;
    if (!vectorLanePosition(objtype, node->methfld, &at)) {
        node->vtype = errorType;
        return 1;
    }
    node->flags |= FlagIndex;
    node->methfld = NULL;
    node->args = newNodes(1);
    nodesAdd(&node->args, (INode*)newULitNodeTC(at, (INode*)usizeType));
    return 1;
}

// 'v.(3, 2, 1, 0)': the lanes of a vector at constant positions, in the order
// written, a repeat copying the lane. One position gives the lane itself, a
// number; more give a vector of that many lanes (up to 64) of the same lane
// type. A value, not a place: a pick is read. The node is replaced by a type
// literal whose arguments are the vector and then each position, which
// generation turns into a shufflevector (or an extract for one lane).
// Answers 0 when the value is not a vector (or reaches one only through a
// pointer), leaving the node as it was.
int vectorPickLower(TypeCheckState *pstate, INode **nodep) {
    DestructNode *node = (DestructNode*)*nodep;
    int derefs = 0;
    while (iexpGetTypeDcl(node->exp)->tag == RefTag && derefs++ < 8) {
        if (!derefInject(&node->exp))
            break;
    }
    INode *vectype = iexpGetTypeDcl(node->exp);
    if (vectype->tag != VectorTag)
        return 0;
    uint32_t count = node->names->used;
    uint32_t *pos = memAllocBlk(count * sizeof(uint32_t));
    int ok = 1;
    for (uint32_t i = 0; ok && i < count; ++i)
        if (!vectorLanePosition(vectype, nodesGet(node->names, i), &pos[i]))
            ok = 0;
    if (ok && count > VectorMaxLanes) {
        errorMsgNode((INode*)node, ErrorVectorPick,
            "A pick of %u lanes would be a vector wider than the %d lanes a vector has.", count, VectorMaxLanes);
        ok = 0;
    }
    if (!ok) {
        *nodep = newErrorNode((INode*)node);
        return 1;
    }
    INode *result = count == 1 ? itypeGetTypeDcl(vectorElemType(vectype))
        : (INode*)vectorMakeType(pstate, (INode*)node, count, vectorElemType(vectype));
    FnCallNode *lit = vectorNewConvert(result, node->exp, (INode*)node);
    for (uint32_t i = 0; i < count; ++i)
        nodesAdd(&lit->args, (INode*)newULitNodeTC(pos[i], (INode*)usizeType));
    *nodep = (INode*)lit;
    return 1;
}

// 'a.join(b)': the lanes of a, then the lanes of b, a vector of twice as many.
// Both are vectors of one type, and the result may be no wider than a vector is;
// a type literal of the two, generation's one shufflevector (a pick of a join
// is the same shufflevector, as 'a.join(b).(0, 4, 1, 5)' writes it).
int vectorJoinLower(TypeCheckState *pstate, FnCallNode *callnode) {
    INode *atype = iexpGetDerefTypeDcl(callnode->objfn);
    callnode->vtype = errorType;
    if (callnode->args == NULL || callnode->args->used != 1) {
        errorMsgNode((INode*)callnode, ErrorVectorFrom, "`join` takes one vector: 'a.join(b)' lays the lanes of b after those of a.");
        return 1;
    }
    INode **bp = &nodesGet(callnode->args, 0);
    if (!isExpNode(*bp) || inodeIsError(*bp))
        return 1;
    INode *btype = iexpGetDerefTypeDcl(*bp);
    if (btype->tag != VectorTag || !itypeIsSame(atype, btype)) {
        errorMsgNode((INode*)callnode, ErrorVectorMixed,
            "`join` lays two vectors of the same lanes end to end, and these are %s and %s. Convert one lane by lane, 'Vector[T, N].from(v)'.",
            itypeName(atype), itypeName(btype));
        return 1;
    }
    uint64_t lanes = vectorLanes(atype) * 2;
    if (lanes > VectorMaxLanes) {
        errorMsgNode((INode*)callnode, ErrorVectorPick,
            "Joining two vectors of %llu lanes makes %llu, and a vector has at most %d.",
            (unsigned long long)vectorLanes(atype), (unsigned long long)lanes, VectorMaxLanes);
        return 1;
    }
    derefInject(&callnode->objfn);
    derefInject(bp);
    VectorNode *result = vectorMakeType(pstate, (INode*)callnode, lanes, vectorElemType(atype));
    Nodes *args = newNodes(2);
    nodesAdd(&args, callnode->objfn);
    nodesAdd(&args, *bp);
    callnode->args = args;
    callnode->tag = TypeLitTag;
    callnode->objfn = (INode*)result;
    callnode->methfld = NULL;
    callnode->vtype = (INode*)result;
    return 1;
}

// 'Vector[f32, 4].load(s, at)': a function of the vector type, found in its
// namespace and named directly in the call from here on, so the call is checked
// as any function's is. A mask has none. Answers 1 when it refused the call.
int vectorStaticCall(TypeCheckState *pstate, FnCallNode *node) {
    if (node->objfn == NULL || node->objfn->tag != VectorTag || node->methfld == NULL
        || !isNameUseNode(node->methfld) || (node->flags & (FlagIndex | FlagOperator)))
        return 0;
    Name *name = ((NameUseNode*)node->methfld)->namesym;
    if (strcmp(&name->namestr, "load") != 0)
        return 0;
    if (!itypeTypeCheck(pstate, &node->objfn)) {
        node->vtype = errorType;
        return 1;
    }
    INode *vectype = itypeGetTypeDcl(node->objfn);
    if (vectype->tag != VectorTag)
        return 0;
    if (vectorIsMask(vectype)) {
        errorMsgNode((INode*)node, ErrorVectorSlice,
            "A mask has no `load`: its lanes are bits, with no form in memory. Load the numbers, 'Vector[u8, %llu].load(s, at)', and compare them.",
            (unsigned long long)vectorLanes(vectype));
        node->vtype = errorType;
        return 1;
    }
    INode *found = iNsTypeFindFnField((INsTypeNode*)vectype, name);
    if (found == NULL || found->tag != FnDclTag)
        return 0;
    NameUseNode *use = newNameUseNode(name);
    inodeLexCopy((INode*)use, node->methfld);
    use->dclnode = found;
    node->objfn = (INode*)use;
    node->methfld = NULL;
    return 0;
}
