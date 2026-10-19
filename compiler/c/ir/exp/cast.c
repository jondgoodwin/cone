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
    node->variants = NULL;
    return node;
}

// Create node for converting exp to a new type
CastNode *newConvCastNode(INode *exp, INode *type) {
    CastNode *node;
    newNode(node, CastNode, CastTag);
    node->flags |= FlagConvert;
    node->typ = node->vtype = type;
    node->exp = exp;
    node->variants = NULL;
    return node;
}

// Clone cast
INode *cloneCastNode(CloneState *cstate, CastNode *node) {
    CastNode *newnode;
    newnode = memAllocBlk(sizeof(CastNode));
    memcpy(newnode, node, sizeof(CastNode));
    newnode->variants = NULL;   // a trait pattern is asked again of the clone's own enum
    newnode->flags &= 0xFFFF - FlagMatchChecked;
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
    node->variants = NULL;
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
// A reference at the root is a pattern's check that the matched value is a
// reference ('&Circle', castPatternAdoptRef); the retired spelling of a managed
// one, '+R-perm T', is refused.
void castPatternMark(INode *typ) {
    // A pattern names only the variant: the matched value supplies the managed
    // reference, its region and its permission, so '+Rc-mut Circle' is refused where
    // it is written, and a reference to a managed region need not be spelled
    if (typ && (typ->tag == RefTag || typ->tag == VirtRefTag) && ((RefNode*)typ)->plusSpelled) {
        ((RefNode*)typ)->plusSpelled = 0;
        errorMsgNode(typ, ErrorPatManaged,
            "A pattern names only the variant: the matched value supplies the managed reference and its permission. Write 'Circle', not '+Rc-mut Circle'.");
    }
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
static int castPatternBindName(CastNode *node, int report) {
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

// A pattern names only the variant [topic 6a]: the matched value supplies the
// reference kind, region and permission. Matched against a reference -- a borrow of
// any permission, or an owning reference into a managed region ('Rc[mut, Shape]') --
// a pattern that names a type, 'Circle', is the same reference to it: '&imm Circle',
// 'Rc[mut, Circle]'. A pattern that is written as a reference, '&Circle', is that
// check instead, and stays what it was written (castIsTypeCheck refuses it where it
// disagrees with the matched value in kind, region or permission). The 'is' test and
// the conversion that binds each do this to their own copy of the pattern.
static void castPatternAdoptRef(CastNode *node) {
    INode *typ = node->typ;
    if (typ == NULL || typ == errorType || typ == unknownType)
        return;
    int isname = isNameUseNode(typ);
    if (!isname && !(typ->tag == FnCallTag && (typ->flags & FlagIndex)))
        return;
    if (!isTypeNode(typ) || itypeIsManagedRefType(typ) || !isExpNode(node->exp))
        return;
    // An alias of a reference type is written as one
    if (isname) {
        INode *named = itypeGetTypeDcl(typ);
        if (named->tag == RefTag || named->tag == VirtRefTag)
            return;
    }
    INode *fromtype = iexpGetTypeDcl(node->exp);
    if (fromtype->tag != RefTag && fromtype->tag != VirtRefTag)
        return;
    RefNode *from = (RefNode*)fromtype;
    // A trait on an enum's reference is a virtual reference built from the variant [topic 7]:
    // only a borrow of the enum can lend one. (Another matched reference keeps the bare name,
    // which castTraitBind refuses where the pattern binds.)
    // (a generic trait's instance, 'Boxed[i32]', is named through its callee)
    INode *root = isname ? typ : ((FnCallNode*)typ)->objfn;
    if (root != NULL && isNameUseNode(root) && castMatchedEnum(node->exp) != NULL) {
        INode *named = itypeGetTypeDcl(root);
        if (named->tag == StructTag && (named->flags & TraitType) && !(named->flags & EnumType)) {
            if (fromtype->tag == RefTag && itypeGetTypeDcl(from->region) == borrowRef)
                node->typ = (INode*)newRefNodeFull(VirtRefTag, typ, from->region, from->perm, typ);
            return;
        }
    }
    // A virtual reference narrows to the reference it was made from
    node->typ = (INode*)newRefNodeFull(RefTag, typ, from->region, from->perm, typ);
}

static int castPatternBind(CastNode *node, int report) {
    if (!castPatternBindName(node, report))
        return 0;
    castPatternAdoptRef(node);
    return 1;
}

static void castSumInterior(CastNode *node, RefNode *from, RefNode *to);

// ---- Trait patterns on an enum [topic 7] ----
//
// 'case let Measured(m)' on an enum matches every variant that complies with the trait. It is
// a test of those variants' tags (no run-time cost beyond the tags), counts toward
// exhaustiveness as the variants it names, and binds a virtual reference to the trait built
// from the variant the matched reference points at, its vtable chosen by the tag. Only a
// closed family can say which values comply, so an open trait has no trait patterns.

// The trait a pattern names, or NULL when it names anything else: an enum, a variant, a
// type that is not a struct, or a name not yet bound. A reference written around it is looked through.
static StructNode *castPatternTrait(INode *typ) {
    if (typ == NULL || typ == errorType || typ == unknownType || castPatternPending(typ) || !isTypeNode(typ))
        return NULL;
    if (typ->tag == RefTag || typ->tag == VirtRefTag)
        typ = ((RefNode*)typ)->vtexp;
    if (typ == NULL || !isTypeNode(typ))
        return NULL;
    INode *dcl = itypeGetTypeDcl(typ);
    if (dcl->tag != StructTag || !(dcl->flags & TraitType) || (dcl->flags & EnumType))
        return NULL;
    return (StructNode*)dcl;
}

// A variant complies with a trait when it can be viewed through it: it declares the trait with
// 'is' or has what it requires, as a generic's 'where T is Trait' asks, and its methods fit a
// vtable's slots. Asking registers the variant's vtable for the trait. A method over the whole enum
// ('self: &Lamp') is one function, with no slot in any variant's vtable, so it makes no variant comply.
static int castVariantComplies(StructNode *variant, StructNode *trait) {
    if (variant->tag != StructTag)
        return 0;
    return genericTypeIs((INode*)variant, trait) && structVirtRefMatches(trait, variant) != NoMatch;
}

// The variants of an enum that comply with a trait, in the enum's order
static Nodes *castTraitVariants(StructNode *enumdcl, StructNode *trait) {
    Nodes *found = newNodes(enumdcl->derived->used);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(enumdcl->derived, cnt, nodesp)) {
        if (castVariantComplies((StructNode*)*nodesp, trait))
            nodesAdd(&found, *nodesp);
    }
    return found;
}

uint32_t castIsVariants(CastNode *isnode, StructNode *enumdcl, uint8_t *has) {
    uint32_t found = 0;
    INode *single = isnode->variants ? NULL : itypeGetDerefTypeDcl(isnode->typ);
    INode **nodesp;
    uint32_t cnt, k = 0;
    for (nodesFor(enumdcl->derived, cnt, nodesp)) {
        int named = *nodesp == single;
        if (isnode->variants) {
            INode **namedp;
            uint32_t namedcnt;
            for (nodesFor(isnode->variants, namedcnt, namedp))
                named = named || *namedp == *nodesp;
        }
        if (named) {
            has[k] = 1;
            ++found;
        }
        ++k;
    }
    return found;
}

// The test of a trait pattern: the variants that comply, and the pattern as it may be written.
// Returns 1 when this is a trait pattern on an enum, whether or not it is well formed.
static int castTraitIsCheck(CastNode *node) {
    StructNode *trait = castPatternTrait(node->typ);
    StructNode *enumdcl = trait ? castMatchedEnum(node->exp) : NULL;
    if (enumdcl == NULL)
        return 0;
    INode *fromtype = iexpGetTypeDcl(node->exp);
    // The pattern names the trait, and the matched value supplies the rest (topic 6a). A plain
    // reference to a trait has no vtable to carry, so one written is refused; a virtual one is
    // the check that the matched value agrees with it
    if (node->typ->tag == RefTag) {
        errorMsgNode(node->typ, ErrorPatTraitBind,
            "A trait pattern binds a virtual reference built from the variant, not a plain reference to %s. Write %s, or &<%s.",
            &trait->namesym->namestr, &trait->namesym->namestr, &trait->namesym->namestr);
        node->vtype = (INode*)boolType;
        return 1;
    }
    if (node->typ->tag == VirtRefTag) {
        RefNode *to = (RefNode*)node->typ;
        RefNode *from = (RefNode*)fromtype;
        if (fromtype->tag != RefTag || itypeGetTypeDcl(from->region) != borrowRef) {
            errorMsgNode(node->typ, ErrorPatTraitBind,
                "A trait pattern written as a virtual reference needs the matched value to be a borrowed reference: match a borrow of the value, '&s', and the pattern is %s.",
                &trait->namesym->namestr);
            return 1;
        }
        if (!itypeIsSame(from->region, to->region) || permMatches(to->perm, from->perm) == NoMatch) {
            errorMsgNode(node->typ, ErrorInvType, "Reference region/permission won't downcast safely this way");
            return 1;
        }
    }
    Nodes *variants = castTraitVariants(enumdcl, trait);
    if (variants->used == 0) {
        errorMsgNode(node->typ, ErrorPatTraitNone,
            "No variant of %s complies with %s, so this pattern can never match. A variant complies when it declares %s with 'is' or has what it requires; a method over the whole enum (a receiver named %s) belongs to no variant.",
            &enumdcl->namesym->namestr, &trait->namesym->namestr, &trait->namesym->namestr, &enumdcl->namesym->namestr);
        return 1;
    }
    node->variants = variants;
    return 1;
}

// A bound pattern's conversion when the pattern names a trait: the virtual reference to it built
// from the matched reference, its vtable chosen by the variant's tag. The matched value must be
// a borrowed reference, since a virtual reference is one. Returns 1 when this is a trait pattern.
static int castTraitBind(CastNode *node) {
    StructNode *trait = castPatternTrait(node->typ);
    StructNode *enumdcl = trait ? castMatchedEnum(node->exp) : NULL;
    if (enumdcl == NULL)
        return 0;
    INode *fromtype = iexpGetTypeDcl(node->exp);
    if (node->typ->tag == RefTag) {      // the test has refused it
        node->vtype = errorType;
        return 1;
    }
    if (node->typ->tag != VirtRefTag) {
        errorMsgNode(node->typ, ErrorPatTraitBind,
            "%s is a trait, so this pattern binds a virtual reference to it, and only a borrowed reference can lend one. Match a borrow of the value, 'match &s' or 'match &mut s', or test with 'case is %s' and bind nothing.",
            &trait->namesym->namestr, &trait->namesym->namestr);
        node->vtype = errorType;
        return 1;
    }
    RefNode *from = (RefNode*)fromtype;
    RefNode *to = (RefNode*)node->typ;
    if (fromtype->tag != RefTag || castTraitVariants(enumdcl, trait)->used == 0) {
        node->vtype = errorType;     // the test has said so
        return 1;
    }
    // An enum laid out as a pointer, null standing for its empty variant, has no tag to choose
    // a vtable by (and a reference to one converts to no virtual reference either)
    if (structPointerField(enumdcl) != NULL) {
        errorMsgNode(node->typ, ErrorPatTraitBind,
            "%s is laid out as a pointer, with no tag to say which variant's vtable a virtual reference to %s would carry. Test with 'case is %s', or bind the variants that comply.",
            &enumdcl->namesym->namestr, &trait->namesym->namestr, &trait->namesym->namestr);
        node->vtype = errorType;
        return 1;
    }
    node->flags |= FlagTraitBind;
    node->vtype = iexpCoerceType(node->exp, node->typ);
    // The collection rule: the variant could change under a reference others share
    castSumInterior(node, from, to);
    return 1;
}

// Create the test for a value alone as a match pattern, positioned on the value.
// A bare name is marked as an 'is' pattern's root is, so that name resolution
// leaves it unbound when it has no lexical meaning: it may be a variant of the
// matched value's enum. Only a bare name: marking the root of '&x' would keep it
// a reference type rather than a borrow.
CastNode *newMatchValueNode(INode *matchee, INode *value) {
    CastNode *node = newIsNode(matchee, value);
    if (value)  // NULL where parseTerm refused the pattern and reported it
        inodeLexCopy((INode*)node, value);
    node->flags |= FlagMatchValue;
    if (value && isNameUseNode(value))
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
        // A variant named through its enum alone, 'Shape.Circle', means 'is' as the
        // variant bare does [topic 6b]
        INode *dcl = (value->flags & FlagQualified) ? itypeGetTypeDcl(value) : NULL;
        StructNode *base = dcl && dcl->tag == StructTag ? structBaseTraitDcl((StructNode*)dcl) : NULL;
        if (base != NULL && base->tag == StructTag && (base->flags & EnumType)) {
            castIsTypeCheck(pstate, node);
            return;
        }
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

// A reference of any kind: a borrow, a slice, an owner, a virtual reference
static int castIsRefKind(INode *type) {
    return type->tag == RefTag || type->tag == ArrayRefTag || type->tag == VirtRefTag;
}

// The name a permission is written with, for a message
static char *castPermName(INode *perm) {
    INode *dcl = itypeGetTypeDcl(perm);
    if (dcl->tag == PermTag && ((PermNode*)dcl)->namesym)
        return &((PermNode*)dcl)->namesym->namestr;
    if (dcl->tag == StructTag && ((StructNode*)dcl)->namesym)
        return &((StructNode*)dcl)->namesym->namestr;
    return "?";
}

// 'as' from one reference to another: reinterpretation is trusted, but a
// reference's permission is what flow and the race checks rest on, so between
// references it may only be a legal coercion (safe-types.html#as, "Reinterpretation: as"):
// the permission is the same or a supertype of the
// source's, the same check an implicit coercion asks (permMatches). Anything
// else needs 'trust', which is not built yet (but for a defaulted 'imm', below). A pointer 'as' a reference, which
// is where library code trusts memory it manages, and a reference 'as' a
// pointer are not asked. Returns whether it reported.
static int castRefPermCheck(CastNode *node, INode *fromtype, INode *totype) {
    if (!castIsRefKind(fromtype) || !castIsRefKind(totype))
        return 0;
    INode *fromperm = ((RefNode*)fromtype)->perm;
    INode *toperm = ((RefNode*)totype)->perm;
    if (permIsSame(toperm, fromperm) || permMatches(toperm, fromperm) != NoMatch)
        return 0;
    // The one case let through, until 'trust' is built: a 'ro' reference made
    // the 'imm' a type declaring Immutable gives a reference that writes no
    // permission, 'bytes as &str', 'p as Uni[str]' (lib-string.html#bytes, "Text and bytes").
    // The permission is the type's default, not written: a
    // written '&imm' (or '&imm str') is checked as any other. It promises that
    // nothing changes the bytes while the text is used, and that they are text,
    // neither of which is checked.
    if (itypeGetTypeDcl(fromperm) == (INode*)roPerm && itypeGetTypeDcl(toperm) == (INode*)immPerm
        && ((RefNode*)totype)->permImmDefault)
        return 0;
    errorMsgNode(node->exp, ErrorCastPerm,
        "`as` changes the reference's permission from %s to %s, which is not a coercion. Between references `as` may only be a legal coercion (`uni` to `mut`, `imm` or `&`; `mut` or `imm` to `&`); anything else needs `trust`, which is not built yet.%s",
        castPermName(fromperm), castPermName(toperm),
        itypeGetTypeDcl(fromperm) == (INode*)roPerm && itypeGetTypeDcl(toperm) == (INode*)immPerm
            ? " (The one exception is a type declaring Immutable, whose reference with no permission written is `imm`: `bytes as &str`.)" : "");
    return 1;
}

#define ptrsize 10000
// Give a rough idea of comparable type size for use with type checking reinterpretation casts
uint32_t castBitsize(INode *type) {
    if (type->tag == UintNbrTag || type->tag == IntNbrTag || type->tag == FloatNbrTag) {
        if (type == (INode*)usizeType || type == (INode*)isizeType)
            return ptrsize;
        return ((NbrNode *)type)->bits;
    }
    switch (type->tag) {
    case PtrTag:
        return ptrsize;
    case RefTag:
        // A reference that carries a length is a pointer and a count, a slice's size
        return refIsFat((RefNode *)type) ? ptrsize << 1 : ptrsize;
    case ArrayRefTag:
        return ptrsize << 1;
    default:
        return 0;
    }
}

// Is this type usize or isize, an integer as wide as a pointer?
static int castPtrSizedNbr(INode *type) {
    return type == (INode*)usizeType || type == (INode*)isizeType;
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
    if (dcl == NULL || dcl->tag != VarDclTag || !nameIsMatchHolder(((VarDclNode *)dcl)->namesym)
        || ((VarDclNode *)dcl)->value == NULL)
        return 0;
    INode *value = ((VarDclNode *)dcl)->value;
    if (value->tag != BorrowTag || !castUniPlace(((RefNode *)value)->vtexp))
        return 0;
    // A 'mut' borrow is an alias loan: the place has other names while it is held, so it
    // is no longer frozen against a change of variant, and the shape check follows it
    INode *type = iexpGetTypeDcl(value);
    return !(type->tag == RefTag && (permGetFlags(((RefNode *)type)->perm) & MayAliasWrite));
}

// A reference narrowed from a sum type -- an enum, a tagged trait, an
// 'Option'-shaped enum -- to one of its variants points into the value's
// payload. Changing which variant the value holds rereads that payload as
// another type. So it is the collection rule's case: the narrowing is allowed,
// and refused only if something could change the variant while the narrowed
// reference is used.
//
// Where nothing can, the narrowing needs no more: the reference is 'uni', 'imm'
// or 'mut1' (MayIntRefSum), or it is a read-only or 'uni' borrow made here
// of a place reached as 'uni' -- a local above all, which the stack alone owns and
// which the loan walk freezes against change while the borrow and the narrowed
// reference live. A 'mut' borrow of one is an alias loan and is followed like the rest.
// A reference of unseen origin -- a parameter, one reached through
// another reference or a shared owner -- may be one of several, any of which may
// change the variant: the loan walk follows the narrowed reference as it follows a
// borrow into a collection (castSumNarrows, flowpath.c pwNarrowShared), and
// refuses the change that could be made while it is used.
// Is this reference narrowed from an enum, tagged trait or 'Option'-shaped enum to one of
// its variants?
static int castIsSumNarrow(RefNode *from, RefNode *to) {
    StructNode *fromstr = (StructNode *)itypeGetTypeDcl(from->vtexp);
    INode *tostr = itypeGetTypeDcl(to->vtexp);
    return fromstr->tag == StructTag && tostr != (INode *)fromstr && (fromstr->flags & TraitType)
        && (fromstr->flags & (HasTagField | SameSize));
}

static int castSumNarrowsRefs(CastNode *node, RefNode *from, RefNode *to) {
    if (!castIsSumNarrow(from, to))
        return 0;
    return !((permGetFlags(from->perm) & MayIntRefSum) || castBorrowsUni(node->exp));
}

// A borrow is followed by the loan walk. An owner narrowed to a variant ('Rc[imm, Shape]'
// to 'Rc[imm, Circle]') is a copy of the owner with another static type [topic 6a]: it
// holds the same allocation, counted one more, and a variant has its enum's address, so
// nothing is adjusted. That is allowed only where no writer can change the variant: no loan
// stands for another owner of the value, so nothing could follow a change through one. An
// owner whose permission lets a writer store another variant ('mut', 'ro', a lock) stays
// refused.
static void castSumInterior(CastNode *node, RefNode *from, RefNode *to) {
    if (!castIsSumNarrow(from, to))
        return;
    if (itypeGetTypeDcl(from->region) != borrowRef) {
        if (permGetFlags(from->perm) & MayIntRefSum) {
            node->flags |= FlagOwnerNarrow;
            return;
        }
        PermNode *perm = (PermNode *)itypeGetTypeDcl(from->perm);
        errorMsgNode((INode *)node, ErrorBadPerm,
            "A '%s' reference to %s, held by an owner (an 'Rc'), may not be narrowed to a reference into one of its variants: another owner could change which variant it holds while this one is used. Bind the payload by copy, or match through a borrow of the value, a 'uni' or 'imm' reference, or the value.",
            &perm->namesym->namestr, &((StructNode *)itypeGetTypeDcl(from->vtexp))->namesym->namestr);
        return;
    }
    if (castSumNarrowsRefs(node, from, to))
        node->flags |= FlagVariantNarrow;
}

// The field of the hidden local holding the matched value, which a variable
// bound to a type-variant's contents reads: 'local.(payload)'
static INode *castPayloadAccess(VarDclNode *local, INode *lexnode) {
    FnCallNode *access = newFnCallLower(lexnode, newNameUseFromDclNode((INode*)local, lexnode), 0);
    access->methfld = (INode*)newMemberUseNode(payloadName);
    inodeLexCopy(access->methfld, lexnode);
    return (INode*)access;
}

// The narrowing of a reference to a reference to one of its enum's variants, made by flow
// typing (narrow.c): a recast, type checked here as a bound pattern's conversion of a
// reference would be, so a reference other references may share is followed by the loan
// walk
CastNode *castNarrowRef(INode *exp, RefNode *to) {
    CastNode *cast = newRecastNode(exp, (INode *)to);
    inodeLexCopy((INode *)cast, exp);
    cast->vtype = (INode *)to;
    cast->flags |= FlagMatchChecked;
    castSumInterior(cast, (RefNode *)iexpGetTypeDcl(exp), to);
    return cast;
}

// Is a variant plain data: nothing in it to move, finalize, count or trace? Reading
// its bytes out of a place is a copy of it, and the place keeps its own.
static int castVariantIsPlain(StructNode *variant) {
    INode *type = (INode *)variant;
    return !itypeIsMove(type) && !itypeNeedsFinal(type) && !itypeHoldsTraced(type)
        && !flowIsRcRef(type) && !flowHeldCounted(type);
}

// A guard's binding reads the matched value before the arm is chosen, so it takes
// nothing from it, whether it is a place or held in a hidden variable. What the pattern binds is copied if it is plain data, and where it
// moves (an owner) the guard is shown it through a read-only borrow of the value, so
// that a guard which fails leaves the place whole for the cases after it. The
// pattern's type node is the 'is' test's, bound by the test before the guard.
static void castGuardBorrow(CastNode *cast) {
    if (!(cast->flags & FlagMatchGuardBind) || castPatternPending(cast->typ) || !isTypeNode(cast->typ))
        return;
    INode *variant = itypeGetTypeDcl(cast->typ);
    if (variant->tag != StructTag || castVariantIsPlain((StructNode *)variant))
        return;
    INode *place = cast->exp;
    RefNode *borrow = newRefNodeFull(BorrowTag, place, borrowRef, newPermUseNode(roPerm), place);
    RefNode *reftype = newRefNodeFull(RefTag, place, borrowRef, newPermUseNode(roPerm), cast->typ);
    cast->exp = (INode *)borrow;
    cast->typ = (INode *)reftype;
    cast->vtype = (INode *)reftype;
}

// An arm that binds a payload that is plain data copies it, and the matched place it
// came from stays whole: the hidden variable the arm takes the place into holds the
// variant alone, a copy of its leading bytes. Where the variant owns something, the
// whole place moves into the variable, as a binding that moves always has.
static void castArmCopies(CastNode *cast, StructNode *variant) {
    // (Anything else, flow must move or count: a conversion's value is a temporary
    // to it, which would leave that undone)
    if (!castVariantIsPlain(variant) || !isNameUseNode(cast->exp))
        return;
    NameUseNode *armuse = (NameUseNode *)cast->exp;
    if (armuse->dclnode == NULL || armuse->dclnode->tag != VarDclTag || !(armuse->dclnode->flags & FlagMatchArm))
        return;
    VarDclNode *arm = (VarDclNode *)armuse->dclnode;
    if (arm->value == NULL || !isExpNode(arm->value))
        return;
    CastNode *take = newConvCastNode(arm->value, (INode *)variant);
    inodeLexCopy((INode *)take, arm->value);
    take->vtype = (INode *)variant;
    arm->value = (INode *)take;
    arm->vtype = (INode *)variant;
    armuse->vtype = (INode *)variant;
}

int castBindProject(TypeCheckState *pstate, VarDclNode *var) {
    CastNode *cast = (CastNode*)var->value;
    if ((cast->flags & FlagMatchChecked) || pstate->extend == NULL || pstate->extend->var != var)
        return 0;
    castGuardBorrow(cast);
    // The conversion is checked here, once, as the variable would have checked it:
    // what it narrows to says whether the pattern named a variant declared as a type
    // (a second check would find the conversion already made, which castTypeCheck
    // leaves for a plain narrowing, and read it as a reinterpretation)
    iexpTypeCheckAny(pstate, &var->value);
    cast->flags |= FlagMatchChecked;
    INode *type = cast->vtype;
    if (type == errorType || type == unknownType || !isTypeNode(type))
        return 0;
    type = itypeGetTypeDcl(type);
    if (type->tag == StructTag)
        castArmCopies(cast, (StructNode *)type);
    INode *target = (type->tag == RefTag || type->tag == VirtRefTag)
        ? itypeGetTypeDcl(((RefNode*)type)->vtexp) : type;
    if (target->tag != StructTag || !structIsTypeVariant((StructNode*)target))
        return 0;
    StructNode *variant = (StructNode*)target;

    // The matched value under the variant's name, in a local of its own that the
    // variable's initializer reads from
    VarDclNode *alias = newVarDclFull(matchAliasName, VarDclTag, unknownType, (INode*)immPerm, (INode*)cast);
    inodeLexCopy((INode*)alias, (INode*)var);
    alias->scope = var->scope;
    varDclTypeCheck(pstate, alias);
    alias->flags |= TypeChecked;

    INode *contents = castPayloadAccess(alias, (INode*)var);
    if (type->tag == RefTag || type->tag == VirtRefTag) {
        RefNode *ref = (RefNode*)type;
        if (itypeGetTypeDcl(ref->region) != borrowRef) {
            errorMsgNode((INode*)var, ErrorTypeVariant,
                "A reference into a managed value (an Rc, a Uni) cannot lend the contents of %s as a binding yet: match a borrow of the value, '&', or the value itself.",
                &variant->namesym->namestr);
            var->vtype = errorType;
            var->value = NULL;
            return 1;
        }
        // A borrow of the contents, as the narrowed reference is of the variant
        RefNode *borrow = newRefNodeFull(BorrowTag, (INode*)var, (INode*)borrowRef, newPermUseNode((PermNode*)itypeGetTypeDcl(ref->perm)), contents);
        var->value = (INode*)borrow;
    }
    else
        var->value = contents;
    varDclHoistBefore(pstate, var, alias);
    return 1;
}

// Type check cast node:
// - reinterpret cast types must be same size
// - a bound pattern's conversion narrows a reference, or a value of a sum type
void castTypeCheck(TypeCheckState *pstate, CastNode *node) {
    // A bound pattern's conversion castBindProject has checked
    if (node->flags & FlagMatchChecked)
        return;
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
    if ((node->flags & FlagMatchBind) && castTraitBind(node))
        return;
    // 'null as *T' is the null of the pointer type named; 'as' anything else
    // asks a pointer to be what it is not
    if (litAdoptNullType(&node->exp, node->typ) && inodeIsError(node->exp)) {
        node->vtype = errorType;
        return;
    }
    INode *fromtype = iexpGetTypeDcl(node->exp);
    INode *totype = itypeGetTypeDcl(node->vtype);

    // A slice 'as' a pointer to its elements is the address of the first, the
    // count dropped. It is no reinterpretation (a slice is two words), so it is
    // the conversion the old implicit coercion made, asked for. Only the
    // pointer of the slice's own element type: it is no way to retype memory.
    if (!(node->flags & FlagConvert) && fromtype->tag == ArrayRefTag && totype->tag == PtrTag
        && itypeIsSame(((RefNode*)fromtype)->vtexp, ((StarNode*)totype)->vtexp)) {
        node->flags |= FlagConvert;
        return;
    }

    // Handle reinterpret casts, which must be same size
    if (!(node->flags & FlagConvert)) {
        if (castRefPermCheck(node, fromtype, totype))
            return;
        // usize and isize are as wide as a pointer, which a fixed-width number
        // is only on some targets, so no reinterpretation joins the two: what
        // compiles on x64 must compile on wasm32
        if (isNbr(fromtype) && isNbr(totype)
            && castPtrSizedNbr(fromtype) != castPtrSizedNbr(totype))
            errorMsgNode(node->exp, ErrorPtrSizedAs,
                "usize and isize are pointer-sized, so 'as' may not reinterpret one as a fixed-width number, or a fixed-width number as one; convert with usize.from(x) or u64.from(x)");
        else if (totype->tag != StructTag) {
            uint32_t tosize = castBitsize(totype);
            if (tosize == 0 || tosize != castBitsize(fromtype))
                errorMsgNode(node->exp, ErrorInvType, "May only reinterpret value to the same sized primitive type");
        }
        // A key's brand is neither lost nor gained by a cast, but through a
        // raw pointer, which nothing checks: so a region makes its keys, '(p as
        // *T) as &'=a mut T', from the memory it hands out
        if (lifeInvariantSeen && lifeBrandKnown(node->typ, node->typ)
            && totype->tag != PtrTag && fromtype->tag != PtrTag)
            lifeBrandsCoerce(((IExpNode*)node->exp)->vtype, node->typ, node->exp);
        return;
    }

    // A pattern's type names the brands the matched value carries, and no others:
    // 'case let Some[&'=a T](s)' rebrands nothing
    if (lifeInvariantSeen && lifeBrandKnown(node->typ, node->typ))
        lifeBrandsCoerce(((IExpNode*)node->exp)->vtype, node->typ, node->typ);

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
    narrowNoteTest((INode*)node);
    // A value already reported has no variants to name, so the pattern says nothing of it
    if (isExpNode(node->exp) && ((IExpNode*)node->exp)->vtype == errorType) {
        NameUseNode *root = castPatternName(node->typ, NULL);
        if (root) {
            root->flags &= 0xFFFF - FlagPattern;
            root->dclnode = errorType;
        }
        return;
    }
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

    // A trait named on an enum matches the variants that comply
    if (castTraitIsCheck(node))
        return;

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
                    if (tostr->flags & EnumType)
                        errorMsgNode((INode*)node, ErrorInvType,
                            "%s is a trait or enum, and a virtual reference narrows only to the concrete type it was made from. Narrow to one of its variants or implementers.",
                            &tostr->namesym->namestr);
                    else
                        errorMsgNode((INode*)node, ErrorPatTrait,
                            "%s is a trait, and a virtual reference narrows only to the concrete type it was made from. A trait pattern is for an enum, which can say which of its variants comply with it; narrow this to one of its implementers.",
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

    StructNode *pattrait = castPatternTrait(node->typ);
    if (pattrait != NULL)
        errorMsgNode((INode*)node, ErrorPatTrait,
            "%s is a trait, and only an enum can say which of its values comply with a trait. Match an enum, or narrow this value to a concrete type.",
            &pattrait->namesym->namestr);
    else
        errorMsgNode((INode*)node, ErrorInvType, "Types are not compatible for this downcast specialization");
}
