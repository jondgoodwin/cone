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
    char *url;        // The url where the source text came from
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
} Lexer;

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
    EllipsisToken,     // '...' range, including its end
    PlusToken,         // '+'
    PlusArrayRefToken, // '+[]'
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
    TypedefToken,  // 'typedef'
    StructToken,   // 'struct'
    ModToken,      // 'mod'
    ActorToken,    // 'actor'
    TraitToken,    // 'trait'
    MoveToken,     // '@move'
    OpaqueToken,   // '@opaque'
    UnsizedToken,  // '@unsized'
    CAttrToken,    // '@c': C naming, on a 'mod' line or a 'fn'
    ExtendsToken,  // 'extends'
    MixinToken,    // 'mixin'
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
    VoidToken,     // 'void'
    nilToken,      // 'nil'
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
// Does this source's first statement begin 'mod' or 'pub mod'? Read off the text
// alone: nothing is lexed and nothing reported.
int lexOpensWithMod(char *src);

#endif
