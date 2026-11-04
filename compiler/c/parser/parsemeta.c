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
    MetaBad         // reported already
} MetaKind;

typedef struct MetaVal {
    MetaKind kind;
    int64_t val;        // The bool, the number or the member's index
    Name *member;       // MetaMember: the name written after the '.'
} MetaVal;

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
    return v;
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
    default:
        // A '#' word the lexer refused took its place: that was reported
        if (lexHashDropped != metaDropMark) {
            metaSkipCond();
            return metaVal(MetaBad, 0);
        }
        errorMsgLex(ErrorMetaFact,
            "A '#if' condition is built from the build's facts -- #os, #arch, #debug, #defined(\"NAME\") and #define(\"NAME\") -- compared with '==' or '!=' and joined with '!', '&&' and '||'. Nothing else is known while the program is being read.");
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

static MetaVal metaAnd() {
    MetaVal lhs = metaCompare();
    while (lexIsAmpAmp()) {
        lexNextToken();
        lexNextToken();
        MetaVal rhs = metaCompare();
        if (lhs.kind == MetaBad || rhs.kind == MetaBad) {
            lhs = metaVal(MetaBad, 0);
            continue;
        }
        if (lhs.kind != MetaBool || rhs.kind != MetaBool) {
            errorMsgLex(ErrorMetaFact, "'&&' joins two bools, and this joins %s and %s.", metaKindName(lhs.kind), metaKindName(rhs.kind));
            lhs = metaVal(MetaBad, 0);
            continue;
        }
        lhs = metaVal(MetaBool, lhs.val && rhs.val);
    }
    return lhs;
}

static MetaVal metaOr() {
    MetaVal lhs = metaAnd();
    while (lexIsToken(OrOrToken)) {
        lexNextToken();
        MetaVal rhs = metaAnd();
        if (lhs.kind == MetaBad || rhs.kind == MetaBad) {
            lhs = metaVal(MetaBad, 0);
            continue;
        }
        if (lhs.kind != MetaBool || rhs.kind != MetaBool) {
            errorMsgLex(ErrorMetaFact, "'||' joins two bools, and this joins %s and %s.", metaKindName(lhs.kind), metaKindName(rhs.kind));
            lhs = metaVal(MetaBad, 0);
            continue;
        }
        lhs = metaVal(MetaBool, lhs.val || rhs.val);
    }
    return lhs;
}

// Read a condition, with the lexer on its first token: whether it holds. A
// condition that is not a bool, or could not be read, is reported and does not
// hold
static int metaCond() {
    MetaVal v = metaOr();
    if (v.kind == MetaBad)
        return 0;
    if (v.kind != MetaBool) {
        errorMsgLex(ErrorMetaFact, "A '#if' condition is a bool, and this is %s. Compare it: '#os == .linux'.", metaKindName(v.kind));
        return 0;
    }
    return (int)v.val;
}

// Pass the '#if' or '#elif' the lexer is on and read its condition: whether it
// holds. An unknown '#' word in it is dropped alone (lexInHashCond), and that
// is already reported, so the rest of the condition is read past quietly
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

// A '#elif' or '#else' that no '#if' chain is waiting for: reported, and its
// condition and block read past so that what is inside is not reported as well
void parseMetaStray(ParseState *parse) {
    int iselif = lexIsToken(HashElifToken);
    errorMsgLex(ErrorMetaIf, iselif
        ? "'#elif' continues a '#if' or another '#elif' and has nothing to continue here. After an '#else' the chain is over."
        : "'#else' ends a '#if' chain and has nothing to end here. After an '#else' the chain is over.");
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
void parseHashIf(ParseState *parse, MetaBranchFn branch, void *ctx) {
    int done = 0;       // A branch has been taken
    Lexer *lexer = lex;
    char *chainat = lex->tokp;
    char *openend = NULL;       // Just past the taken branch's '{'
    char *closeat = NULL;       // The taken branch's '}'
    while (1) {
        int iselse = lexIsToken(HashElseToken);
        int holds = 1;
        if (iselse)
            lexNextToken();
        else
            holds = metaReadCond();
        int taken = !done && holds;
        done |= taken;
        if (taken)
            openend = lex->tokp + 1;
        parseBlockStart();
        MetaScratch scratch;
        if (!taken)
            metaScratchBegin(parse, &scratch);
        branch(parse, ctx, taken);
        if (!taken)
            metaScratchEnd(parse, &scratch);
        else
            closeat = lex->prevend - 1;
        if (iselse || (!lexIsToken(HashElifToken) && !lexIsToken(HashElseToken)))
            break;
    }
    // The include file keeps the branch taken and nothing of the chain around it
    if (openend && closeat > openend - 1) {
        dclCutAdd(lexer, chainat, openend, NULL);
        dclCutAdd(lexer, closeat, lex->prevend, NULL);
    }
    else if (!done)
        dclCutAdd(lexer, chainat, lex->prevend, NULL);
}

// A '#if' that gives a value, where an expression is read, with the lexer on the
// '#if': each branch is a block whose value is the chain's, and the chain ends in
// an '#else', since a value must come from somewhere. The value is the taken
// block's: a block of one expression is that expression, so a constant can be
// made from one ('const sep = #if #os == .windows {'\\'} #else {'/'}'); a longer
// block stays a block, and opens a scope of its own, as a block does
INode *parseHashIfExpr(ParseState *parse) {
    INode *value = NULL;
    int done = 0;
    int sawelse = 0;
    Lexer *lexer = lex;
    char *chainat = lex->tokp;
    char *openat = NULL;        // The taken branch's '{'
    char *closeat = NULL;       // Its '}'
    while (1) {
        int iselse = lexIsToken(HashElseToken);
        int holds = 1;
        if (iselse)
            lexNextToken();
        else
            holds = metaReadCond();
        int taken = !done && holds;
        done |= taken;
        if (taken)
            openat = lex->tokp;
        MetaScratch scratch;
        if (!taken)
            metaScratchBegin(parse, &scratch);
        INode *block = parseExprBlock(parse, 0);
        if (!taken)
            metaScratchEnd(parse, &scratch);
        else {
            value = block;
            closeat = lex->prevend - 1;
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
