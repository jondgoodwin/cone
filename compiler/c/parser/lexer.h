/** Lexer
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef lexer_h
#define lexer_h

typedef struct INode INode;    // ../ast/ast.h
typedef struct Name Name;    // ../ast/nametbl.h

#include "../coneopts.h"
#include <stdint.h>

// Lexer state (one per source file)
typedef struct Lexer {
    // Value info about a discovered token
    union {
        double floatlit;
        uint64_t uintlit;
        char *strlit;
        Name *ident;
    } val;
    uint32_t strlen;   // Size of string literal
    INode *langtype;

    // immutable info about source
    char *url;        // The url where the source text came from, as a location names it
    char *path;       // The file the text was read from, where what it names is looked
                      // for: url's, but where a line mark renamed the url (LexLineMarks)
    char *fname;    // The filename of the url (no extension)
    char *source;    // The source text (0-terminated)

    struct Lexer *next;    // Next lexer (linked list of injected lexers)
    struct Lexer *prev; // Previous lexer

    // Lexer's evolving state
    char *srcp;        // Current pointer
    char *tokp;        // Start of current token
    char *linep;    // Pointer to start of current line

    uint32_t linenbr;    // Current line number
    uint32_t flags;        // Lexer flags
    uint16_t toktype;    // TokenTypes

    // Where the previous token ended. A diagnostic about what should have
    // followed a token - the ';' ending a statement - is reported here, after
    // that token, rather than at whatever the next line happens to start with.
    char *prevend;
    char *prevlinep;
    uint32_t prevlinenbr;

    // A source the compiler wrote itself: the declarations an 'actor'
    // generates (parseactor.c). In it alone, '`#n`' names gennames[n], a name
    // no source can spell (nametblPrivate). A diagnostic against it is
    // reported where 'genat' is, the declaration it was generated from
    INode *genat;
    Name **gennames;
    uint32_t ngennames;
} Lexer;

// Lexer flags
// A generated include file: its line marks are read (lexLineMark)
#define LexLineMarks 0x0001

// How a line mark begins: '//#line 12 "q.cone"', a line of its own in a
// generated include file, says the line after it is line 12 of the package's
// source file q.cone. A comment to every other reader
#define LexLineMark "//#line "

// All the possible types for a token
enum TokenTypes {
    EofToken, // End-of-file

    // Numeric and Identifier tokens
    IntLitToken,    // Integer literal
    FloatLitToken,  // Float literal
    StringLitToken, // String literal
    IdentToken,     // Identifier
    LifetimeToken,  // Lifetime variable ('a)
    PermToken,      // Permission identifier

    // Punctuation tokens
    SemiToken,         // ';'
    ColonToken,        // ':'
    LCurlyToken,       // '{'
    RCurlyToken,       // '}'
    LBracketToken,     // '['
    RBracketToken,     // ']'
    LParenToken,       // '('
    RParenToken,       // ')'
    CommaToken,        // ','
    DotToken,          // '.'
    DotDotToken,       // '..' range, excluding its end
    DotDotLessToken,   // '..<' range, excluding its end
    EllipsisToken,     // '...' range, including its end
    PlusToken,         // '+'
    PlusArrayRefToken, // '+[]', lexed only to be refused (parsePlus)
    PlusVirtRefToken,  // '+<'
    DashToken,         // '-'
    StarToken,         // '*'
    PercentToken,      // '%'
    SlashToken,        // '/'
    AmperToken,        // '&'
    ArrayRefToken,     // '&[]'
    VirtRefToken,      // '&<'
    AndToken,          // 'and'
    BarToken,          // '|'
    OrToken,           // 'or'
    CaretToken,        // '^'
    NotToken,          // '!'
    QuesToken,         // '?'
    TildeToken,        // '~'
    LessDashToken,     // '<-'
    AssgnToken,        // '='
    LAssgnToken,       // ':='
    SwapToken,         // '<=>'
    FatArrowToken,     // '=>': a closure's short form, 'x => x * 2'
    IsToken,           // 'is'
    EqToken,           // '=='
    NeToken,           // '!='
    SameToken,         // '==='
    NotSameToken,      // '!=='
    LtToken,           // '<'
    LeToken,           // '<='
    GtToken,           // '>'
    GeToken,           // '>='
    ShlToken,          // '<<'
    ShrToken,          // '>>'
    PlusEqToken,       // '+='
    MinusEqToken,      // '-='
    MultEqToken,       // '*='
    DivEqToken,        // '/='
    RemEqToken,        // '%='
    OrEqToken,         // '|='
    AndEqToken,        // '&='
    XorEqToken,        // '^='
    ShlEqToken,        // '<<='
    ShrEqToken,        // '>>='
    IncrToken,         // '++'
    DecrToken,         // '--'

    // Keywords
    IncludeToken,  // 'include', retired: kept a keyword so the parser can report the statement
    ImportToken,   // 'import'
    ExternToken,   // 'extern'
    PubToken,      // 'pub'
    StaticToken,   // 'static'
    MacroToken,    // 'macro'
    FnToken,       // 'fn'
    OverloadToken, // 'overload'
    ConstToken,    // 'const'
    AliasToken,    // 'alias'
    TypedefToken,  // 'typedef', retired: kept a keyword so the parser can point it at 'alias'
    StructToken,   // 'struct'
    ModToken,      // 'mod'
    ActorToken,    // 'actor'
    TraitToken,    // 'trait'
    OpaqueToken,   // '@opaque'
    UnsizedToken,  // '@unsized'
    CAttrToken,    // '@c': C naming, on a 'mod' line or a 'fn'
    InitPureToken, // '@initpure': after 'fn', a function a module's 'init' may call, 'init' among them
    IntrinsicAttrToken, // '@intrinsic': after 'fn' in core, a function whose meaning the compiler supplies
    ThreadLocalToken, // '@threadlocal': after a global's permission, storage each thread has its own copy of
    WorkgroupToken, // '@workgroup': after a global's permission, on a GPU storage each workgroup has its own copy of
    ComputeAttrToken, // '@compute(x, y, z)': after 'fn', a compute entry point and its workgroup's size
    ExtendsToken,  // 'extends'
    MixinToken,    // 'mixin', retired: kept a keyword so the parser can point it at 'is'
    UseToken,      // 'use'
    ButToken,      // 'but'
    EnumToken,     // 'enum'
    RetToken,      // 'return'
    WithToken,     // 'with'
    IfToken,       // 'if'
    ElifToken,     // 'elif'
    ElseToken,     // 'else'
    CaseToken,     // 'case'
    MatchToken,    // 'match'
    WhileToken,    // 'while'
    EachToken,     // 'each'
    InToken,       // 'in'
    ByToken,       // 'step'
    BreakToken,    // 'break'
    ContinueToken, // 'continue'
    AsToken,       // 'as'
    IntoToken,     // 'into'
    InlineToken,   // 'inline'
    WhereToken,    // 'where', which opens a generic's constraints
    NewToken,      // 'new': a construction, 'new Point(1, 2)', and an initializer's permission, '&new'
    TrynewToken,   // 'trynew': an allocation that may fail, 'trynew Rc[mut, Node](1)', giving an Option
    AwaitToken,    // 'await': in an actor's behaviour, wait for what is awaited; the behaviour is cut there, a seam
    YieldToken,    // 'yield': in a function declared 'yields', hand the caller a value and wait to be resumed; 'yield each' hands on a sub-generator's
    SelfActorToken, // 'selfactor': in an actor's method, the actor's own handle
    AsyncToken,    // 'async': the first word of 'async do', which declares an actor's behaviour; nothing alone
    DoToken,       // 'do': the second word of 'async do'; nothing alone
    VoidToken,     // 'void'
    nilToken,      // 'nil'
    nullToken,     // 'null'
    trueToken,     // 'true'
    falseToken,    // 'false'
    UndefToken,    // 'undef'

    // Words held for language features that are documented but not implemented.
    // The lexer never returns this token: it diagnoses the use and hands back an
    // identifier, so the rest of the parse proceeds as it would have.
    ReservedToken,

    NbrTokens
};

// Current lexer
extern Lexer *lex;

#define lexIsToken(tok) (lex->toktype == (tok))

// Is the current token one of the range operators?
#define lexIsRangeOp() (lex->toktype == DotDotToken || lex->toktype == DotDotLessToken || lex->toktype == EllipsisToken)

// Lexer functions
void lexInit(ConeOptions *opt);
void lexInjectPath(char *path);
// The two halves of lexInjectPath, apart: read a file into a block that is not
// yet current, and later make that block current
Lexer *lexLoadPath(char *path);
Lexer *lexNew(char *src, char *url);
void lexPush(Lexer *newlex);
void lexPop();
void lexNextToken();
// Is the token after the current one the keyword 'word'? The lexer is left where it was.
int lexNextIsWord(char *word);
// In a function-reference type's parameter list, does the name the lexer is on
// begin a type ('geomath.Vec3', 'List[i32]') rather than name a parameter?
int lexIdentOpensType();
// Is the token after the current one a lifetime ('a)? The lexer is left where it was.
int lexPeekIsLifetime();
// Is the token after the current one a name? The lexer is left where it was.
int lexPeekIsIdent();
// With the lexer on a name, does a value follow it rather than an operator or
// other continuation of an expression the name begins? ('fill' after '<-')
int lexNextOpensValue();
// With the lexer on the 'each' of an entry after '<-': does it name loop
// variables and 'in' ('each x in src'), rather than just a source ('each src')?
int lexEachHasVars();
// Does this source's first statement begin 'mod' or 'pub mod'? Read off the text
// alone: nothing is lexed and nothing reported.
int lexOpensWithMod(char *src);

#endif
