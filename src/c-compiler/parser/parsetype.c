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
// coneref/refstruct.html means the container's permission governs.
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
        return newVarDclFull(anonName, VarDclTag, unknownType, perm, NULL);
    }
    varnode = newVarDclNode(lex->val.ident, VarDclTag, perm);
    lexNextToken();

    // Get value type, if provided
    varnode->vtype = parseType(parse);

    // Get initialization value after '=', if provided
    if (lexIsToken(AssgnToken)) {
        if (!(flags&ParseMayImpl))
            errorMsgLex(ErrorBadImpl, "A default/initial value may not be specified here.");
        lexNextToken();
        if (lexIsToken(UndefToken)) {
            // 'undef' is used to signal that programmer believes variable
            // can be considered safely "initialized", even though it is UB.
            varnode->flowtempflags |= VarInitialized;
            lexNextToken();
        }
        else
            varnode->value = parseAnyExpr(parse);
    }
    else {
        if (!(flags&ParseMaySig))
            errorMsgLex(ErrorNoInit, "Must specify default/initial value.");
    }

    // Only a field folds. Refused here, on a global, a local, a parameter and a
    // type's static alike, so that the diagnostic is the fold's own rather than
    // a missing semicolon; the clause is read and dropped to recover.
    if (lexIsToken(UseToken)) {
        errorMsgLex(ErrorBadFold, "Only a struct's field may fold names in with 'use'.");
        parseFoldClause(parse);
    }

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
static void parseFoldItems(ParseState *parse, FoldClause *fold, int maystar) {
    if (maystar && lexIsToken(StarToken)) {
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
    // 'but' leaves a name out of everything; a list admits only what it names
    if (lexIsToken(ButToken)) {
        errorMsgLex(ErrorBadFold, "'but' leaves a name out of a fold of every member. A listed fold admits only the names it lists.");
        lexNextToken();
        while (lexIsToken(IdentToken) || lexIsToken(CommaToken))
            lexNextToken();
    }
}

// Parse a field's fold clause, with the lexer on its 'use'. The field's type is
// the source, so the clause is nothing but what it admits, and '*' is how it
// says every member.
FoldClause *parseFoldClause(ParseState *parse) {
    FoldClause *fold = newFoldClause();
    lexNextToken();
    parseFoldItems(parse, fold, 1);
    return fold;
}

// Parse a type body's 'use' clause, with the lexer on the 'use': the sibling
// enrichment it folds in, then what it admits of it. Held in a field-like node,
// as 'mixin' and a further 'is' are, because that is what carries a type
// expression and a fold clause through cloning; it is not a field and never
// joins the field list, since a sibling contributes no representation.
static FieldDclNode *parseUseSibling(ParseState *parse) {
    FieldDclNode *use = newFieldDclNode(anonName, (INode*)immPerm);
    FoldClause *fold = newFoldClause();
    use->fold = fold;
    lexNextToken();
    inodeLexCopy((INode*)use, fold->at);
    use->vtype = parseTypeName(parse);
    // Naming the sibling is what asks for its members, so every one of them is
    // the default and there is no second spelling for it
    fold->star = 1;
    if (lexIsToken(StarToken)) {
        errorMsgLex(ErrorBadFold, "A type body's 'use' brings in every member of what it names already; '*' says nothing more.");
        lexNextToken();
    }
    else if (lexIsToken(IdentToken))
        fold->star = 0;
    parseFoldItems(parse, fold, 0);
    parseEndOfStatement();
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
    if (lexIsToken(UseToken)) {
        fldnode->fold = parseFoldClause(parse);
        if (fldnode->vtype == unknownType)
            errorMsgNode(fldnode->fold->at, ErrorBadFold, "A field that folds names in must write its type, which is where the names come from.");
    }

    return fldnode;
}


// Join a variant to the enum that declares it: the closed-type flags, the
// base link back to the enum, the tag number, and the module binding.
//
// The enum owns the layout, so the variant states none of it. Its tag number is
// assigned ascending from zero across the body unless the author pins one, after
// which numbering continues from there -- which is what lines an enum up with an
// external library's constants without renumbering the rest by hand.
static void parseAddVariant(ParseState *parse, StructNode *strnode, StructNode *substruct, uint32_t *nexttag) {
    substruct->flags |= HasTagField | (strnode->flags & SameSize);
    if (substruct->tagnbr == TagUnassigned)
        substruct->tagnbr = *nexttag;

    // The enum already says which enum a variant belongs to and what its type
    // parameters are, so a variant restating either is refused rather than ignored.
    // Reported on the variant, whose node carries the position of its name in
    // both of its spellings, rather than at whatever token the body ended on.
    if (substruct->basetrait)
        errorMsgNode((INode*)substruct, ErrorVariantDcl,
            "%s is a member of the enum it is written inside; remove the 'is'.",
            &substruct->namesym->namestr);
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
            if (((StructNode*)*nodesp)->tagnbr == substruct->tagnbr)
                errorMsgNode((INode*)substruct, ErrorDupTag,
                    "Tag value %d is already taken by variant %s.",
                    (int)substruct->tagnbr, &((StructNode*)*nodesp)->namesym->namestr);
        }
    }
    else
        strnode->derived = newNodes(4);
    nodesAdd(&strnode->derived, (INode*)substruct);
    *nexttag = substruct->tagnbr + 1;

    modAddNode(parse->mod, inodeGetName((INode*)substruct), (INode*)substruct);
    // Bound in the module, but declared inside the enum: the variant's symbols
    // are spelled after the enum, so the enum is its owner
    dclInfoJoin((INode*)substruct, (INode*)strnode);
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
    // declaration -- which is what lets 'mod trait' and 'actor trait' name the
    // abstractions of those kinds when they arrive.
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
    // trait is held as a mixin-style placeholder, exactly as 'mixin' is, and must
    // require no fields at all.
    //
    // 'extends' names a CONCRETE base to enrich with methods, which is a different
    // assertion, so a type may write both clauses. They are therefore read in a
    // loop rather than as alternatives: either order, each one once.
    int sawis = 0, sawextends = 0;
    while (lexIsToken(IsToken) || lexIsToken(ExtendsToken)) {
        if (lexIsToken(IsToken)) {
            lexNextToken();
            if (sawis++) {
                errorMsgLex(ErrorExtends, "A type names its abstractions in one 'is' list, separated by commas.");
                parseTypeName(parse);
                continue;
            }
            strnode->basetrait = parseTypeName(parse);  // Type could be a qualified name or generic
            while (lexIsToken(CommaToken)) {
                lexNextToken();
                FieldDclNode *isafld = newFieldDclNode(anonName, (INode*)immPerm);
                isafld->flags |= IsMixin | FlagMethFld;
                isafld->vtype = parseTypeName(parse);
                structAddField(strnode, isafld);
            }
            // 'is' takes no siblings to fold from: it names abstractions, and an
            // abstraction has no value to reach a folded name through. Delegation is
            // what a field's own 'use' clause is for.
            if (lexIsToken(UseToken)) {
                errorMsgLex(ErrorBadFold, "'is' names abstractions and folds nothing. To delegate, declare a field of the type and write 'use' on it.");
                parseFoldClause(parse);
            }
            continue;
        }
        lexNextToken();
        // An enum extending an enum adds variants to the ones its base declared.
        // That is membership in a wider set rather than enrichment, it is spelled
        // 'extends' too, and it is not implemented (coneref/refenum.html); the
        // clause is read into 'basetrait' as it always has been so that nothing
        // about it moves.
        if (isenum) {
            if (sawextends++)
                errorMsgLex(ErrorExtends, "An enum extends one enum.");
            strnode->basetrait = parseTypeName(parse);
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

    // If block has been provided, process field or method definitions
    int hasEnumFld = 0;
    if (parseHasBlock()) {
        parseBlockStart();
        while (!parseBlockEnd()) {
            uint16_t pubflag = parsePub();
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
                continue;
            }
            parseBadStatic(staticflag);
            if (lexIsToken(FnToken)) {
                FnDclNode *fn = (FnDclNode*)parseFn(parse, methflags);
                if (fn && isNamedNode(fn)) {
                    Nodes *parms = ((FnSigNode *)fn->vtype)->parms;
                    if (parms->used > 0 && ((VarDclNode*)nodesGet(parms, 0))->namesym == selfName)
                        fn->flags |= FlagMethFld;  // function is a method if first parm is 'self'
                    fn->flags |= pubflag;
                    iNsTypeAddFn((INsTypeNode*)strnode, fn);
                }
            }
            else if (lexIsToken(MacroToken)) {
                // A macro is a member by the same rule as a function: it is a
                // method when its first parameter is 'self', and the receiver
                // stands in for that parameter when it is expanded
                MacroDclNode *macro = parseMacro(parse);
                if (macro->namesym != anonName) {
                    Nodes *parms = macro->parms;
                    if (parms->used > 0 && ((GenVarDclNode*)nodesGet(parms, 0))->namesym == selfName)
                        macro->flags |= FlagMethFld;
                    macro->flags |= pubflag;
                    iNsTypeAddMacro((INsTypeNode*)strnode, macro);
                }
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
            }
            else if (lexIsToken(MixinToken)) {
                // Handle a trait mixin, capturing it in a field-like node.
                // It binds no name, so there is nothing for 'pub' to expose.
                if (pubflag)
                    errorMsgLex(ErrorBadPub, "'pub' may not precede a mixin, which declares no name");
                FieldDclNode *field = newFieldDclNode(anonName, (INode*)immPerm);
                field->flags |= IsMixin | FlagMethFld;
                lexNextToken();
                INode *vtype;
                if ((vtype = parseType(parse)))
                    field->vtype = vtype;
                // A mixin brings the trait's members in already; there is
                // nothing left for a fold to admit
                if (lexIsToken(UseToken)) {
                    errorMsgLex(ErrorBadFold, "A mixin brings in every member of the trait; it does not fold.");
                    parseFoldClause(parse);
                }
                structAddField(strnode, field);
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
                    continue;
                }

                parseFieldDclBody(parse, field);
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
    // nothing a value of it could be and nothing a match could account for
    if (isenum && !(strnode->flags & HasTagField))
        errorMsgLex(ErrorNoVariants, "An enum declares its variants: an empty one has no value it could hold.");

    // The tag field belongs to the closed-variant machinery: it is the
    // discriminant a match on a plain reference reads to pick the variant, and
    // only an enum -- whose variants are all declared inside it -- can assign
    // each variant a value. A trait is open, so its implementers may be extended
    // by another module and no value could be unique: there is nothing to
    // synthesize, and dispatch and narrowing go through a virtual reference
    // instead (coneref/refvirtref.html). One is inserted here unless the enum
    // placed its own, which the walk above has already marked.
    if ((strnode->flags & HasTagField) && !hasEnumFld) {
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

// Parse a typedef statement
TypedefNode *parseTypedef(ParseState *parse) {
    lexNextToken();
    // Process struct type name, if provided
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoIdent, "Expected a name for the type");
        return NULL;
    }
    TypedefNode *newnode = newTypedefNode(lex->val.ident);
    lexNextToken();
    newnode->typeval = parseType(parse);
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
