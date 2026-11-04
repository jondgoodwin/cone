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

// The parts of an interpolated string, $"moved {n} files": a text part, or the
// source of a brace's expression (exprp up to exprend, the closing brace), read by
// the parser with a lexer of its own set at exprp (linenbr and linep say where)
typedef struct InterpPart {
    char *text;        // A text part's characters, null-terminated; NULL for an expression
    uint32_t len;      // A text part's length
    char *exprp;       // An expression part's first character, after the open brace
    char *exprend;     // The close brace that ends it
    char *linep;       // Start of the line exprp is on
    uint32_t linenbr;  // That line's number
} InterpPart;

typedef struct InterpLit {
    uint32_t nparts;
    InterpPart parts[1];
} InterpLit;

// Lexer state (one per source file)
typedef struct Lexer {
    // Value info about a discovered token
    union {
        double floatlit;
        uint64_t uintlit;
        char *strlit;
        InterpLit *interp;  // InterpToken
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
    InterpToken,    // Interpolated string literal, $"...{expr}..."
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
    DotDotToken,       // '..', no range operator: lexed to be refused between two operands
    DotDotLessToken,   // '..<' range, excluding its end
    DotDotEqToken,     // '..=' range, including its end
    EllipsisToken,     // '...', no range operator: lexed to be refused
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
    BarToken,          // '|'
    OrOrToken,         // '||': logical or
    CaretToken,        // '^'
    NotToken,          // '!': logical not
    QuesToken,         // '?'
    TildeToken,        // '~'
    LessDashToken,     // '<-'
    AssgnToken,        // '='
    LAssgnToken,       // ':='
    SwapToken,         // '<->'
    FatArrowToken,     // '=>': a closure's short form, 'x => x * 2'
    IsToken,           // 'is'
    EqToken,           // '=='
    NeToken,           // '!='
    SameToken,         // '==='
    NotSameToken,      // '!=='
    LtToken,           // '<'
    LeToken,           // '<='
    ThreeWayToken,     // '<=>': three-way comparison
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
    MustUseToken,  // '@mustUse'
    CAttrToken,    // '@c': C naming, on a 'mod' line or a 'fn'
    InitPureToken, // '@initpure': after 'fn', a function a module's 'init' may call, 'init' among them
    IntrinsicAttrToken, // '@intrinsic': after 'fn' in core, a function whose meaning the compiler supplies
    ThreadLocalToken, // '@threadlocal': after a global's let or var, storage each thread has its own copy of
    WorkgroupToken, // '@workgroup': after a global's let or var, on a GPU storage each workgroup has its own copy of
    ComputeAttrToken, // '@compute(x, y, z)': after 'fn', a compute entry point and its workgroup's size
    ExtendsToken,  // 'extends'
    MixinToken,    // 'mixin', retired: kept a keyword so the parser can point it at 'is'
    UseToken,      // 'use'
    ButToken,      // 'but'
    EnumToken,     // 'enum'
    RetToken,      // 'return'
    WithToken,     // 'with'
    IfToken,       // 'if'
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
    NewToken,      // 'new': a construction, 'Point(1, 2)', and an initializer's permission, '&new'
    TrynewToken,   // 'trynew': an allocation that may fail, 'trynew Rc[mut, Node](1)', giving an Option
    TryToken,      // 'try': a block whose '?' leaves the block, not the function, 'try { a? + b? }'
    AwaitToken,    // 'await': in an actor's behaviour, wait for what is awaited; the behaviour is cut there, a seam
    YieldToken,    // 'yield': in a function declared 'yields', hand the caller a value and wait to be resumed; 'yield each' hands on a sub-generator's
    SelfActorToken, // 'selfactor': in an actor's method, the actor's own handle
    AsyncToken,    // 'async': the first word of 'async fn', which declares an actor's behaviour; nothing alone
    VoidToken,     // 'void'
    nilToken,      // 'nil'
    nullToken,     // 'null'
    trueToken,     // 'true'
    falseToken,    // 'false'
    UndefToken,    // 'undef'
    UsingToken,    // 'using': a pattern run by a function, 'case let rest using prefix("GET ")' (parsefnflow.c)
    LetToken,      // 'let': a declaration whose name is never given a new value (parseDclWord)
    VarToken,      // 'var': a declaration whose name may be given a new value (parseDclWord)

    // The meta layer's words (parsemeta.c). The lexer reads a '#' word whole, as
    // it reads an '@' attribute; these are the ones the language means, and every
    // other '#' word is reported as held (ErrorReserved)
    HashIfToken,   // '#if': chooses which code exists, on facts of the build
    HashElifToken, // '#elif'
    HashElseToken, // '#else'
    HashOsToken,   // '#os': the build's operating system, a fact
    HashArchToken, // '#arch': the build's processor architecture, a fact
    HashDebugToken, // '#debug': whether this is a debug build, a fact
    HashDefinedToken, // '#defined("NAME")': whether a '-D' named it, a fact
    HashDefineToken, // '#define("NAME")': the integer a '-D' gave it, 0 where none did, a fact
    HashEachToken, // '#each': repeats a block once per item of a constant list
    HashFieldsToken, // '#fields': the list of a type parameter's field descriptions, in '#each #f in #fields(T)'
    HashSizeToken, // '#size(T)': the byte size of a type, once its layout settles
    HashAlignToken, // '#align(T)': the byte alignment of a type, once its layout settles
    ReflectToken,  // '@reflect' (placeholder spelling): the type lets reflection see its private fields
    MetaVarToken,  // '#name' where the meta layer has declared it ('#each #x in ..', 'let #n', 'var #i'): the token's ident is the name with its '#'

    // Words held for language features that are documented but not implemented.
    // The lexer never returns this token: it diagnoses the use and hands back an
    // identifier, so the rest of the parse proceeds as it would have.
    ReservedToken,

    NbrTokens
};

// Current lexer
extern Lexer *lex;

// Set by the parser while it reads a '#if' condition, where an unknown '#' word is
// dropped alone and not with the rest of its line; and the count of those the lexer
// has reported and dropped, which tells the parser that a condition lost a word
extern int lexInHashCond;
extern uint32_t lexHashDropped;
// The '#' word the next token scan reads is a meta variable being declared (parsemeta.c); cleared by the scan
extern int lexMetaDecl;
// With the lexer on a token: is the text after it, past white space, a '#'?
int lexNextIsHash();
// Everything the lexer holds of where it stands, to read a stretch of source again
typedef struct LexMark {
    char val[sizeof(((Lexer*)0)->val)];
    uint32_t strlen;
    INode *langtype;
    char *srcp, *tokp, *linep, *prevend, *prevlinep;
    uint32_t linenbr, prevlinenbr;
    uint16_t toktype;
} LexMark;
void lexMarkSave(LexMark *mark);
void lexMarkRestore(LexMark *mark);
// Is the name a meta variable the parser has declared and not yet left behind? (parsemeta.c)
int parseMetaIsBound(Name *name);

#define lexIsToken(tok) (lex->toktype == (tok))

// A declaration begins with 'let' or 'var'. A permission word there is not one:
// 'imm' and 'mut' are the old spellings and 'uni' and the rest declare nothing.
// parseDclWord reports it, so every place that decides a declaration starts here
// takes a permission word too, and the author is told which word to write.
#define lexIsDclStart() (lex->toktype == LetToken || lex->toktype == VarToken || lex->toktype == PermToken)

// Is the current token one of the range operators?
#define lexIsRangeOp() (lex->toktype == DotDotToken || lex->toktype == DotDotLessToken || lex->toktype == DotDotEqToken || lex->toktype == EllipsisToken)

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
// With the lexer on an interpolated string: does an entry of a '<-' list end
// right after it (a ',' ';' ')' '}' or the end of the file comes next)? The lexer is left where it was.
int lexInterpEndsEntry();
// Is the current token a '&' with another '&' directly against it ('&&')? The
// lexer keeps '&&' as two '&' tokens, because it is two borrows in a type or a
// prefix ('&&Pt', '&&x'); only a parser holding an operand reads it as the
// logical and. The lexer is left where it was.
int lexIsAmpAmp();
// In a function-reference type's parameter list, does the name the lexer is on
// begin a type ('geomath.Vec3', 'List[i32]') rather than name a parameter?
int lexIdentOpensType();
// With the lexer on the 'let' or 'var' of a local: does a pattern that binds follow
// ('let Some(n) = x else {...};'), rather than the name of a declaration?
int lexNextOpensPattern();
// With the lexer on a token in the middle of a statement: does the word 'else' stand at the statement's
// own level before its ';'? Read off the text, as lexNextIsWord is.
int lexStatementHasElse();
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
