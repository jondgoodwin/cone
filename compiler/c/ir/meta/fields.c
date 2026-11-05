/** Reflection over a type's declared fields: '#each #f in #fields(T)'
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <stdio.h>

static const char *fieldPropWords[FieldPropCount] = { "name", "type", "region", "perm", "pub", "offset" };

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

void fieldEachSubjectNameRes(NameResState *pstate, FieldEach *fe) {
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

void fieldEachNameRes(NameResState *pstate, BlockNode *blk) {
    fieldEachSubjectNameRes(pstate, blk->fieldeach);
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

Name *genNameFor(GenName *gen, FieldDclNode *field) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s%s%s", gen->prefix ? gen->prefix : "", &field->namesym->namestr, gen->suffix ? gen->suffix : "");
    return nametblFind(buf, strlen(buf));
}

static INode *fieldPropAnswer(FieldEach *fe, int prop, FieldDclNode *field, INode *at, int ispub, INode *subj) {
    switch (prop) {
    case FieldPropOffset: {
        // Only once T's layout has settled: asked inside it, the answer would depend on the order of declaration
        SizeofNode *off = newSizeofNode();
        inodeLexCopy((INode*)off, (INode*)at);
        off->what = SizeofOffset;
        off->type = subj;
        off->field = field;
        return (INode*)off;
    }
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

void fieldEachKeepBegin(FieldEachRun *run) {
    for (int p = 0; p < FieldPropCount; ++p)
        run->fe->propnames[p]->node = (INode*)run->fe->props[p];
}

GenName *fieldEachKeptGen(FieldEachRun *run, GenName *gen) {
    if (run->kept == NULL) {
        run->kept = (FieldEach*)memAllocBlk(sizeof(FieldEach));
        memcpy(run->kept, run->fe, sizeof(FieldEach));
        run->kept->subject = run->subj ? run->subj : run->fe->subject;
    }
    GenName *copy = (GenName*)memAllocBlk(sizeof(GenName));
    memcpy(copy, gen, sizeof(GenName));
    copy->fe = run->kept;
    return copy;
}

FieldEachRun *fieldEachOpen(CloneState *cstate, FieldEach *fe, INode *errnode) {
    FieldEachRun *run = (FieldEachRun*)memAllocBlk(sizeof(FieldEachRun));
    memset(run, 0, sizeof(FieldEachRun));
    run->fe = fe;
    run->oldvar = fe->var->node;
    for (int p = 0; p < FieldPropCount; ++p)
        run->oldhook[p] = fe->propnames[p]->node;
    INode *subj = cloneNode(cstate, fe->subject);
    run->subj = subj;
    run->undecided = subj == NULL
        || (isNameUseNode(subj) && ((NameUseNode*)subj)->dclnode && ((NameUseNode*)subj)->dclnode->tag == GenVarDclTag);
    if (run->undecided)
        return run;

    INode *dcl = isTypeNode(subj) ? itypeGetTypeDcl(subj) : NULL;
    if (dcl == NULL || dcl->tag != StructTag || (dcl->flags & (TraitType | EnumType))) {
        errorMsgNode(errnode, ErrorMetaFieldsType,
            "'#fields(%s)' describes the fields of a struct. This '%s' is not one: it is a primitive, a reference, an enum, a trait or another kind of type.",
            &((NameUseNode*)fe->subject)->namesym->namestr, &((NameUseNode*)fe->subject)->namesym->namestr);
        run->failed = 1;
        return run;
    }
    StructNode *st = (StructNode*)dcl;

    // Reflection sees what the reflecting code could name: its own module's private fields (the rule
    // of a private member, structSeesPrivate), or the type's own methods, or all of them where the
    // type has opted in with '@reflect'
    int sees = st->reflect || fe->mod == dclInfoGetModule(dcl)
        || (cstate->selftype && isTypeNode(cstate->selftype) && itypeGetTypeDcl(cstate->selftype) == dcl);

    run->fields = newNodes(8);
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&st->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        if (field->tag != FieldDclTag || field->hop || field->via)
            continue;
        if (!sees && inodeIsPrivate((INode*)field)) {
            // Leaving it out would give a serializer or an 'equals' a wrong answer with no warning
            errorMsgNode(errnode, ErrorMetaFieldsPrivate,
                "'#fields(%s)' cannot describe the private field '%s' of '%s', which this code cannot see. The module that declares '%s' can open it to reflection by writing 'struct @reflect %s'.",
                &((NameUseNode*)fe->subject)->namesym->namestr, &field->namesym->namestr, &st->namesym->namestr,
                &st->namesym->namestr, &st->namesym->namestr);
            run->failed = 1;
            return run;
        }
        nodesAdd(&run->fields, (INode*)field);
    }
    return run;
}

int fieldEachPassBegin(FieldEachRun *run, uint32_t i, INode *at) {
    FieldEach *fe = run->fe;
    FieldDclNode *field = (FieldDclNode*)nodesGet(run->fields, i);
    // Only what the body asks is looked for: a field with no region is no error unless '.region' is
    // written, and the placeholder carries which were written
    for (int p = 0; p < FieldPropCount; ++p)
        fe->propnames[p]->node = (INode*)fe->props[p];     // Own, until set below
    int errs = errors;
    for (int p = 0; p < FieldPropCount; ++p) {
        if (!fe->used[p])
            continue;
        if (p == FieldPropOffset && sizeofLayoutRefused(at, run->subj, "#f.offset"))
            break;
        INode *answer = fieldPropAnswer(fe, p, field, at, !inodeIsPrivate((INode*)field), run->subj);
        fe->propnames[p]->node = answer ? answer : (INode*)fe->props[p];
    }
    fe->var->node = (INode*)newMemberUseNode(field->namesym);
    return errors == errs;
}

void fieldEachPassEnd(FieldEachRun *run) {
    for (int p = 0; p < FieldPropCount; ++p)
        run->fe->propnames[p]->node = run->oldhook[p];
    run->fe->var->node = run->oldvar;
}

EachPass *fieldEachPassFor(FieldEachRun *run, uint32_t i, INode *loop) {
    FieldDclNode *field = (FieldDclNode*)nodesGet(run->fields, i);
    EachPass *ep = (EachPass*)memAllocBlk(sizeof(EachPass));
    ep->outer = errorEachPass;
    ep->loop = loop;
    ep->pass = i + 1;
    ep->npasses = run->fields->used;
    ep->var = &run->fe->var->namestr;
    ep->item = &field->namesym->namestr;
    return ep;
}

// Both forms of a loop: a statement's (argout NULL: the result is one block of the passes) and a
// call's argument list (the passes' named arguments, and an undecided loop's copy, are added to *argout)
static INode *fieldEachClone(CloneState *cstate, BlockNode *node, Nodes **argout) {
    FieldEach *fe = node->fieldeach;
    uint32_t dclpos = cloneDclPush();
    FieldEachRun *run = fieldEachOpen(cstate, fe, (INode*)node);

    if (run->undecided) {
        // Still the parameter of an enclosing generic: copied whole, to be decided for its instances
        for (int p = 0; p < FieldPropCount; ++p)
            fe->propnames[p]->node = (INode*)fe->props[p];
        BlockNode *copy = memAllocBlk(sizeof(BlockNode));
        memcpy(copy, node, sizeof(BlockNode));
        cloneDclSetMap((INode*)node, (INode*)copy);
        FieldEach *nfe = memAllocBlk(sizeof(FieldEach));
        memcpy(nfe, fe, sizeof(FieldEach));
        nfe->subject = run->subj ? run->subj : fe->subject;
        copy->fieldeach = nfe;
        copy->stmts = cloneNodes(cstate, node->stmts);
        fieldEachPassEnd(run);
        cloneDclPop(dclpos);
        if (argout) {
            nodesAdd(argout, (INode*)copy);
            return NULL;
        }
        return (INode*)copy;
    }

    BlockNode *result = memAllocBlk(sizeof(BlockNode));
    memcpy(result, node, sizeof(BlockNode));
    result->fieldeach = NULL;
    result->stmts = newNodes(4);
    cloneDclSetMap((INode*)node, (INode*)result);
    if (run->failed) {
        cloneDclPop(dclpos);
        return (INode*)result;
    }

    for (uint32_t i = 0; i < run->fields->used; ++i) {
        if (!fieldEachPassBegin(run, i, (INode*)node))
            break;
        EachPass *ep = fieldEachPassFor(run, i, (INode*)node);
        if (argout) {
            // One named argument per pass, the field's name given by '#f' in the template
            EachPass *svpass = errorEachPass;
            errorEachPass = ep;
            INode *arg = cloneNode(cstate, nodesGet(node->stmts, 0));
            errorEachPass = svpass;
            if (arg)
                nodesAdd(argout, arg);
            continue;
        }
        INode *pass = cloneNode(cstate, nodesGet(node->stmts, 0));
        if (pass && pass->tag == BlockTag) {
            ((BlockNode*)pass)->eachpass = ep;
            nodesAdd(&result->stmts, pass);
        }
    }
    fieldEachPassEnd(run);
    cloneDclPop(dclpos);
    return argout ? NULL : (INode*)result;
}

INode *cloneFieldEach(CloneState *cstate, BlockNode *node) {
    return fieldEachClone(cstate, node, NULL);
}

Nodes *cloneFieldEachArgs(CloneState *cstate, Nodes *args) {
    Nodes *out = newNodes(args->avail);
    INode **argp;
    uint32_t cnt;
    for (nodesFor(args, cnt, argp)) {
        if ((*argp)->tag == BlockTag && ((BlockNode*)*argp)->fieldeach)
            fieldEachClone(cstate, (BlockNode*)*argp, &out);
        else
            nodesAdd(&out, cloneNode(cstate, *argp));
    }
    return out;
}
