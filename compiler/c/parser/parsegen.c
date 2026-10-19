/** Parse a generator, and generate what it stands for
 *
 * 'fn walk(t: &Tree, n: i32) yields &Node { ... yield node; ... yield each sub; ... }'
 * declares a generator: a function that hands its caller values one at a time,
 * suspended at each 'yield' until it is asked for the next. Calling it runs
 * nothing: it returns a value that holds the parameters, and every call of that
 * value's 'next' runs the body up to its next 'yield'. No node stands for it.
 * The parser turns the declaration into declarations generated as Cone source
 * and parsed here, in this order (a diagnostic against generated text is
 * reported at the generator's name, Lexer.genat):
 *
 *   struct walk.Gen is Iterator[&Node] {               the generator's value, declaring what it is
 *     let t: &'a Tree;  let n: i32;                    the parameters, as the author wrote them, under names no source can spell
 *     state': u32;                                     where the body resumes
 *     pub fn next(self: &mut) Option[&'a Node] {...}    the author's body
 *     fn final(self: &uni) {}                           what the frame's drop hangs on
 *     fn walk.yield(v: &Node) Option[&Node] inline {Some[&Node](v)}    what a 'yield' hands the caller
 *     fn walk.none() Option[&Node] {None[&Node]}                      what its end hands the caller
 *   }
 *   fn walk(t: &Tree, n: i32) walk.Gen {new walk.Gen(t, n, 0u32)}     the generator, made
 *
 * A generator may be generic, 'fn items[T](l: &List[T]) yields &T': the struct is
 * 'items.Gen[T]', taking the function's type parameters as they are written, and
 * the constructor makes 'new items.Gen[T](l, 0u32)'. An instance of the struct
 * is a clone of it like any generic type's, and a generator of its own
 * (yieldGenCloned).
 *
 * A generator may be a method of a struct, 'fn walk(self: &) yields &Node' inside
 * 'struct Tree[T]'. The struct is 'Tree.walk.Gen[T]', made beside the type with
 * the type's parameters (and the method's own after them), and the constructor,
 * returned in the author's place, is the type's method. The receiver is held in a
 * field under a private name that reads 'self', and 'self' in the body names it;
 * 'Self' in the body names the type, not the struct (parseGenName).
 *
 * A parameter is a field of the struct under a private Name that reads as the
 * parameter's own (the table does not hold it, so no source spells it), as the
 * state is: so a parameter called 'count', 'map' or 'final' does not collide
 * with the Iterator the struct declares, and the generator's value has no member
 * named as any parameter. A bare name in the body that names a parameter, and
 * is not shadowed by a local, is bound to its field (nameUseGenParm).
 *
 * The struct is private: a generator 'pub' in a library is refused, since the
 * include file would have to carry its body whole for an importer to make the
 * struct again (parseFnOrVar, parseStruct).
 *
 * The body is the author's, read once, in place, with 'yield' and 'return'
 * understood (parseGenBegin); it becomes the body of 'next', where the
 * parameters are fields and are named as bare names are in any method. Its end,
 * and a 'return', hand the caller None; a 'yield e' hands it Some(e) and
 * suspends (ir/exp/yield.h says how: the locals that live across a seam are kept
 * in a frame that follows the parameters in the struct, and the body resumes by
 * a jump, genllvm/genlyield.c). 'yield each src' is a loop over the source's
 * 'next', which 'yield's each value: it needs no node of its own.
 *
 * A borrowing generator names its borrows: where a parameter or the yielded type
 * is a borrowed reference, each is given the one lifetime 'a, so that the struct
 * is 'walk.Gen['a]' and its 'next' gives 'Option[&'a Node]' -- tied to what the
 * generator was lent, not to the generator -- as a cursor declaring a lifetime
 * does. The text written for the parameters and the yielded type is copied
 * (parseGenAnnotate puts 'a after each '&'); one that names a lifetime already,
 * or writes a reference a lifetime cannot be put on, makes every borrow the
 * unnamed one, and 'next''s result is tied to the generator.
 *
 * A method of a type that declares lifetimes, 'struct View['a]', has them in its
 * struct too, 'View.twice.Gen['b, 'a]', the borrows' one lifetime ('b, named
 * apart from the type's) first. The receiver's field is '&'b View['a]', and the
 * constructor, which names the same lifetimes, carries what the receiver was lent
 * and what it holds into the generator (parseGenTypeLifes).
 *
 * @file
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
#include <string.h>

// ---- The generated text ------------------------------------------------------

typedef struct GenText {
    char *text;
    size_t len;
    size_t avail;
} GenText;

static void genPutn(GenText *g, const char *s, size_t n) {
    if (g->len + n + 1 > g->avail) {
        size_t avail = g->avail ? g->avail : 1024;
        while (avail < g->len + n + 1)
            avail *= 2;
        char *grown = (char *)memAllocBlk(avail);
        if (g->len)
            memcpy(grown, g->text, g->len);
        g->text = grown;
        g->avail = avail;
    }
    memcpy(g->text + g->len, s, n);
    g->len += n;
    g->text[g->len] = '\0';
}

static void genPuts(GenText *g, const char *s) {
    genPutn(g, s, strlen(s));
}

// A name the author wrote, back-ticked so that it is read as a name whatever it is
static void genName(GenText *g, Name *name) {
    genPuts(g, "`");
    genPutn(g, &name->namestr, name->namesz);
    genPuts(g, "`");
}

// A name no source can spell
static void genSlot(GenText *g, uint32_t slot) {
    char buf[16];
    snprintf(buf, sizeof(buf), "`#%d`", (int)slot);
    genPuts(g, buf);
}

// The names the generated text spells '`#n`'
enum GenSlot {
    GenStruct,      // The generator's struct, 'walk.Gen'
    GenState,       // Its state field
    GenYield,       // The function making what a 'yield' hands the caller
    GenNone,        // The function making what the end hands the caller
    GenSub,         // A 'yield each' template's local, the sub-generator
    GenSource,      // The template's placeholder for the source written
    GenItem,        // The template's match binding
    GenRecv,        // A method's receiver, the field that 'self' in its body names
    GenSlots
};

// ---- Lifetimes ----------------------------------------------------------------

static int parseGenIsIdent(char c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// A type as the author wrote it, with a lifetime put after each '&': '&Tree'
// is '&'a Tree', '&mut Node' is '&'a mut Node'. '*refs' counts the borrows;
// '*bad' is set where one is written some way a lifetime cannot be put after,
// or already names one
static void parseGenAnnotate(GenText *g, char *from, char *to, const char *life, int *refs, int *bad) {
    for (char *p = from; p < to; ++p) {
        if (*p == '\'')
            *bad = 1;
        genPutn(g, p, 1);
        if (*p != '&')
            continue;
        ++*refs;
        char next = p + 1 < to ? p[1] : ' ';
        if (!(parseGenIsIdent(next) || next == ' ')) {
            *bad = 1;
            continue;
        }
        // The lifetime, then a space before the name or permission that follows
        if (life) {
            genPuts(g, life);
            if (next != ' ')
                genPuts(g, " ");
        }
    }
}

// ---- The parameters -------------------------------------------------------------

typedef struct GenParm {
    char *start;            // Where the parameter is written: its permission, if written, else its name
    Name *name;
    int hasperm;            // A permission is written before the name
    char *type, *typeend;   // Its type; just past its name where none is written
    char *nameend;          // Just past its name
    int typed;              // Its type is written
    int bare;               // It is a receiver written without its pointee: 'self: &', 'self: &mut'
    char *valueend;         // Just past its default value, or its type where there is none
} GenParm;

// The parameter list's text read again, for where each parameter's pieces are
// written: the first reading made the nodes, and kept no text. The list is
// read in place, from its '(' to its ')'. Returns how many parameters
static uint32_t parseGenParms(ParseState *parse, GenSig *sig, GenParm **parmsp) {
    uint32_t count = 0, max = 8;
    GenParm *parms = (GenParm *)memAllocBlk(max * sizeof(GenParm));
    Lexer *reread = lexNew(sig->parms, lex->url);
    FnSigNode *svlifesig = parse->lifesig;
    parse->lifesig = newFnSigNode();
    int svinlist = parse->inlist;
    char *svtypep = parse->typep, *svtypeendp = parse->typeendp;
    char *svbodyp = parse->bodyp, *svbodyendp = parse->bodyendp;
    lexPush(reread);
    if (lexIsToken(LParenToken)) {
        lexNextToken();
        parse->inlist = 1;
        uint16_t parseflags = ParseMaySig | ParseMayImpl | ParseInList;
        while (lexIsDclStart() || lexIsToken(IdentToken)) {
            if (count == max) {
                GenParm *grown = (GenParm *)memAllocBlk(max * 2 * sizeof(GenParm));
                memcpy(grown, parms, max * sizeof(GenParm));
                parms = grown;
                max *= 2;
            }
            GenParm *parm = &parms[count++];
            parm->start = lex->tokp;
            parm->hasperm = lexIsDclStart();
            VarDclNode *var = parseVarDcl(parse, immPerm, parseflags);
            parm->name = var->namesym;
            parm->nameend = parse->nameendp;
            parm->typed = parse->typed;
            parm->bare = var->vtype != unknownType && var->vtype->tag == RefTag && ((RefNode *)var->vtype)->vtexp == unknownType;
            parm->type = parse->typed ? parse->typep : parse->nameendp;
            parm->typeend = parse->typed ? parse->typeendp : parse->nameendp;
            parm->valueend = var->value && parse->bodyendp ? parse->bodyendp : parm->typeend;
            if (var->value)
                parseflags = ParseMayImpl | ParseInList;
            if (!lexIsToken(CommaToken))
                break;
            lexNextToken();
        }
    }
    lexPop();
    parse->inlist = svinlist;
    parse->lifesig = svlifesig;
    parse->typep = svtypep;
    parse->typeendp = svtypeendp;
    parse->bodyp = svbodyp;
    parse->bodyendp = svbodyendp;
    *parmsp = parms;
    return count;
}

// ---- The declaration ------------------------------------------------------------

GenCtx *parseGenBegin(ParseState *parse, FnDclNode *fn) {
    GenCtx *ctx = (GenCtx *)memAllocBlk(sizeof(GenCtx));
    char buf[300];
    const char *name = fn->namesym ? &fn->namesym->namestr : "";
    snprintf(buf, sizeof(buf), "%s.yield", name);
    ctx->some = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.none", name);
    ctx->none = nametblPrivate(buf, strlen(buf));
    ctx->name = fn->namesym;
    ctx->yields = 0;
    // A method: the type it is a method of, and its receiver, which the
    // generator holds in a field of its own
    ctx->recv = NULL;
    ctx->type = NULL;
    if (parse->typenode && ((INode *)parse->typenode)->tag == StructTag) {
        ctx->type = parse->typenode;
        Nodes *parms = fn->vtype->tag == FnSigTag ? ((FnSigNode *)fn->vtype)->parms : NULL;
        if (parms && parms->used > 0 && ((VarDclNode *)nodesGet(parms, 0))->namesym == selfName)
            ctx->recv = nametblPrivate("self", 4);
    }
    return ctx;
}

// In a generator method's body, 'self' is the receiver, which the generator
// holds in a field and not in its own 'self', and 'Self' is the type the method
// belongs to, not the generator's struct. A name use read there is made to
// name them
INode *parseGenName(ParseState *parse, NameUseNode *use) {
    GenCtx *ctx = parse->genctx;
    if (use->namesym == selfName && ctx->recv) {
        use->namesym = ctx->recv;
        return (INode *)use;
    }
    if (use->namesym == selfTypeName && ctx->type) {
        StructNode *type = (StructNode *)ctx->type;
        NameUseNode *name = newNameUseNode(type->namesym);
        inodeLexCopy((INode *)name, (INode *)use);
        if (type->genericinfo == NULL || type->genericinfo->parms->used == 0)
            return (INode *)name;
        // Its own parameters, which the generator's struct has under the same names
        FnCallNode *inst = newFnCallNode((INode *)name, 4);
        inst->flags |= FlagIndex;
        inodeLexCopy((INode *)inst, (INode *)use);
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(type->genericinfo->parms, cnt, nodesp)) {
            NameUseNode *arg = newNameUseNode(((GenVarDclNode *)*nodesp)->namesym);
            inodeLexCopy((INode *)arg, (INode *)use);
            nodesAdd(&inst->args, (INode *)arg);
        }
        return (INode *)inst;
    }
    return (INode *)use;
}

INode *parseGenNone(ParseState *parse) {
    FnCallNode *call = newFnCallNode((INode *)newNameUseNode(parse->genctx->none), 1);
    return (INode *)call;
}

// The body is the author's: appended with the end that hands the caller None,
// unless it ends in a 'return', which does
static void parseGenEnd(ParseState *parse, GenCtx *ctx, BlockNode *body) {
    if (body->stmts == NULL)
        body->stmts = newNodes(2);
    if (body->stmts->used > 0 && nodesLast(body->stmts)->tag == ReturnTag)
        return;
    GenCtx *sv = parse->genctx;
    parse->genctx = ctx;
    nodesAdd(&body->stmts, parseGenNone(parse));
    parse->genctx = sv;
}

// Find the member of a struct named 'name'
static INode *parseGenMember(StructNode *strnode, Name *name, int tag) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag == tag && inodeGetName(*nodesp) == name)
            return *nodesp;
    }
    return NULL;
}

// The lifetimes the type of a generator method declares, which the generator's
// struct declares too, to hold a receiver of that type: the ones in its
// brackets, in order, or, where it declares none and its fields name one, that
// one. Answers how many (0 for a type with none), and points *namesp at them
static uint32_t parseGenTypeLifes(StructNode *type, Name ***namesp) {
    LifeParms *parms = type ? type->lifeparms : NULL;
    if (parms == NULL)
        return 0;
    if (parms->count > 0) {
        *namesp = parms->names;
        return parms->count;
    }
    // Declared in no brackets: the one name its fields write, as they are read so far
    Name **one = (Name **)memAllocBlk(sizeof(Name *));
    one[0] = NULL;
    if (parms->usedat) {
        INode **np;
        uint32_t cnt;
        for (nodesFor(parms->usedat, cnt, np)) {
            Name *name = ((NameUseNode *)*np)->namesym;
            if (one[0] && one[0] != name)
                return 0;
            one[0] = name;
        }
    }
    *namesp = one;
    return one[0] ? 1 : 0;
}

// Does the type declare an invariant lifetime?
int parseGenTypeInvariant(StructNode *type) {
    Name **names = NULL;
    uint32_t count = parseGenTypeLifes(type, &names);
    for (uint32_t k = 0; k < count; ++k) {
        if (lifeIsInvariant(names[k]))
            return 1;
    }
    return 0;
}

// The lifetime the generator's borrows are given, 'a unless the type declares
// a lifetime of that name
static const char *parseGenBorrowLife(Name **tlifes, uint32_t ntlifes) {
    static const char *names[] = {"'a", "'b", "'c", "'d", "'e", "'f", "'g", "'h"};
    for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        uint32_t k;
        for (k = 0; k < ntlifes; ++k) {
            if (strcmp(&tlifes[k]->namestr, names[i]) == 0)
                break;
        }
        if (k == ntlifes)
            return names[i];
    }
    return names[0];
}

FnDclNode *parseGenFinish(ParseState *parse, FnDclNode *fn, GenSig *sig, GenCtx *ctx, BlockNode *body) {
    GenParm *parms;
    uint32_t nparms = parseGenParms(parse, sig, &parms);
    char *url = lex->url;
    ModuleNode *mod = parse->mod;

    // The parameters are fields of the struct that is 'self' in the body (a
    // method's receiver is the one exception: the generator holds it under a
    // name of its own, and 'self' in the body names that). Each is held under a
    // name no source can spell, so that no parameter collides with a member of
    // the struct or of Iterator, whatever it is called; only 'self' cannot be
    // a parameter's name, since it names the value the body runs in
    for (uint32_t i = 0; i < nparms; ++i) {
        int isrecv = i == 0 && ctx->recv != NULL;
        if (parms[i].name == selfName && !isrecv) {
            errorMsgNode((INode *)fn, ErrorGenForm,
                "A generator's parameter cannot be named 'self': 'self' in its body is the value the generator is, which holds the parameters. Name it otherwise.");
            return fn;
        }
        // The fields are named apart, so the struct no longer finds two alike
        for (uint32_t j = 0; j < i; ++j) {
            if (parms[j].name == parms[i].name) {
                errorMsgNode((INode *)fn, ErrorDupName,
                    "Duplicate name %s: every parameter of a generator needs its own name.", &parms[i].name->namestr);
                return fn;
            }
        }
    }

    // The type parameters the generator's struct has: its type's, then the
    // function's own, written as they are and then named
    StructNode *type = (StructNode *)ctx->type;
    Nodes *typenames = type && type->genericinfo ? type->genericinfo->parms : NULL;
    GenText tdecl = {NULL, 0, 0}, targs = {NULL, 0, 0}, typeuse = {NULL, 0, 0};
    // The lifetimes the type declares, which the struct declares too
    Name **tlifes = NULL;
    uint32_t ntlifes = parseGenTypeLifes(type, &tlifes);
    GenText lifeuse = {NULL, 0, 0};
    for (uint32_t k = 0; k < ntlifes; ++k) {
        if (k)
            genPuts(&lifeuse, ", ");
        genPutn(&lifeuse, &tlifes[k]->namestr, tlifes[k]->namesz);
    }
    if (parse->tparms && ((typenames && typenames->used > 0) || ntlifes > 0))
        genPutn(&tdecl, parse->tparms, parse->tparmsend - parse->tparms);
    else if (ntlifes > 0)
        genPutn(&tdecl, lifeuse.text, lifeuse.len);
    if (sig->tparms) {
        if (tdecl.len)
            genPuts(&tdecl, ", ");
        genPutn(&tdecl, sig->tparms, sig->tparmsend - sig->tparms);
    }
    for (int from = 0; from < 2; ++from) {
        Nodes *list = from == 0 ? typenames : sig->tnames;
        INode **np;
        uint32_t ncnt;
        if (list == NULL)
            continue;
        for (nodesFor(list, ncnt, np)) {
            if (targs.len)
                genPuts(&targs, ", ");
            genName(&targs, ((GenVarDclNode *)*np)->namesym);
            if (from == 0) {
                if (typeuse.len)
                    genPuts(&typeuse, ", ");
                genName(&typeuse, ((GenVarDclNode *)*np)->namesym);
            }
        }
    }

    // The names no source spells
    char genname[300];
    if (type)
        snprintf(genname, sizeof(genname), "%s.%s.Gen", &type->namesym->namestr, &fn->namesym->namestr);
    else
        snprintf(genname, sizeof(genname), "%s.Gen", &fn->namesym->namestr);
    // (kept by the lexer for the diagnostics against the text, so not on the stack)
    Name **names = (Name **)memAllocBlk((GenSlots + nparms) * sizeof(Name *));
    names[GenStruct] = nametblPrivate(genname, strlen(genname));
    names[GenState] = nametblPrivate("state'", 6);
    names[GenYield] = ctx->some;
    names[GenNone] = ctx->none;
    names[GenSub] = nametblPrivate("sub'", 4);
    names[GenSource] = nametblPrivate("src'", 4);
    names[GenItem] = nametblPrivate("item'", 5);
    names[GenRecv] = ctx->recv;
    // Each parameter's field: its own string, as no source's name (a name the
    // table does not hold), after the slots above. A receiver has GenRecv
    Name **parmuser = (Name **)memAllocBlk((nparms + 1) * sizeof(Name *));
    Name **parmhidden = (Name **)memAllocBlk((nparms + 1) * sizeof(Name *));
    uint32_t nhidden = 0;
    for (uint32_t i = 0; i < nparms; ++i) {
        if (i == 0 && ctx->recv != NULL) {
            names[GenSlots + i] = ctx->recv;
            continue;
        }
        names[GenSlots + i] = nametblPrivate(&parms[i].name->namestr, parms[i].name->namesz);
        parmuser[nhidden] = parms[i].name;
        parmhidden[nhidden++] = names[GenSlots + i];
    }

    // The type itself as the receiver's field names it, where the method wrote
    // 'self' or 'self: &' and left the type to be inferred
    GenText selftext = {NULL, 0, 0};
    if (type) {
        genName(&selftext, type->namesym);
        if (typeuse.len || lifeuse.len) {
            genPuts(&selftext, "[");
            genPutn(&selftext, lifeuse.text ? lifeuse.text : "", lifeuse.len);
            if (typeuse.len && lifeuse.len)
                genPuts(&selftext, ", ");
            genPutn(&selftext, typeuse.text ? typeuse.text : "", typeuse.len);
            genPuts(&selftext, "]");
        }
    }

    // Whether each borrow is given the one lifetime 'a: only when the
    // parameters hold borrows and none is written some way it cannot be
    GenText probe = {NULL, 0, 0};
    int prefs = 0, pbad = 0, yrefs = 0, ybad = 0;
    for (uint32_t i = 0; i < nparms; ++i)
        parseGenAnnotate(&probe, parms[i].type, parms[i].typeend, NULL, &prefs, &pbad);
    parseGenAnnotate(&probe, sig->ytype, sig->ytypeend, NULL, &yrefs, &ybad);
    // (the struct declares the type's lifetimes and the one lifetime the
    // borrows have, and no other, so a lifetime the author wrote would be undeclared)
    if (ntlifes > 0 && (pbad || ybad)) {
        errorMsgNode((INode *)fn, ErrorGenForm,
            "A generator method of a type that declares lifetimes cannot name a lifetime in its signature yet, nor write a borrow some way a lifetime cannot be put after: its borrows are given the one lifetime the generator's struct declares for them.");
        return fn;
    }
    const char *life = prefs > 0 && !pbad && !ybad ? parseGenBorrowLife(tlifes, ntlifes) : NULL;

    GenText g = {NULL, 0, 0};
    GenText ytype = {NULL, 0, 0};
    int dummy1 = 0, dummy2 = 0;
    parseGenAnnotate(&ytype, sig->ytype, sig->ytypeend, life, &dummy1, &dummy2);
    GenText yplain = {NULL, 0, 0};
    genPutn(&yplain, sig->ytype, sig->ytypeend - sig->ytype);

    // The struct. Where the type declares lifetimes the struct declares them
    // too, and the borrows' one lifetime beside them, which is none of the type's
    genPuts(&g, "struct ");
    genSlot(&g, GenStruct);
    if (tdecl.len || (life && ntlifes > 0)) {
        genPuts(&g, "[");
        if (life && ntlifes > 0) {
            genPuts(&g, life);
            if (tdecl.len)
                genPuts(&g, ", ");
        }
        genPutn(&g, tdecl.text ? tdecl.text : "", tdecl.len);
        genPuts(&g, "]");
    }
    // It declares what it is, an Iterator of what it yields, so it has the adapters
    genPuts(&g, " is Iterator[");
    genPutn(&g, yplain.text, yplain.len);
    genPuts(&g, "] {\n");
    for (uint32_t i = 0; i < nparms; ++i) {
        if (!parms[i].hasperm)
            genPuts(&g, "  let ");
        else
            genPuts(&g, "  ");
        // The permission, if written, then the name, then the type. A
        // receiver is held under a name of its own, and where its type was left
        // to be inferred it is written out
        char *typestart = parms[i].type;
        int isrecv = i == 0 && ctx->recv != NULL;
        // The name is the hidden one in either case; the author's text, a
        // back-ticked name's ticks included, is what it replaces
        char *namestart = parms[i].nameend - parms[i].name->namesz;
        if (parms[i].nameend[-1] == '`')
            namestart -= 2;
        genPutn(&g, parms[i].start, namestart - parms[i].start);
        genSlot(&g, isrecv ? (uint32_t)GenRecv : GenSlots + i);
        genPutn(&g, parms[i].nameend, typestart - parms[i].nameend);
        int d1 = 0, d2 = 0;
        parseGenAnnotate(&g, typestart, parms[i].typeend, life, &d1, &d2);
        if (isrecv && (!parms[i].typed || parms[i].bare)) {
            genPuts(&g, parms[i].typed ? " " : ": ");
            genPutn(&g, selftext.text, selftext.len);
        }
        genPuts(&g, ";\n");
    }
    genPuts(&g, "  ");
    genSlot(&g, GenState);
    genPuts(&g, ": u32;\n  pub fn next(self: &mut) Option[");
    genPutn(&g, ytype.text, ytype.len);
    genPuts(&g, "] {}\n  fn final(self: &uni) {}\n");

    // What a 'yield' hands the caller, and what the end does: functions of the
    // struct, so that each instance of a generic generator has its own
    genPuts(&g, "  fn ");
    genSlot(&g, GenYield);
    genPuts(&g, "(v: ");
    genPutn(&g, yplain.text, yplain.len);
    genPuts(&g, ") Option[");
    genPutn(&g, yplain.text, yplain.len);
    genPuts(&g, "] inline {Some[");
    genPutn(&g, yplain.text, yplain.len);
    genPuts(&g, "](v)}\n  fn ");
    genSlot(&g, GenNone);
    genPuts(&g, "() Option[");
    genPutn(&g, yplain.text, yplain.len);
    genPuts(&g, "] {None[");
    genPutn(&g, yplain.text, yplain.len);
    genPuts(&g, "]}\n}\n");

    // The generator, made: its parameters in the struct, unstarted
    genPuts(&g, "fn ");
    genName(&g, fn->namesym);
    if (sig->tparms) {
        genPuts(&g, "[");
        genPutn(&g, sig->tparms, sig->tparmsend - sig->tparms);
        genPuts(&g, "]");
    }
    genPuts(&g, "(");
    for (uint32_t i = 0; i < nparms; ++i) {
        if (i)
            genPuts(&g, ", ");
        // Where the type declares lifetimes, the constructor's result names
        // them, so each borrow it takes names the one lifetime the struct's
        // borrows have, and the receiver is written out to say what it holds
        if (ntlifes > 0 && life) {
            int isrecv = i == 0 && ctx->recv != NULL;
            genPutn(&g, parms[i].start, parms[i].type - parms[i].start);
            int d1 = 0, d2 = 0;
            parseGenAnnotate(&g, parms[i].type, parms[i].typeend, life, &d1, &d2);
            if (isrecv && parms[i].typed && parms[i].bare) {
                genPuts(&g, " ");
                genPutn(&g, selftext.text, selftext.len);
            }
            genPutn(&g, parms[i].typeend, parms[i].valueend - parms[i].typeend);
            continue;
        }
        genPutn(&g, parms[i].start, parms[i].valueend - parms[i].start);
    }
    genPuts(&g, ") ");
    genSlot(&g, GenStruct);
    if (targs.len || ntlifes > 0) {
        genPuts(&g, "[");
        if (ntlifes > 0) {
            if (life) {
                genPuts(&g, life);
                genPuts(&g, ", ");
            }
            genPutn(&g, lifeuse.text, lifeuse.len);
            if (targs.len)
                genPuts(&g, ", ");
        }
        genPutn(&g, targs.text ? targs.text : "", targs.len);
        genPuts(&g, "]");
    }
    genPuts(&g, " {new ");
    genSlot(&g, GenStruct);
    if (targs.len) {
        genPuts(&g, "[");
        genPutn(&g, targs.text, targs.len);
        genPuts(&g, "]");
    }
    genPuts(&g, "(");
    for (uint32_t i = 0; i < nparms; ++i) {
        genName(&g, parms[i].name);
        genPuts(&g, ", ");
    }
    genPuts(&g, "0u32)}\n");

    // Parse it, as the generator's own source, its diagnostics reported at its name
    Lexer *gen = lexNew(g.text, url);
    gen->genat = (INode *)fn;
    gen->gennames = names;
    gen->ngennames = GenSlots + nparms;
    lexPush(gen);
    StructNode *state = (StructNode *)parseStruct(parse, 0);
    modAddNode(mod, state->namesym, (INode *)state);
    FnDclNode *nonefn = (FnDclNode *)parseGenMember(state, names[GenNone], FnDclTag);
    FnDclNode *ctor = (FnDclNode *)parseFn(parse, ParseMayName | ParseMayImpl);
    ctor->overloadsym = fn->overloadsym;    // 'fn forward overload iter(self: &) yields &T'
    if (!lexIsToken(EofToken))
        errorMsgLex(ErrorNoEof, "The declarations generated for generator %s did not parse whole.", &fn->namesym->namestr);
    lexPop();

    // 'next' takes the author's body
    FnDclNode *step = (FnDclNode *)parseGenMember(state, nextName, FnDclTag);
    if (step == NULL) {
        errorUnreachable((INode *)fn, "a generator whose struct generated no 'next'");
        return ctor;
    }
    parseGenEnd(parse, ctx, body);
    step->value = (INode *)body;
    inodeLexCopy((INode *)step, (INode *)fn);
    GenInfo *info = yieldGenNew(step, state, nonefn);
    info->ctor = ctor;
    info->recv = ctx->recv;
    info->recvtype = (StructNode *)ctx->type;
    info->parmuser = parmuser;
    info->parmhidden = parmhidden;
    info->nparms = nhidden;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&state->fields, cnt, nodesp)) {
        if (((FieldDclNode *)*nodesp)->namesym == names[GenState])
            info->state = (FieldDclNode *)*nodesp;
    }
    return ctor;
}

// ---- yield ----------------------------------------------------------------------

// 'yield each src': a loop over the source's 'next', which yields each value.
// Read from text, with the source written put where the text has its
// placeholder. The sub-generator is a local of the loop's block, kept in the
// frame across the seams, so that a generator whose sub-generator is its own
// kind is a type of unknowable size, where it is held on the heap instead (the
// frame, genlyield.c)
static INode *parseYieldEach(ParseState *parse, INode *src, YieldNode *at) {
    // Names no source can spell: the local, the source's placeholder, the match's binding
    static Name *names[GenSlots];
    if (names[GenSub] == NULL) {
        names[GenSub] = nametblPrivate("sub'", 4);
        names[GenSource] = nametblPrivate("src'", 4);
        names[GenItem] = nametblPrivate("item'", 5);
    }
    GenText g = {NULL, 0, 0};
    genPuts(&g, "{ var ");
    genSlot(&g, GenSub);
    genPuts(&g, " = ");
    genSlot(&g, GenSource);
    genPuts(&g, "; while true { match ");
    genSlot(&g, GenSub);
    genPuts(&g, ".next() { case let ");
    genSlot(&g, GenItem);
    genPuts(&g, " Some { yield ");
    genSlot(&g, GenItem);
    genPuts(&g, "; } case is None { break; } } } }");
    Lexer *gen = lexNew(g.text, lex->url);
    gen->genat = (INode *)at;
    gen->gennames = names;
    gen->ngennames = GenSlots;
    lexPush(gen);
    // The binding holds the item the sub-generator gave, which 'yield' hands the
    // caller in a Some of its own
    BlockNode *blk = (BlockNode *)parseExprBlock(parse, 0);
    lexPop();
    VarDclNode *sub = (VarDclNode *)nodesGet(blk->stmts, 0);
    sub->value = src;
    inodeLexCopy((INode *)sub, src);
    return (INode *)blk;
}

INode *parseYield(ParseState *parse) {
    YieldNode *node = newYieldNode();
    GenCtx *ctx = parse->genctx;
    lexNextToken(); // Skip past 'yield'
    int each = 0;
    if (lexIsToken(EachToken)) {
        each = 1;
        lexNextToken();
    }
    if (ctx == NULL) {
        errorMsgNode((INode *)node, ErrorYieldPlace,
            "'yield' stands only in the body of a function declared with 'yields', a generator: 'fn walk(t: &Tree) yields &Node { ... yield n; ... }'.");
        if (!parseIsEndOfStatement())
            parseAnyExpr(parse);
        parseEndOfStatement();
        node->exp = (INode *)newNilLitNode();
        return (INode *)node;
    }
    if (parse->genoperand > 0) {
        errorMsgNode((INode *)node, ErrorYieldPlace,
            "A 'yield' is a statement of its own, not part of an expression: it stands in a block or in the arms of an 'if' or 'match' written as statements, since a value made before it in the same expression would have to be kept across the seam. Compute the value into a variable first.");
        if (!parseIsEndOfStatement())
            parseAnyExpr(parse);
        parseEndOfStatement();
        node->exp = (INode *)newNilLitNode();
        return (INode *)node;
    }
    if (parseIsEndOfStatement()) {
        errorMsgLex(ErrorBadTerm, each ? "'yield each' needs the sub-generator whose values it hands on." : "'yield' needs the value to hand the caller.");
        node->exp = (INode *)newNilLitNode();
        parseEndOfStatement();
        return (INode *)node;
    }
    INode *exp = parseAnyExpr(parse);
    parseEndOfStatement();
    if (each)
        return parseYieldEach(parse, exp, node);
    ++ctx->yields;
    FnCallNode *some = newFnCallNode((INode *)newNameUseNode(ctx->some), 1);
    nodesAdd(&some->args, exp);
    inodeLexCopy((INode *)some, exp);
    node->exp = (INode *)some;
    return (INode *)node;
}
