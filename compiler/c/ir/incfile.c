/** Generating a package's include file
 *
 * The include file is the root module's own source, edited: the generator
 * never prints a declaration from the IR, which by now has been desugared and
 * lowered, but copies the author's text and decides, declaration by
 * declaration, whether it goes in whole, goes in cut to an 'extern'
 * declaration, or stays out. The IR decides which; the spans the parser
 * recorded (dclspan.h) say where each piece of text is.
 *
 * - A declaration whose body an importer expands -- inline, generic, a macro,
 *   a trait's default, a generic type -- goes in whole.
 * - One the package's object defines and exports -- dclIsExported, the rule
 *   generation follows too -- goes in as 'extern', its body or value left out.
 * - A type goes in with every field, and its members by the same two rules.
 * - The 'mod' line, the imports and the declarations with no symbol (typedef,
 *   const, macro, module trait) go in as written.
 * - Everything else stays out, with the comments directly above it.
 *
 * Every comment in the text kept is kept [Jon 25 Sep, Q2].
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"
#include "incfile.h"
#include "../shared/fileio.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// A growable text
typedef struct IncBuf {
    char *text;
    size_t len;
    size_t avail;
} IncBuf;

static void incBufPutn(IncBuf *buf, const char *text, size_t n) {
    if (buf->len + n + 1 > buf->avail) {
        size_t avail = buf->avail ? buf->avail : 1024;
        while (avail < buf->len + n + 1)
            avail *= 2;
        char *grown = (char*)memAllocBlk(avail);
        if (buf->len)
            memcpy(grown, buf->text, buf->len);
        buf->text = grown;
        buf->avail = avail;
    }
    memcpy(buf->text + buf->len, text, n);
    buf->len += n;
    buf->text[buf->len] = '\0';
}

static void incBufPuts(IncBuf *buf, const char *text) {
    incBufPutn(buf, text, strlen(text));
}

// One change to one file's text: [from, to) is replaced by 'text', or removed
// where 'text' is NULL; an insertion has 'from' and 'to' the same
typedef struct IncEdit {
    Lexer *lexer;
    char *from;
    char *to;
    char *text;
    uint32_t seq;
} IncEdit;

typedef struct IncGen {
    ModuleNode *root;
    ModuleNode *core;       // Its types are written bare: every module folds core in
    IncEdit *edits;
    uint32_t nedits, availedits;
    StructNode **needed;    // The root's types an included declaration names
    uint32_t nneeded, availneeded;
    INode **refused;        // Each refusal reported, as a pair: where, and what,
    uint32_t nrefused, availrefused;    // so that each is reported once
    INode *from;            // The included declaration whose types are being walked
    int probing;            // Walking only to learn whether a type reaches a submodule
    int probehit;
    int failed;
} IncGen;

static void incEdit(IncGen *g, Lexer *lexer, char *from, char *to, char *text) {
    if (g->nedits == g->availedits) {
        g->availedits = g->availedits ? g->availedits * 2 : 64;
        IncEdit *edits = (IncEdit*)memAllocBlk(g->availedits * sizeof(IncEdit));
        if (g->nedits)
            memcpy(edits, g->edits, g->nedits * sizeof(IncEdit));
        g->edits = edits;
    }
    IncEdit *edit = &g->edits[g->nedits];
    edit->lexer = lexer;
    edit->from = from;
    edit->to = to;
    edit->text = text;
    edit->seq = g->nedits++;
}

// ---- Reading the text around a declaration ----------------------------------

static int incIsBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// Past a block comment's end, from just inside its '/*': they nest
static char *incSkipBlockComment(char *p, char *limit) {
    int nest = 1;
    while (p < limit && *p && nest) {
        if (p[0] == '*' && p[1] == '/') {
            --nest;
            p += 2;
        }
        else if (p[0] == '/' && p[1] == '*') {
            ++nest;
            p += 2;
        }
        else
            ++p;
    }
    return p;
}

// Where the comments directly above a declaration begin: those after the last
// blank line, each starting a line of its own. 'floor' is where the text that
// could hold them starts -- the end of the statement before, or of a type's
// header -- and 'start' is the declaration's first token. Returns 'start' where
// no comment is attached to it
static char *incLeadingComment(char *filestart, char *floor, char *start) {
    char *cand = NULL;
    int atline = 1;     // only whitespace since the last line end
    for (char *q = floor; q > filestart; --q) {
        if (q[-1] == '\n')
            break;
        if (!incIsBlank(q[-1])) {
            atline = 0;
            break;
        }
    }
    char *p = floor;
    while (p < start) {
        char c = *p;
        if (c == '\n') {
            if (atline)
                cand = NULL;    // a blank line parts what is above it from the declaration
            atline = 1;
            ++p;
        }
        else if (incIsBlank(c))
            ++p;
        else if (c == '/' && (p[1] == '/' || p[1] == '*')) {
            // A comment after code on its line belongs to that code
            if (!atline)
                cand = NULL;
            else if (cand == NULL)
                cand = p;
            if (p[1] == '/') {
                while (p < start && *p != '\n')
                    ++p;
            }
            else
                p = incSkipBlockComment(p + 2, start);
            atline = 0;
        }
        else {
            // Code: a type's header, or a statement sharing the line
            cand = NULL;
            atline = 0;
            if (c == '"') {
                ++p;
                while (p < start && *p != '"') {
                    if (*p == '\\' && p + 1 < start)
                        ++p;
                    ++p;
                }
            }
            ++p;
        }
    }
    return cand ? cand : start;
}

// Leave a declaration out: its text, the comments directly above it, and a
// comment after it on its last line. Where it has its lines to itself, the
// whole lines go, and one blank line after them where one is above them too, so
// that what is left keeps its spacing
static void incDelete(IncGen *g, DclSpan *span, char *floor) {
    char *filestart = span->lexer->source;
    char *from = incLeadingComment(filestart, floor, span->start);
    char *to = span->end;

    char *linefrom = from;
    while (linefrom > floor && incIsBlank(linefrom[-1]))
        --linefrom;
    int ownsline = linefrom == filestart || linefrom[-1] == '\n';
    char *lineto = to;
    while (incIsBlank(*lineto))
        ++lineto;
    if (lineto[0] == '/' && lineto[1] == '/') {
        while (*lineto && *lineto != '\n')
            ++lineto;
    }
    if (ownsline && (*lineto == '\n' || *lineto == '\0')) {
        from = linefrom;
        to = *lineto == '\n' ? lineto + 1 : lineto;
        // The blank line above, where there is one: [above, from)
        char *above = NULL;
        if (from > filestart) {
            char *q = from - 1;
            while (q > filestart && incIsBlank(q[-1]))
                --q;
            if (q == filestart || q[-1] == '\n')
                above = q;
        }
        char *next = to;
        while (incIsBlank(*next))
            ++next;
        if (above && *next == '\n')
            to = next + 1;
        // Nor a blank line left above the brace closing a type
        else if (above && *next == '}')
            from = above;
    }
    // Sharing its line with other code, it goes with the space before it
    else
        from = linefrom;
    incEdit(g, span->lexer, from, to, NULL);
}

// ---- Writing an inferred type -----------------------------------------------

// Write a type as Cone spells it, for a global whose type was inferred from its
// value [Jon 25 Sep, Q6]. A global's value is a literal, so its type is a
// number, a struct (an instance of a generic one included) or an array of
// either. Returns 0 for a type this does not spell
static int incTypeText(IncGen *g, IncBuf *buf, INode *type) {
    if (type == NULL)
        return 0;
    if (isNameUseNode(type)) {
        INode *dcl = nameUseGetDcl((NameUseNode*)type);
        return dcl ? incTypeText(g, buf, dcl) : 0;
    }
    switch (type->tag) {
    case IntNbrTag:
    case UintNbrTag:
    case FloatNbrTag:
        incBufPuts(buf, &((NbrNode*)type)->namesym->namestr);
        return 1;
    case StructTag: {
        StructNode *strnode = (StructNode*)type;
        ModuleNode *mod = dclInfoGetModule(type);
        if (mod == NULL || mod->generic != NULL)
            return 0;
        // Another package's type is reached through its module's name, which
        // its import binds; core's are folded into every module
        if (mod != g->root && mod != g->core) {
            if (mod->dclinfo.owner != NULL)
                return 0;
            incBufPuts(buf, &mod->namesym->namestr);
            incBufPuts(buf, ".");
        }
        // A variant is reached through its enum
        INode *owner = strnode->dclinfo.owner;
        if (owner && owner->tag == StructTag) {
            incBufPuts(buf, &((StructNode*)owner)->namesym->namestr);
            incBufPuts(buf, ".");
        }
        incBufPuts(buf, &strnode->namesym->namestr);
        Nodes *args = itypeInstanceTypeArgs(type);
        if (args) {
            incBufPuts(buf, "[");
            INode **nodesp;
            uint32_t cnt;
            int first = 1;
            for (nodesFor(args, cnt, nodesp)) {
                if (!first)
                    incBufPuts(buf, ", ");
                first = 0;
                if (!incTypeText(g, buf, *nodesp))
                    return 0;
            }
            incBufPuts(buf, "]");
        }
        return 1;
    }
    case ArrayTag: {
        ArrayNode *array = (ArrayNode*)type;
        if (array->dimens->used != 1 || array->elems->used != 1)
            return 0;
        INode *dim = nodesGet(array->dimens, 0);
        if (dim->tag != ULitTag)
            return 0;
        char dimtext[32];
        snprintf(dimtext, sizeof(dimtext), "[%llu; ", (unsigned long long)((ULitNode*)dim)->uintlit);
        incBufPuts(buf, dimtext);
        if (!incTypeText(g, buf, nodesGet(array->elems, 0)))
            return 0;
        incBufPuts(buf, "]");
        return 1;
    }
    default:
        return 0;
    }
}

// ---- What the include file needs --------------------------------------------

// Is this module one of the root's submodules, at any depth?
static int incInRootTree(IncGen *g, ModuleNode *mod) {
    while (mod && mod->dclinfo.owner) {
        mod = dclInfoGetModule(mod->dclinfo.owner);
        if (mod == g->root)
            return 1;
    }
    return 0;
}

// A module as a path names it from the root: 'q.sub'
static void incModulePath(IncBuf *buf, ModuleNode *mod) {
    if (mod->dclinfo.owner && mod->dclinfo.owner->tag == ModuleTag) {
        incModulePath(buf, (ModuleNode*)mod->dclinfo.owner);
        incBufPuts(buf, ".");
    }
    incBufPuts(buf, mod->namesym ? &mod->namesym->namestr : "?");
}

static char *incName(INode *node) {
    Name *name = inodeGetName(node);
    return name ? &name->namestr : "?";
}

// Report something of a submodule that the include file would have to declare,
// once. A generated include file cannot declare a submodule's names yet [Jon 25
// Sep, Q1: a private, pruned nested module is the ruling, and the next step]
static void incRefuse(IncGen *g, INode *at, INode *dcl, char *why) {
    for (uint32_t i = 0; i < g->nrefused; i += 2) {
        if (g->refused[i] == at && g->refused[i + 1] == dcl)
            return;
    }
    if (g->nrefused + 2 > g->availrefused) {
        g->availrefused = g->availrefused ? g->availrefused * 2 : 16;
        INode **refused = (INode**)memAllocBlk(g->availrefused * sizeof(INode*));
        if (g->nrefused)
            memcpy(refused, g->refused, g->nrefused * sizeof(INode*));
        g->refused = refused;
    }
    g->refused[g->nrefused++] = at;
    g->refused[g->nrefused++] = dcl;
    g->failed = 1;
    IncBuf path;
    memset(&path, 0, sizeof(path));
    incModulePath(&path, dclInfoGetModule(dcl));
    if (dcl->tag == ModuleTag) {
        errorMsgNode(at, ErrorIncSubmodule,
            "Package %s's include file would have to declare the names submodule %s holds, since %s. A generated include file cannot declare a submodule's names yet: keep this 'use' private, or move what it re-exports into module %s.",
            &g->root->namesym->namestr, path.text, why, &g->root->namesym->namestr);
        return;
    }
    errorMsgNode(at, ErrorIncSubmodule,
        "Package %s's include file would have to declare %s, which submodule %s holds, since %s. A generated include file cannot declare a submodule's names yet: move %s into module %s, or keep it out of what %s shows its importers.",
        &g->root->namesym->namestr, incName(dcl), path.text, why,
        incName(dcl), &g->root->namesym->namestr, &g->root->namesym->namestr);
}

static int incIsNeeded(IncGen *g, StructNode *strnode) {
    for (uint32_t i = 0; i < g->nneeded; ++i) {
        if (g->needed[i] == strnode)
            return 1;
    }
    return 0;
}

static void incWalkType(IncGen *g, INode *type);
static void incNeed(IncGen *g, StructNode *strnode);

// A type an included declaration names. One of the root's goes in, with what
// its own fields and signatures name; one of a submodule's is refused; one of
// another package's is that package's include file's to declare
static void incReachStruct(IncGen *g, StructNode *strnode) {
    // A variant is declared by its enum, and an instance by its generic
    StructNode *top = strnode;
    while (top->dclinfo.owner && top->dclinfo.owner->tag == StructTag)
        top = (StructNode*)top->dclinfo.owner;
    Nodes *args = itypeInstanceTypeArgs((INode*)top);
    if (args) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(args, cnt, nodesp))
            incWalkType(g, *nodesp);
        ModuleNode *mod = dclInfoGetModule((INode*)top);
        INode *generic = mod ? namespaceFind(&mod->namespace, top->namesym) : NULL;
        if (generic == NULL || generic->tag != StructTag)
            return;
        top = (StructNode*)generic;
    }
    ModuleNode *mod = dclInfoGetModule((INode*)top);
    if (mod == NULL)
        return;
    if (mod == g->root) {
        if (!g->probing)
            incNeed(g, top);
    }
    else if (incInRootTree(g, mod)) {
        if (g->probing)
            g->probehit = 1;
        else if (g->from->tag == ModUseTag)
            incRefuse(g, g->from, (INode*)top, "'pub use' makes what it names public names of the package");
        else {
            char why[256];
            snprintf(why, sizeof(why), "%s, which the include file declares, names it in its signature, type or fields", incName(g->from));
            incRefuse(g, g->from, (INode*)top, why);
        }
    }
}

// Walk a type expression for the types it names
static void incWalkType(IncGen *g, INode *type) {
    if (type == NULL)
        return;
    if (isNameUseNode(type)) {
        INode *dcl = nameUseGetDcl((NameUseNode*)type);
        if (dcl)
            incWalkType(g, dcl);
        return;
    }
    INode **nodesp;
    uint32_t cnt;
    switch (type->tag) {
    case StructTag:
        incReachStruct(g, (StructNode*)type);
        break;
    case AliasDclTag:
        if (type->flags & FlagTypeAlias)
            incWalkType(g, ((AliasDclNode*)type)->target);
        break;
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        incWalkType(g, ((RefNode*)type)->vtexp);
        break;
    case PtrTag:
        incWalkType(g, ((StarNode*)type)->vtexp);
        break;
    case ArrayTag:
        for (nodesFor(((ArrayNode*)type)->elems, cnt, nodesp)) {
            if (isTypeNode(*nodesp))
                incWalkType(g, *nodesp);
        }
        break;
    case TTupleTag:
        for (nodesFor(((TupleNode*)type)->elems, cnt, nodesp))
            incWalkType(g, *nodesp);
        break;
    case FnSigTag:
        for (nodesFor(((FnSigNode*)type)->parms, cnt, nodesp))
            incWalkType(g, ((VarDclNode*)*nodesp)->vtype);
        incWalkType(g, ((FnSigNode*)type)->rettype);
        break;
    case FnCallTag:
        // A generic type's instance, named and not yet lowered
        incWalkType(g, ((FnCallNode*)type)->objfn);
        if (((FnCallNode*)type)->args) {
            for (nodesFor(((FnCallNode*)type)->args, cnt, nodesp))
                incWalkType(g, *nodesp);
        }
        break;
    default:
        break;
    }
}

// Whether a member function of an included type goes in: whole where its body
// travels, and as 'extern' where the object exports it or it is the type's
// 'final' or 'clone', which say what a value of the type does when dropped or
// copied wherever it is
static int incMemberFnWanted(IncGen *g, StructNode *type, INode *fn) {
    return fnDclIsExpanded((FnDclNode*)fn, (INode*)type)
        || fnIsTypeLifecycle(fn) || dclIsExported(g->root, fn);
}

static int incMemberVarWanted(IncGen *g, StructNode *type, INode *var) {
    return type->genericinfo != NULL || dclIsExported(g->root, var);
}

// Walk what an included type names: its bases, its fields, the signatures of
// the members that go in with it, and an enum's variants
static void incWalkStruct(IncGen *g, StructNode *strnode) {
    INode *svfrom = g->from;
    g->from = (INode*)strnode;
    incWalkType(g, strnode->basetrait);
    incWalkType(g, strnode->extendsbase);
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        if (!((*nodesp)->flags & IsTagField))
            incWalkType(g, ((FieldDclNode*)*nodesp)->vtype);
    }
    if (strnode->siblings) {
        for (nodesFor(strnode->siblings, cnt, nodesp))
            incWalkType(g, ((FieldDclNode*)*nodesp)->vtype);
    }
    for (nodelistFor(&strnode->nodelist, cnt, nodesp)) {
        INode *member = *nodesp;
        if (member->tag == FnDclTag && incMemberFnWanted(g, strnode, member))
            incWalkType(g, ((FnDclNode*)member)->vtype);
        else if (member->tag == VarDclTag && incMemberVarWanted(g, strnode, member))
            incWalkType(g, ((VarDclNode*)member)->vtype);
    }
    if ((strnode->flags & EnumType) && strnode->derived) {
        for (nodesFor(strnode->derived, cnt, nodesp))
            incWalkStruct(g, (StructNode*)*nodesp);
    }
    g->from = svfrom;
}

static void incNeed(IncGen *g, StructNode *strnode) {
    if (incIsNeeded(g, strnode))
        return;
    if (g->nneeded == g->availneeded) {
        g->availneeded = g->availneeded ? g->availneeded * 2 : 16;
        StructNode **needed = (StructNode**)memAllocBlk(g->availneeded * sizeof(StructNode*));
        if (g->nneeded)
            memcpy(needed, g->needed, g->nneeded * sizeof(StructNode*));
        g->needed = needed;
    }
    g->needed[g->nneeded++] = strnode;
    incWalkStruct(g, strnode);
}

// Whether a module-level global goes in: it is exported, or it is one the
// module's finalizer drops, which an importer must see to derive that the
// package's finalizer is its 'drop' (module.md, "Init and final")
static int incGlobalWanted(IncGen *g, VarDclNode *var) {
    return dclIsExported(g->root, (INode*)var)
        || (!(var->dclinfo.facts & DclCName) && itypeGetDropFnDcl(var->vtype) != NULL);
}

// Whether a module-level type goes in of its own: it is public, or a body an
// importer expands names it or one of its variants
static int incTypeReached(StructNode *strnode) {
    if (!(strnode->dclinfo.facts & DclPrivate) || (strnode->dclinfo.facts & DclExpandReached))
        return 1;
    if ((strnode->flags & EnumType) && strnode->derived) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(strnode->derived, cnt, nodesp)) {
            if (((StructNode*)*nodesp)->dclinfo.facts & DclExpandReached)
                return 1;
        }
    }
    return 0;
}

// The enum or submodule a standalone 'use' names, as a declaration
static INode *incUseSource(ModUseNode *use) {
    INode *source = use->source;
    if (source && isNameUseNode(source)) {
        INode *dcl = nameUseGetDcl((NameUseNode*)source);
        if (dcl)
            return aliasDclResolve(dcl);
    }
    return NULL;
}

// The declarations of one statement of the root: an extern block's items, or
// the statement's own
static void incSeedDcl(IncGen *g, DclSpan *span) {
    INode *node = span->node;
    if (node == NULL)
        return;
    g->from = node;
    switch (node->tag) {
    case FnDclTag:
        if (dclIsExported(g->root, node))
            incWalkType(g, ((FnDclNode*)node)->vtype);
        break;
    case VarDclTag:
        if (incGlobalWanted(g, (VarDclNode*)node))
            incWalkType(g, ((VarDclNode*)node)->vtype);
        break;
    case StructTag:
        if (incTypeReached((StructNode*)node))
            incNeed(g, (StructNode*)node);
        break;
    default:
        break;
    }
}

// Work out which of the root's types go in: the ones reached of their own, and
// every one an included declaration names, transitively. What names a
// submodule's is refused here
static void incSelect(IncGen *g) {
    DclSpans *spans = g->root->spans;
    for (uint32_t i = 0; spans && i < spans->count; ++i) {
        DclSpan *span = spans->items[i];
        if (span->kind == SpanExternBlock) {
            for (uint32_t j = 0; span->members && j < span->members->count; ++j)
                incSeedDcl(g, span->members->items[j]);
        }
        else if (span->kind == SpanDcl)
            incSeedDcl(g, span);
        else if (span->kind == SpanUse) {
            // 'pub use' makes an enum's variants public names of the root,
            // which an importer can reach, so the enum goes in
            ModUseNode *use = (ModUseNode*)span->node;
            INode *source = incUseSource(use);
            if (use->fold->ispub && use->modfold == NULL && source && source->tag == StructTag) {
                g->from = (INode*)use;
                incReachStruct(g, (StructNode*)source);
            }
        }
    }
}

// ---- Editing the root's text ------------------------------------------------

// Declare a definition 'extern' in place of its body or value: 'extern' written
// in before its keyword, and its body -- a function's from its '{', a global's
// value from its '=' -- left out, with what follows the value (a fold clause)
// kept. A global whose type was inferred gets its type written in [Jon 25 Sep, Q6]
static void incCut(IncGen *g, DclSpan *span) {
    INode *node = span->node;
    Lexer *lexer = span->lexer;
    if (node->tag == VarDclTag && (node->flags & FlagStatic)) {
        // A module's 'static' global is its one copy, which is what any global
        // is until generic modules tell the two apart; 'extern' takes its place
        char *after = span->kw + strlen("static");
        while (incIsBlank(*after))
            ++after;
        incEdit(g, lexer, span->kw, after, "extern ");
    }
    else
        incEdit(g, lexer, span->kw, span->kw, "extern ");
    if (node->tag == VarDclTag && !span->typed) {
        IncBuf type;
        memset(&type, 0, sizeof(type));
        incBufPuts(&type, " ");
        if (incTypeText(g, &type, ((VarDclNode*)node)->vtype))
            incEdit(g, lexer, span->nameend, span->nameend, type.text);
        else {
            errorMsgNode(node, ErrorIncCheck,
                "The include file declares %s 'extern', without its value, so it must write its type, and the generator cannot spell the type inferred from the value. Write the type on the declaration.",
                incName(node));
            g->failed = 1;
        }
    }
    if (span->body) {
        char *from = span->body;
        while (from > span->kw && (incIsBlank(from[-1]) || from[-1] == '\n'))
            --from;
        if (node->tag == FnDclTag)
            incEdit(g, lexer, from, span->end, ";");
        else
            incEdit(g, lexer, from, span->bodyend, NULL);
    }
}

// The members of an included type: each whole, cut to 'extern' or left out
static void incEditType(IncGen *g, StructNode *strnode, char *floor) {
    DclSpans *spans = strnode->spans;
    for (uint32_t i = 0; spans && i < spans->count; ++i) {
        DclSpan *span = spans->items[i];
        INode *node = span->node;
        if (node && span->kind == SpanDcl) {
            switch (node->tag) {
            case FnDclTag:
                if (fnDclIsExpanded((FnDclNode*)node, (INode*)strnode))
                    break;
                if (!incMemberFnWanted(g, strnode, node))
                    incDelete(g, span, floor);
                else if (!(node->flags & FlagExtern))
                    incCut(g, span);
                break;
            case VarDclTag:
                if (!incMemberVarWanted(g, strnode, node))
                    incDelete(g, span, floor);
                break;
            case StructTag:
                incEditType(g, (StructNode*)node, span->kw);
                break;
            default:
                break;
            }
        }
        floor = span->end;
    }
}

// A declaration of the root's: whole, cut to 'extern', or left out
static void incEditDcl(IncGen *g, DclSpan *span, char *floor) {
    INode *node = span->node;
    if (node == NULL)
        return;
    switch (node->tag) {
    case FnDclTag:
        if (!dclIsExported(g->root, node))
            incDelete(g, span, floor);
        else if (!(node->flags & FlagExtern) && !fnDclIsExpanded((FnDclNode*)node, NULL))
            incCut(g, span);
        break;
    case VarDclTag:
        if (!incGlobalWanted(g, (VarDclNode*)node))
            incDelete(g, span, floor);
        else if (!(node->flags & FlagExtern))
            incCut(g, span);
        break;
    case StructTag:
        if (!incIsNeeded(g, (StructNode*)node))
            incDelete(g, span, floor);
        else
            incEditType(g, (StructNode*)node, span->kw);
        break;
    case AliasDclTag:
        // A typedef declares no symbol, and what names it is not recorded, so
        // one goes in unless it names what a submodule holds, which a public one
        // cannot and a private one need not
        if (node->flags & FlagTypeAlias) {
            g->probing = 1;
            g->probehit = 0;
            incWalkType(g, ((AliasDclNode*)node)->target);
            g->probing = 0;
            if (g->probehit) {
                if (node->flags & FlagPub) {
                    g->from = node;
                    incWalkType(g, ((AliasDclNode*)node)->target);
                }
                else
                    incDelete(g, span, floor);
            }
        }
        break;
    default:
        // A const, a macro or a module trait declares no symbol, and what names
        // it is not recorded: each goes in as written
        break;
    }
}

// A standalone 'use': kept where what it names is in the include file or
// another package, left out where it folds a submodule's names privately, and
// refused where it would make a submodule's names public
static void incEditUse(IncGen *g, DclSpan *span, char *floor) {
    ModUseNode *use = (ModUseNode*)span->node;
    INode *source = incUseSource(use);
    ModuleNode *mod = use->modfold ? use->modfold->module
        : source && source->tag == StructTag ? dclInfoGetModule(source) : NULL;
    if (mod == NULL || mod == g->root) {
        if (source && source->tag == StructTag && !incIsNeeded(g, (StructNode*)source))
            incDelete(g, span, floor);
        return;
    }
    if (!incInRootTree(g, mod))
        return;
    if (use->fold->ispub)
        incRefuse(g, (INode*)use, use->modfold ? (INode*)mod : source,
            "'pub use' makes what it names public names of the package");
    else
        incDelete(g, span, floor);
}

// What a body an importer expands names in a submodule, the root's own body
// having named it (DclSubReached): refused where it is declared
static void incRefuseSubReached(IncGen *g, INode *node) {
    DclInfo *dclinfo = inodeGetDclInfo(node);
    if (dclinfo && (dclinfo->facts & DclSubReached))
        incRefuse(g, node, node,
            "a body an importer expands -- an inline, generic or macro body, or a trait's default -- that the root module declares names it");
    if (node->tag == StructTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&((StructNode*)node)->nodelist, cnt, nodesp))
            incRefuseSubReached(g, *nodesp);
    }
}

// ---- The file ---------------------------------------------------------------

static int incEditOrder(const void *a, const void *b) {
    const IncEdit *x = (const IncEdit*)a, *y = (const IncEdit*)b;
    if (x->from != y->from)
        return x->from < y->from ? -1 : 1;
    // An insertion goes before what is replaced at the same place
    int xins = x->from == x->to, yins = y->from == y->to;
    if (xins != yins)
        return xins ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq ? 1 : 0;
}

// The files the root's statements are written in, in the order parsed. 'files'
// has room for one per statement
static uint32_t incFiles(DclSpans *spans, Lexer **files) {
    uint32_t nfiles = 0;
    for (uint32_t i = 0; spans && i < spans->count; ++i) {
        Lexer *lexer = spans->items[i]->lexer;
        uint32_t j;
        for (j = 0; j < nfiles; ++j) {
            if (files[j] == lexer)
                break;
        }
        if (j == nfiles)
            files[nfiles++] = lexer;
    }
    return nfiles;
}

// The banner: that the file is generated, from what, and not to be edited
// [Jon 25 Sep, Q2 and Q3]
static void incBanner(IncBuf *out, ModuleNode *root, Lexer **files, uint32_t nfiles) {
    char *name = &root->namesym->namestr;
    incBufPuts(out, "// Generated by conec: the include file of package ");
    incBufPuts(out, name);
    incBufPuts(out, ", from ");
    for (uint32_t i = 0; i < nfiles; ++i) {
        if (i > 0)
            incBufPuts(out, i + 1 == nfiles ? " and " : ", ");
        char *url = files[i]->url ? files[i]->url : "?";
        incBufPuts(out, url + fileFolder(url));
    }
    incBufPuts(out, ".\n// A program importing ");
    incBufPuts(out, name);
    incBufPuts(out, " compiles against this file in place of that source.\n// Do not edit it: change ");
    incBufPuts(out, name);
    incBufPuts(out, "'s source and compile ");
    incBufPuts(out, name);
    incBufPuts(out, " again.\n\n");
}

// Generate the include file of the program's root module
char *incFileGenerate(ProgramNode *pgm, size_t *lenp) {
    IncGen gen;
    memset(&gen, 0, sizeof(gen));
    IncGen *g = &gen;
    g->root = (ModuleNode*)nodesGet(pgm->modules, 0);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(g->root->imports, cnt, nodesp)) {
        if (((ImportNode*)*nodesp)->iscore)
            g->core = ((ImportNode*)*nodesp)->module;
    }
    if (g->core == NULL)
        g->core = g->root;  // compiling core itself
    DclSpans *spans = g->root->spans;

    // A generic module's every declaration is instantiated where it is used,
    // so its include file is its source, whole
    if (g->root->genericinfo == NULL) {
        incSelect(g);

        // What the root's expanded bodies name in its submodules
        for (nodesFor(pgm->modules, cnt, nodesp)) {
            ModuleNode *mod = (ModuleNode*)*nodesp;
            if (mod == g->root || !incInRootTree(g, mod))
                continue;
            INode **dclp;
            uint32_t dclcnt;
            for (nodesFor(mod->nodes, dclcnt, dclp))
                incRefuseSubReached(g, *dclp);
        }
    }

    // Edit the root's text, statement by statement. A file other than the
    // first has its imports moved up into the first file's header, since an
    // import may not follow a declaration
    Lexer **files = (Lexer**)memAllocBlk((spans ? spans->count : 0) * sizeof(Lexer*) + sizeof(Lexer*));
    uint32_t nfiles = incFiles(spans, files);
    Lexer *first = nfiles ? files[0] : NULL;
    DclSpan *header = NULL;     // The first file's last header statement
    IncBuf moved;
    memset(&moved, 0, sizeof(moved));
    Lexer *curlex = NULL;
    char *floor = NULL;
    for (uint32_t i = 0; spans && i < spans->count; ++i) {
        DclSpan *span = spans->items[i];
        if (span->lexer != curlex) {
            curlex = span->lexer;
            floor = curlex->source;
        }
        switch (span->kind) {
        case SpanModLine:
            if (curlex == first)
                header = span;
            break;
        case SpanImport:
            if (curlex == first)
                header = span;
            else {
                incBufPuts(&moved, "\n");
                incBufPutn(&moved, span->start, span->end - span->start);
                incDelete(g, span, floor);
            }
            break;
        case SpanUse:
            if (g->root->genericinfo == NULL)
                incEditUse(g, span, floor);
            break;
        case SpanExternBlock: {
            if (g->root->genericinfo != NULL)
                break;
            uint32_t kept = 0;
            for (uint32_t j = 0; span->members && j < span->members->count; ++j) {
                if (dclIsExported(g->root, span->members->items[j]->node))
                    ++kept;
            }
            if (kept == 0) {
                incDelete(g, span, floor);
                break;
            }
            char *itemfloor = span->kw;
            for (uint32_t j = 0; span->members && j < span->members->count; ++j) {
                DclSpan *item = span->members->items[j];
                if (!dclIsExported(g->root, item->node))
                    incDelete(g, item, itemfloor);
                itemfloor = item->end;
            }
            break;
        }
        case SpanDcl:
            if (g->root->genericinfo == NULL)
                incEditDcl(g, span, floor);
            break;
        default:
            break;
        }
        floor = span->end;
    }
    if (moved.len && header)
        incEdit(g, header->lexer, header->end, header->end, moved.text);

    if (g->failed)
        return NULL;

    // The banner, then each file's text with its edits made
    qsort(g->edits, g->nedits, sizeof(IncEdit), incEditOrder);
    IncBuf out;
    memset(&out, 0, sizeof(out));
    incBanner(&out, g->root, files, nfiles);
    for (uint32_t f = 0; f < nfiles; ++f) {
        Lexer *lexer = files[f];
        if (f > 0 && out.len > 0) {
            if (out.text[out.len - 1] != '\n')
                incBufPuts(&out, "\n");
            if (out.len < 2 || out.text[out.len - 2] != '\n')
                incBufPuts(&out, "\n");
        }
        char *p = lexer->source;
        for (uint32_t e = 0; e < g->nedits; ++e) {
            IncEdit *edit = &g->edits[e];
            if (edit->lexer != lexer || edit->from < p)
                continue;
            incBufPutn(&out, p, edit->from - p);
            if (edit->text)
                incBufPuts(&out, edit->text);
            p = edit->to;
        }
        incBufPuts(&out, p);
    }
    *lenp = out.len;
    return out.text;
}
