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

// Parse a variable declaration
VarDclNode *parseVarDcl(ParseState *parse, PermNode *defperm, uint16_t flags) {
    VarDclNode *varnode;
    INode *perm;

    // Grab the permission type
    perm = parsePerm();
    if (perm->tag == UnknownTag)
        perm = (INode*)defperm;
    INode *permdcl = itypeGetTypeDcl(perm);
    if (permdcl == (INode*)mut1Perm || permdcl == (INode*)uniPerm || permdcl == (INode*)opaqPerm
        || (permdcl == (INode*)roPerm && !(flags & ParseMayConst)))
        errorMsgNode(perm, ErrorInvType, "Permission not valid for variable/field declaration");

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

    return constnode;
}

INode *parseTypeName(ParseState *parse) {
    INode *node = parseNameUse(parse);
    if (lexIsToken(LBracketToken)) {
        FnCallNode *fncall = newFnCallNode(node, 8);
        fncall->flags |= FlagIndex;
        lexNextToken();
        lexIncrParens();
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

// Parse a field declaration
FieldDclNode *parseFieldDcl(ParseState *parse, PermNode *defperm) {
    FieldDclNode *fldnode;
    INode *vtype;
    INode *perm;

    // Grab the permission type
    perm = parsePerm();
    INode *permdcl = perm == unknownType? unknownType : itypeGetTypeDcl(perm);
    if (permdcl != (INode*)mutPerm && permdcl == (INode*)immPerm)
        errorMsgNode(perm, ErrorInvType, "Permission not valid for field declaration");

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

    return fldnode;
}

// Parse a struct
INode *parseStruct(ParseState *parse, uint16_t strflags) {
    char *svprefix = parse->gennamePrefix;
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
            strflags |= OpaqueType;
            lexNextToken();
        }
        else
            break;
    }

    // Process struct type name, if provided
    if (lexIsToken(IdentToken)) {
        strnode = newStructNode(lex->val.ident);
        strnode->tag = tag;
        strnode->flags |= strflags;
        strnode->mod = parse->mod;
        nameConcatPrefix(&parse->gennamePrefix, &strnode->namesym->namestr);
        parse->typenode = (INsTypeNode *)strnode;
        lexNextToken();
    }
    else {
        errorMsgLex(ErrorNoIdent, "Expected a name for the type");
        return NULL;
    }

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
            lexStmtStart();
            if (lexIsToken(FnToken)) {
                FnDclNode *fn = (FnDclNode*)parseFn(parse, methflags);
                if (fn && isNamedNode(fn)) {
                    Nodes *parms = ((FnSigNode *)fn->vtype)->parms;
                    if (parms->used > 0 && ((VarDclNode*)nodesGet(parms, 0))->namesym == selfName)
                        fn->flags |= FlagMethFld;  // function is a method if first parm is 'self'
                    nameGenFnName(fn, parse->gennamePrefix);
                    iNsTypeAddFn((INsTypeNode*)strnode, fn);
                }
            }
            else if (lexIsToken(MixinToken)) {
                // Handle a trait mixin, capturing it in a field-like node
                FieldDclNode *field = newFieldDclNode(anonName, (INode*)immPerm);
                field->flags |= IsMixin | FlagMethFld;
                lexNextToken();
                INode *vtype;
                if ((vtype = parseType(parse)))
                    field->vtype = vtype;
                structAddField(strnode, field);
                parseEndOfStatement();
            }
            else if (lexIsToken(PermToken) || lexIsToken(IdentToken)) {
                FieldDclNode *field = parseFieldDcl(parse, mutPerm);
                field->index = fieldnbr++;
                field->flags |= FlagMethFld;
                if (field->vtype->tag == EnumTag)
                    hasEnumFld = 1;
                structAddField(strnode, field);
                parseEndOfStatement();
            }
            else if (lexIsToken(StructToken)) {
                // If we see structs in trait/union, treat them as tagged extensions/derived structs
                if (strnode->flags & TraitType) {
                    strnode->flags |= HasTagField;

                    StructNode *substruct = (StructNode *)parseStruct(parse, 0); // Parse sub-struct
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

    // If a trait that needs a tag field doesn't have one, insert default enum field as first field
    if ((strnode->flags & (TraitType | HasTagField)) && !hasEnumFld) {
        FieldDclNode *fldnode = newFieldDclNode(anonName, (INode*)immPerm);
        fldnode->vtype = (INode*)newEnumNode();
        nodelistInsert(&strnode->fields, 0, (INode*)fldnode);
    }

    parse->typenode = svtype;
    parse->gennamePrefix = svprefix;
    return (INode*)strnode;
}

// Parse a function's type signature
INode *parseFnSig(ParseState *parse) {
    FnSigNode *fnsig;
    uint16_t parmnbr = 0;
    uint16_t parseflags = ParseMaySig | ParseMayImpl;

    // Set up memory block for the function's type signature
    fnsig = newFnSigNode();

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

    // Parse return type info - turn into void if none specified
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
        return parsePrefix(parse, 0);
    }
    default:
        return unknownType;
    }
}
