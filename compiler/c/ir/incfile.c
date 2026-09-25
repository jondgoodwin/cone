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
 * What the root reaches in one of its SUBMODULES goes in a private, pruned
 * nested module block, 'mod sub { ... }', holding only what is reached [Jon 25
 * Sep, Q1]: the submodule's own text, edited by the same rules. The block
 * spells each name after its real owner, as the package's object does, and
 * nobody outside the package can name it.
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
    if (n)
        memcpy(buf->text + buf->len, text, n);
    buf->len += n;
    buf->text[buf->len] = '\0';
}

static void incBufPuts(IncBuf *buf, const char *text) {
    incBufPutn(buf, text, strlen(text));
}

// A map from a node to what the generator knows of it, open addressing on
// the node's address
typedef struct IncMap {
    void **keys;
    void **vals;
    size_t avail;
    size_t used;
} IncMap;

static size_t incHash(void *key, size_t avail) {
    size_t h = (size_t)key;
    h ^= h >> 17;
    h *= 0x9E3779B1u;
    return (h ^ (h >> 13)) & (avail - 1);
}

static size_t incMapIndex(IncMap *map, void *key) {
    size_t i = incHash(key, map->avail);
    while (map->keys[i] != NULL && map->keys[i] != key)
        i = (i + 1) & (map->avail - 1);
    return i;
}

static void *incMapGet(IncMap *map, void *key) {
    if (map->avail == 0 || key == NULL)
        return NULL;
    size_t i = incMapIndex(map, key);
    return map->keys[i] ? map->vals[i] : NULL;
}

static void incMapPut(IncMap *map, void *key, void *val) {
    if ((map->used + 1) * 2 > map->avail) {
        void **oldkeys = map->keys, **oldvals = map->vals;
        size_t oldavail = map->avail;
        map->avail = oldavail ? oldavail * 2 : 64;
        map->keys = (void**)memAllocBlk(map->avail * sizeof(void*));
        map->vals = (void**)memAllocBlk(map->avail * sizeof(void*));
        memset(map->keys, 0, map->avail * sizeof(void*));
        memset(map->vals, 0, map->avail * sizeof(void*));
        for (size_t i = 0; i < oldavail; ++i) {
            if (oldkeys[i]) {
                size_t j = incMapIndex(map, oldkeys[i]);
                map->keys[j] = oldkeys[i];
                map->vals[j] = oldvals[i];
            }
        }
    }
    size_t i = incMapIndex(map, key);
    if (map->keys[i] == NULL) {
        map->keys[i] = key;
        ++map->used;
    }
    map->vals[i] = val;
}

// One module of the package: the root, or one of its submodules at any depth
typedef struct IncMod {
    ModuleNode *mod;
    struct IncMod *parent;  // NULL for the root
    int emitted;            // Its text is in the file: the root, or a submodule's block
    DclSpan *header;        // Its first file's last header statement: its 'mod' line or an import
    IncBuf moved;           // The imports of its later files, moved up into that header
} IncMod;

// Where a declaration of the package is written
typedef struct IncDcl {
    IncMod *m;              // Its module
    StructNode *type;       // The type whose braces declare it, at the top; NULL at module level
    DclSpan *span;
} IncDcl;

// One change to one file's text: [from, to) is replaced by 'text', or removed
// where 'text' is NULL; an insertion has 'from' and 'to' the same. 'blocks'
// inserts the moved imports and the nested module blocks of that module
typedef struct IncEdit {
    Lexer *lexer;
    char *from;
    char *to;
    char *text;
    IncMod *blocks;
    uint32_t seq;
} IncEdit;

typedef struct IncGen {
    ModuleNode *root;
    ModuleNode *core;       // Its types are written bare: every module folds core in
    IncMod **mods;          // The root first, then its submodules in the program's module order
    uint32_t nmods;
    IncMap dcls;            // Each declaration of the package with a span -> its IncDcl
    IncMap wanted;          // What goes in beyond the root's own rules: every type the file
                            // declares, each submodule declaration, a typedef reached
    IncEdit *edits;
    uint32_t nedits, availedits;
    int probing;            // Walking only to learn whether a type reaches a submodule
    int probehit;
    int failed;
} IncGen;

static IncEdit *incEdit(IncGen *g, Lexer *lexer, char *from, char *to, char *text) {
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
    edit->blocks = NULL;
    edit->seq = g->nedits++;
    return edit;
}

static int incWanted(IncGen *g, INode *node) {
    return incMapGet(&g->wanted, node) != NULL;
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

// ---- The package's modules --------------------------------------------------

// The package module a module is, or NULL where it is not one of the package's:
// core, another package's include file. An instance of a generic module is its
// generic's, whose block holds its whole text
static IncMod *incModOf(IncGen *g, ModuleNode *mod) {
    if (mod && mod->generic)
        mod = mod->generic;
    for (uint32_t i = 0; i < g->nmods; ++i) {
        if (g->mods[i]->mod == mod)
            return g->mods[i];
    }
    return NULL;
}

// Is 'mod' 'anc' or inside it, at any depth?
static int incWithin(ModuleNode *mod, ModuleNode *anc) {
    while (mod) {
        if (mod == anc)
            return 1;
        mod = mod->dclinfo.owner ? dclInfoGetModule(mod->dclinfo.owner) : NULL;
    }
    return 0;
}

// Record where each declaration of a type's braces is written, against the
// type at the top: a variant's members are its enum's
static void incMapType(IncGen *g, IncMod *m, StructNode *top, StructNode *strnode) {
    DclSpans *spans = strnode->spans;
    for (uint32_t i = 0; spans && i < spans->count; ++i) {
        DclSpan *span = spans->items[i];
        if (span->node == NULL || span->kind != SpanDcl)
            continue;
        IncDcl *dcl = (IncDcl*)memAllocBlk(sizeof(IncDcl));
        dcl->m = m;
        dcl->type = top;
        dcl->span = span;
        incMapPut(&g->dcls, span->node, dcl);
        if (span->node->tag == StructTag)
            incMapType(g, m, top, (StructNode*)span->node);
    }
}

// Record where each declaration of a module is written
static void incMapModule(IncGen *g, IncMod *m) {
    DclSpans *spans = m->mod->spans;
    for (uint32_t i = 0; spans && i < spans->count; ++i) {
        DclSpan *span = spans->items[i];
        DclSpans *items = NULL;
        if (span->kind == SpanExternBlock)
            items = span->members;
        else if (span->kind != SpanDcl || span->node == NULL)
            continue;
        for (uint32_t j = 0; j < (items ? items->count : 1); ++j) {
            DclSpan *one = items ? items->items[j] : span;
            if (one->node == NULL)
                continue;
            IncDcl *dcl = (IncDcl*)memAllocBlk(sizeof(IncDcl));
            dcl->m = m;
            dcl->type = NULL;
            dcl->span = one;
            incMapPut(&g->dcls, one->node, dcl);
            if (one->node->tag == StructTag)
                incMapType(g, m, (StructNode*)one->node, (StructNode*)one->node);
        }
    }
}

// ---- Writing an inferred type -----------------------------------------------

// A module's name as a declaration of 'inmod' reaches it: bare from inside it,
// a path down to one of its own submodules, a sister's name, another package's
// name. Returns 0 for a module 'inmod' does not reach by name
static int incModuleRef(IncGen *g, IncBuf *buf, ModuleNode *mod, ModuleNode *inmod) {
    if (mod == inmod || mod == g->core)
        return 1;
    if (incModOf(g, mod) == NULL) {
        // Another package's module, reached through the name its import binds
        if (mod->dclinfo.owner != NULL)
            return 0;
        incBufPuts(buf, &mod->namesym->namestr);
        incBufPuts(buf, ".");
        return 1;
    }
    ModuleNode *parent = mod->dclinfo.owner ? dclInfoGetModule(mod->dclinfo.owner) : NULL;
    if (parent == NULL)
        return 0;
    // A sister, which 'inmod' imported to name it
    ModuleNode *inparent = inmod->dclinfo.owner ? dclInfoGetModule(inmod->dclinfo.owner) : NULL;
    if (parent == inparent) {
        incBufPuts(buf, &mod->namesym->namestr);
        incBufPuts(buf, ".");
        return 1;
    }
    // One of 'inmod''s own, or of a sister's, at any depth, by the path down
    if (parent != inmod && !incModuleRef(g, buf, parent, inmod))
        return 0;
    incBufPuts(buf, &mod->namesym->namestr);
    incBufPuts(buf, ".");
    return 1;
}

// Write a type as Cone spells it inside module 'inmod', for a global whose type
// was inferred from its value [Jon 25 Sep, Q6]. A global's value is a literal,
// so its type is a number, a struct (an instance of a generic one included) or
// an array of either. Returns 0 for a type this does not spell
static int incTypeText(IncGen *g, IncBuf *buf, INode *type, ModuleNode *inmod) {
    if (type == NULL)
        return 0;
    if (isNameUseNode(type)) {
        INode *dcl = nameUseGetDcl((NameUseNode*)type);
        return dcl ? incTypeText(g, buf, dcl, inmod) : 0;
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
        if (!incModuleRef(g, buf, mod, inmod))
            return 0;
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
                if (!incTypeText(g, buf, *nodesp, inmod))
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
        if (!incTypeText(g, buf, nodesGet(array->elems, 0), inmod))
            return 0;
        incBufPuts(buf, "]");
        return 1;
    }
    default:
        return 0;
    }
}

// ---- What the include file needs --------------------------------------------
//
// The root's own declarations go in by the rules above. What they reach in the
// package -- a type an included signature, field or global names; what a body
// the file copies whole names (exportReachesOf); what a 'pub use' re-exports --
// is WANTED, and a wanted declaration of a submodule puts that submodule's
// block in the file, with everything the declaration reaches in turn.

static void incWalkType(IncGen *g, INode *type);
static void incNeed(IncGen *g, StructNode *strnode);
static void incWant(IncGen *g, INode *node);
static void incEmit(IncGen *g, IncMod *m);

// Want what a declaration the file holds reaches (exportReachesOf): everything
// a body it copies whole names, and what a parameter's default value names,
// which an importer evaluates where it calls -- a function, global or type, a
// macro, typedef or const, in whichever of the package's modules
static void incFollow(IncGen *g, INode *from) {
    Nodes *reached = exportReachesOf(from);
    if (reached == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(reached, cnt, nodesp))
        incWant(g, *nodesp);
}

// Want the declaration a binding stands for, and a typedef on the way to it:
// a fold's alias is followed to its target
static void incWantChain(IncGen *g, INode *node) {
    while (node) {
        if (isNameUseNode(node))
            node = ((NameUseNode*)node)->dclnode;
        else if (node->tag == AliasDclTag) {
            if ((node->flags & FlagTypeAlias) && incMapGet(&g->dcls, node)) {
                incWant(g, node);
                return;
            }
            node = ((AliasDclNode*)node)->target;
        }
        else
            break;
    }
    if (node)
        incWant(g, node);
}

// Want each name a fold clause lists
static void incWantItems(IncGen *g, FoldClause *fold) {
    if (fold == NULL || fold->star)
        return;
    INode **itemp;
    uint32_t cnt;
    for (nodesFor(fold->items, cnt, itemp))
        incWantChain(g, *itemp);
}

// Want every public name of a module a star clause folds: a 'pub use' of a
// submodule makes each one a public name of the package
static void incWantAll(IncGen *g, ModuleNode *src, FoldClause *fold) {
    Namespace *ns = &src->namespace;
    namespaceFor(ns) {
        NameNode *nn = &ns->namenodes[__i];
        if (nn->name == NULL || nn->node == NULL)
            continue;
        if (nn->name == selfTypeName || nn->name == anonName || nn->name == finalName
            || nn->name == cloneName || nn->name == src->namesym || foldExcluded(fold, nn->name))
            continue;
        if (inodeIsPrivate(nn->node))
            continue;
        incWantChain(g, nn->node);
    }
}

// A type an included declaration names. One of the root's goes in, with what
// its own fields and signatures name; one of a submodule's goes in that
// submodule's block; one of another package's is that package's include file's
// to declare
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
    IncMod *m = incModOf(g, dclInfoGetModule((INode*)top));
    if (m == NULL)
        return;
    if (g->probing) {
        if (!incWanted(g, (INode*)top))
            g->probehit = 1;
        return;
    }
    incNeed(g, top);
}

// Walk a type expression for the types it names
static void incWalkType(IncGen *g, INode *type) {
    if (type == NULL)
        return;
    if (isNameUseNode(type)) {
        INode *dcl = ((NameUseNode*)type)->dclnode;
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
        // A typedef of the package goes in, and with it what it names
        if ((type->flags & FlagTypeAlias) && !g->probing && incMapGet(&g->dcls, type))
            incWant(g, type);
        else
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
// the members that go in with it, what the bodies it carries whole name, and
// an enum's variants. The type is marked DclIncluded first: an importer holds
// its values, so the object exports its public methods (dclIsExported), and
// that decides which members go in
static void incWalkStruct(IncGen *g, StructNode *strnode) {
    strnode->dclinfo.facts |= DclIncluded;
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
        if (member->tag == FnDclTag && incMemberFnWanted(g, strnode, member)) {
            incWalkType(g, ((FnDclNode*)member)->vtype);
            incFollow(g, member);
        }
        else if (member->tag == MacroDclTag)
            incFollow(g, member);
        else if (member->tag == VarDclTag && incMemberVarWanted(g, strnode, member))
            incWalkType(g, ((VarDclNode*)member)->vtype);
    }
    if ((strnode->flags & EnumType) && strnode->derived) {
        for (nodesFor(strnode->derived, cnt, nodesp))
            incWalkStruct(g, (StructNode*)*nodesp);
    }
}

static void incNeed(IncGen *g, StructNode *strnode) {
    if (incWanted(g, (INode*)strnode))
        return;
    incMapPut(&g->wanted, strnode, strnode);
    IncMod *m = incModOf(g, dclInfoGetModule((INode*)strnode));
    if (m)
        incEmit(g, m);
    incWalkStruct(g, strnode);
}

// A module-trait's members: the signatures, and what the defaults name
static void incWalkModTrait(IncGen *g, ModTraitNode *trait) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(trait->nodes, cnt, nodesp)) {
        if ((*nodesp)->tag == FnDclTag) {
            incWalkType(g, ((FnDclNode*)*nodesp)->vtype);
            incFollow(g, *nodesp);
        }
        else if ((*nodesp)->tag == VarDclTag)
            incWalkType(g, ((VarDclNode*)*nodesp)->vtype);
    }
}

// Want a declaration of the package the file must hold, with what it reaches.
// A member of a type brings its type, which decides its members. The root's
// functions, globals, macros and consts go in by the root's own rules; a
// submodule's go in because they are wanted
static void incWant(IncGen *g, INode *node) {
    if (g->probing || node == NULL)
        return;
    IncDcl *dcl = (IncDcl*)incMapGet(&g->dcls, node);
    if (dcl == NULL) {
        // A declaration of an instance of one of the package's generic
        // modules brings the generic's block, which holds its whole text.
        // Anything else is not the package's -- core's, another package's --
        // or a copy with no text of its own
        DclInfo *info = inodeGetDclInfo(node);
        ModuleNode *mod = info && info->owner ? dclInfoGetModule(node) : NULL;
        IncMod *m = mod && mod->generic ? incModOf(g, mod) : NULL;
        if (m)
            incEmit(g, m);
        return;
    }
    if (dcl->type) {
        incNeed(g, dcl->type);
        return;
    }
    if (node->tag == StructTag) {
        incNeed(g, (StructNode*)node);
        return;
    }
    if (incWanted(g, node))
        return;
    int isroot = dcl->m->mod == g->root;
    if (isroot && node->tag != AliasDclTag)
        return;
    incMapPut(&g->wanted, node, node);
    incEmit(g, dcl->m);
    switch (node->tag) {
    case FnDclTag:
        incWalkType(g, ((FnDclNode*)node)->vtype);
        incFollow(g, node);
        break;
    case VarDclTag:
        incWalkType(g, ((VarDclNode*)node)->vtype);
        break;
    case AliasDclTag:
        if (node->flags & FlagTypeAlias)
            incWalkType(g, ((AliasDclNode*)node)->target);
        break;
    case MacroDclTag:
        incFollow(g, node);
        break;
    case ModTraitTag:
        incWalkModTrait(g, (ModTraitNode*)node);
        break;
    default:
        break;
    }
}

// Whether a module-level global goes in: it is exported, or it is one the
// module's finalizer drops, which an importer must see to derive that the
// package's finalizer is its 'drop' (module.md, "Init and final")
static int incGlobalDropped(VarDclNode *var) {
    return !(var->dclinfo.facts & DclCName) && itypeGetDropFnDcl(var->vtype) != NULL;
}

static int incGlobalWanted(IncGen *g, VarDclNode *var) {
    return dclIsExported(g->root, (INode*)var) || incGlobalDropped(var);
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

// What a module's standalone 'use' needs in the file. A 'pub use' at the root
// makes names public names of the package, so what it re-exports goes in: an
// enum, or every name it folds from a submodule. A list names what it folds,
// so each of those goes in wherever the 'use' is kept
static void incSeedUse(IncGen *g, IncMod *m, ModUseNode *use) {
    INode *source = incUseSource(use);
    int ispub = use->fold->ispub && m->mod == g->root;
    if (use->modfold) {
        IncMod *src = incModOf(g, use->modfold->module);
        if (src == NULL)
            return;
        incWantItems(g, use->fold);
        if (ispub) {
            incEmit(g, src);
            if (use->fold->star)
                incWantAll(g, src->mod, use->fold);
        }
    }
    else if (ispub && source && source->tag == StructTag)
        incReachStruct(g, (StructNode*)source);
}

// Put a submodule's block in the file, and its parent's around it. What its
// own header and 'mod' line name comes with it: a sister it extends, and each
// name an import of a sister lists. A module conforming to a module trait
// keeps every declaration, since the trait decides which it needs
static void incEmit(IncGen *g, IncMod *m) {
    if (m->emitted)
        return;
    m->emitted = 1;
    if (m->parent)
        incEmit(g, m->parent);
    ModuleNode *mod = m->mod;
    if (mod->extends && mod->extends->module) {
        IncMod *base = incModOf(g, mod->extends->module);
        if (base)
            incEmit(g, base);
    }
    DclSpans *spans = mod->spans;

    // A generic module's block is its whole text, since every declaration is
    // instantiated where it is used, and what that text reaches in a sister is
    // not recorded: a sister it imports goes in whole too
    if (mod->genericinfo) {
        for (uint32_t i = 0; spans && i < spans->count; ++i) {
            DclSpan *span = spans->items[i];
            if (span->kind != SpanImport || span->node == NULL)
                continue;
            IncMod *sister = incModOf(g, ((ImportNode*)span->node)->module);
            if (sister == NULL)
                continue;
            incEmit(g, sister);
            DclSpans *sspans = sister->mod->spans;
            for (uint32_t j = 0; sspans && j < sspans->count; ++j) {
                DclSpan *sspan = sspans->items[j];
                if (sspan->kind == SpanDcl)
                    incWant(g, sspan->node);
                else if (sspan->kind == SpanExternBlock) {
                    for (uint32_t k = 0; sspan->members && k < sspan->members->count; ++k)
                        incWant(g, sspan->members->items[k]->node);
                }
            }
        }
        return;
    }
    for (uint32_t i = 0; spans && i < spans->count; ++i) {
        DclSpan *span = spans->items[i];
        if (span->node == NULL)
            continue;
        if (span->kind == SpanImport) {
            ImportNode *import = (ImportNode*)span->node;
            if (import->module && incModOf(g, import->module))
                incWantItems(g, import->fold);
        }
        else if (span->kind == SpanUse)
            incSeedUse(g, m, (ModUseNode*)span->node);
        else if (mod->traitname && m->parent) {
            if (span->kind == SpanDcl)
                incWant(g, span->node);
            else if (span->kind == SpanExternBlock) {
                for (uint32_t j = 0; span->members && j < span->members->count; ++j)
                    incWant(g, span->members->items[j]->node);
            }
        }
    }
}

// The root's declarations of one statement: an extern block's items, or the
// statement's own
static void incSeedDcl(IncGen *g, DclSpan *span) {
    INode *node = span->node;
    if (node == NULL)
        return;
    switch (node->tag) {
    case FnDclTag:
        if (dclIsExported(g->root, node)) {
            incWalkType(g, ((FnDclNode*)node)->vtype);
            incFollow(g, node);
        }
        break;
    case VarDclTag:
        if (incGlobalWanted(g, (VarDclNode*)node))
            incWalkType(g, ((VarDclNode*)node)->vtype);
        break;
    case StructTag:
        if (incTypeReached((StructNode*)node))
            incNeed(g, (StructNode*)node);
        break;
    case AliasDclTag:
        if ((node->flags & FlagTypeAlias) && (node->flags & FlagPub))
            incWant(g, node);
        break;
    case MacroDclTag:
        incFollow(g, node);
        break;
    case ModTraitTag:
        incWalkModTrait(g, (ModTraitNode*)node);
        break;
    default:
        break;
    }
}

// A submodule's declarations the program reaches without naming them: its
// 'init' and 'final', which the program's stitched pair calls, and each global
// its finalizer drops, from which the program derives that finalizer
static void incSeedLifecycle(IncGen *g, DclSpan *span) {
    INode *node = span->node;
    if (node == NULL)
        return;
    if ((node->tag == FnDclTag && (((FnDclNode*)node)->dclinfo.facts & DclLifecycle))
        || (node->tag == VarDclTag && incGlobalDropped((VarDclNode*)node)))
        incWant(g, node);
}

// Work out what goes in: from the root's own declarations, what each reaches,
// transitively, in the root and in its submodules
static void incSelect(IncGen *g) {
    IncMod *root = g->mods[0];
    root->emitted = 1;
    DclSpans *spans = g->root->spans;
    for (uint32_t i = 0; spans && i < spans->count; ++i) {
        DclSpan *span = spans->items[i];
        if (span->kind == SpanExternBlock) {
            for (uint32_t j = 0; span->members && j < span->members->count; ++j)
                incSeedDcl(g, span->members->items[j]);
        }
        else if (span->kind == SpanDcl)
            incSeedDcl(g, span);
        else if (span->kind == SpanUse && span->node)
            incSeedUse(g, root, (ModUseNode*)span->node);
    }
    for (uint32_t k = 1; k < g->nmods; ++k) {
        spans = g->mods[k]->mod->spans;
        for (uint32_t i = 0; spans && i < spans->count; ++i) {
            DclSpan *span = spans->items[i];
            if (span->kind == SpanExternBlock) {
                for (uint32_t j = 0; span->members && j < span->members->count; ++j)
                    incSeedLifecycle(g, span->members->items[j]);
            }
            else if (span->kind == SpanDcl)
                incSeedLifecycle(g, span);
        }
    }
}

// ---- Editing the text -------------------------------------------------------

// Declare a definition 'extern' in place of its body or value: 'extern' written
// in before its keyword, and its body -- a function's from its '{', a global's
// value from its '=' -- left out, with what follows the value (a fold clause)
// kept. A global whose type was inferred gets its type written in [Jon 25 Sep, Q6]
static void incCut(IncGen *g, IncMod *m, DclSpan *span) {
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
        if (incTypeText(g, &type, ((VarDclNode*)node)->vtype, m->mod))
            incEdit(g, lexer, span->nameend, span->nameend, type.text);
        else {
            errorMsgNode(node, ErrorIncCheck,
                "The include file declares %s 'extern', without its value, so it must write its type, and the generator cannot spell the type inferred from the value. Write the type on the declaration.",
                &inodeGetName(node)->namestr);
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
static void incEditType(IncGen *g, IncMod *m, StructNode *strnode, char *floor) {
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
                    incCut(g, m, span);
                break;
            case VarDclTag:
                if (!incMemberVarWanted(g, strnode, node))
                    incDelete(g, span, floor);
                break;
            case StructTag:
                incEditType(g, m, (StructNode*)node, span->kw);
                break;
            default:
                break;
            }
        }
        floor = span->end;
    }
}

// A declaration of the root's: whole, cut to 'extern', or left out
static void incEditRootDcl(IncGen *g, IncMod *m, DclSpan *span, char *floor) {
    INode *node = span->node;
    if (node == NULL)
        return;
    switch (node->tag) {
    case FnDclTag:
        if (!dclIsExported(g->root, node))
            incDelete(g, span, floor);
        else if (!(node->flags & FlagExtern) && !fnDclIsExpanded((FnDclNode*)node, NULL))
            incCut(g, m, span);
        break;
    case VarDclTag:
        if (!incGlobalWanted(g, (VarDclNode*)node))
            incDelete(g, span, floor);
        else if (!(node->flags & FlagExtern))
            incCut(g, m, span);
        break;
    case StructTag:
        if (!incWanted(g, node))
            incDelete(g, span, floor);
        else
            incEditType(g, m, (StructNode*)node, span->kw);
        break;
    case AliasDclTag:
        // A typedef declares no symbol. One goes in unless it is private,
        // nothing the file holds reaches it, and it names a type of the
        // package's that the file leaves out
        if ((node->flags & FlagTypeAlias) && !(node->flags & FlagPub) && !incWanted(g, node)) {
            g->probing = 1;
            g->probehit = 0;
            incWalkType(g, ((AliasDclNode*)node)->target);
            g->probing = 0;
            if (g->probehit)
                incDelete(g, span, floor);
        }
        break;
    default:
        // A const, a macro or a module trait declares no symbol, and each goes
        // in as written
        break;
    }
}

// A declaration of a submodule's: in the block only where wanted, then whole or
// cut to 'extern' by the root's rules
static void incEditSubDcl(IncGen *g, IncMod *m, DclSpan *span, char *floor) {
    INode *node = span->node;
    if (node == NULL)
        return;
    if (!incWanted(g, node)) {
        incDelete(g, span, floor);
        return;
    }
    switch (node->tag) {
    case FnDclTag:
        if (!(node->flags & FlagExtern) && !fnDclIsExpanded((FnDclNode*)node, NULL))
            incCut(g, m, span);
        break;
    case VarDclTag:
        if (!(node->flags & FlagExtern))
            incCut(g, m, span);
        break;
    case StructTag:
        incEditType(g, m, (StructNode*)node, span->kw);
        break;
    default:
        break;
    }
}

// A standalone 'use': kept where what it names is in the file or is another
// package's, and left out where it names an enum or a submodule the file does
// not hold
static void incEditUse(IncGen *g, DclSpan *span, char *floor) {
    ModUseNode *use = (ModUseNode*)span->node;
    if (use->modfold) {
        IncMod *src = incModOf(g, use->modfold->module);
        if (src && !src->emitted)
            incDelete(g, span, floor);
        return;
    }
    INode *source = incUseSource(use);
    if (source && source->tag == StructTag && incModOf(g, dclInfoGetModule(source))
        && !incWanted(g, source))
        incDelete(g, span, floor);
}

// An import: kept, unless it imports a sister whose block is not in the file
static int incImportKept(IncGen *g, DclSpan *span) {
    ImportNode *import = (ImportNode*)span->node;
    if (import == NULL || import->module == NULL)
        return 1;
    IncMod *im = incModOf(g, import->module);
    return im == NULL || im->emitted;
}

// A submodule's 'mod' line becomes its block's opening: 'mod name {', with its
// '@c', 'extends' and 'is' as written. A block the root holds loses its 'pub',
// since it is private to the package: an importer names nothing through it. A
// block inside another keeps it, since there 'pub' opens it to its parent's
// neighbours, all inside the package, and the root may reach through it as the
// source does. Its default fold goes, which says what an import of it folds,
// and nothing imports it
static char *incBlockOpening(IncMod *m, DclSpan *span) {
    char *from = m->parent && m->parent->parent ? span->start : span->kw;
    char *to = span->end;
    FoldClause *deffold = m->mod->deffold;
    if (deffold && deffold->at && deffold->at->srcp > from && deffold->at->srcp < to)
        to = deffold->at->srcp;
    else if (to > from && to[-1] == ';')
        --to;
    while (to > from && (incIsBlank(to[-1]) || to[-1] == '\n'))
        --to;
    IncBuf buf;
    memset(&buf, 0, sizeof(buf));
    incBufPutn(&buf, from, to - from);
    incBufPuts(&buf, " {");
    return buf.text;
}

// Whether any of a module's submodules has a block in the file
static int incHasBlocks(IncGen *g, IncMod *m) {
    for (uint32_t k = 0; k < g->nmods; ++k) {
        if (g->mods[k]->parent == m && g->mods[k]->emitted)
            return 1;
    }
    return 0;
}

// Edit one module's text, statement by statement. A file other than the
// first has its imports moved up into the first file's header, since an
// import may not follow a declaration, and the header is where the module's
// nested blocks go
static void incEditModule(IncGen *g, IncMod *m) {
    int isroot = m->mod == g->root;
    int generic = m->mod->genericinfo != NULL;
    DclSpans *spans = m->mod->spans;
    Lexer *first = spans && spans->count ? spans->items[0]->lexer : NULL;
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
            if (curlex == first) {
                m->header = span;
                if (!isroot) {
                    // The block's opening, and a blank line after it goes
                    // unless nested blocks follow it: a separate edit, so that
                    // the header's end, where those go, is not inside either
                    incEdit(g, curlex, span->start, span->end, incBlockOpening(m, span));
                    int importnext = i + 1 < spans->count && spans->items[i + 1]->kind == SpanImport
                        && spans->items[i + 1]->lexer == curlex;
                    char *p = span->end;
                    while (incIsBlank(*p))
                        ++p;
                    if (*p == '\n' && (importnext || !incHasBlocks(g, m))) {
                        char *q = p + 1;
                        while (incIsBlank(*q))
                            ++q;
                        if (*q == '\n')
                            incEdit(g, curlex, span->end, p + 1, NULL);
                    }
                }
            }
            break;
        case SpanImport:
            if (curlex == first) {
                m->header = span;
                if (!incImportKept(g, span))
                    incDelete(g, span, floor);
            }
            else {
                if (incImportKept(g, span)) {
                    incBufPuts(&m->moved, "\n");
                    incBufPutn(&m->moved, span->start, span->end - span->start);
                }
                incDelete(g, span, floor);
            }
            break;
        case SpanUse:
            if (!generic && span->node)
                incEditUse(g, span, floor);
            break;
        case SpanExternBlock: {
            if (generic)
                break;
            uint32_t kept = 0;
            for (uint32_t j = 0; span->members && j < span->members->count; ++j) {
                INode *item = span->members->items[j]->node;
                if (isroot ? dclIsExported(g->root, item) : incWanted(g, item))
                    ++kept;
            }
            if (kept == 0) {
                incDelete(g, span, floor);
                break;
            }
            char *itemfloor = span->kw;
            for (uint32_t j = 0; span->members && j < span->members->count; ++j) {
                DclSpan *item = span->members->items[j];
                if (!(isroot ? dclIsExported(g->root, item->node) : incWanted(g, item->node)))
                    incDelete(g, item, itemfloor);
                itemfloor = item->end;
            }
            break;
        }
        case SpanDcl:
            if (generic)
                break;
            if (isroot)
                incEditRootDcl(g, m, span, floor);
            else
                incEditSubDcl(g, m, span, floor);
            break;
        default:
            break;
        }
        floor = span->end;
    }

    // Where the moved imports and the nested blocks go
    if (m->moved.len || incHasBlocks(g, m)) {
        IncEdit *edit = m->header
            ? incEdit(g, m->header->lexer, m->header->end, m->header->end, NULL)
            : incEdit(g, first, first->source, first->source, NULL);
        edit->blocks = m;
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

// The files a module's statements are written in, in the order parsed. 'files'
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

static void incAssemble(IncGen *g, IncMod *m, IncBuf *out);

// A module's moved imports, then the block of each of its submodules the file
// holds, in the program's module order: a sister a block imports is ahead of it
static void incPutBlocks(IncGen *g, IncMod *m, IncBuf *out) {
    incBufPutn(out, m->moved.text, m->moved.len);
    for (uint32_t k = 0; k < g->nmods; ++k) {
        IncMod *child = g->mods[k];
        if (child->parent != m || !child->emitted)
            continue;
        IncBuf text;
        memset(&text, 0, sizeof(text));
        incAssemble(g, child, &text);
        char *start = text.text ? text.text : "";
        char *end = start + text.len;
        while (start < end && (incIsBlank(*start) || *start == '\n'))
            ++start;
        while (end > start && (incIsBlank(end[-1]) || end[-1] == '\n'))
            --end;
        incBufPuts(out, "\n\n");
        incBufPutn(out, start, end - start);
        incBufPuts(out, "\n}");
    }
}

// A module's text, its edits made, its files one after another
static void incAssemble(IncGen *g, IncMod *m, IncBuf *out) {
    DclSpans *spans = m->mod->spans;
    Lexer **files = (Lexer**)memAllocBlk((spans ? spans->count : 0) * sizeof(Lexer*) + sizeof(Lexer*));
    uint32_t nfiles = incFiles(spans, files);
    for (uint32_t f = 0; f < nfiles; ++f) {
        Lexer *lexer = files[f];
        if (f > 0 && out->len > 0) {
            if (out->text[out->len - 1] != '\n')
                incBufPuts(out, "\n");
            if (out->len < 2 || out->text[out->len - 2] != '\n')
                incBufPuts(out, "\n");
        }
        char *p = lexer->source;
        for (uint32_t e = 0; e < g->nedits; ++e) {
            IncEdit *edit = &g->edits[e];
            if (edit->lexer != lexer || edit->from < p)
                continue;
            incBufPutn(out, p, edit->from - p);
            if (edit->text)
                incBufPuts(out, edit->text);
            if (edit->blocks)
                incPutBlocks(g, edit->blocks, out);
            p = edit->to;
        }
        incBufPuts(out, p);
    }
}

// The banner: that the file is generated, from what, and not to be edited
// [Jon 25 Sep, Q2 and Q3]. Its first words are what marks the file as
// generated where it is read (incFileIsGenerated)
static void incBanner(IncBuf *out, ModuleNode *root, Lexer **files, uint32_t nfiles) {
    char *name = &root->namesym->namestr;
    incBufPuts(out, IncFileBanner);
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

// Whether a file's text is a generated include file: it opens with the banner
int incFileIsGenerated(char *text) {
    if (text == NULL)
        return 0;
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text += 3;
    return strncmp(text, IncFileBanner, strlen(IncFileBanner)) == 0;
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

    // The package's modules: the root, then its submodules in the program's
    // module order, where each module follows what it depends on
    Nodes *order = pgm->initorder && pgm->initorder->used ? pgm->initorder : pgm->modules;
    g->mods = (IncMod**)memAllocBlk((order->used + 1) * sizeof(IncMod*));
    IncMod *rootm = (IncMod*)memAllocBlk(sizeof(IncMod));
    memset(rootm, 0, sizeof(IncMod));
    rootm->mod = g->root;
    g->mods[g->nmods++] = rootm;
    if (g->root->genericinfo == NULL) {
        for (nodesFor(order, cnt, nodesp)) {
            ModuleNode *mod = (ModuleNode*)*nodesp;
            if (mod == g->root || mod->tag != ModuleTag || mod->generic || !incWithin(mod, g->root))
                continue;
            IncMod *m = (IncMod*)memAllocBlk(sizeof(IncMod));
            memset(m, 0, sizeof(IncMod));
            m->mod = mod;
            g->mods[g->nmods++] = m;
        }
        for (uint32_t k = 1; k < g->nmods; ++k)
            g->mods[k]->parent = incModOf(g, dclInfoGetModule(g->mods[k]->mod->dclinfo.owner));
        for (uint32_t k = 0; k < g->nmods; ++k)
            incMapModule(g, g->mods[k]);

        // A generic module's every declaration is instantiated where it is
        // used, so its include file is its source, whole
        incSelect(g);
    }
    else
        rootm->emitted = 1;

    for (uint32_t k = 0; k < g->nmods; ++k) {
        if (g->mods[k]->emitted)
            incEditModule(g, g->mods[k]);
    }
    if (g->failed)
        return NULL;

    // The banner, then the root's text with its edits made, its submodules'
    // blocks inside it
    qsort(g->edits, g->nedits, sizeof(IncEdit), incEditOrder);
    DclSpans *spans = g->root->spans;
    Lexer **files = (Lexer**)memAllocBlk((spans ? spans->count : 0) * sizeof(Lexer*) + sizeof(Lexer*));
    uint32_t nfiles = incFiles(spans, files);
    IncBuf out;
    memset(&out, 0, sizeof(out));
    incBanner(&out, g->root, files, nfiles);
    incAssemble(g, rootm, &out);
    *lenp = out.len;
    return out.text;
}
