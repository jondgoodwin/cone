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
INode *parsePerm(PermNode *defperm) {
    if (lexIsToken(PermToken)) {
        INode *perm = newPermUseNode((PermNode*)lex->val.ident->node);
        lexNextToken();
        return perm;
    }
    else if (defperm)
        return (INode*)newPermUseNode(defperm);
    else
        return (INode*)defperm;
}

// Parse an allocator + permission for a reference type
void parseAllocPerm(RefNode *refnode) {
    if (lexIsToken(IdentToken)
        && lex->val.ident->node && lex->val.ident->node->tag == AllocTag) {
        refnode->alloc = (INode*)lex->val.ident->node;
        lexNextToken();
        refnode->perm = parsePerm(uniPerm);
    }
    else {
        refnode->alloc = voidType;
        refnode->perm = parsePerm(constPerm);
    }
}

// Parse a variable declaration
VarDclNode *parseVarDcl(ParseState *parse, PermNode *defperm, uint16_t flags) {
    VarDclNode *varnode;
    INode *vtype;
    INode *perm;

    // Grab the permission type
    perm = parsePerm(defperm);
    INode *permdcl = itypeGetTypeDcl(perm);
    if (permdcl == (INode*)mut1Perm || permdcl == (INode*)uniPerm || permdcl == (INode*)opaqPerm
        || (permdcl == (INode*)constPerm && !(flags & ParseMayConst)))
        errorMsgNode(perm, ErrorInvType, "Permission not valid for variable/field declaration");

    // Obtain variable's name
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoIdent, "Expected variable name for declaration");
        return newVarDclFull(nametblFind("_", 1), VarDclTag, voidType, perm, NULL);
    }
    varnode = newVarDclNode(lex->val.ident, VarDclTag, perm);
    lexNextToken();

    // Get value type, if provided
    if ((vtype = parseVtype(parse)))
        varnode->vtype = vtype;

    // Get initialization value after '=', if provided
    if (lexIsToken(AssgnToken)) {
        if (!(flags&ParseMayImpl))
            errorMsgLex(ErrorBadImpl, "A default/initial value may not be specified here.");
        lexNextToken();
        varnode->value = parseAnyExpr(parse);
    }
    else {
        if (!(flags&ParseMaySig))
            errorMsgLex(ErrorNoInit, "Must specify default/initial value.");
    }

    return varnode;
}

// Parse a variable declaration
FieldDclNode *parseFieldDcl(ParseState *parse, PermNode *defperm) {
    FieldDclNode *fldnode;
    INode *vtype;
    INode *perm;

    // Grab the permission type
    perm = parsePerm(defperm);
    INode *permdcl = itypeGetTypeDcl(perm);
    if (permdcl != (INode*)mutPerm && permdcl == (INode*)immPerm)
        errorMsgNode(perm, ErrorInvType, "Permission not valid for field declaration");

    // Obtain variable's name
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoIdent, "Expected variable name for declaration");
        return newFieldDclNode(nametblFind("_", 1), perm);
    }
    fldnode = newFieldDclNode(lex->val.ident, perm);
    lexNextToken();

    // Get value type, if provided
    if ((vtype = parseVtype(parse)))
        fldnode->vtype = vtype;

    // Get initialization value after '=', if provided
    if (lexIsToken(AssgnToken)) {
        lexNextToken();
        fldnode->value = parseAnyExpr(parse);
    }

    return fldnode;
}

// Parse a struct
INode *parseStruct(ParseState *parse) {
    char *svprefix = parse->gennamePrefix;
    INsTypeNode *svtype = parse->typenode;
    StructNode *strnode;
    int16_t fieldnbr = 0;

    // Capture the kind of type, then get next token (name)
    uint16_t tag = StructTag;
    lexNextToken();

    // Process struct type name, if provided
    if (lexIsToken(IdentToken)) {
        strnode = newStructNode(lex->val.ident);
        strnode->tag = tag;
        strnode->mod = parse->mod;
        nameConcatPrefix(&parse->gennamePrefix, &strnode->namesym->namestr);
        parse->typenode = (INsTypeNode *)strnode;
        lexNextToken();
    }
    else {
        errorMsgLex(ErrorNoIdent, "Expected a name for the type");
        return NULL;
    }

    // Process field or method definitions
    if (lexIsToken(LCurlyToken)) {
        lexNextToken();
        while (1) {
            if (lexIsToken(SetToken)) {
                lexNextToken();
                if (!lexIsToken(FnToken))
                    errorMsgLex(ErrorNotFn, "Expected fn declaration");
                else {
                    FnDclNode *fn = (FnDclNode*)parseFn(parse, FlagMethFld, ParseMayName | ParseMayImpl);
                    if (fn && isNamedNode(fn)) {
                        fn->flags |= FlagSetMethod;
                        nameGenFnName(fn, parse->gennamePrefix);
                        iNsTypeAddFn((INsTypeNode*)strnode, fn);
                    }
                }
            }
            else if (lexIsToken(FnToken)) {
                FnDclNode *fn = (FnDclNode*)parseFn(parse, FlagMethFld, ParseMayName | ParseMayImpl);
                if (fn && isNamedNode(fn)) {
                    nameGenFnName(fn, parse->gennamePrefix);
                    iNsTypeAddFn((INsTypeNode*)strnode, fn);
                }
            }
            else if (lexIsToken(PermToken) || lexIsToken(IdentToken)) {
                FieldDclNode *field = parseFieldDcl(parse, mutPerm);
                field->index = fieldnbr++;
                field->flags |= FlagMethFld;
                structAddField(strnode, field);
                parseEndOfStatement();
            }
            else
                break;
        }
        parseRCurly();
    }
    // Opaque struct
    else if (lexIsToken(SemiToken))
        lexNextToken();
    else
        errorMsgLex(ErrorNoLCurly, "Expected left curly bracket enclosing fields or methods");

    parse->typenode = svtype;
    parse->gennamePrefix = svprefix;
    return (INode*)strnode;
}

void parseInjectSelf(FnSigNode *fnsig, Name *typename) {
    NameUseNode *selftype = newNameUseNode(typename);
    VarDclNode *selfparm = newVarDclFull(nametblFind("self", 4), VarDclTag, (INode*)selftype, newPermUseNode(constPerm), NULL);
    selfparm->scope = 1;
    selfparm->index = 0;
    nodesAdd(&fnsig->parms, (INode*)selfparm);
}

// Parse a function's type signature
INode *parseFnSig(ParseState *parse) {
    FnSigNode *fnsig;
    int16_t parmnbr = 0;
    uint16_t parseflags = ParseMaySig | ParseMayImpl;

    // Set up memory block for the function's type signature
    fnsig = newFnSigNode();

    // Process parameter declarations
    if (lexIsToken(LParenToken)) {
        lexNextToken();
        // A type's method with no parameters should still define self
        if (lexIsToken(RParenToken) && parse->typenode)
            parseInjectSelf(fnsig, parse->typenode->namesym);
        while (lexIsToken(PermToken) || lexIsToken(IdentToken)) {
            VarDclNode *parm = parseVarDcl(parse, immPerm, parseflags);
            // Do special inference if function is a type's method
            if (parse->typenode) {
                // Create default self parm, if 'self' was not specified
                if (parmnbr == 0 && parm->namesym != selfName) {
                    parseInjectSelf(fnsig, parse->typenode->namesym);
                    ++parmnbr;
                }
                // Infer value type of a parameter (or its reference) if unspecified
                if (parm->vtype == voidType) {
                    parm->vtype = (INode*)newNameUseNode(parse->typenode->namesym);
                }
                else if (parm->vtype->tag == RefTag) {
                    PtrNode *refnode = (PtrNode *)parm->vtype;
                    if (refnode->pvtype == voidType) {
                        refnode->pvtype = (INode*)newNameUseNode(parse->typenode->namesym);
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
    if ((fnsig->rettype = parseVtype(parse))) {
        // Handle multiple return types
        if (lexIsToken(CommaToken)) {
            TTupleNode *rettype = newTTupleNode(4);
            nodesAdd(&rettype->types, fnsig->rettype);
            while (lexIsToken(CommaToken)) {
                lexNextToken();
                nodesAdd(&rettype->types, parseVtype(parse));
            }
            fnsig->rettype = (INode*)rettype;
        }
    }
    else {
        fnsig->rettype = voidType;
    }

    return (INode*)fnsig;
}

// Parse an array type
INode *parseArrayType(ParseState *parse) {
    ArrayNode *atype = newArrayNode();

    // Obtain size as an unsigned integer literal
    if (lexIsToken(IntLitToken)) {
        atype->size = (uint32_t) lex->val.uintlit;
        lexNextToken();
    }
    else
        errorMsgLex(ErrorBadTok, "Expected integer literal for array size");

    if (lexIsToken(RBracketToken))
        lexNextToken();
    else
        errorMsgLex(ErrorBadTok, "Expected closing square bracket");

    // Obtain array's element type
    if ((atype->elemtype = parseVtype(parse)) == NULL) {
        errorMsgLex(ErrorNoVtype, "Missing array element type");
        atype->elemtype = voidType;
    }

    return (INode *)atype;
}

// Parse a pointer type
INode *parsePtrType(ParseState *parse) {
    PtrNode *ptype = newPtrNode();
    lexNextToken();

    // Get value type, if provided
    if (lexIsToken(FnToken)) {
        lexNextToken();
        if (lexIsToken(IdentToken)) {
            errorMsgLex(WarnName, "Unnecessary name is ignored");
            lexNextToken();
        }
        ptype->pvtype = parseFnSig(parse);
    }
    else if ((ptype->pvtype = parseVtype(parse)) == NULL) {
        errorMsgLex(ErrorNoVtype, "Missing value type for the pointer");
        ptype->pvtype = voidType;
    }

    return (INode *)ptype;
}

// Parse a reference type
INode *parseRefType(ParseState *parse) {
    RefNode *reftype = newRefNode();
    reftype->pvtype = voidType;
    lexNextToken();

    // Handle indicator for nullable references
    if (lexIsToken(QuesToken)) {
        reftype->flags |= FlagRefNull;
        lexNextToken();
    }

    parseAllocPerm(reftype); // Get allocator and permission, if provided

    // Get value type for reference to a function
    if (lexIsToken(FnToken)) {
        lexNextToken();
        if (lexIsToken(IdentToken)) {
            errorMsgLex(WarnName, "Unnecessary name is ignored");
            lexNextToken();
        }
        reftype->pvtype = parseFnSig(parse);
    }
    // Get value type for ref to array or slice
    else if (lexIsToken(LBracketToken)) {
        reftype->tag = ArrayRefTag;
        lexNextToken();
        if (lexIsToken(RBracketToken)) {
            lexNextToken();
            reftype->pvtype = parseVtype(parse);
        }
        else
            reftype->pvtype = parseArrayType(parse);
    }
    else if ((reftype->pvtype = parseVtype(parse)) == NULL) {
        errorMsgLex(ErrorNoVtype, "Missing value type for the pointer");
    }

    return (INode *)reftype;
}

// Parse a value type signature. Return NULL if none found.
INode* parseVtype(ParseState *parse) {
    INode *vtype;
    switch (lex->toktype) {
    case AmperToken:
        return parseRefType(parse);
    case StarToken:
        return parsePtrType(parse);
    case LBracketToken:
        lexNextToken();
        return parseArrayType(parse);
    case IdentToken:
        vtype = (INode*)newNameUseNode(lex->val.ident);
        lexNextToken();
        return vtype;
    default:
        return voidType;
    }
}
