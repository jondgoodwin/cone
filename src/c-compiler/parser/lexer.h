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

#include <stdint.h>

#define LEX_MAX_INDENTS 1024

// Lexer state (one per source file)
typedef struct Lexer {
    // Value info about a discovered token
    union {
        double floatlit;
        uint64_t uintlit;
        char *strlit;
        Name *ident;
    } val;
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

    // ** Significant indentation state -->
    int16_t curindent;       // Indentation level of current line
    int16_t stmtindent;      // Indentation level of current statement
    int16_t tokPosInLine;    // 0=First token in line, 1=Second, etc.
    char indentch;           // Are we using spaces or tabs?
} Lexer;

// All the possible types for a token
enum TokenTypes {
    EofToken,        // End-of-file

    // Numeric and Identifier tokens
    IntLitToken,    // Integer literal
    FloatLitToken,    // Float literal
    StringLitToken,    // String literal
    IdentToken,        // Identifier
    MetaIdentToken,   // Metaprogramming identifier (#if)
    AttrIdentToken,   // Attribute identifier (@samesize)
    LifetimeToken,    // Lifetime variable ('a)
    PermToken,        // Permission identifier

    // Punctuation tokens
    SemiToken,          // ';'
    ColonToken,         // ':'
    DblColonToken,      // '::'
    LCurlyToken,        // '{'
    RCurlyToken,        // '}'
    LBracketToken,      // '['
    RBracketToken,      // ']'
    LParenToken,        // '('
    RParenToken,        // ')'
    CommaToken,         // ','
    DotToken,           // '.'
    QuesDotToken,       // '?.'
    PlusToken,          // '+'
    DashToken,          // '-'
    StarToken,          // '*'
    PercentToken,       // '%'
    SlashToken,         // '/'
    AmperToken,         // '&'
    AndToken,           // 'and'
    BarToken,           // '|'
    OrToken,            // 'or'
    CaretToken,         // '^'
    NotToken,           // '!'
    QuesToken,          // '?'
    TildeToken,         // '~'
    LessDashToken,      // '<-'
    AssgnToken,         // '='
    IsToken,            // 'is'
    EqToken,            // '=='
    NeToken,            // '!='
    LtToken,            // '<'
    LeToken,            // '<='
    GtToken,            // '>'
    GeToken,            // '>='
    ShlToken,           // '<<'
    ShrToken,           // '>>'
    PlusEqToken,        // '+='
    MinusEqToken,       // '-='
    MultEqToken,        // '*='
    DivEqToken,         // '/='
    RemEqToken,         // '%='
    OrEqToken,          // '|='
    AndEqToken,         // '&='
    XorEqToken,         // '^='
    ShlEqToken,         // '<<='
    ShrEqToken,         // '>>='
    IncrToken,          // '++'
    DecrToken,          // '--'

    // Keywords
    IncludeToken,   // 'include'
    ModToken,       // 'mod'
    ExternToken,    // 'extern'
    SetToken,       // 'set'
    MacroToken,     // 'macro'
    FnToken,        // 'fn'
    TypedefToken,   // 'typedef'
    StructToken,    // 'struct'
    TraitToken,     // 'trait'
    SamesizeToken,  // '@samesize'
    MixinToken,     // 'mixin'
    EnumToken,      // 'enum'
    RegionToken,    // 'region'
    RetToken,       // 'return'
    DoToken,        // 'do'
    WithToken,      // 'with'
    IfToken,        // 'if'
    ElifToken,      // 'elif'
    ElseToken,      // 'else'
    MatchToken,     // 'match'
    LoopToken,      // 'loop'
    WhileToken,     // 'while'
    EachToken,      // 'each'
    InToken,        // 'in'
    ByToken,        // 'step'
    BreakToken,        // 'break'
    ContinueToken,    // 'continue'
    AsToken,        // 'as'
    IntoToken,      // 'into'
    trueToken,        // 'true'
    falseToken,        // 'false'
    nullToken,      // 'null'

    NbrTokens
};

// Current lexer
Lexer *lex;

#define lexIsToken(tok) (lex->toktype == (tok))

// Lexer functions
void lexInit();
void lexInjectFile(char *url);
void lexInject(char *url, char *src);
void lexPop();
void lexNextToken();

// Is next token at start of line?
int lexIsEndOfLine();

// Parser signals the start of a new statement (for continuation analysis)
void lexStmtStart();

// Return true if current token is first on a line that has not been indented
// This is used by parser to determine whether an operator that starts a new line
// should be treated as a continuation (infix) or a new statement (prefix).
int lexIsStmtBreak();

#endif
