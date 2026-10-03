/** Handling for cast nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create node for recasting to a new type without conversion
CastNode *newRecastNode(INode *exp, INode *type) {
    CastNode *node;
    newNode(node, CastNode, CastTag);
    node->typ = node->vtype = type;
    node->exp = exp;
    return node;
}

// Create node for converting exp to a new type
CastNode *newConvCastNode(INode *exp, INode *type) {
    CastNode *node;
    newNode(node, CastNode, CastTag);
    node->flags |= FlagConvert;
    node->typ = node->vtype = type;
    node->exp = exp;
    return node;
}

// Clone cast
INode *cloneCastNode(CloneState *cstate, CastNode *node) {
    CastNode *newnode;
    newnode = memAllocBlk(sizeof(CastNode));
    memcpy(newnode, node, sizeof(CastNode));
    newnode->exp = cloneNode(cstate, node->exp);
    newnode->typ = cloneNode(cstate, node->typ);
    return (INode *)newnode;
}

// Create a new cast node
CastNode *newIsNode(INode *exp, INode *type) {
    CastNode *node;
    newNode(node, CastNode, IsTag);
    node->vtype = (INode*)boolType;  // 'is' answers a question, whatever it asks about
    node->typ = type;
    node->exp = exp;
    return node;
}

// The name at the root of a pattern's type. A reference pattern narrows a
// reference, and the name is its referent's. A generic variant written with its
// type arguments is a call until type check instantiates it, and the name is the
// callee. A path, 'Shape.Circle', is a member access until name resolution
// collapses it into a qualified name, and is taken as written: it has no root.
NameUseNode *castPatternName(INode *typ, int *hasargs) {
    if (hasargs)
        *hasargs = 0;
    while (typ != NULL) {
        if (typ->tag == RefTag || typ->tag == VirtRefTag)
            typ = ((RefNode*)typ)->vtexp;
        else if (typ->tag == FnCallTag && (typ->flags & FlagIndex) && ((FnCallNode*)typ)->methfld == NULL) {
            if (hasargs)
                *hasargs = 1;
            typ = ((FnCallNode*)typ)->objfn;
        }
        else
            break;
    }
    if (typ == NULL || !isNameUseNode(typ) || (typ->flags & FlagQualified))
        return NULL;
    return (NameUseNode*)typ;
}

// Mark a pattern's bare root name, so that name resolution leaves it for
// castPatternBind to look up in the matched value's enum.
//
// An owning reference at the root keeps its '+R-perm T' spelling, which is
// refused as a type anywhere else: its root is read here, at parse time, before
// anything knows that 'R' in 'R[mut, Circle]' is a region rather than a generic
// variant, and how a pattern spells a managed reference is not settled yet.
void castPatternMark(INode *typ) {
    if (typ && (typ->tag == RefTag || typ->tag == VirtRefTag))
        ((RefNode*)typ)->plusSpelled = 0;
    NameUseNode *name = castPatternName(typ, NULL);
    if (name)
        name->flags |= FlagPattern;
}

// Is this pattern's root name still waiting to be bound against the matched value?
int castPatternPending(INode *typ) {
    NameUseNode *name = castPatternName(typ, NULL);
    return name != NULL && (name->flags & FlagPattern);
}

// The enum of the value a pattern is matched against, reached through a
// reference, or NULL when that value is not of an enum. A variant is not one:
// the value is already narrowed, and its variants are nobody's.
static StructNode *castMatchedEnum(INode *exp) {
    if (!isExpNode(exp))
        return NULL;
    INode *type = iexpGetTypeDcl(exp);
    if (type->tag == RefTag || type->tag == VirtRefTag)
        type = itypeGetTypeDcl(((RefNode*)type)->vtexp);
    if (type->tag != StructTag || !(type->flags & EnumType))
        return NULL;
    return (StructNode*)type;
}

// The variant of this name among an enum's variants, or NULL. 'derived' is the
// list, not the enum's namespace: an instance of a generic enum lists its own
// instantiated variants there (genericMemoize), and an extension lists its
// copies of its base's variants ahead of its own.
static INode *castEnumVariant(StructNode *enumdcl, Name *name) {
    if (enumdcl == NULL || enumdcl->derived == NULL)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(enumdcl->derived, cnt, nodesp)) {
        if (((StructNode*)*nodesp)->namesym == name)
            return *nodesp;
    }
    return NULL;
}

// Bind a pattern's bare root name against the value being matched. A variant of
// that value's enum is what the name means, whatever it means lexically, so
// 'Red' on a 'Lamp' is the Lamp's 'Red' while a 'use' of another enum with a 'Red'
// is in force. Only a name the enum has no variant of keeps its lexical meaning.
// Because the enum's list is consulted, the matched value also supplies a
// generic variant's type arguments: on an 'Option[i32]', 'Some' is 'Some[i32]'.
// A name written with arguments is taken lexically, since the list holds
// instances and the arguments would be applied to one.
//
// The 'is' test and the conversion that binds a matched value share one type
// node, so whichever is checked first binds it for both. A clone -- a generic
// instance's body, a default method's copy -- copies the node once per holder,
// and then each binds its own, to the same answer. Only the 'is' test reports:
// the conversion is checked after it and would only say it again.
//
// Returns 0 when the pattern names nothing to narrow to, reported, and there is
// no type to check.
static int castPatternBind(CastNode *node, int report) {
    int hasargs;
    NameUseNode *name = castPatternName(node->typ, &hasargs);
    if (name == NULL)
        return 1;
    if (name->flags & FlagPattern) {
        name->flags &= 0xFFFF - FlagPattern;
        StructNode *enumdcl = castMatchedEnum(node->exp);
        INode *variant = castEnumVariant(enumdcl, name->namesym);
        if (variant && !hasargs)
            name->dclnode = variant;
        else if (name->dclnode == NULL) {
            if (report && variant)
                errorMsgNode((INode*)name, ErrorPatArgs,
                    "%s is a variant of the enum being matched, and that value supplies its type arguments. Drop the arguments, or name the variant through its enum, as %s.%s.",
                    &name->namesym->namestr, &enumdcl->namesym->namestr, &name->namesym->namestr);
            else if (report)
                errorMsgNode((INode*)name, ErrorUnkName, "The name %s does not refer to a declared name",
                    &name->namesym->namestr);
            name->dclnode = errorType;
        }
        else if (isExpNode(name)) {
            if (report)
                errorMsgNode((INode*)name, ErrorNotType,
                    "%s is not a type, so a pattern cannot narrow to it.", &name->namesym->namestr);
            name->dclnode = errorType;
        }
    }
    return name->dclnode != errorType;
}

// Create the test for a value alone as a match pattern, positioned on the value.
// A bare name is marked as an 'is' pattern's root is, so that name resolution
// leaves it unbound when it has no lexical meaning: it may be a variant of the
// matched value's enum. Only a bare name: marking the root of '&x' would keep it
// a reference type rather than a borrow.
CastNode *newMatchValueNode(INode *matchee, INode *value) {
    CastNode *node = newIsNode(matchee, value);
    inodeLexCopy((INode*)node, value);
    node->flags |= FlagMatchValue;
    if (isNameUseNode(value))
        value->flags |= FlagPattern;
    return node;
}

// Type check a value alone as a match pattern ('case 2', 'case K', 'case Circle').
// A value alone means equality with the matched value, but a bare name is asked
// of the matched value's enum first, as an 'is' pattern's is: a variant of it
// makes this the 'is' test the node already is, and castIsTypeCheck binds it.
// Anything else is a value, and the node is replaced by 'matched == value',
// positioned on the value and checked as any comparison is. A type is no value,
// and the author is told to write 'is' if narrowing is what was meant.
void castMatchValueTypeCheck(TypeCheckState *pstate, INode **nodep) {
    CastNode *node = (CastNode*)*nodep;
    node->flags &= 0xFFFF - FlagMatchValue;
    iexpTypeCheckAny(pstate, &node->exp);
    INode *value = node->typ;
    if (isNameUseNode(value) && (value->flags & FlagPattern)) {
        NameUseNode *name = (NameUseNode*)value;
        if (castEnumVariant(castMatchedEnum(node->exp), name->namesym)) {
            castIsTypeCheck(pstate, node);
            return;
        }
        name->flags &= 0xFFFF - FlagPattern;
        if (name->dclnode == NULL) {
            errorMsgNode(value, ErrorUnkName, "The name %s does not refer to a declared name",
                &name->namesym->namestr);
            node->typ = errorType;
            return;
        }
    }
    if (isTypeNode(value)) {
        errorMsgNode(value, ErrorPatType,
            "A type alone is not a pattern: a value alone is compared with the matched value, as by '=='. Write 'is' before the type to narrow to it.");
        node->typ = errorType;
        return;
    }
    FnCallNode *eqnode = newFnCallOpnameLower(value, node->exp, eqName, 2);
    nodesAdd(&eqnode->args, value);
    *nodep = (INode*)eqnode;
    inodeTypeCheckAny(pstate, nodep);
}

// Serialize cast
void castPrint(CastNode *node) {
    inodeFprint(node->tag==CastTag? "(cast, " : "(is, ");
    inodePrintNode(node->typ);
    inodeFprint(", ");
    inodePrintNode(node->exp);
    inodeFprint(")");
}

// Name resolution of cast node
void castNameRes(NameResState *pstate, CastNode *node) {
    inodeNameRes(pstate, &node->exp);
    // A pattern's conversion holds the type node of the 'is' test before it,
    // which that test resolved. A second resolution is not idempotent: a reference
    // whose referent names no type has been turned into a borrow, which has no
    // name resolution of its own.
    if (!(node->flags & FlagMatchBind))
        inodeNameRes(pstate, &node->typ);
    // A path, 'Shape.Circle', collapses by replacing the node that held it
    // (fnCallNameResPath), so only the test's own slot received the name it
    // collapsed to. This one still holds the hop, and takes its member, which
    // is that name.
    else if (node->typ->tag == FnCallTag) {
        INode *member = ((FnCallNode*)node->typ)->methfld;
        if (member && isNameUseNode(member) && (member->flags & FlagQualified))
            node->typ = member;
    }
}

#define ptrsize 10000
// Give a rough idea of comparable type size for use with type checking reinterpretation casts
uint32_t castBitsize(INode *type) {
    if (type->tag == UintNbrTag || type->tag == IntNbrTag || type->tag == FloatNbrTag) {
        if (type == (INode*)usizeType)
            return ptrsize;
        return ((NbrNode *)type)->bits;
    }
    switch (type->tag) {
    case PtrTag:
    case RefTag:
        return ptrsize;
    case ArrayRefTag:
        return ptrsize << 1;
    default:
        return 0;
    }
}

// Is this place reached as 'uni': a variable of this function held by value
// (a local, or a parameter taken by value, but not a global, which a callee
// may change), a field or an array element of one, or what a 'uni' reference
// such a place holds points at? Nothing else can reach it while a borrow of it
// made here lives, since the loan walk freezes it.
static int castUniPlace(INode *node) {
    if (isNameUseNode(node) && isExpNode(node)) {
        INode *dcl = ((NameUseNode *)node)->dclnode;
        return dcl && dcl->tag == VarDclTag && ((VarDclNode *)dcl)->scope >= 1
            && !(dcl->flags & FlagStatic);
    }
    switch (node->tag) {
    case FldAccessTag:
    case ArrIndexTag:
    {
        // A reference's field or element is reached through an injected
        // dereference, the case below; a virtual reference's, a slice's or a
        // pointer's is reached with none, and is not reached as 'uni'
        INode *obj = ((FnCallNode *)node)->objfn;
        uint16_t objtag = iexpGetTypeDcl(obj)->tag;
        if (objtag == RefTag || objtag == VirtRefTag || objtag == ArrayRefTag || objtag == PtrTag)
            return 0;
        return castUniPlace(obj);
    }
    case DerefTag:
    {
        INode *ref = ((StarNode *)node)->vtexp;
        INode *reftype = iexpGetTypeDcl(ref);
        return reftype->tag == RefTag && !(permGetFlags(((RefNode *)reftype)->perm) & MayAlias)
            && castUniPlace(ref);
    }
    default:
        return 0;
    }
}

// Is this reference, whatever its permission, the value a 'match' or bound
// 'if' captured in its hidden variable, a borrow made there of a place reached
// as 'uni' ('match &s')? A variable the program names is not asked: a copy of
// it could reach the value another way, which freezing does not see.
static int castBorrowsUni(INode *exp) {
    if (!isNameUseNode(exp) || !isExpNode(exp))
        return 0;
    INode *dcl = ((NameUseNode *)exp)->dclnode;
    if (dcl == NULL || dcl->tag != VarDclTag || ((VarDclNode *)dcl)->namesym != anonName
        || ((VarDclNode *)dcl)->value == NULL)
        return 0;
    INode *value = ((VarDclNode *)dcl)->value;
    return value->tag == BorrowTag && castUniPlace(((RefNode *)value)->vtexp);
}

// A reference narrowed from a sum type -- an enum, a tagged trait, an
// 'Option'-shaped enum -- to one of its variants points into the value's
// payload. Changing which variant the value holds rereads that payload as
// another type. Jon's 2018 rule ("Interior References and Shared
// Mutability"): for 'mut' references to shape-changing types, no interior
// references. So it narrows only when nothing can change the variant while the
// narrowed reference is used: the reference is 'uni', 'imm' or 'mut1'
// (MayIntRefSum), or, whatever its permission, it is a borrow made here of a
// place reached as 'uni' -- a local above all, which the stack alone owns and
// which the loan walk freezes against change while the borrow and the
// narrowed reference live. A reference of unseen origin -- a parameter, one
// reached through another reference or a shared owner -- may be one of
// several, any of which may change the variant.
static void castSumInterior(CastNode *node, RefNode *from, RefNode *to) {
    StructNode *fromstr = (StructNode *)itypeGetTypeDcl(from->vtexp);
    INode *tostr = itypeGetTypeDcl(to->vtexp);
    if (fromstr->tag != StructTag || tostr == (INode *)fromstr || !(fromstr->flags & TraitType)
        || !(fromstr->flags & (HasTagField | SameSize)))
        return;
    if ((permGetFlags(from->perm) & MayIntRefSum) || castBorrowsUni(node->exp))
        return;
    PermNode *perm = (PermNode *)itypeGetTypeDcl(from->perm);
    errorMsgNode((INode *)node, ErrorBadPerm,
        "A '%s' reference to %s, not borrowed here from a local, may not be narrowed to a reference into one of its variants: another reference may change which variant it holds while this one is used. Match a borrow of a local, a 'uni' or 'imm' reference, or the value.",
        &perm->namesym->namestr, &fromstr->namesym->namestr);
}

// Type check cast node:
// - reinterpret cast types must be same size
// - a bound pattern's conversion narrows a reference, or a value of a sum type
void castTypeCheck(TypeCheckState *pstate, CastNode *node) {
    if (iexpTypeCheckAny(pstate, &node->exp) == 0)
        return;
    // A bound pattern's conversion binds its pattern as the 'is' test before it
    // did. If that names nothing, the test has said so, and the variable this
    // conversion initializes takes the error type from it and says nothing more.
    if ((node->flags & FlagMatchBind) && !castPatternBind(node, 0)) {
        node->vtype = errorType;
        return;
    }
    if (itypeTypeCheck(pstate, &node->typ) == 0)
        return;

    node->vtype = node->typ;
    // 'null as *T' is the null of the pointer type named; 'as' anything else
    // asks a pointer to be what it is not
    if (litAdoptNullType(&node->exp, node->typ) && inodeIsError(node->exp)) {
        node->vtype = errorType;
        return;
    }
    INode *fromtype = iexpGetTypeDcl(node->exp);
    INode *totype = itypeGetTypeDcl(node->vtype);

    // Handle reinterpret casts, which must be same size
    if (!(node->flags & FlagConvert)) {
        if (totype->tag != StructTag) {
            uint32_t tosize = castBitsize(totype);
            if (tosize == 0 || tosize != castBitsize(fromtype))
                errorMsgNode(node->exp, ErrorInvType, "May only reinterpret value to the same sized primitive type");
        }
        return;
    }

    // A conversion checked here is a bound pattern's: no operator builds one,
    // and one a coercion injects is built already typed. A reference narrowed
    // to a reference is a bitcast after all, and one narrowed from a sum type
    // must be one nothing can change the variant under while it is used.
    if (fromtype->tag == RefTag && totype->tag == RefTag) {
        node->flags &= 0xFFFF - FlagConvert;
        castSumInterior(node, (RefNode *)fromtype, (RefNode *)totype);
        return;
    }
    // A virtual reference narrows to the reference it was made from, and a
    // value of a sum type, a struct carrying SameSize, to its variant
    if (totype->tag == RefTag && fromtype->tag == VirtRefTag)
        return;
    if (totype->tag == StructTag && fromtype->tag == StructTag && (fromtype->flags & SameSize))
        return;
    // Anything else is usually a pattern its 'is' test, checked first, has
    // refused already
    errorMsgNode(node->vtype, ErrorInvType, "Unsupported built-in type conversion");
}

// Analyze type comparison (is) node.
// This only supports whether downcasting specialization is possible
void castIsTypeCheck(TypeCheckState *pstate, CastNode *node) {
    node->vtype = (INode*)boolType;
    iexpTypeCheckAny(pstate, &node->exp);
    if (!castPatternBind(node, 1))
        return;
    itypeTypeCheck(pstate, &node->typ);
    if (!isExpNode(node->exp)) {
        errorMsgNode(node->exp, ErrorInvType, "'is' requires a typed expression to the left");
        return;
    }
    if (!isTypeNode(node->typ)) {
        errorMsgNode(node->typ, ErrorInvType, "'is' requires a type to the right");
        return;
    }

    // Downcasting type check from a virt ref to a ref, or from a ref to a ref?
    INode *totype = itypeGetTypeDcl(node->typ);
    INode *fromtype = iexpGetTypeDcl(node->exp);
    if (totype->tag == RefTag && (fromtype->tag == VirtRefTag || fromtype->tag == RefTag)) {
        RefNode *from = (RefNode*)fromtype;
        RefNode *to = (RefNode*)totype;

        // Regions must match. A downcast may not move a reference from one region
        // to another: they are represented differently in memory, so there is no
        // recast between them, not even into a borrow.
        TypeCompare result = itypeIsSame(from->region, to->region) ? EqMatch : NoMatch;
        if (result != NoMatch)
            result = permMatches(to->perm, from->perm);
        if (result == NoMatch) {
            errorMsgNode((INode*)node, ErrorInvType, "Reference region/permission won't downcast safely this way");
            return;
        }

        // Under the refs should be a structure. Be sure to is a subtype of from
        // Note that region/permission downcast covariantly, but the structure is contravariant
        StructNode *fromstr = (StructNode*)itypeGetTypeDcl(((RefNode*)fromtype)->vtexp);
        StructNode *tostr = (StructNode*)itypeGetTypeDcl(((RefNode*)totype)->vtexp);
        if (fromstr->tag == StructTag && tostr->tag == StructTag) {
            if (from->tag == VirtRefTag) {
                // A virtual reference to an open trait was made from a concrete
                // type, and its vtable pointer says which. A trait or an enum is
                // no such type: no vtable is ever built for one, only for its
                // implementers or variants, so there is nothing to compare with.
                if ((tostr->flags & TraitType) && !(fromstr->flags & HasTagField)) {
                    errorMsgNode((INode*)node, ErrorInvType,
                        "%s is a trait or enum, and a virtual reference narrows only to the concrete type it was made from. Narrow to one of its variants or implementers.",
                        &tostr->namesym->namestr);
                    return;
                }
                if (structVirtRefMatches(fromstr, tostr))
                    return;
            }
            // Downcasting tagged ref-to-trait to ref-to-struct requires tag
            if (!(fromstr->flags & HasTagField)) {
                errorMsgNode((INode*)node, ErrorInvType, "Impossible to downcast without a tag");
                return;
            }
            // Narrowing to the type the value already has is no downcast, and
            // structMatches is only for two different types: it falls through
            // to the "already narrowed" diagnostic below
            if (fromstr != tostr && structMatches(fromstr, (INode*)tostr, Regref))
                return;
        }
    }

    // Downcasting type check from a tagged trait to a subtype struct
    else if (fromtype->tag == StructTag && totype->tag == StructTag) {
        StructNode *from = (StructNode*)fromtype;
        if (!(from->flags & HasTagField)) {
            errorMsgNode((INode*)node, ErrorInvType, "Impossible to downcast without a tag");
            return;
        }
        if ((INode*)from != totype && structMatches(from, totype, Coercion))
            return;
    }

    // Narrowing something already concrete, which is a different mistake from
    // naming two incompatible types and deserves to be told apart from it. The
    // usual way in is not a downcast the author wrote: a method with a body
    // declared on a union or closed trait is a *default*, cloned into every
    // variant with 'Self' repointed, so inside it 'self' is one variant and
    // 'match self' asks to narrow a type that is already as narrow as it gets.
    // Reporting only that the types do not match sends the reader to look at the
    // pattern, where nothing is wrong.
    INode *fromstrnode = (fromtype->tag == RefTag || fromtype->tag == VirtRefTag)
        ? itypeGetTypeDcl(((RefNode*)fromtype)->vtexp) : fromtype;
    if (fromstrnode->tag == StructTag && !(fromstrnode->flags & TraitType)) {
        StructNode *fromstr = (StructNode*)fromstrnode;
        StructNode *base = structBaseTraitDcl(fromstr);
        if (base == NULL)
            errorMsgNode((INode*)node, ErrorInvType,
                "%s is a concrete type, so there is nothing to narrow.",
                &fromstr->namesym->namestr);
        else if (node->instnode)
            // Cloned from somewhere, which is what a default method's copy is
            errorMsgNode((INode*)node, ErrorInvType,
                "%s is one variant of %s, so it is already narrowed. A method with a body is copied into each variant, and `self` is that variant inside the copy. Declare the method without a body and implement it in each variant.",
                &fromstr->namesym->namestr, &base->namesym->namestr);
        else
            errorMsgNode((INode*)node, ErrorInvType,
                "%s is one variant of %s, so it is already narrowed. Only a reference to %s narrows to a variant.",
                &fromstr->namesym->namestr, &base->namesym->namestr, &base->namesym->namestr);
        return;
    }

    errorMsgNode((INode*)node, ErrorInvType, "Types are not compatible for this downcast specialization");
}
