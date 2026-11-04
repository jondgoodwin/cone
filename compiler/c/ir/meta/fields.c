/** Reflection over a type's declared fields: '#each #f in #fields(T)'
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <stdio.h>

static const char *fieldPropWords[FieldPropCount] = { "name", "type", "region", "perm", "pub" };

FieldEach *newFieldEach(Name *var, INode *subject, ModuleNode *mod) {
    FieldEach *fe = (FieldEach*)memAllocBlk(sizeof(FieldEach));
    memset(fe, 0, sizeof(FieldEach));
    fe->var = var;
    fe->subject = subject;
    fe->mod = mod;
    for (int p = 0; p < FieldPropCount; ++p) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s.%s", &var->namestr, fieldPropWords[p]);
        fe->propnames[p] = nametblFind(buf, strlen(buf));
        fe->props[p] = newGVarDclNode(fe->propnames[p]);
    }
    return fe;
}

int fieldPropFromWord(const char *word) {
    for (int p = 0; p < FieldPropCount; ++p)
        if (strcmp(word, fieldPropWords[p]) == 0)
            return p;
    return strcmp(word, "attrs") == 0 ? -1 : FieldPropCount;
}

// A description's property is a use of a pseudo generic parameter: cloning for an instance puts the
// field's own answer where the use stands (cloneNode, a use of a generic parameter)
INode *fieldEachProp(FieldEach *fe, FieldProp prop, INode *lexnode) {
    NameUseNode *use = newNameUseNode(fe->propnames[prop]);
    inodeLexCopy((INode*)use, lexnode);
    use->dclnode = (INode*)fe->props[prop];
    fe->used[prop] = 1;
    return (INode*)use;
}

void fieldEachNameRes(NameResState *pstate, BlockNode *blk) {
    FieldEach *fe = blk->fieldeach;
    inodeNameRes(pstate, &fe->subject);
    INode *dcl = isNameUseNode(fe->subject) ? ((NameUseNode*)fe->subject)->dclnode : NULL;
    // 'Self' in a trait's default is the type the default is taken into, known when it is taken in
    int traitself = dcl && ((NameUseNode*)fe->subject)->namesym == selfTypeName
        && dcl->tag == StructTag && (dcl->flags & TraitType);
    if (dcl && dcl->tag != GenVarDclTag && !traitself)
        errorMsgNode(fe->subject, ErrorMetaFields,
            "'#fields(%s)' describes the fields of a generic's type parameter, or of 'Self' in a trait's default, which is known when the generic is cloned for its arguments. '%s' is not one.",
            &((NameUseNode*)fe->subject)->namesym->namestr, &((NameUseNode*)fe->subject)->namesym->namestr);
}

INode *fieldEachSplice(NameUseNode *node) {
    Name *name = node->namesym;
    if (node->dclnode != NULL || (&name->namestr)[0] != '#')
        return NULL;
    INode *hooked = name->node;
    if (hooked == NULL || hooked->tag != NameUseTag)
        return NULL;
    NameUseNode *member = newMemberUseNode(((NameUseNode*)hooked)->namesym);
    inodeLexCopy((INode*)member, (INode*)node);
    return (INode*)member;
}

static INode *fieldPropAnswer(FieldEach *fe, int prop, FieldDclNode *field, BlockNode *at, int ispub) {
    switch (prop) {
    case FieldPropName: {
        size_t len = strlen(&field->namesym->namestr);
        char *text = (char*)memAllocBlk(len + 1);
        memcpy(text, &field->namesym->namestr, len + 1);
        SLitNode *lit = newSLitNode(text, (uint32_t)len);
        inodeLexCopy((INode*)lit, (INode*)at);
        return (INode*)lit;
    }
    case FieldPropType:
        return field->vtype;
    case FieldPropPub:
    {
        // Made after name resolution, so its type is bound already
        ULitNode *lit = newULitNodeTC(ispub ? 1 : 0, (INode*)boolType);
        inodeLexCopy((INode*)lit, (INode*)at);
        return (INode*)lit;
    }
    case FieldPropPerm:
        if (field->perm == NULL)
            errorMsgNode((INode*)at, ErrorMetaFieldsType, "The field '%s' has no written permission for '%s.perm' to describe.",
                &field->namesym->namestr, &fe->var->namestr);
        return field->perm;
    default: {
        INode *vt = field->vtype;
        if (vt && vt->tag == RefTag && ((RefNode*)vt)->region)
            return ((RefNode*)vt)->region;
        errorMsgNode((INode*)at, ErrorMetaFieldsType, "The field '%s' is not a reference with a region written, which '%s.region' describes.",
            &field->namesym->namestr, &fe->var->namestr);
        return NULL;
    }
    }
}

INode *cloneFieldEach(CloneState *cstate, BlockNode *node) {
    FieldEach *fe = node->fieldeach;
    INode *subj = cloneNode(cstate, fe->subject);
    uint32_t dclpos = cloneDclPush();
    INode *oldhook[FieldPropCount];
    INode *oldvar = fe->var->node;
    for (int p = 0; p < FieldPropCount; ++p)
        oldhook[p] = fe->propnames[p]->node;

    int undecided = subj == NULL
        || (isNameUseNode(subj) && ((NameUseNode*)subj)->dclnode && ((NameUseNode*)subj)->dclnode->tag == GenVarDclTag);
    if (undecided) {
        // Still the parameter of an enclosing generic: copied whole, to be decided for its instances
        for (int p = 0; p < FieldPropCount; ++p)
            fe->propnames[p]->node = (INode*)fe->props[p];
        BlockNode *copy = memAllocBlk(sizeof(BlockNode));
        memcpy(copy, node, sizeof(BlockNode));
        cloneDclSetMap((INode*)node, (INode*)copy);
        FieldEach *nfe = memAllocBlk(sizeof(FieldEach));
        memcpy(nfe, fe, sizeof(FieldEach));
        nfe->subject = subj ? subj : fe->subject;
        copy->fieldeach = nfe;
        copy->stmts = cloneNodes(cstate, node->stmts);
        for (int p = 0; p < FieldPropCount; ++p)
            fe->propnames[p]->node = oldhook[p];
        cloneDclPop(dclpos);
        return (INode*)copy;
    }

    BlockNode *result = memAllocBlk(sizeof(BlockNode));
    memcpy(result, node, sizeof(BlockNode));
    result->fieldeach = NULL;
    result->stmts = newNodes(4);
    cloneDclSetMap((INode*)node, (INode*)result);

    INode *dcl = isTypeNode(subj) ? itypeGetTypeDcl(subj) : NULL;
    if (dcl == NULL || dcl->tag != StructTag || (dcl->flags & (TraitType | EnumType))) {
        errorMsgNode((INode*)node, ErrorMetaFieldsType,
            "'#fields(%s)' describes the fields of a struct. This '%s' is not one: it is a primitive, a reference, an enum, a trait or another kind of type.",
            &((NameUseNode*)fe->subject)->namesym->namestr, &((NameUseNode*)fe->subject)->namesym->namestr);
        cloneDclPop(dclpos);
        return (INode*)result;
    }
    StructNode *st = (StructNode*)dcl;

    // Reflection sees what the reflecting code could name: its own module's private fields (the rule
    // of a private member, structSeesPrivate), or the type's own methods, or all of them where the
    // type has opted in with '@reflect'
    int sees = st->reflect || fe->mod == dclInfoGetModule(dcl)
        || (cstate->selftype && isTypeNode(cstate->selftype) && itypeGetTypeDcl(cstate->selftype) == dcl);

    Nodes *fields = newNodes(8);
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&st->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        if (field->tag != FieldDclTag || field->hop || field->via)
            continue;
        if (!sees && inodeIsPrivate((INode*)field)) {
            // Leaving it out would give a serializer or an 'equals' a wrong answer with no warning
            errorMsgNode((INode*)node, ErrorMetaFieldsPrivate,
                "'#fields(%s)' cannot describe the private field '%s' of '%s', which this code cannot see. The module that declares '%s' can open it to reflection by writing 'struct @reflect %s'.",
                &((NameUseNode*)fe->subject)->namesym->namestr, &field->namesym->namestr, &st->namesym->namestr,
                &st->namesym->namestr, &st->namesym->namestr);
            cloneDclPop(dclpos);
            return (INode*)result;
        }
        nodesAdd(&fields, (INode*)field);
    }

    Name *name = fe->var;
    for (uint32_t i = 0; i < fields->used; ++i) {
        FieldDclNode *field = (FieldDclNode*)nodesGet(fields, i);
        int errs = errors;
        for (int p = 0; p < FieldPropCount; ++p) {
            // Only what the body asks is looked for: a field with no region is no error unless '.region' is
            // written, and the placeholder carries which were written
            fe->propnames[p]->node = (INode*)fe->props[p];     // Own, until set below
        }
        for (int p = 0; p < FieldPropCount; ++p) {
            if (!fe->used[p])
                continue;
            INode *answer = fieldPropAnswer(fe, p, field, node, !inodeIsPrivate((INode*)field));
            fe->propnames[p]->node = answer ? answer : (INode*)fe->props[p];
        }
        NameUseNode *member = newMemberUseNode(field->namesym);
        fe->var->node = (INode*)member;
        if (errors != errs)
            break;
        EachPass *ep = (EachPass*)memAllocBlk(sizeof(EachPass));
        ep->outer = errorEachPass;
        ep->loop = (INode*)node;
        ep->pass = i + 1;
        ep->npasses = fields->used;
        ep->var = &name->namestr;
        ep->item = &field->namesym->namestr;
        INode *pass = cloneNode(cstate, nodesGet(node->stmts, 0));
        if (pass && pass->tag == BlockTag) {
            ((BlockNode*)pass)->eachpass = ep;
            nodesAdd(&result->stmts, pass);
        }
    }
    for (int p = 0; p < FieldPropCount; ++p)
        fe->propnames[p]->node = oldhook[p];
    fe->var->node = oldvar;
    cloneDclPop(dclpos);
    return (INode*)result;
}
