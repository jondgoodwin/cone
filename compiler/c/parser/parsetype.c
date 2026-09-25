/** Parse type signatures
 * @file
 *
 * The parser translates the lexer's tokens into IR nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ir/ir.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../ir/nametbl.h"
#include "lexer.h"

#include <stdio.h>
#include <assert.h>

// Parse a permission, return reference to defperm if not found
INode *parsePerm() {
    if (lexIsToken(PermToken)) {
        INode *perm = newPermUseNode((PermNode*)lex->val.ident->node);
        lexNextToken();
        return perm;
    }
    return unknownType;
}

// Parse the permission a declaration carries, defaulting to 'defperm' when the
// source writes none.
//
// A declaration names one storage location that only it owns, so the aliasing
// distinctions the reference permissions draw have nothing to say about it --
// MayAlias is read only off a reference type's permission, and MayAliasWrite is
// read nowhere. What is left to choose is whether the value may change, and
// 'mut' and 'imm' are the two that say it.
//
// The defaults differ, and deliberately: a variable defaults to 'imm', so
// mutation is opted into, while a field defaults to 'mut', which per
// doc/reference/refstruct.html means the container's permission governs.
//
// workitems/permissions.md records what is not settled here, including that a
// global needs a third answer this vocabulary cannot give.
INode *parseDclPerm(PermNode *defperm) {
    INode *perm = parsePerm();
    if (perm->tag == UnknownTag)
        perm = (INode*)defperm;
    INode *permdcl = itypeGetTypeDcl(perm);
    if (permdcl != (INode*)mutPerm && permdcl != (INode*)immPerm)
        errorMsgNode(perm, ErrorInvType, "A declaration's permission must be 'mut' or 'imm'");
    return perm;
}

// Parse a variable declaration
VarDclNode *parseVarDcl(ParseState *parse, PermNode *defperm, uint16_t flags) {
    VarDclNode *varnode;
    INode *perm = parseDclPerm(defperm);

    // Obtain variable's name
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoIdent, "Expected variable name for declaration");
        parse->bodyp = parse->bodyendp = parse->nameendp = NULL;
        parse->typed = 0;
        return newVarDclFull(anonName, VarDclTag, unknownType, perm, NULL);
    }
    varnode = newVarDclNode(lex->val.ident, VarDclTag, perm);
    lexNextToken();
    char *nameendp = lex->prevend;

    // Get value type, if provided
    varnode->vtype = parseType(parse);
    int typed = varnode->vtype != unknownType;

    // Get initialization value after '=', if provided
    char *bodyp = NULL, *bodyendp = NULL;
    if (lexIsToken(AssgnToken)) {
        if (!(flags&ParseMayImpl))
            errorMsgLex(ErrorBadImpl, "A default/initial value may not be specified here.");
        bodyp = lex->tokp;
        lexNextToken();
        if (lexIsToken(UndefToken)) {
            // 'undef' is used to signal that programmer believes variable
            // can be considered safely "initialized", even though it is UB.
            varnode->flowtempflags |= VarInitialized;
            lexNextToken();
        }
        else
            varnode->value = parseAnyExpr(parse);
        bodyendp = lex->prevend;
    }
    else {
        if (!(flags&ParseMaySig))
            errorMsgLex(ErrorNoInit, "Must specify default/initial value.");
    }

    // A field folds, and so does a module's global: a global is the one-instance
    // analogue of a field, so its clause admits names of its type as names of the
    // module, reached through the global. Refused on a local, a parameter and a
    // type's static, so that the diagnostic is the fold's own rather than a
    // missing semicolon; the clause is read and dropped to recover.
    if (parseIsFoldClause()) {
        if (flags & ParseMayFold)
            varnode->fold = parseFoldClause(parse, FoldMayPub);
        else {
            errorMsgLex(ErrorBadFold, "Only a struct's field or a module's global may fold names in with 'use'.");
            parseFoldClause(parse, FoldRecover);
        }
    }

    // Where the name ends and the value is, for the span the caller records
    parse->bodyp = bodyp;
    parse->bodyendp = bodyendp;
    parse->nameendp = nameendp;
    parse->typed = typed;
    return varnode;
}

// Parse a named constant declaration
ConstDclNode *parseConstDcl(ParseState *parse) {
    ConstDclNode *constnode;
    lexNextToken();

    // Obtain name
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoIdent, "Expected name for const declaration");
        return newConstDclNode(anonName);
    }
    constnode = newConstDclNode(lex->val.ident);
    lexNextToken();

    // Get value type, if provided
    constnode->vtype = parseType(parse);

    // Get initialization value after '=', if provided
    if (lexIsToken(AssgnToken)) {
        lexNextToken();
        constnode->value = parseAnyExpr(parse);
    }
    else {
        errorMsgLex(ErrorNoInit, "Must specify const value.");
    }
    parseEndOfStatement();

    return constnode;
}

INode *parseTypeName(ParseState *parse) {
    INode *node = parseNameUse(parse);
    // A path through namespaces parses as a chain of member accesses, which
    // name resolution collapses once it knows what the base name is
    while (lexIsToken(DotToken))
        node = parseDotCall(parse, node, 0);
    if (lexIsToken(LBracketToken)) {
        FnCallNode *fncall = newFnCallNode(node, 8);
        fncall->flags |= FlagIndex;
        lexNextToken();
        if (!lexIsToken(RBracketToken)) {
            nodesAdd(&fncall->args, parseType(parse));
            while (lexIsToken(CommaToken)) {
                lexNextToken();
                nodesAdd(&fncall->args, parseType(parse));
            }
        }
        parseCloseTok(RBracketToken);
        node = (INode *)fncall;
    }
    return node;
}

// Is the lexer on 'tag', where a field's type is written?
//
// 'tag' is recognized here and nowhere else, so it is not a reserved word and
// 'pub tag i32' still declares a field named 'tag'. What it names is the
// discriminant type an enum synthesizes for itself; an author writes it, as
// '_ tag', only to place that field somewhere other than position 0, which is
// done for alignment.
static int parseIsTagType() {
    return lexIsToken(IdentToken) && lex->val.ident == tagName;
}

// Parse the discriminant type, with the lexer on its 'tag'
static INode* parseTagType(ParseState *parse) {
    EnumNode *node = newEnumNode();
    lexNextToken();
    return (INode*)node;
}

// Parse what a fold clause admits, with the lexer past its 'use' and past the
// source type where one is written there: '*' with an optional 'but name, name',
// or 'name [as name], name [as name]'. Each listed name becomes an alias under
// its local spelling, positioned at the item, whose target spells the name in
// the source type; whether that is a field or a method is not known until the
// source type is, so the clause is expanded during name resolution, which is
// what binds each target.
//
// A star clause arrives with 'star' already set for the type-body form, whose
// default is the whole member set: naming a sibling is what asks for it, so the
// star has nothing left to say and may not be written.
//
// A long list may be written as a block instead, which is nothing but a way of
// spreading the same items over lines: the braces hold the list and nothing
// else, so a block is never a star clause and 'but' has no place in one.
static void parseFoldItems(ParseState *parse, FoldClause *fold, int maystar) {
    int block = 0;
    if (lexIsToken(LCurlyToken)) {
        block = 1;
        lexNextToken();
        fold->star = 0;
    }
    else if (maystar && lexIsToken(StarToken)) {
        fold->star = 1;
        lexNextToken();
    }
    if (fold->star) {
        if (lexIsToken(ButToken)) {
            lexNextToken();
            fold->excludes = newNodes(4);
            while (1) {
                if (!lexIsToken(IdentToken)) {
                    errorMsgLex(ErrorNoIdent, "Expected the name of a member to leave out of the fold");
                    break;
                }
                nodesAdd(&fold->excludes, (INode*)newMemberUseNode(lex->val.ident));
                lexNextToken();
                if (!lexIsToken(CommaToken))
                    break;
                lexNextToken();
            }
        }
        return;
    }
    while (1) {
        // A block may hold a trailing comma, and an empty one admits nothing
        if (block && lexIsToken(RCurlyToken))
            break;
        if (!lexIsToken(IdentToken)) {
            errorMsgLex(ErrorNoIdent, "Expected the name of a member to fold in");
            break;
        }
        NameUseNode *target = newMemberUseNode(lex->val.ident);
        AliasDclNode *alias = newAliasDclNode(lex->val.ident, (INode*)target);
        lexNextToken();
        if (lexIsToken(AsToken)) {
            lexNextToken();
            if (!lexIsToken(IdentToken)) {
                errorMsgLex(ErrorNoIdent, "Expected the name the folded member is known by here");
                break;
            }
            alias->namesym = lex->val.ident;
            lexNextToken();
        }
        nodesAdd(&fold->items, (INode*)alias);
        if (!lexIsToken(CommaToken))
            break;
        lexNextToken();
    }
    if (block)
        parseCloseTok(RCurlyToken);
    // 'but' leaves a name out of everything; a list admits only what it names
    if (lexIsToken(ButToken)) {
        errorMsgLex(ErrorBadFold, "'but' leaves a name out of a fold of every member. A listed fold admits only the names it lists.");
        lexNextToken();
        while (lexIsToken(IdentToken) || lexIsToken(CommaToken))
            lexNextToken();
    }
}

// Is the lexer on a fold clause: its 'use', or the 'pub' written before it? A
// 'pub' after a declaration is either that or the start of the next statement
// after a missing ';', and only the word after it can say which.
int parseIsFoldClause() {
    return lexIsToken(UseToken) || (lexIsToken(PubToken) && lexNextIsWord("use"));
}

// 'pub' comes first, before the keyword of whatever it makes public, as it does
// for every declaration. 'use pub' was the fold's own spelling, now retired, and
// it is refused at every site that admits a 'pub' fold, naming the one to write.
static char *usePubMsg = "'pub' comes first, before the keyword of what it makes public: write 'pub use', not 'use pub'.";

// A folded member of a field's type is as public as the field it is reached
// through, so a 'pub' on a field's fold would claim what the fold does not decide
static char *foldNoPubMsg = "A folded member is as visible as what it is reached through, so this fold has no visibility of its own to declare.";

// Parse a field's, a global's or an import's fold clause, with the lexer on its
// 'use' or on the 'pub' before it. The declaration's type is the source, so the
// clause is nothing but what it admits, and '*' is how it says every member.
//
// 'pub use' says the bindings it makes are visible outside the namespace folding
// them, which only a module has an answer for (FoldMayPub); a field's clause
// refuses it (FoldNoPub). 'fold->at' is the 'use', whichever was written.
FoldClause *parseFoldClause(ParseState *parse, int maypub) {
    int saidpub = 0;
    if (lexIsToken(PubToken)) {
        if (maypub == FoldNoPub)
            errorMsgLex(ErrorBadPub, "%s", foldNoPubMsg);
        saidpub = 1;
        lexNextToken();
    }
    FoldClause *fold = newFoldClause();
    lexNextToken();
    // The retired spelling. Taken as meant once refused, so that nothing after
    // it is reported for want of the 'pub' it asked for; at a field, the refusal
    // is the field's own, since no spelling of 'pub' is right there
    if (lexIsToken(PubToken)) {
        if (maypub == FoldMayPub)
            errorMsgLex(ErrorBadPub, "%s", usePubMsg);
        else if (maypub == FoldNoPub && !saidpub)
            errorMsgLex(ErrorBadPub, "%s", foldNoPubMsg);
        saidpub = 1;
        lexNextToken();
    }
    if (saidpub && maypub == FoldMayPub)
        fold->ispub = 1;
    parseFoldItems(parse, fold, 1);
    return fold;
}

// Parse what a 'use' statement admits of the source it has just named, and the
// end of the statement. Naming the source is what asks for its members, so every
// one of them is the default and there is no second spelling for it: '*' is
// refused with 'starmsg'. A name after the source starts a list.
static void parseUseAdmits(ParseState *parse, FoldClause *fold, char *starmsg) {
    fold->star = 1;
    if (lexIsToken(StarToken)) {
        errorMsgLex(ErrorBadFold, "%s", starmsg);
        lexNextToken();
    }
    else if (lexIsToken(IdentToken))
        fold->star = 0;
    parseFoldItems(parse, fold, 0);
    parseEndOfStatement();
}

// Parse a type body's 'use' clause, with the lexer on the 'use': the sibling
// enrichment it folds in, then what it admits of it. Held in a field-like node,
// as a further name in an 'is' list is, because that is what carries a type
// expression and a fold clause through cloning; it is not a field and never
// joins the field list, since a sibling contributes no representation.
static FieldDclNode *parseUseSibling(ParseState *parse) {
    FieldDclNode *use = newFieldDclNode(anonName, (INode*)immPerm);
    FoldClause *fold = newFoldClause();
    use->fold = fold;
    lexNextToken();
    inodeLexCopy((INode*)use, fold->at);
    // A sibling fold declares no name of its own and every folded name carries
    // its target's visibility, so there is nothing here for 'pub' to say
    if (lexIsToken(PubToken)) {
        errorMsgLex(ErrorBadPub,
            "A sibling fold declares no name of its own, and each folded name carries its target's visibility.");
        lexNextToken();
    }
    use->vtype = parseTypeName(parse);
    parseUseAdmits(parse, fold,
        "A type body's 'use' brings in every member of what it names already; '*' says nothing more.");
    return use;
}

// Parse a module's standalone 'use' statement, with the lexer on the 'use': the
// enum or submodule whose names it folds in as names of the module, then which
// of them. What follows the source is a type body's 'use' exactly -- every name
// by default, a list with 'as', a block, or every name 'but' some.
//
// 'pub use' makes the bindings public names of this module, so a module that
// imports this one with 'use *' receives them too. It is how core makes Some, None,
// Ok and Error bare in every program. The 'pub' is read with the statement's
// other leading words, before the 'use', and arrives as 'pubflag'.
//
// The source is a type expression, a path included, and nothing about what it
// names is known until name resolution; so the statement is held as written and
// expanded in the module's fold pass (foldModUseExpand), which is where an enum
// and a submodule part ways.
ModUseNode *parseModUse(ParseState *parse, uint16_t pubflag) {
    ModUseNode *use = newModUseNode();
    FoldClause *fold = use->fold;
    fold->ispub = pubflag ? 1 : 0;
    lexNextToken();
    // The retired spelling, refused and then taken as meant
    if (lexIsToken(PubToken)) {
        errorMsgLex(ErrorBadPub,
            "'pub' comes first, before the keyword of what it makes public: write 'pub use', as in 'pub use Colors;'.");
        fold->ispub = 1;
        lexNextToken();
    }
    use->source = parseTypeName(parse);
    parseUseAdmits(parse, fold,
        "A module's 'use' brings in every variant of the enum, or every public name of the submodule, it names already; '*' says nothing more.");
    return use;
}

// Parse what follows a field's name: its type, an initial value and a fold
// clause. The node arrives already built, because an enum's body cannot tell a
// field from a bare-name variant until the name has been read and what follows it
// looked at -- and the node has to be built while the lexer is still on the name,
// so that a diagnostic about the member points there and not at its type.
static FieldDclNode *parseFieldDclBody(ParseState *parse, FieldDclNode *fldnode) {
    INode *vtype;

    // Get value type, if provided
    if (parseIsTagType())
        fldnode->vtype = parseTagType(parse);
    else if ((vtype = parseType(parse)))
        fldnode->vtype = vtype;

    // Get initialization value after '=', if provided
    if (lexIsToken(AssgnToken)) {
        lexNextToken();
        fldnode->value = parseAnyExpr(parse);
    }

    // A fold clause takes its names from the field's type, so the type is written
    if (parseIsFoldClause()) {
        fldnode->fold = parseFoldClause(parse, FoldNoPub);
        if (fldnode->vtype == unknownType)
            errorMsgNode(fldnode->fold->at, ErrorBadFold, "A field that folds names in must write its type, which is where the names come from.");
    }

    return fldnode;
}


// Does a type name, as written, spell the declaration 'type' by its own name?
// A bare name, or that name instantiated with arguments. Asked before anything is
// resolved, so it sees only the spelling: a path or an alias reaching the same
// type is left for name resolution to find.
static int parseNamesType(INode *named, StructNode *type) {
    if (named->tag == FnCallTag)
        named = ((FnCallNode*)named)->objfn;
    return named->tag == NameUseTag && ((NameUseNode*)named)->namesym == type->namesym;
}

// Join a variant to the enum that declares it: the closed-type flags, the
// base link back to the enum, the tag number, and its name.
//
// The enum owns the layout, so the variant states none of it. Its tag number is
// assigned ascending from zero across the body unless the author pins one, after
// which numbering continues from there -- which is what lines an enum up with an
// external library's constants without renumbering the rest by hand.
static void parseAddVariant(ParseState *parse, StructNode *strnode, StructNode *substruct, uint32_t *nexttag) {
    substruct->flags |= HasTagField | (strnode->flags & SameSize);

    // An extension's numbering cannot be settled here. Its base's variants come
    // first in its set and their values are not known until the base is resolved,
    // so an unpinned variant keeps 'TagUnassigned' and name resolution numbers it
    // after the base's last -- structEnumSeedVariants, in ir/types/struct.c.
    int deferred = strnode->extendsbase != NULL;
    if (substruct->tagnbr == TagUnassigned && !deferred)
        substruct->tagnbr = *nexttag;

    // The enum already says which enum a variant belongs to and what its type
    // parameters are, so a variant restating either is refused rather than ignored.
    // Reported on the variant, whose node carries the position of its name in
    // both of its spellings, rather than at whatever token the body ended on.
    // Its enum written in its own 'is' list was refused where the list was read,
    // and what else the list names is held as placeholders beside this base.
    if (substruct->genericinfo)
        errorMsgNode((INode*)substruct, ErrorVariantDcl,
            "%s takes its enum's type parameters; it may not declare its own.",
            &substruct->namesym->namestr);
    // A variant's fields are its enum's, spliced in, so it has no representation
    // of its own for an enrichment to stand on
    if (substruct->extendsbase) {
        errorMsgNode((INode*)substruct, ErrorVariantDcl,
            "%s takes its layout from the enum it is written inside; remove the 'extends'.",
            &substruct->namesym->namestr);
        substruct->extendsbase = NULL;
    }

    INode *traitref = (INode*)newNameUseNode(strnode->namesym);
    if (strnode->genericinfo) {
        substruct->genericinfo = newGenericInfo();
        substruct->genericinfo->parms = newNodes(strnode->genericinfo->parms->used);
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(strnode->genericinfo->parms, cnt, nodesp)) {
            GenVarDclNode *parm = newGVarDclNode(((GenVarDclNode*)*nodesp)->namesym);
            nodesAdd(&substruct->genericinfo->parms, (INode*)parm);
        }
        // traitref needs to be a generic-qualified base trait name
        FnCallNode *gentraitref = newFnCallNode(traitref, strnode->genericinfo->parms->used);
        gentraitref->flags |= FlagIndex;
        for (nodesFor(strnode->genericinfo->parms, cnt, nodesp)) {
            nodesAdd(&gentraitref->args, (INode*)newNameUseNode(((GenVarDclNode *)*nodesp)->namesym));
        }
        traitref = (INode *)gentraitref;
    }
    substruct->basetrait = traitref;

    // Two variants holding the same tag value are indistinguishable at a match,
    // so the second one is refused where it is written
    if (strnode->derived) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(strnode->derived, cnt, nodesp)) {
            if (substruct->tagnbr != TagUnassigned && ((StructNode*)*nodesp)->tagnbr == substruct->tagnbr)
                errorMsgNode((INode*)substruct, ErrorDupTag,
                    "Tag value %d is already taken by variant %s.",
                    (int)substruct->tagnbr, &((StructNode*)*nodesp)->namesym->namestr);
        }
    }
    else
        strnode->derived = newNodes(4);
    nodesAdd(&strnode->derived, (INode*)substruct);
    if (!deferred)
        *nexttag = substruct->tagnbr + 1;

    // A MODULE NODE, BUT NOT A MODULE NAME. The variant stays on the module's
    // list, so the module's walks resolve, check and generate it as they always
    // have; what changes is where its name is bound, which is the enum's
    // namespace. So it is reached as 'Colors.Red', two enums may each have a
    // 'Quit', and a module that wants the name bare says so with 'use Colors;'.
    modAddNode(parse->mod, NULL, (INode*)substruct);
    // Declared inside the enum: the variant's symbols are spelled after the enum,
    // so the enum is its owner
    dclInfoJoin((INode*)substruct, (INode*)strnode);
    // One namespace for the enum's variants, fields and methods, so a variant may
    // not share a spelling with any of them. A member declared later in the body
    // reports the clash where it joins.
    Name *name = inodeGetName((INode*)substruct);
    if (name != anonName && namespaceAdd(&strnode->namespace, name, (INode*)substruct) != NULL)
        errorMsgNode((INode*)substruct, ErrorDupName,
            "%s is already a name of %s: a variant shares its enum's namespace with the enum's fields and methods.",
            &name->namestr, &strnode->namesym->namestr);
}

// Is this an enum that extends another? What such an enum declares beside its
// variants is what can be given to every variant of its set, the copies of its
// base's included: a method with a body, which is cloned into each of them, and a
// static function or a static, which stay the extension's own.
static int parseIsEnumExtension(int isenum, StructNode *strnode) {
    return isenum && strnode->extendsbase != NULL;
}

// Report a member an enum extension may not declare, at the token the member
// starts on, and then let the member be parsed as any other: one diagnostic is the
// whole of it. For a macro: it does not reach the copies of the base's variants,
// so it belongs on the base, whose members come along with the copies.
// A common field and a requirement each have a reason of their own, said where
// they are recognized.
static void parseEnumExtensionMember(int isenum, StructNode *strnode, char *what) {
    if (!parseIsEnumExtension(isenum, strnode))
        return;
    errorMsgLex(ErrorEnumExtends,
        "%s extends an enum, so beside its variants it declares methods with a body, static functions and statics: %s belongs on the enum it extends, and comes along with the variants it copies from there.",
        &strnode->namesym->namestr, what);
}

// Parse the '= value' pinning a variant's tag value, if one is written, leaving
// the tag unassigned when none is. Written where the variant's name is, both for
// a bare name and for a struct's.
static void parseVariantTagPin(StructNode *substruct) {
    substruct->tagnbr = TagUnassigned;
    if (!lexIsToken(AssgnToken))
        return;
    lexNextToken();
    if (!lexIsToken(IntLitToken)) {
        errorMsgLex(ErrorNotLit, "A variant's tag value is an integer literal.");
        return;
    }
    substruct->tagnbr = (uint32_t)lex->val.uintlit;
    lexNextToken();
}

// Parse a struct, a trait or an enum. They are one node, and what tells them
// apart is in 'strflags': see the flag block in ir/inode.h.
INode *parseStruct(ParseState *parse, uint16_t strflags) {
    INsTypeNode *svtype = parse->typenode;
    StructNode *strnode;
    uint16_t fieldnbr = 0;
    uint32_t nexttag = 0;
    int isenum = (strflags & EnumType) != 0;

    // Capture the kind of type, then get next token (name)
    uint16_t tag = StructTag;
    lexNextToken();

    // 'trait' is a modifier on the kind rather than a kind of its own: 'struct
    // trait X' declares the abstraction of a struct, and 'trait X' by itself is
    // a synonym for it. The family a trait serves therefore comes from the kind
    // keyword, with nothing inferred and nothing extra carried on the
    // declaration -- which is what lets 'mod trait' name the abstraction of a
    // module (parseModTrait), and 'actor trait' an actor's when actors arrive.
    int isvariant = svtype && ((INode*)svtype)->tag == StructTag && (((INode*)svtype)->flags & EnumType);
    while (lexIsToken(TraitToken)) {
        // An enum's identity is its variant set, so there is no abstraction that
        // corresponds to one: anything a caller could hold behind it either is
        // that variant set, and so is the enum, or is open, and so is a trait.
        // A variant is one concrete member of such a set and has no abstraction
        // for the same reason, which is why both wear the one code.
        if (isenum)
            errorMsgLex(ErrorEnumAbstract, "An enum is concrete: its variant set is its identity. Use 'trait' for an open abstraction.");
        else if (isvariant)
            errorMsgLex(ErrorEnumAbstract, "A variant is one concrete member of its enum's set, so no abstraction corresponds to one.");
        else if (strflags & TraitType)
            errorMsgLex(ErrorDupTrait, "'trait' by itself already means 'struct trait'. Write one or the other.");
        else
            strflags |= TraitType;
        lexNextToken();
    }

    // Handle attributes
    while (1) {
        if (lex->toktype == MoveToken) {
            strflags |= MoveType;
            lexNextToken();
        }
        else if (lex->toktype == OpaqueToken) {
            strflags |= OpaqueType | DeclaredOpaque;
            lexNextToken();
        }
        else if (lex->toktype == UnsizedToken) {
            // Every variant is padded out to the size of the largest by default,
            // which is what lets a value of the enum be held in a variable,
            // copied, passed and swapped in place. '@unsized' declines the
            // padding and the value semantics with it: the enum is then reached
            // only by reference. Nothing else has padding to decline.
            if (!isenum)
                errorMsgLex(ErrorBadUnsized, "'@unsized' declines an enum's padding. Only an enum pads its variants.");
            strflags &= ~SameSize;
            lexNextToken();
        }
        else if (lex->toktype == CAttrToken) {
            // '@c' spells a symbol, and a type has none: its methods are named
            // one at a time, and its layout is not a name
            errorMsgLex(ErrorCAttr, "'@c' gives a function or a module's functions and globals C names. A type has no symbol for it to name.");
            DclInfo ignored;
            dclInfoInit(&ignored);
            parseCAttr(&ignored, 0);
        }
        else
            break;
    }

    // Process struct type name, if provided.
    //
    // An unnamed type is built under the anonymous name rather than abandoned,
    // so that the block below is still parsed: leaving here would hand the
    // caller nothing to work with and drop the whole body on the parser's floor,
    // where its opening brace becomes the next global statement. Callers keep
    // such a declaration out of the module namespace, since '_' names nothing.
    int named = lexIsToken(IdentToken);
    if (!named)
        errorMsgLex(ErrorNoIdent, "Expected a name for the type");
    strnode = newStructNode(named ? lex->val.ident : anonName);
    strnode->tag = tag;
    strnode->flags |= strflags;
    parse->typenode = (INsTypeNode *)strnode;
    if (named)
        lexNextToken();

    uint16_t methflags = ParseMayName | ParseMayImpl;
    if (strnode->flags & TraitType)
        methflags |= ParseMaySig;

    // Handle if generic parameters are found
    if (lexIsToken(LBracketToken)) {
        strnode->genericinfo = newGenericInfo();
        strnode->genericinfo->parms = parseGenericParms(parse);
    }

    // A variant may pin its tag value, written where its name is so that the
    // bare-name form and the struct form read the same way
    if (isvariant)
        parseVariantTagPin(strnode);

    // An enum may name the integer type its tag values are laid out in, which is
    // what lines the enum up with an external library's constants. It fixes the
    // tag's width, so a pinned value too large for it is refused rather than
    // silently widening the tag.
    INode *underlying = NULL;
    if (isenum && lexIsToken(IdentToken))
        underlying = parseTypeName(parse);

    // 'is' asserts, in the type's own declaration, that it complies with one or
    // more abstractions. It is the only subtype relationship in Cone that is
    // asserted rather than noticed, and the only way to comply with an
    // abstraction that has nothing in it to notice.
    //
    // This is the same predicate the expression 'is' applies to a value, asked of
    // a type instead: 'p is Mobile' tests at runtime because a value's variant is
    // dynamic, while 'struct Gauge is Meter' asserts at the declaration because a
    // type's relationships are fixed. The two cannot be confused, because 'is' is
    // infix in an expression and so never begins one, and no expression is parsed
    // anywhere in a type declaration's header.
    //
    // The first trait is the base: the only one that may require fields, which is
    // what makes its requirement a positional prefix at position 0. Each further
    // trait is held as a placeholder field flagged IsMixin, taken in at name
    // resolution, and must require no fields at all.
    //
    // An enum and a variant hold EVERY name that way, the first included. An enum
    // stands on nothing: it is the base of its own variants, which is what a NULL
    // 'basetrait' on an enum means throughout. A variant's base is its enum,
    // which parseAddVariant supplies. So what either names is an open trait whose
    // requirements and defaults it takes in beside that relationship, and never a
    // base. Every name in every list is checked at name resolution for being a
    // closed type, which none of them may be (structRefuseClosedIs).
    //
    // 'extends' names a CONCRETE base to enrich with methods, which is a different
    // assertion, so a type may write both clauses. They are therefore read in a
    // loop rather than as alternatives: either order, each one once.
    int sawis = 0, sawextends = 0;
    INode *firstis = NULL;
    while (lexIsToken(IsToken) || lexIsToken(ExtendsToken)) {
        if (lexIsToken(IsToken)) {
            lexNextToken();
            if (sawis++) {
                errorMsgLex(ErrorExtends, "A type names its abstractions in one 'is' list, separated by commas.");
                parseTypeName(parse);
                continue;
            }
            int nth = 0;
            do {
                if (nth++ > 0)
                    lexNextToken();     // the comma
                INode *named = parseTypeName(parse);  // Could be a qualified name or generic
                if (nth == 1)
                    firstis = named;
                if (nth == 1 && !(isenum || isvariant)) {
                    strnode->basetrait = named;
                    continue;
                }
                // A variant is a member of its enum already, which is not an
                // abstraction it could also assert. A spelling of the enum this
                // cannot see through is refused at name resolution as a closed type.
                if (isvariant && parseNamesType(named, (StructNode*)svtype)) {
                    errorMsgNode((INode*)strnode, ErrorVariantDcl,
                        "%s is a member of the enum it is written inside; remove the 'is'.",
                        &strnode->namesym->namestr);
                    continue;
                }
                FieldDclNode *isafld = newFieldDclNode(anonName, (INode*)immPerm);
                isafld->flags |= IsMixin | FlagMethFld;
                isafld->vtype = named;
                structAddField(strnode, isafld);
            } while (lexIsToken(CommaToken));
            // 'is' takes no siblings to fold from: it names abstractions, and an
            // abstraction has no value to reach a folded name through. Delegation is
            // what a field's own 'use' clause is for.
            if (parseIsFoldClause()) {
                errorMsgLex(ErrorBadFold, "'is' names abstractions and folds nothing. To delegate, declare a field of the type and write 'use' on it.");
                parseFoldClause(parse, FoldRecover);
            }
            continue;
        }
        lexNextToken();
        // An enum extending an enum adds variants to the ones its base declared,
        // which is membership in a wider set rather than enrichment. It is read
        // into 'extendsbase' -- the slot for whatever base an 'extends' names --
        // and never into 'basetrait', because 'basetrait' is what every
        // substitution walk follows and these two enums do not substitute for each
        // other in either direction (doc/reference/refenum.html, compiler/c/doc/nodes/struct.md).
        if (isenum) {
            if (sawextends++)
                errorMsgLex(ErrorExtends, "An enum extends one enum.");
            strnode->extendsbase = parseTypeName(parse);
            continue;
        }
        // A trait is an abstraction: it states requirements and holds no value, so
        // there is nothing of it to enrich. What a trait asserts about another
        // abstraction is conformance, which is 'is'.
        if (strflags & TraitType) {
            errorMsgLex(ErrorExtends, "A trait is an abstraction, so there is nothing of it to enrich. To assert conformance to another abstraction, write 'is'.");
            parseTypeName(parse);
            continue;
        }
        if (sawextends++) {
            errorMsgLex(ErrorExtends, "A type enriches one concrete base.");
            parseTypeName(parse);
            continue;
        }
        strnode->extendsbase = parseTypeName(parse);
    }

    // An enum that extends another takes its base's abstractions with the variants
    // it copies from there: those copies already answer everything the base is,
    // and were written against the base with no body of their own to meet a new
    // requirement in. So an 'is' belongs on the base, whose variants come along,
    // or on a variant this enum declares, which may name its own. It is the same
    // reason a macro or a requirement is refused in such an enum's body. A struct
    // may write both clauses because it owns what its 'extends' copies in; an
    // extension does not own the variants it copies.
    if (isenum && firstis && strnode->extendsbase)
        errorMsgNode(firstis, ErrorEnumExtends,
            "%s extends an enum, and takes its base's abstractions with the variants it copies from there: an 'is' belongs on the enum it extends, or on a variant declared here.",
            &strnode->namesym->namestr);

    // An extension's discriminant is its base's: the type node is shared with the
    // base and every variant, so the integer type it is laid out in was settled there
    if (isenum && underlying && strnode->extendsbase) {
        errorMsgNode(underlying, ErrorEnumExtends,
            "%s takes its base's discriminant, so the integer type its tag values are laid out in is declared on the enum it extends.",
            &strnode->namesym->namestr);
        underlying = NULL;
    }

    // If block has been provided, process field or method definitions
    int hasEnumFld = 0;
    if (parseHasBlock()) {
        parseBlockStart();
        while (!parseBlockEnd()) {
            // Where the member starts, at its 'pub', and its keyword after that:
            // each member's span is recorded once it is parsed (dclspan.h)
            char *mstart = lex->tokp;
            uint16_t pubflag = parsePub();
            char *mkw = lex->tokp;
            uint16_t staticflag = parseStatic();
            if (staticflag && (lexIsToken(PermToken) || lexIsToken(IdentToken))) {
                // One copy shared by every value of the type: a variable in the
                // type's namespace, reached as Type.name from outside and by its
                // bare name from the type's own functions and methods. It is not
                // a field, so it has no slot in the value and no receiver.
                VarDclNode *var = parseVarDcl(parse, immPerm, ParseMayImpl | ParseMaySig);
                var->flags |= FlagStatic | pubflag;
                iNsTypeAddStatic((INsTypeNode*)strnode, var);
                parseEndOfStatement();
                parseSpan(parse, &strnode->spans, (INode*)var, mstart, mkw, SpanDcl);
                continue;
            }
            parseBadStatic(staticflag);
            // 'extern' before a method or a type's function: defined elsewhere,
            // so written without a body, as an include file declares what a
            // package's object defines. Its symbol is the one the definition
            // has, spelled after the module and the type. A trait's methods and
            // a generic type's are copied into each implementer or instance,
            // which is where their bodies are needed, so they have no one
            // definition elsewhere to name
            uint16_t externflag = 0;
            if (lexIsToken(ExternToken)) {
                lexNextToken();
                if (!lexIsToken(FnToken)) {
                    errorMsgLex(ErrorBadExtern,
                        "Inside a type, 'extern' declares a method or function defined elsewhere, written 'extern fn'. A field is part of the value, and is declared as it is.");
                    parseSkipToNextStmt();
                    continue;
                }
                if (strnode->flags & TraitType)
                    errorMsgLex(ErrorBadExtern,
                        "A trait's method is a requirement each implementer meets, or a default copied into each, so it is defined in no one place for 'extern' to name.");
                else if (strnode->genericinfo)
                    errorMsgLex(ErrorBadExtern,
                        "A generic type's methods are instantiated with it where it is used, so they are defined in no one place for 'extern' to name.");
                externflag = FlagExtern;
            }
            if (lexIsToken(FnToken)) {
                FnDclNode *fn = (FnDclNode*)parseFn(parse, externflag ? ParseMayName | ParseMaySig : methflags);
                fn->flags |= externflag;
                parseExternFnCheck(fn);
                if (fn && isNamedNode(fn)) {
                    Nodes *parms = ((FnSigNode *)fn->vtype)->parms;
                    if (parms->used > 0 && ((VarDclNode*)nodesGet(parms, 0))->namesym == selfName)
                        fn->flags |= FlagMethFld;  // function is a method if first parm is 'self'
                    // A method an extension declares is given to every variant of
                    // its set, and most of those are copies of the base's, written
                    // against the base with no body of their own to meet it in. So a
                    // requirement declared here could never be met: it is declared on
                    // the base, which is where the variants that implement it are.
                    if (parseIsEnumExtension(isenum, strnode) && (fn->flags & FlagMethFld) && fn->value == NULL)
                        errorMsgNode((INode*)fn, ErrorEnumExtends,
                            "%s extends an enum, so the method %s needs a body: every variant of %s answers it, and the copies of its base's variants were written without it, with no body of their own to implement it in. Declare the requirement on the enum it extends.",
                            &strnode->namesym->namestr, &fn->namesym->namestr, &strnode->namesym->namestr);
                    fn->flags |= pubflag;
                    iNsTypeAddFn((INsTypeNode*)strnode, fn);
                }
                parseSpan(parse, &strnode->spans, (INode*)fn, mstart, mkw, SpanDcl);
            }
            else if (lexIsToken(MacroToken)) {
                // A macro is a member by the same rule as a function: it is a
                // method when its first parameter is 'self', and the receiver
                // stands in for that parameter when it is expanded
                parseEnumExtensionMember(isenum, strnode, "a macro");
                MacroDclNode *macro = parseMacro(parse);
                if (macro->namesym != anonName) {
                    Nodes *parms = macro->parms;
                    if (parms->used > 0 && ((GenVarDclNode*)nodesGet(parms, 0))->namesym == selfName)
                        macro->flags |= FlagMethFld;
                    macro->flags |= pubflag;
                    iNsTypeAddMacro((INsTypeNode*)strnode, macro);
                }
                parseSpan(parse, &strnode->spans, (INode*)macro, mstart, mkw, SpanDcl);
            }
            else if (lexIsToken(UseToken)) {
                // A type body's 'use' folds in a SIBLING: another type that
                // declared this type's base, whose methods therefore already
                // take a receiver this type's values substitute for. Producer-side
                // composition, never a consumer-side import: ordinary lookup
                // inside a type comes from the enclosing module.
                //
                // It declares no name of its own, so there is nothing for 'pub'
                // to expose; each folded name carries its target's visibility.
                if (pubflag)
                    errorMsgLex(ErrorBadPub, "'pub' may not precede a 'use', which declares no name of its own");
                // Only a struct enriches a concrete base, so only a struct has a
                // base to share with a sibling. An enum before a trait, because
                // an enum carries TraitType too: it is the closed abstraction
                // over its own variants.
                if (isenum)
                    errorMsgLex(ErrorBadFold, "An enum's variant set is its identity: it has no concrete base to share with a sibling.");
                else if (strnode->flags & TraitType)
                    errorMsgLex(ErrorBadFold, "A trait is an abstraction: it has no concrete base to share with a sibling, and nothing of its own to fold through.");
                FieldDclNode *use = parseUseSibling(parse);
                if (strnode->siblings == NULL)
                    strnode->siblings = newNodes(4);
                nodesAdd(&strnode->siblings, (INode*)use);
                parseSpan(parse, &strnode->spans, (INode*)use, mstart, mkw, SpanMember);
            }
            else if (lexIsToken(MixinToken)) {
                // 'mixin' is retired: what it took in is declared with 'is' on the
                // declaration line, the type's, the enum's or the variant's, where
                // every abstraction a type complies with is named in one list.
                // Reported once, at the keyword, and the statement read through so
                // that the body recovers. A parse diagnostic ends the compile before
                // name resolution, so nothing is built for it.
                errorMsgLex(ErrorMixin,
                    "'mixin' is retired: declare conformance with 'is' on the declaration line -- the type's, the enum's or the variant's -- as in 'struct Gauge is Meter'.");
                lexNextToken();
                parseType(parse);
                if (parseIsFoldClause())
                    parseFoldClause(parse, FoldRecover);
                parseEndOfStatement();
            }
            else if (lexIsToken(PermToken) || lexIsToken(IdentToken)) {
                INode *perm = parseDclPerm(mutPerm);
                if (!lexIsToken(IdentToken)) {
                    errorMsgLex(ErrorNoIdent, "Expected field name for declaration");
                    parseSkipToNextStmt();
                    continue;
                }
                // Built while the lexer is still on the name, so that a
                // diagnostic about this member points at the name rather than at
                // whatever follows it
                FieldDclNode *field = newFieldDclNode(lex->val.ident, perm);
                lexNextToken();

                // In an enum, a bare name is an empty struct variant -- which is
                // what makes one construct serve both the payload-carrying form
                // and the plain set of named symbols. What follows the name says
                // which was written: a separator or a pinned tag value makes it a
                // variant, and anything else is a common field's type.
                if (isenum && (lexIsToken(CommaToken) || lexIsToken(SemiToken) || lexIsToken(AssgnToken))) {
                    strnode->flags |= HasTagField;
                    uint16_t variantflags = pubflag | (strnode->flags & FlagPub);
                    StructNode *substruct = newStructNode(field->namesym);
                    inodeLexCopy((INode*)substruct, (INode*)field);  // the name's position
                    substruct->tag = StructTag;
                    substruct->flags |= variantflags;
                    parseVariantTagPin(substruct);
                    parseAddVariant(parse, strnode, substruct, &nexttag);
                    while (lexIsToken(CommaToken)) {
                        lexNextToken();
                        if (!lexIsToken(IdentToken)) {
                            errorMsgLex(ErrorNoIdent, "Expected the name of the next variant");
                            break;
                        }
                        // On the name, so this one needs no position copied
                        StructNode *next = newStructNode(lex->val.ident);
                        next->tag = StructTag;
                        next->flags |= variantflags;
                        lexNextToken();
                        parseVariantTagPin(next);
                        parseAddVariant(parse, strnode, next, &nexttag);
                    }
                    parseEndOfStatement();
                    parseSpan(parse, &strnode->spans, NULL, mstart, mkw, SpanMember);
                    continue;
                }

                parseFieldDclBody(parse, field);
                // A common field is part of every variant's layout, the copies of the
                // base's included, and their methods were written against the
                // base's: the fields they read sit where the base put them. The
                // discriminant is one of those fields, and the base's.
                if (parseIsEnumExtension(isenum, strnode)) {
                    if (field->vtype->tag == EnumTag)
                        errorMsgNode((INode*)field, ErrorEnumExtends,
                            "%s takes its base's discriminant, so where its tag is laid out was settled on the enum it extends.",
                            &strnode->namesym->namestr);
                    else
                        errorMsgNode((INode*)field, ErrorEnumExtends,
                            "%s extends an enum, so it declares no common field: %s would change the layout of the copies of its base's variants, whose methods were written against the base's. Declare it on the enum it extends.",
                            &strnode->namesym->namestr, &field->namesym->namestr);
                }
                field->index = fieldnbr++;
                field->flags |= FlagMethFld | pubflag;
                // Only a struct folds: a trait and an enum are abstractions over
                // their implementers, with no organizing details of their own to
                // fold through
                if (field->fold && (strnode->flags & TraitType))
                    errorMsgNode(field->fold->at, ErrorBadFold, "Only a struct folds a field's members in. A trait and an enum have no organizing details of their own to fold through.");
                // An enum's first 'tag'-typed field is its discriminant, written
                // only to place it somewhere other than position 0, which is done
                // for alignment. Marked here because a variant copies the enum's
                // fields as soon as it is name resolved, ahead of the enum's type
                // check; type check validates the mark and refuses a second one.
                //
                // Only an enum carries a discriminant at all: it is the tag that
                // says which variant a value holds, and a struct, a trait and a
                // variant each have nothing for one to distinguish.
                if (field->vtype->tag == EnumTag) {
                    if (!isenum)
                        errorMsgNode((INode*)field, ErrorInvType,
                            "Only an enum carries a discriminant: there is nothing here for a tag to tell apart.");
                    else if (!hasEnumFld && strnode->basetrait == NULL)
                        field->flags |= IsTagField;
                    hasEnumFld = 1;
                }
                structAddField(strnode, field);
                parseEndOfStatement();
                parseSpan(parse, &strnode->spans, (INode*)field, mstart, mkw, SpanMember);
            }
            else if (lexIsToken(StructToken)) {
                // A struct written inside an enum is one of its variants.
                //
                // Only an enum: a trait is the open abstraction, and its
                // implementers are ordinary structs declared beside it that name
                // it with 'is'. A closed set of variants is an enum, which
                // is where the tag and the exhaustive match live.
                if (isenum) {
                    strnode->flags |= HasTagField;

                    // A variant is as visible as its enum: a use that can name
                    // the enum can match on it. It may also be declared 'pub' itself.
                    StructNode *substruct = (StructNode *)parseStruct(parse, pubflag | (strnode->flags & FlagPub));
                    parseAddVariant(parse, strnode, substruct, &nexttag);
                    parseSpan(parse, &strnode->spans, (INode*)substruct, mstart, mkw, SpanDcl);
                }
                else if (strnode->flags & TraitType) {
                    errorMsgLex(ErrorOpenTrait, "A trait is open: its implementers are declared beside it and name it with 'is'. A closed set of variants is an 'enum'.");
                    parseStruct(parse, 0);
                }
                else {
                    errorMsgLex(ErrorNoIdent, "structs in structs not yet supported");
                    parseStruct(parse, 0);
                }
            }
            else {
                errorMsgLex(ErrorNoSemi, "Unknown struct statement.");
                parseSkipToNextStmt();
            }
        }
    }
    else
        parseEndOfStatement();

    // An enum's identity is its variant set, so an enum with no variants names
    // nothing a value of it could be and nothing a match could account for. An
    // extension that adds none is a second name for its base rather than the
    // wider set it was written to be, which is its own mistake to name.
    if (isenum && !(strnode->flags & HasTagField)) {
        if (strnode->extendsbase)
            // At the declaration's own name: the block that should have held a
            // variant has ended, so the lexer is on whatever follows it
            errorMsgNode((INode*)strnode, ErrorEnumExtends,
                "%s extends an enum and adds no variant, which makes it a second name for the same set rather than a wider one.",
                &strnode->namesym->namestr);
        else
            errorMsgLex(ErrorNoVariants, "An enum declares its variants: an empty one has no value it could hold.");
    }

    // The tag field belongs to the closed-variant machinery: it is the
    // discriminant a match on a plain reference reads to pick the variant, and
    // only an enum -- whose variants are all declared inside it -- can assign
    // each variant a value. A trait is open, so its implementers may be extended
    // by another module and no value could be unique: there is nothing to
    // synthesize, and dispatch and narrowing go through a virtual reference
    // instead (doc/reference/refvirtref.html). One is inserted here unless the enum
    // placed its own, which the walk above has already marked.
    //
    // An extension has no discriminant of its own to place or synthesize: its
    // base's arrives with the fields name resolution splices in, so the copies of
    // the base's variants and the variants it adds read one discriminant type.
    if ((strnode->flags & HasTagField) && !hasEnumFld && !(isenum && strnode->extendsbase)) {
        FieldDclNode *fldnode = newFieldDclNode(anonName, (INode*)immPerm);
        fldnode->vtype = (INode*)newEnumNode();
        fldnode->flags |= IsTagField;
        nodelistInsert(&strnode->fields, 0, (INode*)fldnode);
    }

    // The declared integer type belongs to the discriminant, wherever it sits
    if (underlying) {
        INode **nodesp;
        uint32_t cnt;
        int found = 0;
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            if ((*nodesp)->flags & IsTagField) {
                ((EnumNode*)((FieldDclNode*)*nodesp)->vtype)->underlying = underlying;
                found = 1;
            }
        }
        if (!found)
            errorMsgNode(underlying, ErrorInvType, "An integer type here lays out the enum's tag values, and this enum has no variants to number.");
    }

    parse->typenode = svtype;
    return (INode*)strnode;
}

// Parse a function's type signature
INode *parseFnSig(ParseState *parse) {
    FnSigNode *fnsig;
    uint16_t parmnbr = 0;
    uint16_t parseflags = ParseMaySig | ParseMayImpl;

    // Set up memory block for the function's type signature
    fnsig = newFnSigNode();

    // A parameter's type is bounded by the parentheses around it, so a '{'
    // there opens no enclosing block, whoever this signature belongs to.
    int svinrettype = parse->inrettype;
    parse->inrettype = 0;

    // Process parameter declarations
    if (lexIsToken(LParenToken)) {
        lexNextToken();
        while (lexIsToken(PermToken) || lexIsToken(IdentToken)) {
            VarDclNode *parm = parseVarDcl(parse, immPerm, parseflags);
            parm->flowtempflags |= VarInitialized;   // parameter vars always start with a valid value
            // Do special inference if function is a type's method
            if (parse->typenode) {
                // Infer value type of a parameter (or its reference) if unspecified
                if (parm->vtype == unknownType) {
                    parm->vtype = (INode*)newNameUseNode(selfTypeName);
                }
                else if (parm->vtype->tag == RefTag) {
                    RefNode *refnode = (RefNode *)parm->vtype;
                    if (refnode->vtexp == unknownType) {
                        refnode->vtexp = (INode*)newNameUseNode(selfTypeName);
                    }
                }
            }
            // Add parameter to function's parm list
            parm->scope = 1;
            parm->index = parmnbr++;
            if (parm->value)
                parseflags = ParseMayImpl; // force remaining parms to specify default
            nodesAdd(&fnsig->parms, (INode*)parm);
            if (!lexIsToken(CommaToken))
                break;
            lexNextToken();
        }
        parseCloseTok(RParenToken);
    }
    else
        errorMsgLex(ErrorNoLParen, "Expected left parenthesis for parameter declarations");

    // Parse return type info - turn into void if none specified.
    // A '{' after the return type opens the body of the function being
    // declared, so nothing read here may claim it as its own.
    parse->inrettype = 1;
    if ((fnsig->rettype = parseType(parse)) != unknownType) {
        // Handle multiple return types
        if (lexIsToken(CommaToken)) {
            TupleNode *rettype = newTupleNode(4);
            nodesAdd(&rettype->elems, fnsig->rettype);
            while (lexIsToken(CommaToken)) {
                lexNextToken();
                nodesAdd(&rettype->elems, parseType(parse));
            }
            fnsig->rettype = (INode*)rettype;
        }
    }
    else {
        fnsig->rettype = (INode*)newVoidNode();
        inodeLexCopy(fnsig->rettype, (INode*)fnsig);  // Make invisible void show up in error msg
    }
    parse->inrettype = svinrettype;

    return (INode*)fnsig;
}

// Parse a typedef statement.
//
// A typedef is an alias: a name in the module's namespace standing for what
// another expression names, with a local spelling and a visibility of its own.
// It is the same binding record a fold makes, with a type expression as the
// target instead of a member name -- which is what it is here to prove.
AliasDclNode *parseTypedef(ParseState *parse) {
    lexNextToken();
    // Process struct type name, if provided
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoIdent, "Expected a name for the type");
        return NULL;
    }
    AliasDclNode *newnode = newTypeAliasDclNode(lex->val.ident, NULL);
    lexNextToken();
    newnode->target = parseType(parse);
    parseEndOfStatement();
    return newnode;
}

// Parse a type expression. Return unknownType if none found.
INode* parseType(ParseState *parse) {
    // This is a placeholder since parser converges type and value expression parsing
    switch (lex->toktype) {
    case IdentToken:    // type identifier (or generic)
    case VoidToken:     // void
    case QuesToken:     // Optional type sugar
    case LBracketToken: // Array
    case LParenToken:   // Tuple

    // References and pointers
    case AmperToken:
    case ArrayRefToken:
    case VirtRefToken:
    case PlusToken:
    case PlusArrayRefToken:
    case PlusVirtRefToken:
    case StarToken:
    {    
        // The parsing logic for value expressions also works for types (although overkill)
        return parsePrefix(parse);
    }
    default:
        return unknownType;
    }
}
