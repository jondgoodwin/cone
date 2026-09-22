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

// Parse an enum type
INode* parseEnum(ParseState *parse) {
    EnumNode *node = newEnumNode();
    lexNextToken();
    return (INode*)node;
}

// Parse a field's fold clause, with the lexer on its 'use': '*' with an
// optional 'but name, name', or 'name [as name], name [as name]'. Each listed
// name becomes an alias under its local spelling, positioned at the item,
// whose target spells the name in the field's type; whether that is a field
// or a method is not known until the field's type is, so the fold is expanded
// by name resolution (structFoldExpand), which is what binds each target.
FoldClause *parseFoldClause(ParseState *parse) {
    FoldClause *fold = newFoldClause();
    lexNextToken();
    if (lexIsToken(StarToken)) {
        fold->star = 1;
        lexNextToken();
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
        return fold;
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
        errorMsgLex(ErrorBadFold, "'but' leaves a name out of 'use *'. A listed fold admits only the names it lists.");
        lexNextToken();
        while (lexIsToken(IdentToken) || lexIsToken(CommaToken))
            lexNextToken();
    }
    return fold;
}

// Parse a field declaration
FieldDclNode *parseFieldDcl(ParseState *parse, PermNode *defperm) {
    FieldDclNode *fldnode;
    INode *vtype;
    INode *perm = parseDclPerm(defperm);

    // Obtain variable's name
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoIdent, "Expected field name for declaration");
        return newFieldDclNode(anonName, perm);
    }
    fldnode = newFieldDclNode(lex->val.ident, perm);
    lexNextToken();

    // Get value type, if provided
    if (lexIsToken(EnumToken))
        fldnode->vtype = parseEnum(parse);
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

// Parse a struct
INode *parseStruct(ParseState *parse, uint16_t strflags) {
    INsTypeNode *svtype = parse->typenode;
    StructNode *strnode;
    uint16_t fieldnbr = 0;

    // Capture the kind of type, then get next token (name)
    uint16_t tag = StructTag;
    lexNextToken();

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

    // Obtain base trait, if specified
    if (lexIsToken(ExtendsToken)) {
        lexNextToken();
        strnode->basetrait = parseTypeName(parse);  // Type could be a qualified name or generic
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
                FieldDclNode *field = parseFieldDcl(parse, mutPerm);
                field->index = fieldnbr++;
                field->flags |= FlagMethFld | pubflag;
                // Only a struct folds: a trait is an abstraction, with no
                // organizing details of its own to fold through
                if (field->fold && (strnode->flags & TraitType))
                    errorMsgNode(field->fold->at, ErrorBadFold, "Only a struct folds a field's members in. A trait or union is an abstraction and has no organizing details to fold through.");
                // A base trait's first enum-typed field is its discriminant.
                // Marked here because a variant copies the trait's fields as
                // soon as it is name resolved, ahead of the trait's type check;
                // type check validates the mark and refuses a second enum field.
                if (field->vtype->tag == EnumTag) {
                    if (!hasEnumFld && (strnode->flags & TraitType) && strnode->basetrait == NULL)
                        field->flags |= IsTagField;
                    hasEnumFld = 1;
                }
                structAddField(strnode, field);
                parseEndOfStatement();
            }
            else if (lexIsToken(StructToken)) {
                // If we see structs in trait/union, treat them as tagged extensions/derived structs
                if (strnode->flags & TraitType) {
                    strnode->flags |= HasTagField;

                    // A variant is as visible as its trait: a use that can name
                    // the trait can match on it. It may also be declared 'pub' itself.
                    StructNode *substruct = (StructNode *)parseStruct(parse, pubflag | (strnode->flags & FlagPub)); // Parse sub-struct
                    substruct->flags |= HasTagField | (strnode->flags & SameSize);

                    // Build node that indicates this struct extends from trait
                    if (substruct->basetrait)
                        errorMsgLex(ErrorNoIdent, "trait's struct must not specify extends");
                    INode *traitref = (INode*)newNameUseNode(strnode->namesym);

                    // Inherit generic parms
                    if (substruct->genericinfo)
                        errorMsgLex(ErrorNoIdent, "trait's struct must not specify generic parms");
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
                    substruct->basetrait = (INode*)traitref;

                    // Add substruct to trait's list of derived, and capture enum value
                    if (!strnode->derived)
                        strnode->derived = newNodes(4);
                    substruct->tagnbr = strnode->derived->used;
                    nodesAdd(&strnode->derived, (INode*)substruct);
                    modAddNode(parse->mod, inodeGetName((INode*)substruct), (INode*)substruct);
                    // Bound in the module, but declared inside the trait: the
                    // variant's symbols are spelled after the trait, so the trait
                    // is its owner
                    dclInfoJoin((INode*)substruct, (INode*)strnode);
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

    // The tag field belongs to the closed-variant machinery: it is the
    // discriminant a match on a plain reference reads to pick the variant, and
    // only a closed type -- a union, or a trait whose variants are declared
    // inside it -- can assign each variant a value. An open trait's variants
    // may be extended by another module, so there is no tag to synthesize:
    // dispatch and narrowing go through a virtual reference instead
    // (coneref/reftraitvar.html). One is inserted here unless the type wrote
    // its own enum-typed field, which the walk above has already marked.
    if ((strnode->flags & HasTagField) && !hasEnumFld) {
        FieldDclNode *fldnode = newFieldDclNode(anonName, (INode*)immPerm);
        fldnode->vtype = (INode*)newEnumNode();
        fldnode->flags |= IsTagField;
        nodelistInsert(&strnode->fields, 0, (INode*)fldnode);
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
