/** Parse expressions
 * @file
 *
 * The parser translates the lexer's tokens into IR nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ir/ir.h"
#include "../ir/nametbl.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "lexer.h"

#include <stdio.h>
#include <assert.h>

// Parse a name use: one identifier, bound to nothing until name resolution.
// A path through namespaces is written with periods and parses as a chain of
// member accesses, which fnCallNameRes collapses once it knows what the base
// name is. Every caller has already established that a name is here.
INode *parseNameUse(ParseState *parse) {
    NameUseNode *nameuse = newNameUseNode(NULL);
    if (lexIsToken(IdentToken)) {
        nameuse->namesym = lex->val.ident;
        lexNextToken();
    }
    else
        errorMsgLex(ErrorNoVar, "Missing variable name");
    return (INode*)nameuse;
}

static INode *parseContentsAfter(ParseState *parse, INode *node);

// Parse an array literal. '[n; x]', the retired fill literal, is still read
// here, so that name resolution can tell its two spellings apart: a type for x
// is the old array type, a value the old fill (arrayNameRes).
INode *parseArrayLit(ParseState *parse) {
    ArrayNode *array = newArrayNode();
    lexNextToken();
    int svinlist = parse->inlist;
    parse->inlist = 1;

    // Gather comma-separated expressions that are likely elements or element type
    while (1) {
        nodesAdd(&array->elems, parseContentsAfter(parse, parseSimpleExpr(parse)));
        if (!lexIsToken(CommaToken))
            break;
        lexNextToken();
    }

    // Semi-colon signals we had dimensions instead, swap and then get elements
    if (lexIsToken(SemiToken)) {
        lexNextToken();
        Nodes *elems = array->dimens;
        array->dimens = array->elems;
        while (1) {
            nodesAdd(&elems, parseSimpleExpr(parse));
            if (!lexIsToken(CommaToken))
                break;
            lexNextToken();
        };
        array->elems = elems;
    }
    parse->inlist = svinlist;
    parseCloseTok(RBracketToken);

    return (INode *)array;
}

static Nodes *parseIndexArgs(ParseState *parse, FnCallNode *fncall);
Nodes *parseArgs(ParseState *parse);

// Parse a construction, 'new Point(1, 2)': 'new', the type, then in
// parentheses the arguments the type's 'init' takes, which may be left off
// when there are none ('new Point' is 'new Point()'). The type is a name, a
// path through namespaces ('geomath.Vec3') and type arguments in brackets
// ('Pair[i32, f32]'); every '.' and '[' before the parentheses belongs to it,
// and the suffixes after them apply to the value constructed. Type check
// selects the 'init' (typeLitNewCheck). A managed reference type,
// 'new Rc[mut, Node](1)' or an alias of one, allocates in its region, the
// parentheses carrying the value's 'init' arguments.
//
// 'trynew Rc[mut, Node](1)' is an allocation that may fail, giving an Option
// of the reference. It is parsed as '?' is, an Option around the construction,
// which name resolution takes apart once 'Option' is bound
// (allocateQuesNameRes).
INode *parseNew(ParseState *parse) {
    FnCallNode *ctor = newFnCallNode(NULL, 0);
    ctor->flags |= FlagNew;
    FnCallNode *opttype = NULL;
    if (lexIsToken(TrynewToken)) {
        ctor->flags |= FlagTryNew;
        opttype = newFnCallNode((INode*)newNameUseNode(optionName), 1);
        opttype->tag = QuesTag;
        nodesAdd(&opttype->args, (INode*)ctor);
    }
    lexNextToken();
    if (!lexIsToken(IdentToken))
        errorMsgLex(ErrorBadTerm, "Expected the type to construct after 'new': 'new Point(1, 2)'");
    INode *type = parseNameUse(parse);
    while (1) {
        if (lexIsToken(DotToken)) {
            FnCallNode *path = newFnCallNode(type, 0);
            lexNextToken();
            if (lexIsToken(IdentToken))
                path->methfld = (INode*)newMemberUseNode(lex->val.ident);
            else
                errorMsgLex(ErrorNoMbr, "Expected a name after '.' in the type to construct");
            lexNextToken();
            type = (INode*)path;
        }
        else if (lexIsToken(LBracketToken)) {
            FnCallNode *inst = newFnCallNode(type, 0);
            inst->flags |= FlagIndex;
            inst->args = parseIndexArgs(parse, inst);
            type = (INode*)inst;
        }
        else
            break;
    }
    ctor->objfn = type;
    ctor->args = lexIsToken(LParenToken) ? parseArgs(parse) : newNodes(2);
    return opttype ? (INode*)opttype : (INode*)ctor;
}

static INode *parseEntries(ParseState *parse);

// Parse a term: literal, identifier, etc.
INode *parseTerm(ParseState *parse) {
    // A '(' that begins an entry after '<-' holds a list of entries; the word
    // is for this term alone
    int entryparen = parse->entryparen;
    parse->entryparen = 0;
    switch (lex->toktype) {
    case nilToken:
    {
        NilLitNode *node = newNilLitNode();
        lexNextToken();
        return (INode *)node;
    }
    case nullToken:
    {
        NullLitNode *node = newNullLitNode();
        lexNextToken();
        return (INode *)node;
    }
    case trueToken:
    {
        ULitNode *node = newULitNode(1, (INode*)boolType);
        lexNextToken();
        return (INode *)node;
    }
    case falseToken:
    {
        ULitNode *node = newULitNode(0, (INode*)boolType);
        lexNextToken();
        return (INode *)node;
    }
    case VoidToken:
    {
        VoidTypeNode *voidnode = newVoidNode();
        lexNextToken();
        return (INode *)voidnode;
    }
    case IntLitToken:
        {
            ULitNode *node = newULitNode(lex->val.uintlit, lex->langtype);
            lexNextToken();
            return (INode *)node;
        }
    case FloatLitToken:
        {
            FLitNode *node = newFLitNode(lex->val.floatlit, lex->langtype);
            lexNextToken();
            return (INode *)node;
        }
    case StringLitToken:
        {
            SLitNode *node = newSLitNode(lex->val.strlit, lex->strlen);
            lexNextToken();
            return (INode *)node;
        }
    case IdentToken:
        return (INode*)parseNameUse(parse);
    case LParenToken:
        {
            INode *node;
            lexNextToken();
            int svinlist = parse->inlist;
            parse->inlist = 1;
            node = entryparen ? parseEntries(parse) : parseAnyExpr(parse);
            parse->inlist = svinlist;
            parseCloseTok(RParenToken);
            return node;
        }
    case LBracketToken:
        return parseArrayLit(parse);
    case NewToken:
    case TrynewToken:
        return parseNew(parse);
    case IfToken:
        return parseIf(parse);
    case MatchToken:
        return parseMatch(parse);
    case WhileToken:
        return parseWhile(parse, NULL, 0);
    case LifetimeToken:
        return parseLifetime(parse, 0);
    case LCurlyToken:
        return parseExprBlock(parse, 0);
    default:
        errorMsgLex(ErrorBadTerm, "Invalid term: expected name, literal, etc.");
        lexNextToken(); // Avoid infinite loop
        return NULL;
    }
}

// Parse a function/method call argument
INode *parseArg(ParseState *parse) {
    INode *arg = parseContentsAfter(parse, parseSimpleExpr(parse));
    if (lexIsToken(ColonToken)) {
        if (arg->tag != NameUseTag)
            errorMsgNode((INode*)arg, ErrorNoName, "Expected a named identifier");
        arg = (INode*)newNamedValNode(arg);
        lexNextToken();
        ((NamedValNode *)arg)->val = parseContentsAfter(parse, parseSimpleExpr(parse));
    }
    return arg;
}

// Parse multiple arguments inside () or []. Return a Nodes containing all argument nodes.
Nodes *parseArgs(ParseState *parse) {
    int closetok = lex->toktype == LBracketToken ? RBracketToken : RParenToken;
    lexNextToken();
    Nodes *args = newNodes(8);
    int svinlist = parse->inlist;
    parse->inlist = 1;
    if (!lexIsToken(closetok)) {
        nodesAdd(&args, parseArg(parse));
        while (lexIsToken(CommaToken)) {
            lexNextToken();
            nodesAdd(&args, parseArg(parse));
        }
    }
    parse->inlist = svinlist;
    parseCloseTok(closetok);
    return args;
}

// One argument inside '[...]'. A static permission is a keyword, so it is no
// term, and is taken here as the permission it names: 'Rc[mut, Node]' is a
// managed reference type, its permission first (fnCallLowerManagedRef). What a
// permission may be an argument of is type check's to judge.
static INode *parseIndexArg(ParseState *parse) {
    if (lexIsToken(PermToken)) {
        INode *perm = newPermUseNode((PermNode*)lex->val.ident->node);
        lexNextToken();
        return perm;
    }
    // A lifetime: 'Cursor['a]'. Held as a name use of its name, which begins
    // with the quote no other name can, until parseIndexArgs takes it out.
    if (lexIsToken(LifetimeToken)) {
        INode *life = (INode*)newNameUseNode(lex->val.ident);
        lexNextToken();
        return life;
    }
    return parseArg(parse);
}

// Is this an argument parseIndexArg read as a lifetime?
static int parseIsLifeArg(INode *arg) {
    return arg->tag == NameUseTag && ((NameUseNode*)arg)->namesym->namestr == '\'';
}

// A lifetime is named only in a type of a function's signature, or of a
// struct's field, where it is checked (lifetime.h): answer whether it may be
// here, refusing it if not. A field's names are noted for its struct to
// declare or check (lifeStructDeclare). 'at' is where the name is written,
// or NULL while the lexer is on it.
static int parseLifeNamed(ParseState *parse, Name *name, INode *at) {
    if (parse->intype && parse->lifesig) {
        parse->lifesig->lifenamed = 1;
        return 1;
    }
    if (parse->intype && parse->lifestruct) {
        if (name != staticLifeName) {
            if (parse->lifestruct->lifeparms == NULL)
                parse->lifestruct->lifeparms = newLifeParms();
            LifeParms *parms = parse->lifestruct->lifeparms;
            if (parms->usedat == NULL)
                parms->usedat = newNodes(4);
            nodesAdd(&parms->usedat, at ? at : (INode*)newNameUseNode(name));
        }
        return 1;
    }
    // An invariant lifetime may be named in a type inside a body too -- a
    // cast making a key from a pointer, a variable's type, a generic's type
    // argument ('None[&'=a Node['=a]]', which the parser cannot yet tell from
    // a value) -- since what it stands for is checked as an identity wherever
    // a value meets a type (lifeBrandsCoerce): it must be one the function's
    // signature names (lifeBrandKnown)
    if (lifeIsInvariant(name))
        return 1;
    char *msg = parse->intype
        ? "A lifetime is named in the types of a function's signature and of a struct's fields, not in a variable's type."
        : "A lifetime is named on a borrowed reference type ('&'a T') or on a type's use ('Cursor['a]'), not on a borrow or a value.";
    if (at)
        errorMsgNode(at, ErrorLifetimePlace, "%s", msg);
    else
        errorMsgLex(ErrorLifetimePlace, "%s", msg);
    return 0;
}

// Parse the arguments of an index, 'x[...]': a list of expressions, or one range
// that a borrow makes a slice of part of an array (doc/reference/
// refarrayref.html, "Subslices"). 'a..b' excludes b and 'a...b' includes it; a
// missing start is 0, and a missing end, 'a..', is the array's end. A range is
// held on the index as FlagRange, its arguments the start and, unless it runs
// to the end, the end; FlagRangeIncl says the end was written with '...'.
//
// The lifetimes a type's use names, 'Cursor['a]', 'Parser['s, 'r]', or
// 'Cursor['a, T]' beside its type arguments, are taken out of the arguments
// into the name's LifeUse (lifetime.h), in the order written: a lifetime is
// never instanced, so it is no argument of the instance. Where a lifetime may
// not be named (parseLifeNamed), or on a type not named alone, it is refused.
static Nodes *parseIndexArgsIn(ParseState *parse, FnCallNode *fncall);
static Nodes *parseIndexArgs(ParseState *parse, FnCallNode *fncall) {
    int svinlist = parse->inlist;
    parse->inlist = 1;
    Nodes *args = parseIndexArgsIn(parse, fncall);
    parse->inlist = svinlist;
    uint32_t nlifes = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(args, cnt, nodesp)) {
        if (parseIsLifeArg(*nodesp))
            ++nlifes;
    }
    if (nlifes == 0)
        return args;
    Nodes *types = newNodes(args->used - nlifes + 1);
    LifeUse *lifeuse = memAllocBlk(sizeof(LifeUse));
    lifeuse->names = memAllocBlk(nlifes * sizeof(Name *));
    lifeuse->count = 0;
    lifeuse->typeargs = NULL;
    lifeuse->at = NULL;
    lifeuse->held = NULL;
    int allowed = 1;
    for (nodesFor(args, cnt, nodesp)) {
        if (!parseIsLifeArg(*nodesp)) {
            nodesAdd(&types, *nodesp);
            continue;
        }
        Name *name = ((NameUseNode*)*nodesp)->namesym;
        if (lifeuse->at == NULL)
            lifeuse->at = *nodesp;
        if (allowed && !parseLifeNamed(parse, name, *nodesp))
            allowed = 0;
        lifeuse->names[lifeuse->count++] = name;
    }
    if (allowed) {
        if (fncall->objfn && fncall->objfn->tag == NameUseTag)
            ((NameUseNode*)fncall->objfn)->lifeuse = lifeuse;
        else
            errorMsgNode(lifeuse->at, ErrorLifetimeArgs, "Lifetimes are named on a struct's use by its name: 'Cursor['a]'.");
    }
    return types;
}

static Nodes *parseIndexArgsIn(ParseState *parse, FnCallNode *fncall) {
    lexNextToken();
    Nodes *args = newNodes(2);
    INode *start = NULL;
    if (!lexIsToken(DotDotToken) && !lexIsToken(EllipsisToken)) {
        if (lexIsToken(RBracketToken)) {
            lexNextToken();
            return args;
        }
        start = parseIndexArg(parse);
        if (!lexIsToken(DotDotToken) && !lexIsToken(EllipsisToken)) {
            nodesAdd(&args, start);
            while (lexIsToken(CommaToken)) {
                lexNextToken();
                nodesAdd(&args, parseIndexArg(parse));
            }
            parseCloseTok(RBracketToken);
            return args;
        }
    }

    // A range
    fncall->flags |= FlagRange;
    if (lexIsToken(EllipsisToken))
        fncall->flags |= FlagRangeIncl;
    lexNextToken();
    if (start == NULL)
        start = (INode*)newULitNode(0, (INode*)usizeType);
    nodesAdd(&args, start);
    if (!lexIsToken(RBracketToken))
        nodesAdd(&args, parseSimpleExpr(parse));
    else if (fncall->flags & FlagRangeIncl)
        errorMsgLex(ErrorBadIndex, "A range that includes its end, '...', must say where it ends");
    parseCloseTok(RBracketToken);
    return args;
}

// Parse a '.'-based method call/field access
INode *parseDotCall(ParseState *parse, INode *node, uint16_t flags) {
    FnCallNode *fncall = newFnCallNode(node, 0);
    fncall->flags |= flags;
    lexNextToken();

    // Get field/method name
    if (lexIsToken(IdentToken))
        fncall->methfld = (INode*)newMemberUseNode(lex->val.ident);
    // Or integer constant (for tuple element)
    else if (lexIsToken(IntLitToken)) {
        fncall->methfld = (INode*)newULitNode(lex->val.uintlit, lex->langtype);
    }
    else
        errorMsgLex(ErrorNoMbr, "This should be a named field/method");
    lexNextToken();

    // Parentheses after '.' are part of same fncall operation
    if (lexIsToken(LParenToken))
        fncall->args = parseArgs(parse);
    return (INode*)fncall;
}

// This processes all suffix operators successively
// Returns a node with first suffix inner-most, last is outermost.
INode *parseSuffix(ParseState *parse, INode *node, uint16_t flags) {

    // Process as many suffixes as we have, each applying to the term before
    while (1) {
        if (lexIsToken(DotToken)) {
            node = parseDotCall(parse, node, flags);
        }

        // Handle () suffix and enclosed arguments
        else if (lexIsToken(LParenToken)) {
            FnCallNode *fncall = newFnCallNode(node, 0);
            fncall->flags |= flags;
            fncall->args = parseArgs(parse);
            node = (INode*)fncall;
        }

        // Handle [] indexing suffix and enclosed arguments
        else if (lexIsToken(LBracketToken)) {
            FnCallNode *fncall = newFnCallNode(node, 0);
            fncall->flags |= flags | FlagIndex;
            fncall->args = parseIndexArgs(parse, fncall);
            // A struct's use naming only its lifetimes, 'Cursor['a]', is the
            // name itself, which holds them
            if (fncall->args->used == 0 && node->tag == NameUseTag && ((NameUseNode*)node)->lifeuse)
                continue;
            node = (INode*)fncall;
        }

        // Handle postfix ++
        else if (lexIsToken(IncrToken)) {
            node = (INode*)newFnCallOpname(node, incrPostName, 0);
            node->flags |= FlagLvalOp;
            lexNextToken();
        }

        // Handle postfix --
        else if (lexIsToken(DecrToken)) {
            node = (INode*)newFnCallOpname(node, decrPostName, 0);
            node->flags |= FlagLvalOp;
            lexNextToken();
        }

        // No suffix, we are done with suffixes
        else
            return node;
    }
    return node; // we never get here
}

// Parse a term wrapped by any suffixes.
// This is the normal precedence (except for borrowed references)
INode *parseSuffixTerm(ParseState *parse) {
    return parseSuffix(parse, parseTerm(parse), 0);
}

INode *parsePrefix(ParseState *parse);

// Parse an "ampersand term" for a borrowed ref type or constructor:
// - Some reference type ('&', '&[]' or '&<')
// - Lifetime, in a type
// - Static permission
// - Borrowed ref term (including to an anonymous function or closure)
INode *parseAmper(ParseState *parse) {
    // Create appropriate RefNode, depending on ampersand operator
    RefNode *anode;
    switch (lex->toktype) {
    case AmperToken:
        anode = newRefNode(RefTag); break;
    case ArrayRefToken:
        anode = newRefNode(ArrayRefTag); break;
    case VirtRefToken:
        anode = newRefNode(VirtRefTag); break;
    }
    lexNextToken();

    // Lifetime (optional), before the permission, as the grammar and Rust
    // place it: '&'a mut T'. It is named only on a borrowed reference type in a
    // function's signature or a struct's field, where it is checked
    // (lifetime.h).
    if (lexIsToken(LifetimeToken)) {
        // A key is a plain reference: a slice or a virtual reference reaches
        // what it points at by indexing or dispatch, which no arena's '[]' does
        if (lifeIsInvariant(lex->val.ident) && anode->tag != RefTag)
            errorMsgLex(ErrorLifetimeInvariant, "An invariant lifetime is on a plain reference, '&'=a T': a slice or a virtual reference does not take one.");
        else if (parseLifeNamed(parse, lex->val.ident, NULL))
            anode->lifename = lex->val.ident;
        lexNextToken();
    }

    // Static permission (optional). In a type, 'new' is the permission of an
    // initializer's 'self', '&new'; in a value, '&new Point(1, 2)' is a borrow
    // of a construction.
    if (lexIsToken(NewToken) && parse->intype) {
        anode->perm = newPermUseNode(newPerm);
        lexNextToken();
    }
    else
        anode->perm = parsePerm();

    // Handle borrowed reference to anonymous function/closure
    // Note: This could also be a ref to a function signature. We sort this out later.
    if (lexIsToken(FnToken)) {
        // In a return type, '&fn' is a function-signature type and nothing
        // more: a '{' after the signature opens the body of the function whose
        // return type this is. The signature is read here rather than by
        // parseFn, which would take that block as an anonymous function's own.
        if (parse->inrettype) {
            lexNextToken();
            if (lexIsToken(IdentToken)) {
                errorMsgLex(WarnName, "Unnecessary function name is ignored");
                lexNextToken();
            }
            anode->vtexp = parseFnSig(parse, 1);
            parseFnSigSettle(parse, (FnSigNode*)anode->vtexp, 1);
            return (INode *)anode;
        }
        FnDclNode *fndcl = (FnDclNode*)parseFn(parse, ParseMayAnon | ParseMayImpl | ParseMaySig | ParseEmbedded);
        if (fndcl->value) {
            // If we have an implemented function, we need to move it to the module so it gets generated
            // Then refer to it using a nameuse node as part of this reference node
            nodesAdd(&parse->mod->nodes, (INode*)fndcl);
            dclInfoJoin((INode*)fndcl, (INode*)parse->mod);
            NameUseNode *fnname = newNameUseNode(anonName);
            // The name use is built after the whole function was parsed, so
            // it would otherwise point at the token after the body. Anything
            // reported on the use should point at the function it names.
            inodeLexCopy((INode*)fnname, (INode*)fndcl);
            fnname->dclnode = (INode*)fndcl;
            fnname->vtype = fndcl->vtype;
            anode->vtexp = (INode*)fnname;
        }
        else {
            // If no implementation, assume we have a function signature type instead
            anode->vtexp = fndcl->vtype;
        }
        return (INode *)anode;
    }

    // For a function parameter type, we allow incomplete reference types
    // where the type the reference points-to can be inferred later (typically, Self)
    if (lexIsToken(CommaToken) || lexIsToken(RParenToken)) {
        anode->vtexp = unknownType;
        return (INode *)anode;
    }

    // A borrow applies to the whole suffixed term, at the same precedence every
    // other prefix operator has: '&x.a' references the field and '&x[4]' the
    // fifth element, which is what doc/reference/refborref.html documents.
    //
    // This used to consume only the prefixed term and then re-apply the suffixes
    // to the borrow, so '&x.a' was '(&x).a' and reached codegen typed as the
    // field while returning the field's address. What made a borrow reach an
    // element -- '&[]' dispatch on a type that declares it -- is now borrow.c's
    // business, where the receiver's type is known.
    anode->vtexp = parsePrefix(parse);

    // A bound, '&<Trait + 'a': what the value a virtual reference points at
    // holds lives at least as long as ''a' (lifetime.h, "Lifetime bounds").
    // It is named where a lifetime may be.
    if (parse->intype && lexIsToken(PlusToken) && lexPeekIsLifetime()) {
        lexNextToken();
        Name *bound = lex->val.ident;
        if (anode->tag != VirtRefTag)
            errorMsgLex(ErrorLifetimeBound, "A lifetime bound is said of a type whose insides are unknown: a virtual reference's, '&<Trait + 'a', or a type parameter's, '[T + 'a]'. A plain reference or slice names its own lifetime, '&'a T'.");
        else if (lifeIsInvariant(bound))
            errorMsgLex(ErrorLifetimeInvariant, "A bound says what the borrows inside a type outlive, and an invariant lifetime has no order to say it with.");
        else if (parseLifeNamed(parse, bound, NULL))
            anode->bound = bound;
        lexNextToken();
    }
    return (INode *)anode;
}

// Parse a "plus term", the retired spelling of a region-managed allocation,
// '+Rc-mut 5', and of a reference type, '+Rc-mut Node':
// - Some reference type ('+' or '+<')
// - Region and permission annotations
// An allocation is written 'new Rc[i32](5)' or 'new Rc[mut, Node](1)'
// (parseNew); one spelled with '+' is refused at type check (ErrorPlusAlloc),
// and '+<' at name resolution. A single or virtual reference TYPE is written
// 'Rc[mut, Node]', and type check refuses this spelling of one outside a match
// pattern's root (plusSpelled), which keeps it until patterns have their own.
//
// There is no owning array reference: an owned runtime-sized array is a List,
// and one shared is 'Rc[List[T]]'. '+[]' stays a token so that it can be
// refused, once, at the token, and read through as the thin form so that what
// follows parses as written. A parse diagnostic ends the compile before name
// resolution, so what is built for it is never analysed.
INode *parsePlus(ParseState *parse) {
    // Create appropriate RefNode, depending on ampersand operator
    RefNode *anode;
    switch (lex->toktype) {
    case PlusToken:
        anode = newRefNode(RefTag); break;
    case PlusArrayRefToken:
        errorMsgLex(ErrorOwnedArrayRef,
            "There is no owning array reference '+[]': an owned runtime-sized array is a List, shared as 'Rc[List[T]]'; a borrowed slice is '&[]T'.");
        anode = newRefNode(RefTag); break;
    case PlusVirtRefToken:
        anode = newRefNode(VirtRefTag); break;
    }
    anode->plusSpelled = 1;
    lexNextToken();

    // Region-managed reference starts with a region annotation
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorBadTerm, "Expected region annotation.");
        return (INode *)anode;
    }
    anode->region = parseNameUse(parse);

    // Handle permission, if specified
    if (lexIsToken(DashToken)) {
        lexNextToken();
        if (lexIsToken(PermToken)) {
            anode->perm = newPermUseNode((PermNode*)lex->val.ident->node);
            lexNextToken();
        }
        else if (lexIsToken(IdentToken)) {
            anode->perm = parseNameUse(parse);
        }
        else {
            errorMsgLex(ErrorBadTerm, "Expected permission annotation.");
            return (INode *)anode;
        }
    }
    else
        anode->perm = newPermUseNode(uniPerm);

    // Handle type or value expression
    anode->vtexp = parsePrefix(parse);
    return (INode *)anode;
}

// Parse a prefix operator, then a term with its suffixes.
INode *parsePrefix(ParseState *parse) {
    // Only a '(' first opens an entry list (parseEntry): one after a prefix
    // operator is that operand's
    int entryparen = parse->entryparen;
    parse->entryparen = 0;
    switch (lex->toktype) {

    // '.' sugar for: this.suffixes
    case DotToken:
    {
        INode *node = parseDotCall(parse, (INode*)newNameUseNode(thisName), 0);
        return parseSuffix(parse, node, 0);
    }

    // '*' (dereference or pointer type)
    case StarToken:
    {
        StarNode *node = newStarNode(StarTag);
        lexNextToken();
        node->vtexp = parsePrefix(parse);
        return (INode *)node;
    }

    // '&', '&[]', '&<' (borrow ref type/constructor)
    case AmperToken:
    case ArrayRefToken:
    case VirtRefToken:
        return parseAmper(parse);

    // '+', '+<' (a pattern's root, or an allocation to be refused), and '+[]'
    // to be refused
    case PlusToken:
    case PlusArrayRefToken:
    case PlusVirtRefToken:
        return parsePlus(parse);

    // '?' (Option type)
    case QuesToken:
    {
        // Lower into 'Option[expr]'
        NameUseNode *option = newNameUseNode(optionName);
        FnCallNode *opttype = newFnCallNode((INode*)option, 1);
        opttype->tag = QuesTag;  // In name resolution pass, we will lower to FnCallTag or AllocTag
        lexNextToken();
        nodesAdd(&opttype->args, parsePrefix(parse));
        return (INode*)opttype;
    }

    // '-' (negative). Optimize for literals. A folded integer literal records
    // that it was negated, because its two's complement value alone cannot tell
    // '-1' from '18446744073709551615' when its range is checked
    case DashToken:
    {
        FnCallNode *node = newFnCallOpname(NULL, minusName, 0);
        lexNextToken();
        INode *argnode = parsePrefix(parse);
        if (argnode->tag == ULitTag) {
            ((ULitNode*)argnode)->uintlit = 0 - ((ULitNode*)argnode)->uintlit;
            argnode->flags ^= FlagLitNeg;
            return argnode;
        }
        else if (argnode->tag == FLitTag) {
            ((FLitNode*)argnode)->floatlit = -((FLitNode*)argnode)->floatlit;
            return argnode;
        }
        node->objfn = argnode;
        return (INode *)node;
    }

    // '~' (bitwise not)
    case TildeToken:
    {
        FnCallNode *node = newFnCallOp(NULL, "~", 0);
        lexNextToken();
        node->objfn = parsePrefix(parse);
        return (INode *)node;
    }

    // '++' (prefix increment)
    case IncrToken:
    {
        FnCallNode *node = newFnCallOpname(NULL, incrName, 0);
        node->flags |= FlagLvalOp;
        lexNextToken();
        node->objfn = parsePrefix(parse);
        return (INode *)node;
    }

    // '--' (prefix decrement)
    case DecrToken:
    {
        FnCallNode *node = newFnCallOpname(NULL, decrName, 0);
        node->flags |= FlagLvalOp;
        lexNextToken();
        node->objfn = parsePrefix(parse);
        return (INode *)node;
    }

    // No prefix operator: get the term with its suffixes
    default:
        parse->entryparen = entryparen;
        return parseSuffixTerm(parse);
    }
}

// 'into' is retired. A value converts with its type's method, 'u64.from(n)',
// and a reference narrows to a variant only where the variant the value holds
// is checked, by a 'match' or a bound 'if'. The word stays a keyword only to be
// refused; the type after it, if one is written, is read so that what follows
// parses, and the parse error keeps analysis from running.
static void parseRetiredInto(ParseState *parse) {
    errorMsgLex(ErrorInto,
        "'into' is retired: a value converts with its type's method, 'T.from(x)', and a reference narrows to a variant with a check, by a 'match' or 'if imm x &Variant = &value'.");
    lexNextToken();
    parseType(parse);
}

// Parse type cast. Casts chain left to right: 'p as *T as usize' casts
// 'p as *T' to usize.
INode *parseCast(ParseState *parse) {
    INode *lhnode = parsePrefix(parse);
    while (1) {
        if (lexIsToken(IntoToken)) {
            parseRetiredInto(parse);
            continue;
        }
        if (!lexIsToken(AsToken))
            return lhnode;
        CastNode *node = newRecastNode(lhnode, unknownType);
        lexNextToken();
        node->typ = parseTypeReq(parse, "'as'");
        lhnode = (INode*)node;
    }
}

// Parse binary multiply, divide, rem operator
INode *parseMult(ParseState *parse) {
    INode *lhnode = parseCast(parse);
    while (1) {
        if (lexIsToken(StarToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, multName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseCast(parse));
            lhnode = (INode*)node;
        }
        else if (lexIsToken(SlashToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, divName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseCast(parse));
            lhnode = (INode*)node;
        }
        else if (lexIsToken(PercentToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, remName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseCast(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse binary add, subtract operator
INode *parseAdd(ParseState *parse) {
    INode *lhnode = parseMult(parse);
    while (1) {
        if (lexIsToken(PlusToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, plusName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseMult(parse));
            lhnode = (INode*)node;
        }
        else if (lexIsToken(DashToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, minusName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseMult(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse << and >> operators
INode *parseShift(ParseState *parse) {
    INode *lhnode;
    // Prefix '<<' or '>>' implies 'this'
    if (lexIsToken(ShlToken) || lexIsToken(ShrToken))
        lhnode = (INode *)newNameUseNode(thisName);
    else
        lhnode = parseAdd(parse);
    while (1) {
        if (lexIsToken(ShlToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, shlName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseAdd(parse));
            lhnode = (INode*)node;
        }
        else if (lexIsToken(ShrToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, shrName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseAdd(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse bitwise And
INode *parseAnd(ParseState *parse) {
    INode *lhnode = parseShift(parse);
    while (1) {
        if (lexIsToken(AmperToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, andName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseShift(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse bitwise Xor
INode *parseXor(ParseState *parse) {
    INode *lhnode = parseAnd(parse);
    while (1) {
        if (lexIsToken(CaretToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, xorName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseAnd(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse bitwise or
INode *parseOr(ParseState *parse) {
    INode *lhnode = parseXor(parse);
    while (1) {
        if (lexIsToken(BarToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, orName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseXor(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// The method a comparison token names, or NULL when the current token is not one
char *parseCmpOp() {
    switch (lex->toktype) {
    case EqToken:  return "==";
    case NeToken:  return "!=";
    case SameToken:    return "===";
    case NotSameToken: return "!==";
    case LtToken:  return "<";
    case LeToken:  return "<=";
    case GtToken:  return ">";
    case GeToken:  return ">=";
    default:       return NULL;
    }
}

// Parse comparison operator, following a left operand already parsed
static INode *parseCmpFrom(ParseState *parse, INode *lhnode) {
    char *cmpop = parseCmpOp();
    if (cmpop == NULL) {
        if (lexIsToken(IsToken)) {
            CastNode *node = newIsNode(lhnode, unknownType);
            lexNextToken();
            node->typ = parseTypeReq(parse, "'is'");
            castPatternMark(node->typ);
            return (INode*)node;
        }
        else
            return lhnode;
    }

    FnCallNode *node = newFnCallOp(lhnode, cmpop, 2);
    lexNextToken();
    nodesAdd(&node->args, parseOr(parse));
    return (INode*)node;
}

// Parse comparison operator
INode *parseCmp(ParseState *parse) {
    return parseCmpFrom(parse, parseOr(parse));
}

// Parse 'not' logical operator
INode *parseNotLogic(ParseState *parse) {
    if (lexIsToken(NotToken)) {
        parse->entryparen = 0;
        LogicNode *node = newLogicNode(NotLogicTag);
        lexNextToken();
        node->lexp = parseNotLogic(parse);
        return (INode*)node;
    }
    return parseCmp(parse);
}

// Parse 'and' logical operator, following a left operand already parsed
static INode *parseAndLogicFrom(ParseState *parse, INode *lhnode) {
    while (lexIsToken(AndToken)) {
        LogicNode *node = newLogicNode(AndLogicTag);
        lexNextToken();
        node->lexp = lhnode;
        node->rexp = parseNotLogic(parse);
        lhnode = (INode*)node;
    }
    return lhnode;
}

// Parse 'and' logical operator
INode *parseAndLogic(ParseState *parse) {
    return parseAndLogicFrom(parse, parseNotLogic(parse));
}

// Parse 'or' logical operator, following a left operand already parsed
static INode *parseOrExprFrom(ParseState *parse, INode *lhnode) {
    while (lexIsToken(OrToken)) {
        LogicNode *node = newLogicNode(OrLogicTag);
        lexNextToken();
        node->lexp = lhnode;
        node->rexp = parseAndLogic(parse);
        lhnode = (INode*)node;
    }
    return lhnode;
}

// Parse 'or' logical operator
INode *parseOrExpr(ParseState *parse) {
    return parseOrExprFrom(parse, parseAndLogic(parse));
}

// This parses any kind of expression, including blocks, assignment or tuple
INode *parseSimpleExpr(ParseState *parse) {
    return parseOrExpr(parse);
}

// Finish parsing a simple expression whose first operand -- everything that
// binds tighter than a comparison, which parseOr parses -- is already in hand.
// A match's case reads that much before it can tell a range pattern, whose
// '..' or '...' follows it, from a condition.
INode *parseSimpleExprFrom(ParseState *parse, INode *lhnode) {
    return parseOrExprFrom(parse, parseAndLogicFrom(parse, parseCmpFrom(parse, lhnode)));
}

// Parse a comma-separated expression tuple
INode *parseTuple(ParseState *parse) {
    INode *exp = parseSimpleExpr(parse);
    if (lexIsToken(CommaToken)) {
        TupleNode *tuple = newTupleNode(4);
        nodesAdd(&tuple->elems, exp);
        while (lexIsToken(CommaToken)) {
            lexNextToken();
            nodesAdd(&tuple->elems, parseSimpleExpr(parse));
        }
        return (INode*)tuple;
    }
    else
        return exp;
}

// Parse an operator assignment
INode *parseOpEq(ParseState *parse, INode *lval, Name *opeqname) {
    FnCallNode *node = newFnCallOpname(lval, opeqname, 2);
    node->flags |= FlagOpAssgn | FlagLvalOp;
    lexNextToken();
    nodesAdd(&node->args, parseAnyExpr(parse));
    return (INode*)node;
}

// Is this a construction, 'new Point(1, 2)', which contents may follow after '<-'?
static int parseIsConstruction(INode *node) {
    return node != NULL && node->tag == FnCallTag && (node->flags & FlagNew);
}

static INode *parseEntry(ParseState *parse);

// A value inside an entry: an expression, and a construction's contents after it
static INode *parseEntryValue(ParseState *parse) {
    return parseContentsAfter(parse, parseSimpleExpr(parse));
}

// Parse one entry on the right of '<-'. Besides a value, three forms are read
// here and nowhere else, each named by a contextual word or by ':' after its
// first expression:
// - 'n of x', n values, x evaluated for each: 'of' after an expression, where
//   no name could otherwise follow one;
// - 'fill x', x until the collection is full: 'fill' first, followed by a
//   value (lexNextOpensValue), so 'xs <- fill;' still appends a variable 'fill';
// - 'k: v', a key and its value.
// An entry that begins with '(' is a parenthesized list of entries
// (parseTerm), which is how a construction's contents are written inside
// another comma list: 'draw(new List[i32] <- (1, 2), x)'.
static INode *parseEntry(ParseState *parse) {
    if (lexIsToken(IdentToken) && lex->val.ident == fillName && lexNextOpensValue()) {
        EntryNode *fill = newEntryNode(FillEntryTag, NULL);
        lexNextToken();
        fill->val = parseEntryValue(parse);
        return (INode*)fill;
    }
    parse->entryparen = 1;
    INode *first = parseSimpleExpr(parse);
    parse->entryparen = 0;
    first = parseContentsAfter(parse, first);
    uint16_t tag;
    if (lexIsToken(IdentToken) && lex->val.ident == ofName)
        tag = OfEntryTag;
    else if (lexIsToken(ColonToken))
        tag = PairEntryTag;
    else
        return first;
    EntryNode *entry = newEntryNode(tag, first);
    inodeLexCopy((INode*)entry, first);
    lexNextToken();
    entry->val = parseEntryValue(parse);
    return (INode*)entry;
}

// Parse the entries on the right of '<-', separated by commas: one entry, or
// a tuple of them, which type check takes apart into one application each
// (contentsLower). The list runs to whatever ends it, the statement's ';'
// or a ')', across lines.
static INode *parseEntries(ParseState *parse) {
    INode *entry = parseEntry(parse);
    if (!lexIsToken(CommaToken))
        return entry;
    TupleNode *tuple = newTupleNode(4);
    inodeLexCopy((INode*)tuple, entry);
    nodesAdd(&tuple->elems, entry);
    while (lexIsToken(CommaToken)) {
        lexNextToken();
        nodesAdd(&tuple->elems, parseEntry(parse));
    }
    return (INode*)tuple;
}

// After a construction inside a comma list -- an argument, a named value, an
// array literal's element, an entry -- '<-' takes one entry, since the comma
// after it belongs to the list: 'draw(new List[i32] <- (1, 2), x)' parenthesizes
// several, and 'f(new Array[f32, 4] <- fill 0.0)' needs nothing more.
// Anything else is returned as it is.
static INode *parseContentsAfter(ParseState *parse, INode *node) {
    if (!lexIsToken(LessDashToken) || !parseIsConstruction(node))
        return node;
    FnCallNode *append = newFnCallOpname(node, lessDashName, 2);
    append->flags |= FlagOpAssgn | FlagLvalOp;
    lexNextToken();
    nodesAdd(&append->args, parseEntry(parse));
    return (INode*)append;
}

// Parse the append operator (<-) and the entries it is given, which run to
// the end of the statement
INode *parseAppend(ParseState *parse, INode *lval) {
    FnCallNode *node = newFnCallOpname(lval, lessDashName, 2);
    node->flags |= FlagOpAssgn | FlagLvalOp;
    lexNextToken();
    nodesAdd(&node->args, parseEntries(parse));  // A list of entries is lowered at type check (contentsLower)
    return (INode*)node;
}

// Parse an assignment expression
INode *parseAssign(ParseState *parse) {
    // Prefix <- operator applies to 'this'
    if (lexIsToken(LessDashToken)) {
        return parseAppend(parse, (INode*)newNameUseNode(thisName));
    }

    INode *lval = parseTuple(parse);
    switch (lex->toktype) {
    case AssgnToken:
    {
        lexNextToken();
        INode *rval = parseAnyExpr(parse);
        return (INode*)newAssignNode(NormalAssign, lval, rval);
    }

    case LAssgnToken:
    {
        lexNextToken();
        INode *rval = parseAnyExpr(parse);
        return (INode*)newAssignNode(LeftAssign, lval, rval);
    }

    case SwapToken:
    {
        lexNextToken();
        INode *rval = parseAnyExpr(parse);
        return (INode*)newSwapNode(lval, rval);
    }

    case PlusEqToken:
        return parseOpEq(parse, lval, plusEqName);
    case MinusEqToken:
        return parseOpEq(parse, lval, minusEqName);
    case MultEqToken:
        return parseOpEq(parse, lval, multEqName);
    case DivEqToken:
        return parseOpEq(parse, lval, divEqName);
    case RemEqToken:
        return parseOpEq(parse, lval, remEqName);
    case OrEqToken:
        return parseOpEq(parse, lval, orEqName);
    case AndEqToken:
        return parseOpEq(parse, lval, andEqName);
    case XorEqToken:
        return parseOpEq(parse, lval, xorEqName);
    case ShlEqToken:
        return parseOpEq(parse, lval, shlEqName);
    case ShrEqToken:
        return parseOpEq(parse, lval, shrEqName);
    case LessDashToken:
        return parseAppend(parse, lval);
    default:
        return lval;
    }
}

// This parses any kind of expression, including blocks, assignment or tuple
INode *parseAnyExpr(ParseState *parse) {
    return parseAssign(parse);
}
