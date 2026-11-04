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
#include "../ir/nametbl.h"
#include "../coneopts.h"
#include "lexer.h"

#include <stdio.h>
#include <string.h>

// The options the facts are read from, set once by parseInit. The target
// triple is filled in by generation's setup, which runs before any source is
// parsed, so facts are read from the options when they are asked for
static ConeOptions *metaOpt = NULL;

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
static MetaVal metaTypeFact() {
    INode *subject = (INode*)newNameUseNode(lex->val.ident);
    lexNextToken();
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
        lexNextToken();
        return metaVal(MetaBool, !(metaOpt && metaOpt->release));
    case HashOsToken:
        lexNextToken();
        return metaVal(MetaOs, metaOsIndex());
    case HashArchToken:
        lexNextToken();
        return metaVal(MetaArch, metaArchIndex());
    case HashDefinedToken:
        return metaDefine(1);
    case HashDefineToken:
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
        return metaTypeFact();
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
void parseMetaStray(ParseState *parse) {
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
INode *parseHashIf(ParseState *parse, MetaBranchFn branch, void *ctx, const char *nodefer) {
    metaParse = parse;
    int done = 0;       // A branch has been taken, or the deferred chain has its 'else'
    Lexer *lexer = lex;
    char *chainat = lex->tokp;
    char *openend = NULL;       // Just past the taken branch's '{'
    char *closeat = NULL;       // The taken branch's '}'
    IfNode *ifnode = NULL;      // The deferred chain, once a branch is
    int deferredany = 0;
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
            holds = metaReadCond();
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
    // The include file keeps the branch taken and nothing of the chain around it;
    // a chain with a deferred branch stays whole, for an importer to clone
    if (deferredany)
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
INode *parseHashIfExpr(ParseState *parse) {
    metaParse = parse;
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
            holds = metaReadCond();
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
    if (openat && closeat > openat) {
        dclCutAdd(lexer, chainat, single ? openat + 1 : openat, single ? "(" : NULL);
        dclCutAdd(lexer, single ? closeat : closeat + 1, lex->prevend, single ? ")" : NULL);
    }
    if (single)
        return nodesGet(blk->stmts, 0);
    return value;
}
