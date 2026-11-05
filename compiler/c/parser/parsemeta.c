/** The meta layer's '#if' on facts of the build
 *
 * '#if', '#elif' and '#else' choose which code exists, from facts the compiler
 * knows before it reads a line of source: the target's operating system and
 * architecture, whether this is a debug build, and the names and numbers '-D'
 * gave. They are resolved here, while parsing: the taken branch is parsed into
 * the enclosing list as though it were written there, and an untaken branch is
 * parsed only to be sure it is well-formed, then dropped. Nothing in an untaken
 * branch is ever looked up, so code for another platform may name things that
 * do not exist on this one.
 *
 * This file owns the condition, the chain and the scratch an untaken branch is
 * read into. The four places a list of items is parsed -- a module, a type's
 * braces, an 'extern' block and a block of statements -- each call parseHashIf
 * with the function that reads their own items (compiler/c/doc/phases/parse.md,
 * "The meta layer").
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ir/ir.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../shared/fileio.h"
#include "../ir/nametbl.h"
#include "../coneopts.h"
#include "lexer.h"

#include <stdio.h>
#include <string.h>

// The options the facts are read from, set once by parseInit. The target
// triple is filled in by generation's setup, which runs before any source is
// parsed, so facts are read from the options when they are asked for
static ConeOptions *metaOpt = NULL;

// Inside the second or later pass of a '#each' (below): what the first pass noted for the include
// file is noted, and the same text read again adds nothing to it
static int metaRepeat = 0;

void parseMetaInit(ConeOptions *opt) {
    metaOpt = opt;
}

// ----------------------------------------------------------------------------
// The facts

// What a fact's value is. A fact is an enumeration, not a string: '#os' is one of
// a closed set of members, '.windows' among them, and a member the set lacks is
// an error rather than a comparison that is quietly false
typedef enum {
    MetaBool,       // true or false: '#debug', '#defined("NAME")', a comparison
    MetaInt,        // a number: '#define("NAME")', a literal
    MetaOs,         // a member of '#os'
    MetaArch,       // a member of '#arch'
    MetaMember,     // '.name' not yet known to belong to either: the other side of '==' says
    MetaType,       // A bool not known yet: what a type parameter is ('T is Float'), decided
                    // when a generic is cloned for its arguments. 'tree' is the condition
    MetaBad         // reported already
} MetaKind;

typedef struct MetaVal {
    MetaKind kind;
    int64_t val;        // The bool, the number or the member's index
    Name *member;       // MetaMember: the name written after the '.'
    INode *tree;        // MetaType: the clauses, as a 'where' condition holds them
} MetaVal;

// The parse in progress, for the type names a condition holds
static ParseState *metaParse = NULL;

static const char *metaOsNames[] = {"windows", "linux", "macos", "wasm", "gpu", NULL};
static const char *metaArchNames[] = {"x64", "arm64", "wasm32", "spirv", NULL};

static int metaTripleHas(const char *word) {
    const char *triple = metaOpt ? metaOpt->triple : NULL;
    return triple != NULL && strstr(triple, word) != NULL;
}

static int metaTripleStarts(const char *word) {
    const char *triple = metaOpt ? metaOpt->triple : NULL;
    return triple != NULL && strncmp(triple, word, strlen(word)) == 0;
}

// The index in metaOsNames of the build's operating system, or -1 for one the
// vocabulary does not name. A SPIR-V target is a GPU, and a WebAssembly one is
// 'wasm', whatever else its triple says
static int metaOsIndex() {
    if (metaOpt && metaOpt->gpu)
        return 4;
    if (metaTripleStarts("wasm"))
        return 3;
    if (metaTripleHas("windows") || metaTripleHas("win32"))
        return 0;
    if (metaTripleHas("linux"))
        return 1;
    if (metaTripleHas("darwin") || metaTripleHas("macos"))
        return 2;
    return -1;
}

// The index in metaArchNames of the build's architecture (the triple's first
// part), or -1
static int metaArchIndex() {
    if (metaTripleStarts("x86_64") || metaTripleStarts("amd64"))
        return 0;
    if (metaTripleStarts("aarch64") || metaTripleStarts("arm64"))
        return 1;
    if (metaTripleStarts("wasm32"))
        return 2;
    if (metaTripleStarts("spirv"))
        return 3;
    return -1;
}

// The '-D' that defined a name, or NULL
static ConeDefine *metaDefineFind(const char *name, uint32_t len) {
    if (metaOpt == NULL)
        return NULL;
    for (int i = 0; i < metaOpt->ndefines; ++i) {
        ConeDefine *def = &metaOpt->defines[i];
        if (strlen(def->name) == len && memcmp(def->name, name, len) == 0)
            return def;
    }
    return NULL;
}

// The index of a member among a fact's names, or -1
static int metaMemberIndex(const char **names, Name *member) {
    for (int i = 0; names[i]; ++i)
        if (strcmp(&member->namestr, names[i]) == 0)
            return i;
    return -1;
}

static const char *metaKindName(MetaKind kind) {
    switch (kind) {
    case MetaBool: return "a bool";
    case MetaInt: return "a number";
    case MetaOs: return "an '#os' member";
    case MetaArch: return "an '#arch' member";
    case MetaType: return "a question of what a type is";
    default: return "a member";
    }
}

// Say which members a fact has, for a member it has not
static void metaMembersMsg(char *buf, size_t size, const char **names) {
    buf[0] = 0;
    for (int i = 0; names[i]; ++i) {
        strncat(buf, i ? ", ." : ".", size - strlen(buf) - 1);
        strncat(buf, names[i], size - strlen(buf) - 1);
    }
}

// ----------------------------------------------------------------------------
// The condition

static MetaVal metaOr(void);

// The count of dropped '#' words when the condition being read began
static uint32_t metaDropMark = 0;

static MetaVal metaVal(MetaKind kind, int64_t val) {
    MetaVal v;
    v.kind = kind;
    v.val = val;
    v.member = NULL;
    v.tree = NULL;
    return v;
}

static void metaSkipCond();

static const char *metaCondForms =
    "A '#if' condition is built from the build's facts -- #os, #arch, #debug, #defined(\"NAME\") and #define(\"NAME\") -- compared with '==' or '!=' and joined with '!', '&&' and '||'. Nothing else is known while the program is being read. (In a generic, it may also ask what a type parameter is: 'T is Float', 'T == i32'.)";

// A condition about what a type is, 'tree', which a generic's clone decides
static MetaVal metaTypeVal(INode *tree) {
    MetaVal v = metaVal(MetaType, 0);
    v.tree = tree;
    return v;
}

// 'T is Name' or 'T == Name' (or '!=') with the lexer on the type parameter's
// name: a clause as a 'where' clause writes one, 'T == Name' being 'T is Name'
// where Name is a type, which a type is met by alone. Whether it holds is
// known only when a generic is cloned for its arguments (ifHashClone)
static INode *metaTypeAt = NULL;   // The first question about a type the condition being read has asked

static INode *metaFieldSubject(void);

// 'subject' is read already, where it is a field's '#f.type' (a '#each #f in #fields(T)' loop's)
static MetaVal metaTypeFact(INode *given) {
    INode *subject = given;
    if (subject == NULL) {
        subject = (INode*)newNameUseNode(lex->val.ident);
        lexNextToken();
    }
    if (metaTypeAt == NULL)
        metaTypeAt = subject;
    int negate = 0;
    if (lexIsToken(NeToken)) {
        negate = 1;
        lexNextToken();
    }
    else if (lexIsToken(EqToken) || lexIsToken(IsToken))
        lexNextToken();
    else {
        errorMsgNode(subject, ErrorMetaFact, "%s", metaCondForms);
        metaSkipCond();
        return metaVal(MetaBad, 0);
    }
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorMetaFact, "What a type parameter is, in a '#if' condition, is a trait or a type, named: 'T is Float', 'T == i32'.");
        metaSkipCond();
        return metaVal(MetaBad, 0);
    }
    INode *rhs = parseTypeName(metaParse);
    CastNode *clause = newIsNode(subject, rhs);
    inodeLexCopy((INode*)clause, subject);
    INode *tree = (INode*)clause;
    if (negate) {
        LogicNode *not = newLogicNode(NotLogicTag);
        inodeLexCopy((INode*)not, subject);
        not->lexp = tree;
        tree = (INode*)not;
    }
    return metaTypeVal(tree);
}

// Skip the rest of a condition that could not be read, to its block, so that
// what is left of it is not reported as well
static void metaSkipCond() {
    while (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken) && !lexIsToken(RCurlyToken)
        && !lexIsToken(EofToken))
        lexNextToken();
}

// Settle a '.name' against the fact it is compared with
static MetaVal metaResolve(MetaVal member, MetaKind kind) {
    const char **names = kind == MetaOs ? metaOsNames : metaArchNames;
    int idx = metaMemberIndex(names, member.member);
    if (idx < 0) {
        char buf[200];
        metaMembersMsg(buf, sizeof(buf), names);
        errorMsgLex(ErrorMetaFact, "'%s' has no member '.%s'. Its members are %s.",
            kind == MetaOs ? "#os" : "#arch", &member.member->namestr, buf);
        return metaVal(MetaBad, 0);
    }
    return metaVal(kind, idx);
}

// The parenthesised string literal of '#defined("NAME")' and '#define("NAME")',
// with the lexer on the word. Answers whether the '-D' named is there, and its
// value (0 where it is not, as C's '#if' reads an undefined name)
static MetaVal metaDefine(int isdefined) {
    const char *word = isdefined ? "#defined" : "#define";
    lexNextToken();
    if (!lexIsToken(LParenToken)) {
        errorMsgLex(ErrorMetaIf, "'%s' takes the name in parentheses: '%s(\"NAME\")'.", word, word);
        metaSkipCond();
        return metaVal(MetaBad, 0);
    }
    lexNextToken();
    if (!lexIsToken(StringLitToken)) {
        errorMsgLex(ErrorMetaFact, "'%s' takes a string literal naming the '-D', as '%s(\"NAME\")': the name is read at compile time, so it cannot be a variable.", word, word);
        metaSkipCond();
        return metaVal(MetaBad, 0);
    }
    ConeDefine *def = metaDefineFind(lex->val.strlit, lex->strlen);
    lexNextToken();
    if (!lexIsToken(RParenToken)) {
        errorMsgLex(ErrorMetaIf, "'%s(\"NAME\")' is closed by a ')' after the name.", word);
        metaSkipCond();
        return metaVal(MetaBad, 0);
    }
    lexNextToken();
    if (isdefined)
        return metaVal(MetaBool, def != NULL);
    return metaVal(MetaInt, def ? def->value : 0);
}

// An operand: a fact, a literal, a member '.name' or a parenthesised condition.
// 'hint' is the kind of the left side of a comparison, which a member written on
// the right is checked against where it stands
static uint32_t metaBuildReads = 0;   // Facts of the build read so far; parseHashIf compares it before and after a condition

static MetaVal metaPrimary(MetaKind hint) {
    switch (lex->toktype) {
    case LParenToken: {
        lexNextToken();
        MetaVal inner = metaOr();
        if (lexIsToken(RParenToken))
            lexNextToken();
        else {
            errorMsgLex(ErrorMetaIf, "A '(' in a '#if' condition is closed by a ')' after what it groups.");
            metaSkipCond();
            return metaVal(MetaBad, 0);
        }
        return inner;
    }
    case HashDebugToken:
        ++metaBuildReads;
        lexNextToken();
        return metaVal(MetaBool, !(metaOpt && metaOpt->release));
    case HashOsToken:
        ++metaBuildReads;
        lexNextToken();
        return metaVal(MetaOs, metaOsIndex());
    case HashArchToken:
        ++metaBuildReads;
        lexNextToken();
        return metaVal(MetaArch, metaArchIndex());
    case HashDefinedToken:
        ++metaBuildReads;
        return metaDefine(1);
    case HashDefineToken:
        ++metaBuildReads;
        return metaDefine(0);
    case trueToken:
        lexNextToken();
        return metaVal(MetaBool, 1);
    case falseToken:
        lexNextToken();
        return metaVal(MetaBool, 0);
    case IntLitToken: {
        MetaVal v = metaVal(MetaInt, (int64_t)lex->val.uintlit);
        lexNextToken();
        return v;
    }
    case DashToken: {
        lexNextToken();
        if (!lexIsToken(IntLitToken)) {
            errorMsgLex(ErrorMetaFact, "A '-' in a '#if' condition signs a number.");
            metaSkipCond();
            return metaVal(MetaBad, 0);
        }
        MetaVal v = metaVal(MetaInt, -(int64_t)lex->val.uintlit);
        lexNextToken();
        return v;
    }
    case DotToken: {
        lexNextToken();
        if (!lexIsToken(IdentToken)) {
            errorMsgLex(ErrorMetaFact, "A '.' in a '#if' condition names a member of the fact it is compared with: '#os == .linux'.");
            metaSkipCond();
            return metaVal(MetaBad, 0);
        }
        MetaVal member = metaVal(MetaMember, 0);
        member.member = lex->val.ident;
        // Checked here, where the name is, when the left side says what it is a member of
        if (hint == MetaOs || hint == MetaArch)
            member = metaResolve(member, hint);
        lexNextToken();
        return member;
    }
    // A type parameter and what it is, in a generic: decided when the generic is cloned
    case IdentToken:
        return metaTypeFact(NULL);
    // A field of a '#fields' loop: '#if #f.type is Copy', its type asked what it is
    case MetaVarToken:
        if (parseMetaIsFieldVar(lex->val.ident)) {
            INode *subject = metaFieldSubject();
            if (subject == NULL) {
                metaSkipCond();
                return metaVal(MetaBad, 0);
            }
            return metaTypeFact(subject);
        }
        // A '#each' variable is not a type parameter: what it stands for is known as the passes are
        // read, but asking about it ('#T is Float') belongs to the layer's reflection
        errorMsgLex(ErrorMetaFact, "A '#if' condition asks about the build's facts and, in a generic, about what a type parameter is; it cannot ask about a meta variable ('%s').",
            &lex->val.ident->namestr);
        metaSkipCond();
        return metaVal(MetaBad, 0);
    // A layout fact is the code generator's constant, so a '#if' cannot be decided by it yet
    case HashSizeToken:
    case HashAlignToken:
        errorMsgLex(ErrorMetaLayoutConst, "'%s' is the code generator's constant, which the front end has not worked out, so it cannot decide a '#if' yet. Ask it in an expression the program runs ('if %s(T) > 16 {..}'), where the code generator folds it.",
            lexIsToken(HashAlignToken) ? "#align" : "#size", lexIsToken(HashAlignToken) ? "#align" : "#size");
        metaSkipCond();
        return metaVal(MetaBad, 0);
    default:
        // A '#' word the lexer refused took its place: that was reported
        if (lexHashDropped != metaDropMark) {
            metaSkipCond();
            return metaVal(MetaBad, 0);
        }
        errorMsgLex(ErrorMetaFact, "%s", metaCondForms);
        metaSkipCond();
        return metaVal(MetaBad, 0);
    }
}

static MetaVal metaUnary(MetaKind hint) {
    if (lexIsToken(NotToken)) {
        lexNextToken();
        MetaVal v = metaUnary(MetaBad);
        if (v.kind == MetaBad)
            return v;
        if (v.kind == MetaType) {
            LogicNode *not = newLogicNode(NotLogicTag);
            inodeLexCopy((INode*)not, v.tree);
            not->lexp = v.tree;
            return metaTypeVal((INode*)not);
        }
        if (v.kind != MetaBool) {
            errorMsgLex(ErrorMetaFact, "'!' takes a bool, and this is %s. A comparison is negated in parentheses, '!(#os == .linux)', or written with '!='.", metaKindName(v.kind));
            return metaVal(MetaBad, 0);
        }
        return metaVal(MetaBool, !v.val);
    }
    return metaPrimary(hint);
}

// A comparison: '==' or '!=' of two operands of one kind
static MetaVal metaCompare() {
    MetaVal lhs = metaUnary(MetaBad);
    if (!lexIsToken(EqToken) && !lexIsToken(NeToken))
        return lhs;
    int negate = lexIsToken(NeToken);
    lexNextToken();
    MetaVal rhs = metaUnary(lhs.kind);
    if (lhs.kind == MetaBad || rhs.kind == MetaBad)
        return metaVal(MetaBad, 0);
    // A member written on the left is a member of what the right side is
    if (lhs.kind == MetaMember) {
        if (rhs.kind != MetaOs && rhs.kind != MetaArch) {
            errorMsgLex(ErrorMetaFact, "A member '.%s' is compared with '#os' or '#arch', whose members it names.", &lhs.member->namestr);
            return metaVal(MetaBad, 0);
        }
        lhs = metaResolve(lhs, rhs.kind);
        if (lhs.kind == MetaBad)
            return lhs;
    }
    if (rhs.kind == MetaMember) {
        errorMsgLex(ErrorMetaFact, "A member '.%s' is compared with '#os' or '#arch', whose members it names.", &rhs.member->namestr);
        return metaVal(MetaBad, 0);
    }
    if (lhs.kind != rhs.kind) {
        errorMsgLex(ErrorMetaFact, "A comparison needs two of one kind, and this compares %s with %s.",
            metaKindName(lhs.kind), metaKindName(rhs.kind));
        return metaVal(MetaBad, 0);
    }
    return metaVal(MetaBool, (lhs.val == rhs.val) != negate);
}

// '&&' or '||' of two operands, each a bool of the build or a question about a
// type. A build fact is known now, so it folds: it decides the join (false for
// '&&', true for '||') or it drops out and the question is what is left; two
// questions are joined as a 'where' condition joins its clauses
static MetaVal metaJoin(int isand, MetaVal lhs, MetaVal rhs) {
    if (lhs.kind == MetaBad || rhs.kind == MetaBad)
        return metaVal(MetaBad, 0);
    if ((lhs.kind != MetaBool && lhs.kind != MetaType) || (rhs.kind != MetaBool && rhs.kind != MetaType)) {
        errorMsgLex(ErrorMetaFact, "'%s' joins two bools, and this joins %s and %s.", isand ? "&&" : "||",
            metaKindName(lhs.kind), metaKindName(rhs.kind));
        return metaVal(MetaBad, 0);
    }
    if (lhs.kind == MetaBool && rhs.kind == MetaBool)
        return metaVal(MetaBool, isand ? lhs.val && rhs.val : lhs.val || rhs.val);
    if (lhs.kind == MetaBool)
        return (lhs.val != 0) == !isand ? metaVal(MetaBool, lhs.val) : rhs;
    if (rhs.kind == MetaBool)
        return (rhs.val != 0) == !isand ? metaVal(MetaBool, rhs.val) : lhs;
    LogicNode *join = newLogicNode(isand ? AndLogicTag : OrLogicTag);
    inodeLexCopy((INode*)join, lhs.tree);
    join->lexp = lhs.tree;
    join->rexp = rhs.tree;
    return metaTypeVal((INode*)join);
}

static MetaVal metaAnd() {
    MetaVal lhs = metaCompare();
    while (lexIsAmpAmp()) {
        lexNextToken();
        lexNextToken();
        MetaVal rhs = metaCompare();
        lhs = metaJoin(1, lhs, rhs);
    }
    return lhs;
}

static MetaVal metaOr() {
    MetaVal lhs = metaAnd();
    while (lexIsToken(OrOrToken)) {
        lexNextToken();
        MetaVal rhs = metaAnd();
        lhs = metaJoin(0, lhs, rhs);
    }
    return lhs;
}

// Read a condition, with the lexer on its first token: whether it holds. A
// condition that is not a bool, or could not be read, is reported and does not
// hold
//
// A condition that asks what a type parameter is holds for a build or does not
// only once a generic is cloned: it answers MetaDeferred, and the clauses are in
// 'metaCondTree'
static INode *metaCondTree = NULL;

static int metaCond() {
    MetaVal v = metaOr();
    if (v.kind == MetaBad)
        return 0;
    if (v.kind == MetaType) {
        metaCondTree = v.tree;
        return MetaDeferred;
    }
    if (v.kind != MetaBool) {
        errorMsgLex(ErrorMetaFact, "A '#if' condition is a bool, and this is %s. Compare it: '#os == .linux'.", metaKindName(v.kind));
        return 0;
    }
    return (int)v.val;
}

// Pass the '#if' or '#elif' the lexer is on and read its condition: whether it
// holds, or MetaDeferred, a question about a type parameter whose clauses are
// then in metaCondTree. An unknown '#' word in it is dropped alone
// (lexInHashCond), and that is already reported, so the rest of the condition is
// read past quietly
static int metaReadCond() {
    metaDropMark = lexHashDropped;
    lexInHashCond = 1;
    lexNextToken();
    int holds = metaCond();
    lexInHashCond = 0;
    return holds;
}

// A fact met anywhere but in a '#if' condition (parseTerm)
void parseMetaFactStray(void) {
    errorMsgLex(ErrorMetaFact, "'%s' is a fact of the build, read in the condition of a '#if'. It is not a value the program holds.",
        &lex->val.ident->namestr);
    lexNextToken();
}

// ----------------------------------------------------------------------------
// The scratch an untaken branch is read into

// Read an untaken branch with everything it would make thrown away: its
// declarations join a module that keeps and binds nothing, and the notes the
// parser takes of a body (an actor's text and awaits, a generator's yields) are
// put back as they were. Its errors are real, and are counted.
typedef struct MetaScratch {
    ModuleNode *mod;            // parse->mod, put back at the end
    INsTypeNode *typenode;      // parse->typenode, likewise
    DclTexts *dcl;             // An actor's notes, and what they stood at
    uint32_t dcount, dawaits, dselfactors, dawaiting, dbehaviours;
    GenCtx *gen;
    int yields;
} MetaScratch;

static ModuleNode *metaScratchMod = NULL;

static void metaScratchBegin(ParseState *parse, MetaScratch *s) {
    s->mod = parse->mod;
    s->typenode = parse->typenode;
    if (metaScratchMod == NULL) {
        metaScratchMod = newModuleNode();
        metaScratchMod->scratch = 1;
    }
    ModuleNode *scratch = metaScratchMod;
    scratch->spans = NULL;
    if (s->mod) {
        scratch->namesym = s->mod->namesym;
        // A name the compiler adopts as its own is adopted in core alone
        if (scratch->namesym == nametblFind("core", 4))
            scratch->namesym = nametblFind("core#", 5);
        scratch->filesym = s->mod->filesym;
        scratch->foldersym = s->mod->foldersym;
        scratch->dclinfo = s->mod->dclinfo;
        inodeLexCopy((INode*)scratch, (INode*)s->mod);
        scratch->lexer = lex;
    }
    parse->mod = scratch;
    s->dcl = parse->dcltexts;
    if (s->dcl) {
        s->dcount = s->dcl->count;
        s->dawaits = s->dcl->awaits;
        s->dselfactors = s->dcl->selfactors;
        s->dawaiting = s->dcl->awaiting ? s->dcl->awaiting->used : 0;
        s->dbehaviours = s->dcl->behaviours ? s->dcl->behaviours->used : 0;
    }
    s->gen = parse->genctx;
    s->yields = s->gen ? s->gen->yields : 0;
}

static void metaScratchEnd(ParseState *parse, MetaScratch *s) {
    parse->mod = s->mod;
    parse->typenode = s->typenode;
    if (s->dcl) {
        s->dcl->count = s->dcount;
        s->dcl->awaits = s->dawaits;
        s->dcl->selfactors = s->dselfactors;
        if (s->dcl->awaiting)
            s->dcl->awaiting->used = s->dawaiting;
        if (s->dcl->behaviours)
            s->dcl->behaviours->used = s->dbehaviours;
    }
    if (s->gen)
        s->gen->yields = s->yields;
}

// ----------------------------------------------------------------------------
// The chain

// The conditions of the chain being read that ask what a type is, in the order
// written, up to the branch being read; a NULL entry is its '#else'
typedef struct MetaChain {
    Nodes *conds;
} MetaChain;

static MetaChain *metaCurChain = NULL;

// A copy of an unresolved condition, so that two members may each hold their own
static INode *metaCopy(INode *node) {
    CloneState cstate;
    uint32_t dclpos = cloneDclPush();
    clonePushState(&cstate, NULL, NULL, 0, NULL, NULL);
    INode *copy = cloneNode(&cstate, node);
    clonePopState();
    cloneDclPop(dclpos);
    return copy;
}

// The condition a member of a deferred branch exists under, a copy of its own: the
// branch's condition, with each condition before it in the chain negated. A branch
// is called with the chain being read, and asks this once its members are read
INode *parseMetaMemberCond(void) {
    Nodes *conds = metaCurChain->conds;
    INode *result = NULL;
    for (uint32_t j = 0; j < conds->used; ++j) {
        INode *cond = nodesGet(conds, j);
        INode *term = NULL;
        if (j + 1 == conds->used)
            term = cond ? metaCopy(cond) : NULL;
        else {
            LogicNode *not = newLogicNode(NotLogicTag);
            inodeLexCopy((INode*)not, cond);
            not->lexp = metaCopy(cond);
            term = (INode*)not;
        }
        if (term == NULL)
            continue;
        if (result == NULL)
            result = term;
        else {
            LogicNode *both = newLogicNode(AndLogicTag);
            inodeLexCopy((INode*)both, result);
            both->lexp = result;
            both->rexp = term;
            result = (INode*)both;
        }
    }
    return result;
}

// A '#elif' or '#else' that no '#if' chain is waiting for: reported, and its
// condition and block read past so that what is inside is not reported as well
static void metaEachSkip(void);

void parseMetaStray(ParseState *parse) {
    // A '#each' where a function's statements are not: at a module's level, in a type's braces, in an
    // 'extern' block, or inside an expression
    if (lexIsToken(HashEachToken)) {
        errorMsgLex(ErrorMetaEachPlace, "A '#each' repeats the statements of a function's body. It is not built at a module's level, in a type's braces (other than a generic struct's, where only '#each #f in #fields(T) { ... }' makes members), in an 'extern' block or inside an expression other than a call's argument list, where '#each #f in #fields(T) { #f: value }' makes named arguments: generating declarations waits for generated names.");
        metaEachSkip();
        return;
    }
    int iselif = lexIsToken(HashElifToken);
    errorMsgLex(ErrorMetaIf, iselif
        ? "'#elif' continues a '#if' or another '#elif' and has nothing to continue here. After an '#else' the chain is over."
        : "'#else' ends a '#if' chain and has nothing to end here. After an '#else' the chain is over.");
    metaParse = parse;
    if (iselif)
        metaReadCond();
    else
        lexNextToken();
    if (lexIsToken(LCurlyToken)) {
        int depth = 0;
        do {
            if (lexIsToken(LCurlyToken))
                ++depth;
            else if (lexIsToken(RCurlyToken))
                --depth;
            else if (lexIsToken(EofToken))
                return;
            lexNextToken();
        } while (depth > 0);
    }
}

// A '#if' chain, with the lexer on the '#if': '#if c {..} #elif c {..} #else {..}'.
// Each branch is read by 'branch', which is called with the lexer after the
// branch's '{' and must read through the matching '}'. 'taken' says whether it
// is the branch the build takes: if it is, 'branch' adds what it reads to the
// enclosing list; if it is not, parse->mod is a scratch module and 'branch'
// reads into scratch of its own for everything else it would add to. A condition
// is read in every branch, so that a fact misspelled in one the build does not
// take is still refused
//
// A condition that asks what a type parameter is cannot be decided here: it is
// decided when a generic is cloned for its arguments (ifHashClone). Where the
// list can hold such a chain ('nodefer' is NULL), the branches it leads to are
// read as real code (mode MetaDeferred), and the chain is given back as a '#if'
// node of the 'if' kind whose blocks the branch function returned; a place that
// keeps its branches in the enclosing list itself (a type's braces) returns NULL
// and has the branch's own condition from parseMetaMemberCond. Where it cannot
// ('nodefer' is why), the condition is refused and its branch read as one not
// taken. A branch of the same chain that follows a deferred one and holds
// whatever the type is (an '#else', or a condition of the build that holds) ends
// that chain as its 'else'; the build's facts before it are folded in already.
INode *parseHashIf(ParseState *parse, MetaBranchFn branch, void *ctx, const char *nodefer, const char *nomix) {
    metaParse = parse;
    int done = 0;       // A branch has been taken, or the deferred chain has its 'else'
    Lexer *lexer = lex;
    char *chainat = lex->tokp;
    char *openend = NULL;       // Just past the taken branch's '{'
    char *closeat = NULL;       // The taken branch's '}'
    IfNode *ifnode = NULL;      // The deferred chain, once a branch is
    int deferredany = 0;
    int buildany = 0;           // A condition of the chain rests on a fact of the build
    INode *firsttype = NULL;    // The first condition that asks about a type
    MetaChain chain;
    chain.conds = newNodes(4);
    MetaChain *svchain = metaCurChain;
    metaCurChain = &chain;
    while (1) {
        int iselse = lexIsToken(HashElseToken);
        int holds = 1;
        INode *tree = NULL;
        if (iselse)
            lexNextToken();
        else {
            uint32_t reads = metaBuildReads;
            metaTypeAt = NULL;
            holds = metaReadCond();
            if (metaBuildReads != reads)
                buildany = 1;
            // Folded away or not, the condition asked about a type
            if (metaTypeAt && firsttype == NULL)
                firsttype = metaTypeAt;
            tree = metaCondTree;
            metaCondTree = NULL;
        }
        int mode = MetaUntaken;
        if (done)
            ;
        else if (holds == MetaDeferred) {
            if (nodefer) {
                errorMsgNode(tree, ErrorMetaTypeFact, "%s", nodefer);
                nodefer = NULL;
            }
            else {
                mode = MetaDeferred;
                deferredany = 1;
            }
        }
        else if (holds) {
            // Holds whatever the type is: the chain's 'else' if a branch asked about a type
            mode = ifnode || deferredany ? MetaDeferred : MetaTaken;
            done = 1;
            tree = NULL;
        }
        if (mode == MetaTaken)
            openend = lex->tokp + 1;
        if (mode == MetaDeferred)
            nodesAdd(&chain.conds, tree);
        parseBlockStart();
        MetaScratch scratch;
        if (mode == MetaUntaken)
            metaScratchBegin(parse, &scratch);
        INode *made = branch(parse, ctx, mode);
        if (mode == MetaUntaken)
            metaScratchEnd(parse, &scratch);
        else if (mode == MetaTaken)
            closeat = lex->prevend - 1;
        else if (made) {
            if (ifnode == NULL) {
                ifnode = newIfNode();
                ifnode->flags |= FlagIfHash;
                inodeLexCopy((INode*)ifnode, made);
            }
            nodesAdd(&ifnode->condblk, tree ? tree : elseCond);
            nodesAdd(&ifnode->condblk, made);
        }
        if (iselse || (!lexIsToken(HashElifToken) && !lexIsToken(HashElseToken)))
            break;
    }
    metaCurChain = svchain;
    // A chain the include file keeps whole is read again by each importer, with the facts of
    // the importer's build; where that decides what a type is made of, a package's object and
    // its importer could disagree about it
    if (nomix && firsttype && buildany)
        errorMsgNode(firsttype, ErrorMetaMixFact, "%s", nomix);
    // The include file keeps the branch taken and nothing of the chain around it;
    // a chain with a deferred branch stays whole, for an importer to clone
    // (A later pass of a '#each' reads the same text and has cut it already)
    if (deferredany || metaRepeat)
        ;
    else if (openend && closeat > openend - 1) {
        dclCutAdd(lexer, chainat, openend, NULL);
        dclCutAdd(lexer, closeat, lex->prevend, NULL);
    }
    else if (!done)
        dclCutAdd(lexer, chainat, lex->prevend, NULL);
    return (INode*)ifnode;
}

// A '#if' that gives a value, where an expression is read, with the lexer on the
// '#if': each branch is a block whose value is the chain's, and the chain ends in
// an '#else', since a value must come from somewhere. The value is the taken
// block's: a block of one expression is that expression, so a constant can be
// made from one ('const sep = #if #os == .windows {'\\'} #else {'/'}'); a longer
// block stays a block, and opens a scope of its own, as a block does
const char parseMixBodyMsg[] =
    "A '#if' chain in a function body asks either about the build or about a type, not both: the include file keeps the chain whole and an importer reads the build's facts again for its own build, so the two could choose different branches (and share one static between them). Put the build's '#if' around the chain, or inside a branch of it: the package decides it.";

INode *parseHashIfExpr(ParseState *parse) {
    metaParse = parse;
    int buildany = 0;
    INode *firsttype = NULL;
    INode *value = NULL;
    int done = 0;
    int sawelse = 0;
    Lexer *lexer = lex;
    char *chainat = lex->tokp;
    char *openat = NULL;        // The taken branch's '{'
    char *closeat = NULL;       // Its '}'
    IfNode *ifnode = NULL;      // The chain, if a branch asks what a type parameter is (see parseHashIf)
    while (1) {
        int iselse = lexIsToken(HashElseToken);
        int holds = 1;
        INode *tree = NULL;
        if (iselse)
            lexNextToken();
        else {
            uint32_t reads = metaBuildReads;
            metaTypeAt = NULL;
            holds = metaReadCond();
            if (metaBuildReads != reads)
                buildany = 1;
            if (metaTypeAt && firsttype == NULL)
                firsttype = metaTypeAt;
            tree = metaCondTree;
            metaCondTree = NULL;
        }
        int mode = MetaUntaken;
        if (done)
            ;
        else if (holds == MetaDeferred)
            mode = MetaDeferred;
        else if (holds) {
            mode = ifnode ? MetaDeferred : MetaTaken;
            done = 1;
            tree = NULL;
        }
        if (mode == MetaTaken)
            openat = lex->tokp;
        MetaScratch scratch;
        if (mode == MetaUntaken)
            metaScratchBegin(parse, &scratch);
        INode *block = parseExprBlock(parse, 0);
        if (mode == MetaUntaken)
            metaScratchEnd(parse, &scratch);
        else if (mode == MetaTaken) {
            value = block;
            closeat = lex->prevend - 1;
        }
        else {
            if (ifnode == NULL) {
                ifnode = newIfNode();
                ifnode->flags |= FlagIfHash | FlagIfHashExpr;
                inodeLexCopy((INode*)ifnode, block);
            }
            nodesAdd(&ifnode->condblk, tree ? tree : elseCond);
            nodesAdd(&ifnode->condblk, block);
        }
        if (iselse) {
            sawelse = 1;
            break;
        }
        if (!lexIsToken(HashElifToken) && !lexIsToken(HashElseToken))
            break;
    }
    if (!sawelse)
        errorMsgLexAfter(ErrorMetaIf,
            "A '#if' that gives a value ends in an '#else': with no branch taken there would be no value.");
    if (firsttype && buildany)
        errorMsgNode(firsttype, ErrorMetaMixFact, "%s", parseMixBodyMsg);
    // A chain with a branch that asks about a type is an 'if' for a generic's clone to decide,
    // and stays whole in the include file
    if (ifnode)
        return (INode*)ifnode;
    if (value == NULL)
        return (INode*)newNilLitNode();
    BlockNode *blk = (BlockNode*)value;
    int single = blk->tag == BlockTag && blk->stmts->used == 1 && (blk->flags & FlagTailOpen);
    // The include file keeps the taken value alone: a single expression in
    // parentheses, since it is no block, and a longer block as the block it is
    if (openat && closeat > openat && !metaRepeat) {
        dclCutAdd(lexer, chainat, single ? openat + 1 : openat, single ? "(" : NULL);
        dclCutAdd(lexer, single ? closeat : closeat + 1, lex->prevend, single ? ")" : NULL);
    }
    if (single)
        return nodesGet(blk->stmts, 0);
    return value;
}

// ----------------------------------------------------------------------------
// '#each' and the meta variables
//
// '#each #x in list { body }' is expanded here, while the body is parsed: the lexer is put back at
// the body's '{' for each item of the list, '#x' is bound to the item, and the body is parsed again
// as a block of its own. The passes are blocks that follow one another in the enclosing block, each
// with a scope to itself (ruled 1 Nov). A '#x' met in a pass is a copy of the item: a value where a
// value is read, a type where a type is. The IR that name resolution and type checking see is the
// expanded one; they never meet a '#each'.
//
// A meta variable is a number or a list the parser holds while it reads a function's body. 'let #n'
// is fixed; 'var #i' may be given a new value ('#i += 1;'), and the passes see it in the order they
// were read, which is how a count accumulates across them. A variable belongs to the block it is
// declared in, so everything a pass depends on is written inside the one function body the include
// file copies whole: an importer reads the same text and makes the same passes.

typedef enum {
    MetaBindItem,       // A loop's variable: one item of a list
    MetaBindInt,        // A number: 'let #n = 3', 'var #i = 0'
    MetaBindList,       // A list: 'let #xs = [8, 16, 32]', 'let #ts = (i32, i64)'
    MetaBindField       // A '#each #f in #fields(T)' variable: the description of one field of T
} MetaBindKind;

// A list read from the source: its items, as nodes to copy, and as the text they were written in
typedef struct MetaItems {
    Nodes *nodes;
    char **texts;
    uint32_t count, avail;
    int istype;         // The items are types
} MetaItems;

typedef struct MetaBind {
    Name *name;         // With its '#'
    MetaBindKind kind;
    int isvar;          // 'var': may be given a new value
    int64_t ival;       // A number, or an item that is an integer literal (hasint)
    int hasint;
    INode *item;        // MetaBindItem
    char *text;         // MetaBindItem: the item as written
    int istype;         // MetaBindItem: the item is a type
    MetaItems *list;    // MetaBindList
    FieldEach *fe;      // MetaBindField
} MetaBind;

static MetaBind *metaEnv = NULL;
static uint32_t metaEnvUsed = 0, metaEnvAvail = 0;
static int metaSkipping = 0;        // A stretch is being read past: any '#' word in it is quietly a variable
static int metaDepth = 0;           // '#each's being expanded, one inside the other
static int64_t metaStepsLeft = 0;   // Passes the outermost '#each' still has, with the ones inside it
static int metaQuotaHit = 0;        // The quota was passed: the rest of the outermost '#each' is read past
static FILE *metaFile = NULL;       // --meta-ir

int parseMetaIsBound(Name *name) {
    if (metaSkipping)
        return 1;
    for (uint32_t i = metaEnvUsed; i > 0; --i)
        if (metaEnv[i - 1].name == name)
            return 1;
    return 0;
}

static MetaBind *metaFind(Name *name) {
    for (uint32_t i = metaEnvUsed; i > 0; --i)
        if (metaEnv[i - 1].name == name)
            return &metaEnv[i - 1];
    return NULL;
}

static void metaBindPush(MetaBind *bind) {
    if (metaEnvUsed == metaEnvAvail) {
        uint32_t avail = metaEnvAvail ? metaEnvAvail * 2 : 32;
        MetaBind *grown = (MetaBind*)memAllocBlk(avail * sizeof(MetaBind));
        if (metaEnvUsed)
            memcpy(grown, metaEnv, metaEnvUsed * sizeof(MetaBind));
        metaEnv = grown;
        metaEnvAvail = avail;
    }
    metaEnv[metaEnvUsed++] = *bind;
}

uint32_t parseMetaScope(void) {
    return metaEnvUsed;
}

void parseMetaScopeEnd(uint32_t mark) {
    if (mark < metaEnvUsed)
        metaEnvUsed = mark;
}

// The source from 'from' to 'to' as one line, shortened, for an error to quote
static char *metaTextOf(char *from, char *to) {
    while (from < to && (*from == ' ' || *from == '\t' || *from == '\r' || *from == '\n'))
        ++from;
    while (to > from && (to[-1] == ' ' || to[-1] == '\t' || to[-1] == '\r' || to[-1] == '\n'))
        --to;
    size_t len = (size_t)(to - from);
    int cut = len > 40;
    if (cut)
        len = 40;
    char *text = (char*)memAllocBlk(len + 4);
    for (size_t i = 0; i < len; ++i)
        text[i] = from[i] == '\n' || from[i] == '\r' || from[i] == '\t' ? ' ' : from[i];
    text[len] = 0;
    if (cut)
        strcat(text, "...");
    return text;
}

// Read past a '#each' that is not to be expanded: its header, then the block. The lexer is on
// the '#each' or anywhere in the header; a '#' word met is read quietly
static void metaEachSkip(void) {
    ++metaSkipping;
    while (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken) && !lexIsToken(RCurlyToken)
        && !lexIsToken(EofToken))
        lexNextToken();
    if (lexIsToken(LCurlyToken)) {
        int depth = 0;
        do {
            if (lexIsToken(LCurlyToken))
                ++depth;
            else if (lexIsToken(RCurlyToken))
                --depth;
            else if (lexIsToken(EofToken))
                break;
            lexNextToken();
        } while (depth > 0);
    }
    --metaSkipping;
}

// Read a statement's remaining tokens, to its ';' or its block's end
static void metaStmtSkip(void) {
    ++metaSkipping;
    while (!lexIsToken(SemiToken) && !lexIsToken(RCurlyToken) && !lexIsToken(EofToken))
        lexNextToken();
    if (lexIsToken(SemiToken))
        lexNextToken();
    --metaSkipping;
}

// ---- Numbers

static int metaIntBad = 0;

static void metaIntError(const char *msg) {
    if (!metaIntBad)
        errorMsgLex(ErrorMetaVar, "%s", msg);
    metaIntBad = 1;
}

static int64_t metaIntSum(void);

// A number: an integer literal, a meta variable that holds one, or a parenthesised sum
static int64_t metaIntPrimary(void) {
    switch (lex->toktype) {
    case IntLitToken: {
        int64_t v = (int64_t)lex->val.uintlit;
        if (lex->langtype == (INode*)charType)
            metaIntError("A meta number is built from integers; this is a character.");
        lexNextToken();
        return v;
    }
    case MetaVarToken: {
        MetaBind *bind = metaFind(lex->val.ident);
        int64_t v = 0;
        if (bind && bind->kind == MetaBindInt)
            v = bind->ival;
        else if (bind && bind->kind == MetaBindItem && bind->hasint)
            v = bind->ival;
        else
            metaIntError(bind ? "A meta number is built from integers, and this meta variable holds something else (a list, or an item that is no integer literal)."
                : "This meta variable is not declared here.");
        lexNextToken();
        return v;
    }
    case LParenToken: {
        lexNextToken();
        int64_t v = metaIntSum();
        if (lexIsToken(RParenToken))
            lexNextToken();
        else
            metaIntError("A '(' in a meta number is closed by a ')'.");
        return v;
    }
    case DashToken:
        lexNextToken();
        return -metaIntPrimary();
    default:
        metaIntError("A meta number is built from integer literals and meta variables that hold one, joined with '+', '-', '*', '/' and '%'.");
        return 0;
    }
}

static int64_t metaIntProduct(void) {
    int64_t v = metaIntPrimary();
    while (!metaIntBad && (lexIsToken(StarToken) || lexIsToken(SlashToken) || lexIsToken(PercentToken))) {
        uint16_t op = lex->toktype;
        lexNextToken();
        int64_t rhs = metaIntPrimary();
        if (op == StarToken)
            v = v * rhs;
        else if (rhs == 0)
            metaIntError("A meta number cannot be divided by zero.");
        else
            v = op == SlashToken ? v / rhs : v % rhs;
    }
    return v;
}

static int64_t metaIntSum(void) {
    int64_t v = metaIntProduct();
    while (!metaIntBad && (lexIsToken(PlusToken) || lexIsToken(DashToken))) {
        uint16_t op = lex->toktype;
        lexNextToken();
        int64_t rhs = metaIntProduct();
        v = op == PlusToken ? v + rhs : v - rhs;
    }
    return v;
}

// Read a number, with the lexer on its first token. Bad ones are reported and the statement read past
static int metaIntRead(int64_t *out) {
    metaIntBad = 0;
    *out = metaIntSum();
    if (metaIntBad) {
        metaStmtSkip();
        return 0;
    }
    return 1;
}

// A number as a literal that takes whatever integer type it is wanted as
static INode *metaIntLit(int64_t v) {
    ULitNode *lit = newULitNode(v < 0 ? (uint64_t)(-v) : (uint64_t)v, unknownType);
    if (v < 0) {
        lit->uintlit = 0 - lit->uintlit;
        lit->flags ^= FlagLitNeg;
    }
    return (INode*)lit;
}

// ---- Lists

static void metaItemsAdd(MetaItems *list, INode *node, char *text) {
    if (list->count == list->avail) {
        uint32_t avail = list->avail ? list->avail * 2 : 8;
        char **grown = (char**)memAllocBlk(avail * sizeof(char*));
        if (list->count)
            memcpy(grown, list->texts, list->count * sizeof(char*));
        list->texts = grown;
        list->avail = avail;
    }
    nodesAdd(&list->nodes, node);
    list->texts[list->count++] = text;
}

// Does the parenthesis the lexer is on hold a comma of its own, as '(i32, i64)' does and '(1 + 2)' does not?
// Read off the text, as lexNextIsWord is
static int metaParenHasComma(void) {
    char *p = lex->srcp;
    int depth = 1;
    while (*p && depth > 0) {
        switch (*p) {
        case '(': case '[': case '{': ++depth; break;
        case ')': case ']': case '}': --depth; break;
        case ',': if (depth == 1) return 1; break;
        case '"':
            for (++p; *p && *p != '"' && *p != '\n'; ++p)
                if (*p == '\\' && p[1])
                    ++p;
            break;
        case '/':
            if (p[1] == '/')
                while (*p && *p != '\n')
                    ++p;
            continue;
        }
        if (*p)
            ++p;
    }
    return 0;
}

// Is a node an integer literal, as written, that takes whatever integer type it is wanted as?
static int metaItemInt(INode *node, int64_t *v) {
    if (node && node->tag == ULitTag && (node->flags & FlagUnkType) && !(node->flags & FlagCharLit)) {
        *v = (int64_t)((ULitNode*)node)->uintlit;
        return 1;
    }
    return 0;
}

// Read a list: '[a, b, c]' of values, '(T, U)' of types, or a meta variable that holds one. The
// lexer is on the opener. Returns NULL where it could not be read (reported)
static MetaItems *metaListRead(ParseState *parse) {
    MetaItems *list = (MetaItems*)memAllocBlk(sizeof(MetaItems));
    memset(list, 0, sizeof(MetaItems));
    list->nodes = newNodes(8);
    if (lexIsToken(MetaVarToken)) {
        MetaBind *bind = metaFind(lex->val.ident);
        if (bind == NULL || bind->kind != MetaBindList) {
            errorMsgLex(ErrorMetaEach, "A '#each' goes through a list: '[a, b]' of values, '(T, U)' of types, or a meta variable declared as one ('let #xs = [1, 2];'). '%s' is %s.",
                &lex->val.ident->namestr, bind ? "not a list" : "not declared here");
            return NULL;
        }
        list = bind->list;
        lexNextToken();
        return list;
    }
    int istype = lexIsToken(LParenToken);
    uint16_t closer = istype ? RParenToken : RBracketToken;
    list->istype = istype;
    lexNextToken();
    int svinlist = parse->inlist;
    parse->inlist = 1;
    int errs = errors;
    while (!lexIsToken(closer) && !lexIsToken(EofToken)) {
        char *from = lex->tokp;
        INode *item = istype ? parseTypeReq(parse, "'('") : parseSimpleExpr(parse);
        if (errors != errs || item == NULL) {
            parse->inlist = svinlist;
            return NULL;
        }
        metaItemsAdd(list, item, metaTextOf(from, lex->prevend));
        if (!lexIsToken(CommaToken))
            break;
        lexNextToken();
    }
    parse->inlist = svinlist;
    if (!lexIsToken(closer)) {
        errorMsgLex(ErrorMetaEach, "A list of a '#each' is closed by a '%s' after its last item, with its items separated by ','.", istype ? ")" : "]");
        return NULL;
    }
    lexNextToken();
    return list;
}

// ---- Declarations and assignments

int parseMetaIsDecl(void) {
    return (lexIsToken(LetToken) || lexIsToken(VarToken)) && lexNextIsHash();
}

// 'let #n = 3;', 'var #i = 0;', 'let #xs = [8, 16, 32];' or 'let #ts = (i32, i64);', the lexer on the
// 'let' or 'var'
void parseMetaDecl(ParseState *parse) {
    int isvar = lexIsToken(VarToken);
    lexMetaDecl = 1;
    lexNextToken();
    if (!lexIsToken(MetaVarToken)) {
        errorMsgLex(ErrorMetaVar, "A meta variable's name is a '#' word that is not one of the layer's own: 'let #n = 3;'.");
        metaStmtSkip();
        return;
    }
    MetaBind bind;
    memset(&bind, 0, sizeof(bind));
    bind.name = lex->val.ident;
    bind.isvar = isvar;
    lexNextToken();
    if (!lexIsToken(AssgnToken)) {
        errorMsgLex(ErrorMetaVar, "A meta variable is given its value where it is declared: 'let #n = 3;'.");
        metaStmtSkip();
        return;
    }
    lexNextToken();
    // A list is read first, as the value is read: '#n' in 'let #n = #n + 1;' is the outer one
    if (lexIsToken(LBracketToken) || (lexIsToken(LParenToken) && metaParenHasComma())
        || (lexIsToken(MetaVarToken) && metaFind(lex->val.ident) && metaFind(lex->val.ident)->kind == MetaBindList)) {
        bind.kind = MetaBindList;
        bind.list = metaListRead(parse);
        if (bind.list == NULL) {
            metaStmtSkip();
            return;
        }
        bind.isvar = 0;
    }
    else {
        bind.kind = MetaBindInt;
        if (!metaIntRead(&bind.ival))
            return;
    }
    metaBindPush(&bind);
    parseEndOfStatement();
}

int parseMetaIsAssign(void) {
    if (!lexIsToken(MetaVarToken))
        return 0;
    LexMark mark;
    lexMarkSave(&mark);
    lexNextToken();
    int yes = lexIsToken(AssgnToken) || lexIsToken(PlusEqToken) || lexIsToken(MinusEqToken)
        || lexIsToken(MultEqToken) || lexIsToken(DivEqToken) || lexIsToken(RemEqToken);
    lexMarkRestore(&mark);
    return yes;
}

// '#i = 3;', '#i += 1;' (and '-=', '*=', '/=', '%='), the lexer on the variable. Done now, as it is read
void parseMetaAssign(ParseState *parse) {
    Name *name = lex->val.ident;
    MetaBind *bind = metaFind(name);
    if (bind == NULL || bind->kind != MetaBindInt || !bind->isvar) {
        errorMsgLex(ErrorMetaVar, "'%s' cannot be given a new value: only a number declared with 'var' can ('var #i = 0; ... #i += 1;'). A 'let' is fixed, and a '#each' variable is the item of its pass.",
            &name->namestr);
        metaStmtSkip();
        return;
    }
    lexNextToken();
    uint16_t op = lex->toktype;
    lexNextToken();
    int64_t rhs;
    if (!metaIntRead(&rhs))
        return;
    // (The binding is looked for again: reading the number pushed nothing, but it is cheap to be sure)
    bind = metaFind(name);
    switch (op) {
    case AssgnToken: bind->ival = rhs; break;
    case PlusEqToken: bind->ival += rhs; break;
    case MinusEqToken: bind->ival -= rhs; break;
    case MultEqToken: bind->ival *= rhs; break;
    case DivEqToken:
    case RemEqToken:
        if (rhs == 0) {
            errorMsgLex(ErrorMetaVar, "A meta number cannot be divided by zero.");
            metaStmtSkip();
            return;
        }
        bind->ival = op == DivEqToken ? bind->ival / rhs : bind->ival % rhs;
        break;
    }
    parseEndOfStatement();
}

// '#size(T)' or '#align(T)', the lexer on the word: the byte size or alignment of a type, a usize that
// the code generator reads from the layout it generated. T may be a type parameter, cloned for its
// argument, or any type. Asked while T's layout is in flight, it is refused (sizeofLayoutRefused)
INode *parseMetaLayout(ParseState *parse) {
    int isalign = lexIsToken(HashAlignToken);
    const char *word = isalign ? "#align" : "#size";
    SizeofNode *node = newSizeofNode();
    node->what = isalign ? SizeofAlign : SizeofSize;
    lexNextToken();
    if (!lexIsToken(LParenToken)) {
        errorMsgLex(ErrorMetaFields, "'%s' names the type it measures: '%s(T)'.", word, word);
        return NULL;
    }
    lexNextToken();
    node->type = parseTypeReq(parse, "'(' of a layout question");
    parseCloseTok(RParenToken);
    return (INode*)node;
}

// A meta variable met as a term: a copy of the loop's item, or the number it holds
int parseMetaIsFieldVar(Name *name) {
    MetaBind *bind = metaFind(name);
    return bind && bind->kind == MetaBindField;
}

// '#f.name', '#f.type', '#f.region', '#f.perm' or '#f.pub', the lexer on the '#f': a placeholder the
// clone for an instance replaces with that field's answer (fields.h). A bare '#f' is only spliced
// ('val.#f', handled by parseDotCall)
static INode *metaFieldTerm(FieldEach *fe) {
    INode *at = (INode*)newNilLitNode();
    lexNextToken();
    if (!lexIsToken(DotToken)) {
        errorMsgNode(at, ErrorMetaFields, "'%s' describes a field of the type: ask it '%s.name', '%s.type', '%s.region', '%s.perm' or '%s.pub', or splice it as 'value.%s'.",
            &fe->var->namestr, &fe->var->namestr, &fe->var->namestr, &fe->var->namestr, &fe->var->namestr, &fe->var->namestr, &fe->var->namestr);
        return NULL;
    }
    lexNextToken();
    int prop = lexIsToken(IdentToken) || lexIsToken(PubToken) || lexIsToken(PermToken)
        ? fieldPropFromWord(&lex->val.ident->namestr) : FieldPropCount;
    if (prop == -1) {
        errorMsgLex(ErrorMetaFields, "Attributes on fields are not built yet, so '%s.attrs' has nothing to describe.", &fe->var->namestr);
        lexNextToken();
        return NULL;
    }
    if (prop == FieldPropCount) {
        errorMsgLex(ErrorMetaFields, "A field description answers '.name', '.type', '.region', '.perm', '.pub' and '.offset'.");
        if (!lexIsToken(DotToken))
            lexNextToken();
        return NULL;
    }
    INode *node = fieldEachProp(fe, (FieldProp)prop, at);
    lexNextToken();
    return node;
}

// '#f.type' as the subject of a condition ('#if #f.type is Copy'), the lexer on the '#f'
static INode *metaFieldSubject(void) {
    FieldEach *fe = metaFind(lex->val.ident)->fe;
    INode *subject = metaFieldTerm(fe);
    if (subject == NULL)
        return NULL;
    if (!isNameUseNode(subject) || ((NameUseNode*)subject)->namesym != fe->propnames[FieldPropType]) {
        errorMsgNode(subject, ErrorMetaFact, "A '#if' condition asks what a field's type is: '%s.type is Copy'.", &fe->var->namestr);
        return NULL;
    }
    return subject;
}

INode *parseMetaTerm(ParseState *parse) {
    MetaBind *bind = metaFind(lex->val.ident);
    INode *node = NULL;
    if (bind && bind->kind == MetaBindField)
        return metaFieldTerm(bind->fe);
    if (bind == NULL)
        errorMsgLex(ErrorMetaVar, "'%s' is not a meta variable declared here ('#each %s in ...', 'let %s = ...').",
            &lex->val.ident->namestr, &lex->val.ident->namestr, &lex->val.ident->namestr);
    else if (bind->kind == MetaBindInt)
        node = metaIntLit(bind->ival);
    else if (bind->kind == MetaBindItem)
        node = metaCopy(bind->item);
    else
        errorMsgLex(ErrorMetaVar, "'%s' holds a list, which a '#each' goes through ('#each #x in %s { ... }'); it is not a value.",
            &lex->val.ident->namestr, &lex->val.ident->namestr);
    lexNextToken();
    return node;
}

// ---- '#each'

// The IR printer's state (inode.c)
extern FILE *irfile;
extern int irIndent, irIsNL;

// What --meta-ir writes of a loop: it, and each pass the parser made of it
static void metaEachDump(INode *loop, Name *var, MetaItems *list, Nodes *passes, EachPass **eps) {
    if (metaFile == NULL) {
        char *path = fileMakePath(metaOpt->output, metaOpt->srcname, "meta");
        metaFile = fopen(path, "wb");
        if (metaFile == NULL)
            return;
    }
    FILE *svfile = irfile;
    int svindent = irIndent, svnl = irIsNL;
    irfile = metaFile;
    irIndent = 0;
    irIsNL = 1;
    uint32_t line = loop->linenbr;
    fprintf(metaFile, "#each %s: %u item%s, at line %u of %s\n", &var->namestr, list->count,
        list->count == 1 ? "" : "s", line, loop->lexer ? loop->lexer->url : "?");
    for (uint32_t i = 0; i < passes->used; ++i) {
        fprintf(metaFile, "pass %u of %u, %s = %s\n", i + 1, list->count, &var->namestr, eps[i]->item);
        inodePrintNode(nodesGet(passes, i));
        inodePrintNL();
    }
    fprintf(metaFile, "\n");
    fflush(metaFile);
    irfile = svfile;
    irIndent = svindent;
    irIsNL = svnl;
}

// ---- Members generated in a generic struct's braces

// A member read inside '#each #f in #fields(T) { ... }' in a generic struct's braces belongs to the
// loop: its name must have been made of '#f', and it is one of the loop's, made once per field of T
static void metaGenMark(INode *member, FieldEach *fe) {
    GenName *gen = NULL;
    if (member->tag == FieldDclTag)
        gen = ((FieldDclNode*)member)->gen;
    else if (member->tag == FnDclTag)
        gen = ((FnDclNode*)member)->gen;
    else {
        errorMsgNode(member, ErrorMetaGenName,
            "A '#each #f in #fields(T)' in a type's braces generates fields and methods; a static, a macro or an alias in it is not built.");
        return;
    }
    if (gen == NULL || gen->fe != fe) {
        errorMsgNode(member, ErrorMetaGenName,
            "A member in a '#each %s in #fields(T)' is made once for each field, so its name is made of %s: '%s' or '#name(\"get\", %s)'.",
            &fe->var->namestr, &fe->var->namestr, &fe->var->namestr, &fe->var->namestr);
    }
}

// Does a member name begin here: '#f' of a loop, or '#name(...)'
int parseMetaIsGenName(void) {
    return lexIsToken(HashNameToken) || (lexIsToken(MetaVarToken) && parseMetaIsFieldVar(lex->val.ident));
}

// A generated member's name: '#f' (the described field's own name), '#name("get", #f)' or
// '#name(#f, "Count")' (that name with a fixed word before or after it; nothing else is computed).
// The name the template holds it under is its spelling. NULL where it is malformed
Name *parseMetaGenName(ParseState *parse, GenName **out) {
    *out = NULL;
    GenName *gen = (GenName*)memAllocBlk(sizeof(GenName));
    memset(gen, 0, sizeof(GenName));
    char buf[400];
    if (lexIsToken(MetaVarToken)) {
        gen->fe = metaFind(lex->val.ident)->fe;
        snprintf(buf, sizeof(buf), "%s", &lex->val.ident->namestr);
        lexNextToken();
    }
    else {
        lexNextToken();
        if (!lexIsToken(LParenToken)) {
            errorMsgLex(ErrorMetaGenName, "'#name' takes a fixed word and the loop's variable: '#name(\"get\", #f)'.");
            return NULL;
        }
        lexNextToken();
        char *word = NULL;
        int wordfirst = 0;
        for (int k = 0; k < 2; ++k) {
            if (lexIsToken(StringLitToken) && word == NULL) {
                word = (char*)memAllocBlk(lex->strlen + 1);
                memcpy(word, lex->val.strlit, lex->strlen);
                word[lex->strlen] = 0;
                wordfirst = k == 0;
                lexNextToken();
            }
            else if (lexIsToken(MetaVarToken) && parseMetaIsFieldVar(lex->val.ident) && gen->fe == NULL) {
                gen->fe = metaFind(lex->val.ident)->fe;
                lexNextToken();
            }
            else {
                errorMsgLex(ErrorMetaGenName, "'#name' takes one string literal and the loop's variable, in either order: '#name(\"get\", #f)'.");
                return NULL;
            }
            if (k == 0) {
                if (!lexIsToken(CommaToken)) {
                    errorMsgLex(ErrorMetaGenName, "'#name' takes a fixed word and the loop's variable, separated by a comma: '#name(\"get\", #f)'.");
                    return NULL;
                }
                lexNextToken();
            }
        }
        if (!lexIsToken(RParenToken)) {
            errorMsgLex(ErrorMetaGenName, "'#name' ends after its two parts: '#name(\"get\", #f)'.");
            return NULL;
        }
        lexNextToken();
        if (wordfirst)
            gen->prefix = word;
        else
            gen->suffix = word;
        const char *var = &gen->fe->var->namestr;
        if (wordfirst)
            snprintf(buf, sizeof(buf), "#name(\"%s\", %s)", word, var);
        else
            snprintf(buf, sizeof(buf), "#name(%s, \"%s\")", var, word);
    }
    *out = gen;
    return nametblFind(buf, strlen(buf));
}

// The body of a '#each #f in #fields(T)' written among a call's arguments, the lexer on its '{': one
// named argument, '#f: value', which each pass makes with the field's own name. Read through the '}'.
// The result is the NamedValNode, or NULL after an error
static INode *metaArgBodyRead(ParseState *parse, Name *var, INode *loop) {
    int ok = 0;
    NamedValNode *named = NULL;
    lexNextToken();
    if (lexIsToken(MetaVarToken) && lex->val.ident == var) {
        NameUseNode *name = newNameUseNode(var);
        inodeLexCopy((INode*)name, loop);
        lexNextToken();
        if (lexIsToken(ColonToken)) {
            lexNextToken();
            named = newNamedValNode((INode*)name);
            named->val = parseArgValue(parse);
            if (lexIsToken(CommaToken))
                lexNextToken();
            ok = lexIsToken(RCurlyToken) && named->val != NULL;
        }
        if (!ok)
            errorMsgLex(ErrorMetaEachArg, "A '#each' in a call's arguments makes one named argument for each field, so its block holds exactly that: '#each %s in #fields(T) { %s: value }'.",
                &var->namestr, &var->namestr);
    }
    else if (lexIsToken(IdentToken)) {
        Name *written = lex->val.ident;
        lexNextToken();
        if (lexIsToken(ColonToken))
            errorMsgLex(ErrorMetaEachArgName, "A '#each' in a call's arguments makes one named argument for each field, named by the loop's variable. '%s' would be given to every field: write '%s: value'.",
                &written->namestr, &var->namestr);
        else
            errorMsgLex(ErrorMetaEachArg, "A '#each' in a call's arguments makes one named argument for each field, so its block holds exactly that: '#each %s in #fields(T) { %s: value }'.",
                &var->namestr, &var->namestr);
    }
    else
        errorMsgLex(ErrorMetaEachArg, "A '#each' in a call's arguments makes one named argument for each field, so its block holds exactly that: '#each %s in #fields(T) { %s: value }'.",
            &var->namestr, &var->namestr);
    if (!lexIsToken(RCurlyToken) || !ok) {
        ++metaSkipping;
        int depth = 1;
        while (depth > 0 && !lexIsToken(EofToken)) {
            if (lexIsToken(LCurlyToken))
                ++depth;
            else if (lexIsToken(RCurlyToken))
                --depth;
            if (depth > 0)
                lexNextToken();
        }
        --metaSkipping;
    }
    if (lexIsToken(RCurlyToken))
        lexNextToken();
    return ok ? (INode*)named : NULL;
}

// '#fields(T) { body }' after '#each #f in', the lexer on the '#fields': the body is read once, with
// '#f' standing for a field of T, as a block (the loop) that holds it; cloning that for an instance of
// T makes the passes (fields.h). T is a type parameter of the function or type this is written in
static void parseHashFields(ParseState *parse, BlockNode *blk, INode *loop, Name *var,
        StructNode *strnode, MetaMembersFn items, void *ctx, INode **argout) {
    lexNextToken();
    if (!lexIsToken(LParenToken)) {
        errorMsgLex(ErrorMetaFields, "'#fields' names the type whose fields it describes: '#each #f in #fields(T) { ... }'.");
        metaEachSkip();
        return;
    }
    lexNextToken();
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorMetaFields, "'#fields' names a type parameter of the generic it is written in: '#fields(T)'.");
        metaEachSkip();
        return;
    }
    NameUseNode *subject = newNameUseNode(lex->val.ident);
    inodeLexCopy((INode*)subject, loop);
    lexNextToken();
    if (!lexIsToken(RParenToken)) {
        errorMsgLex(ErrorMetaFields, "'#fields(T)' takes one type parameter's name, closed by ')'.");
        metaEachSkip();
        return;
    }
    lexNextToken();
    if (!lexIsToken(LCurlyToken)) {
        errorMsgLex(ErrorMetaEach, "A '#each' repeats a block, which follows its list: '#each #f in #fields(T) { ... }'.");
        metaEachSkip();
        return;
    }
    FieldEach *fe = newFieldEach(var, (INode*)subject, parse->mod);
    uint32_t scope = metaEnvUsed;
    MetaBind bind;
    memset(&bind, 0, sizeof(bind));
    bind.name = var;
    bind.kind = MetaBindField;
    bind.fe = fe;
    metaBindPush(&bind);
    if (strnode) {
        // The braces of a generic struct: the members read here are the loop's body, each carrying the
        // loop, and the instance makes one of each per field of T (cloneStructNode)
        uint32_t f0 = strnode->fields.used, n0 = strnode->nodelist.used;
        parseBlockStart();
        items(parse, ctx);
        metaEnvUsed = scope;
        for (uint32_t i = f0; i < strnode->fields.used; ++i)
            metaGenMark(strnode->fields.nodes[i], fe);
        for (uint32_t i = n0; i < strnode->nodelist.used; ++i)
            metaGenMark(strnode->nodelist.nodes[i], fe);
        return;
    }
    INode *body = argout ? metaArgBodyRead(parse, var, loop) : parseExprBlock(parse, 0);
    metaEnvUsed = scope;
    if (argout) {
        if (body == NULL)
            return;
        BlockNode *wrap = newBlockNode();
        inodeLexCopy((INode*)wrap, loop);
        nodesAdd(&wrap->stmts, body);
        wrap->fieldeach = fe;
        *argout = (INode*)wrap;
        return;
    }
    if (body == NULL || body->tag != BlockTag)
        return;
    BlockNode *wrap = newBlockNode();
    inodeLexCopy((INode*)wrap, loop);
    nodesAdd(&wrap->stmts, body);
    wrap->fieldeach = fe;
    nodesAdd(&blk->stmts, (INode*)wrap);
}

// '#each #x in list { body }' among a block's statements, the lexer on the '#each'. The passes are
// added to 'blk' as blocks of their own
void parseHashEach(ParseState *parse, BlockNode *blk) {
    INode *loop = (INode*)newNilLitNode();      // Stands where the '#each' is
    lexMetaDecl = 1;
    lexNextToken();
    if (!lexIsToken(MetaVarToken)) {
        errorMsgLex(ErrorMetaEach, "A '#each' binds the item of each pass to a '#' name of its own: '#each #x in [1, 2, 3] { ... }'.");
        metaEachSkip();
        return;
    }
    Name *var = lex->val.ident;
    lexNextToken();
    if (!lexIsToken(InToken)) {
        errorMsgLex(ErrorMetaEach, "A '#each' names its list after 'in': '#each #x in [1, 2, 3] { ... }'.");
        metaEachSkip();
        return;
    }
    lexNextToken();
    if (lexIsToken(HashFieldsToken)) {
        parseHashFields(parse, blk, loop, var, NULL, NULL, NULL, NULL);
        return;
    }
    if (!lexIsToken(LBracketToken) && !lexIsToken(LParenToken) && !lexIsToken(MetaVarToken)) {
        errorMsgLex(ErrorMetaEach, "A '#each' goes through a list: '[a, b]' of values, '(T, U)' of types, or a meta variable declared as one ('let #xs = [1, 2];').");
        metaEachSkip();
        return;
    }
    MetaItems *list = metaListRead(parse);
    if (list == NULL) {
        metaEachSkip();
        return;
    }
    if (!lexIsToken(LCurlyToken)) {
        errorMsgLex(ErrorMetaEach, "A '#each' repeats a block, which follows its list: '#each #x in [1, 2, 3] { ... }'.");
        metaEachSkip();
        return;
    }

    int outermost = metaDepth == 0;
    if (outermost) {
        metaStepsLeft = metaOpt ? metaOpt->meta_steps : 10000;
        metaQuotaHit = 0;
    }
    ++metaDepth;
    Nodes *passes = newNodes(list->count < 16 ? list->count + 1 : 16);
    EachPass **eps = (EachPass**)memAllocBlk((list->count + 1) * sizeof(EachPass*));
    LexMark body;
    lexMarkSave(&body);
    int consumed = 0;       // A pass has read the body
    for (uint32_t k = 0; k < list->count; ++k) {
        if (metaQuotaHit)
            break;
        if (metaStepsLeft-- <= 0) {
            metaQuotaHit = 1;
            errorMsgNode(loop, ErrorMetaQuota,
                "The '#each' over %s is at pass %u of %u, and the outermost '#each' (with the ones inside it) has used up its %d steps. A runaway list or nested loops stop here; --meta-steps raises the quota.",
                &var->namestr, k + 1, list->count, metaOpt ? metaOpt->meta_steps : 10000);
            break;
        }
        lexMarkRestore(&body);
        uint32_t scope = metaEnvUsed;
        MetaBind bind;
        memset(&bind, 0, sizeof(bind));
        bind.name = var;
        bind.kind = MetaBindItem;
        bind.item = nodesGet(list->nodes, k);
        bind.text = list->texts[k];
        bind.istype = list->istype;
        bind.hasint = metaItemInt(bind.item, &bind.ival);
        metaBindPush(&bind);
        EachPass *ep = (EachPass*)memAllocBlk(sizeof(EachPass));
        ep->outer = errorEachPass;
        ep->loop = loop;
        ep->pass = k + 1;
        ep->npasses = list->count;
        ep->var = &var->namestr;
        ep->item = bind.text;
        errorEachPass = ep;
        if (k > 0)
            ++metaRepeat;
        int errs = errors;
        INode *pass = parseExprBlock(parse, 0);
        if (k > 0)
            --metaRepeat;
        errorEachPass = ep->outer;
        metaEnvUsed = scope;
        if (pass && pass->tag == BlockTag) {
            ((BlockNode*)pass)->eachpass = ep;
            nodesAdd(&blk->stmts, pass);
            eps[passes->used] = ep;
            nodesAdd(&passes, pass);
        }
        consumed = 1;
        // What was wrong would be wrong in the passes that follow, and be said again in each
        if (errors != errs)
            break;
    }
    --metaDepth;
    // Where no pass was read (no items, or the quota gone at the first), the body is read past
    if (!consumed)
        metaEachSkip();
    if (metaOpt && metaOpt->print_meta && outermost && parse->mod != metaScratchMod && passes->used)
        metaEachDump(loop, var, list, passes, eps);
}

// '#each #f in #fields(T) { members }' in a generic struct's braces, the lexer on the '#each'. The
// members are read once, by 'items' (which reads through the '}'), and join the struct as its own,
// each marked with the loop; the instance makes one of each per field of T
void parseHashEachMembers(ParseState *parse, StructNode *strnode, MetaMembersFn items, void *ctx) {
    INode *loop = (INode*)newNilLitNode();
    lexMetaDecl = 1;
    lexNextToken();
    if (!lexIsToken(MetaVarToken)) {
        errorMsgLex(ErrorMetaEach, "A '#each' binds the item of each pass to a '#' name of its own: '#each #f in #fields(T) { ... }'.");
        metaEachSkip();
        return;
    }
    Name *var = lex->val.ident;
    lexNextToken();
    if (!lexIsToken(InToken)) {
        errorMsgLex(ErrorMetaEach, "A '#each' names its list after 'in': '#each #f in #fields(T) { ... }'.");
        metaEachSkip();
        return;
    }
    lexNextToken();
    if (!lexIsToken(HashFieldsToken)) {
        errorMsgLex(ErrorMetaEachPlace, "In a type's braces a '#each' goes through the fields of a type parameter: '#each #f in #fields(T) { ... }'.");
        metaEachSkip();
        return;
    }
    parseHashFields(parse, NULL, loop, var, strnode, items, ctx, NULL);
}

// '#each #f in #fields(T) { #f: value }' among a call's arguments, the lexer on the '#each'. The
// result is the loop, a block with 'fieldeach' whose one statement is the named argument; the call's
// clone makes one named argument per field (cloneFieldEachArgs). NULL after an error
INode *parseHashEachArg(ParseState *parse) {
    INode *loop = (INode*)newNilLitNode();
    lexMetaDecl = 1;
    lexNextToken();
    if (!lexIsToken(MetaVarToken)) {
        errorMsgLex(ErrorMetaEach, "A '#each' binds the item of each pass to a '#' name of its own: '#each #f in #fields(T) { #f: value }'.");
        metaEachSkip();
        return NULL;
    }
    Name *var = lex->val.ident;
    lexNextToken();
    if (!lexIsToken(InToken)) {
        errorMsgLex(ErrorMetaEach, "A '#each' names its list after 'in': '#each #f in #fields(T) { #f: value }'.");
        metaEachSkip();
        return NULL;
    }
    lexNextToken();
    if (!lexIsToken(HashFieldsToken)) {
        errorMsgLex(ErrorMetaEachPlace, "In a call's arguments a '#each' goes through the fields of a type parameter: '#each #f in #fields(T) { #f: value }'.");
        metaEachSkip();
        return NULL;
    }
    INode *out = NULL;
    parseHashFields(parse, NULL, loop, var, NULL, NULL, NULL, &out);
    return out;
}

