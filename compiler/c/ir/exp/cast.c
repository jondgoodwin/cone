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
// castPatternBind to look up in the matched value's enum
void castPatternMark(INode *typ) {
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

// Answer whether a value of fromtype may be converted to Bool.
// 'value into Bool' and the constructor form 'Bool[value]' are the same
// conversion, so typeLitNbrCheck asks here rather than keeping a second list
// that would have to be maintained alongside this one.
int castConvertsToBool(INode *fromtype) {
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

// Type check cast node:
// - reinterpret cast types must be same size
// - Ensure type can be safely converted to target type
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
    else {
        // Auto-generated downcasting "conversion" may in face be a bitcast
        if (fromtype->tag == RefTag && totype->tag == RefTag) {
            node->flags &= 0xFFFF - FlagConvert;
        }
    }

    // Handle conversion to bool
    if (totype == (INode*)boolType) {
        if (!castConvertsToBool(fromtype))
            errorMsgNode(node->exp, ErrorInvType, "Only numbers and ref/ptr may convert to Bool");
        return;
    }
    switch (totype->tag) {
    // A slice is two words, so "convert it to an integer" has no single answer:
    // the length and the data address are both candidates and both are already
    // spelled better, as 's.len' and 'p as usize'. This used to type check and
    // then emit 'trunc { i32*, i64 } to i64', which --verify rejects.
    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
        if (fromtype->tag == UintNbrTag || fromtype->tag == IntNbrTag || fromtype->tag == FloatNbrTag)
            return;
        break;
    // A reference is reached from a virtual reference, which is the
    // auto-generated downcast, or from another reference, which the block above
    // has already turned back into a bitcast. Not from a pointer: a reference
    // carries a region, a permission and a lifetime, and a raw pointer supplies
    // none of them, so there is nothing to build one out of. This used to fall
    // through into the pointer case and be accepted, and genlConvert has no arm
    // for it -- 'p into &i32' reached the arm's assert, which a Release build
    // compiles out, and died on a null LLVM value. 'p as &i32' is the spelling
    // for keeping the bits, and it generates a bitcast.
    case RefTag:
        if (fromtype->tag == VirtRefTag || fromtype->tag == RefTag)
            return;
        break;
    case PtrTag:
        if (fromtype->tag == RefTag || fromtype->tag == PtrTag)
            return;
        break;
    case VirtRefTag:
        break;
    case StructTag:
        if (fromtype->tag == StructTag && (fromtype->flags & SameSize))
            return;
        break;
    }
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
            if (structMatches(fromstr, (INode*)tostr, Regref))
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
        if (structMatches(from, totype, Coercion))
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
