/** Lexer
 * @file
 *
 * The lexer divides up the source program into tokens, producing each for the parser on demand.
 * The lexer assumes UTF-8 encoding for the source program.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "lexer.h"
#include "../ir/ir.h"
#include "../ir/nametbl.h"
#include "../shared/error.h"
#include "../shared/fileio.h"
#include "../shared/memory.h"
#include "../shared/timer.h"
#include "../shared/utf8.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

// Global lexer state
Lexer *lex = NULL;        // Current lexer

// A lexer block for a source, positioned at its start and not yet current.
//
// Every source gets its own block, never a recycled one. A block outlives
// parsing: each IR node stores the Lexer that was current when it was built and
// reads ->url from it whenever a diagnostic is reported, and conec.c reads
// ->fname off the program node to name its output files. So re-using a popped
// block rewrote the url out from under every node still pointing at it, and a
// diagnostic against an earlier module named a later module's file while
// echoing the earlier one's source line.
Lexer *lexNew(char *src, char *url) {
    Lexer *newlex = (Lexer*) memAllocBlk(sizeof(Lexer));
    newlex->next = NULL;
    newlex->prev = NULL;

    // Skip over UTF8 Byte-order mark (BOM = U+FEFF) at start of source, if there
    if (*src=='\xEF' && *(src+1)=='\xBB' && *(src+2)=='\xBF')
        src += 3;

    // Initialize lexer's source info
    newlex->url = url;
    newlex->fname = fileName(url);
    newlex->source = src;

    // Initialize lexer context
    newlex->srcp = newlex->tokp = newlex->linep = src;
    newlex->linenbr = 1;
    newlex->flags = 0;
    newlex->prevend = src;
    newlex->prevlinep = src;
    newlex->prevlinenbr = 1;
    return newlex;
}

// Make a block lexNew built the current lexer, and read its first token
void lexPush(Lexer *newlex) {
    Lexer *prev = lex;
    lex = newlex;
    if (prev)
        prev->next = lex;
    lex->next = NULL;
    lex->prev = prev;

    // Prime the pump with the first token
    lexNextToken();
}

// Inject a new source stream into the lexer
void lexInject(char *src, char *url) {
    lexPush(lexNew(src, url));
}

// Add a reserved identifier and its node to the global name table
Name *keyAdd(char *keyword, uint16_t toktype) {
    Name *sym;
    INode *node;
    sym = nametblFind(keyword, strlen(keyword));
    sym->node = node = (INode*)memAllocBlk(sizeof(INode));
    node->tag = KeywordTag;
    node->flags = toktype;
    return sym;
}

// Populate global name table with all reserved identifiers & their nodes
void keywordInit() {
    // Retired rather than reserved-ahead: the parser reports the statement it
    // began and skips it, which the reserved words' release to an identifier
    // below would turn into a cascade
    keyAdd("include", IncludeToken);
    keyAdd("import", ImportToken);
    keyAdd("extern", ExternToken);
    keyAdd("pub", PubToken);
    keyAdd("static", StaticToken);
    keyAdd("macro", MacroToken);
    keyAdd("fn", FnToken);
    keyAdd("overload", OverloadToken);
    keyAdd("const", ConstToken);
    keyAdd("typedef", TypedefToken),
    // The kinds a type declaration may be, and the modifier that makes one
    // abstract. 'trait' is not a kind of its own: written by itself it means
    // 'struct trait'. 'mod' and 'actor' name kinds the grammar admits and the
    // compiler does not build yet, so that 'mod trait' and 'actor trait' can be
    // written the day those kinds arrive rather than having to be designed then.
    keyAdd("struct", StructToken);
    keyAdd("mod", ModToken);
    keyAdd("actor", ActorToken);
    keyAdd("trait", TraitToken);
    keyAdd("@move", MoveToken);
    keyAdd("@opaque", OpaqueToken);
    keyAdd("@unsized", UnsizedToken);
    keyAdd("extends", ExtendsToken);
    keyAdd("mixin", MixinToken);
    keyAdd("use", UseToken);
    keyAdd("but", ButToken);
    keyAdd("enum", EnumToken);
    keyAdd("return", RetToken);
    keyAdd("with", WithToken);
    keyAdd("if", IfToken);
    keyAdd("elif", ElifToken);
    keyAdd("else", ElseToken);
    keyAdd("case", CaseToken);
    keyAdd("match", MatchToken);
    keyAdd("while", WhileToken);
    keyAdd("each", EachToken);
    keyAdd("in", InToken);
    keyAdd("by", ByToken);
    keyAdd("break", BreakToken);
    keyAdd("continue", ContinueToken);
    keyAdd("not", NotToken);
    keyAdd("or", OrToken);
    keyAdd("and", AndToken);
    keyAdd("as", AsToken);
    keyAdd("is", IsToken);
    keyAdd("into", IntoToken);
    keyAdd("inline", InlineToken);

    keyAdd("void", VoidToken);
    keyAdd("nil", nilToken);
    keyAdd("true", trueToken);
    keyAdd("false", falseToken);
    keyAdd("undef", UndefToken);

    // Words the language claims but has not implemented yet. Holding them now
    // costs one rename in a program written today; letting a program bind one
    // costs that program a rewrite when the feature arrives.
    //
    // The first group is every keyword reftoken.html already publishes as
    // reserved that is not a token above. 'self' and 'this' are on that list too
    // and are deliberately absent here: both already work, as a method's first
    // parameter and as a 'with' block's value, so they are implemented rather
    // than reserved.
    keyAdd("async", ReservedToken);
    keyAdd("baseurl", ReservedToken);
    keyAdd("context", ReservedToken);
    keyAdd("local", ReservedToken);
    keyAdd("new", ReservedToken);
    keyAdd("selfmethod", ReservedToken);
    keyAdd("using", ReservedToken);
    keyAdd("wait", ReservedToken);
    keyAdd("yield", ReservedToken);

    // The second group spells the syntax of features the reference describes but
    // reftoken.html has not caught up with: refexcept.html for error handling,
    // refcorout.html for coroutines, refconccomm.html for actors. 'actor' itself
    // is not here: it names a kind of declaration, so it is a token above and
    // the parser reports it where the declaration is written.
    keyAdd("throw", ReservedToken);
    keyAdd("catch", ReservedToken);
    keyAdd("panic", ReservedToken);
    keyAdd("assert", ReservedToken);
    keyAdd("spawn", ReservedToken);
}

// Initialize lexer
void lexInit(ConeOptions *opt) {
    fileSearchPaths = opt->package_search_paths;
    lexInject("", "init");
    keywordInit();
}

// Inject an already-located source file into the lexer. Locating a file is the
// caller's, because the path is what the file registry is keyed by and what
// every diagnostic against the file names: it has to be in hand, and asked
// about, before the file is read
void lexInjectPath(char *path) {
    lexPush(lexLoadPath(path));
}

// Read an already-located source file into a block of its own that is not yet
// current. A module takes its position from its designated file's block before
// that file is parsed, and lexPush later makes the same block current, so the
// file is read once
Lexer *lexLoadPath(char *path) {
    timerBegin(LoadTimer);
    char *src = fileLoad(path);
    if (!src)
        errorExit(ExitNF, "Cannot read source file %s", path);

    timerBegin(ParseTimer);
    return lexNew(src, path);
}

// Restore previous lexer's stream
void lexPop() {
    if (lex)
        lex = lex->prev;
}

// Handle new line character: count it, and remember where the line starts.
// Indentation is not measured. The grammar has no use for it: a block is
// delimited by braces and a statement ends at ';', so a line's leading
// whitespace is formatting and nothing more.
char *lexNewLine(char *srcp) {
    srcp++;
    lex->linep = srcp;
    ++lex->linenbr;
    return srcp;
}

// ******  TOKEN-SPECIFIC LEXING **********

/** Return value of hex digit, or -1 if not correct */
char *lexHexDigits(int cnt, char *srcp, uint64_t *val) {
    *val = 0;
    while (cnt--) {
        *val <<= 4;
        if (*srcp>='0' && *srcp<='9')
            *val += *srcp++ - '0';
        else if (*srcp>='A' && *srcp<='F')
            *val += *srcp++ - ('A' - 10);
        else if (*srcp>='a' && *srcp<='f')
            *val += *srcp++ - ('a' - 10);
        else {
            errorMsgLex(ErrorBadTok, "Invalid hexadecimal character '%.*s'", utf8ByteSkip(srcp), srcp);
            return srcp;
        }
    }
    return srcp;
}

/** Turn escape sequence into a single character */
char *lexScanEscape(char *srcp, uint64_t *charval) {
    switch (*++srcp) {
    case 'a': *charval = '\a'; return ++srcp;
    case 'b': *charval = '\b'; return ++srcp;
    case 'f': *charval = '\f'; return ++srcp;
    case 'n': *charval = '\n'; return ++srcp;
    case 'r': *charval = '\r'; return ++srcp;
    case 't': *charval = '\t'; return ++srcp;
    case 'v': *charval = '\v'; return ++srcp;
    case '\'': *charval = '\''; return ++srcp;
    case '\"': *charval = '\"'; return ++srcp;
    case '\\': *charval = '\\'; return ++srcp;
    case ' ': *charval = ' '; return ++srcp;
    case '\0': *charval = '\0'; return ++srcp;
    case 'x': return lexHexDigits(2, ++srcp, charval);
    case 'u': return lexHexDigits(4, ++srcp, charval);
    case 'U': return lexHexDigits(8, ++srcp, charval);
    default:
        errorMsgLex(ErrorBadTok, "Invalid escape sequence '%.*s'", utf8ByteSkip(srcp), srcp);
        *charval = *srcp++;
        return srcp;
    }
}

/** Tokenize a lifetime annotation or character literal */
void lexScanChar(char *srcp) {
    char *srcbeg = srcp;
    lex->tokp = srcp++;

    // Assume we have a lifetime variable if it starts with a letter, not followed by close single quote
    if (isalpha(*srcp) && *(srcp+1)!='\'') {
        while (isalnum(*srcp))
            ++srcp;
        // Accept it if next char is non-single quote punctuation
        if (*srcp != '\'' && !(*srcp & 0x80)) {
            lex->val.ident = nametblFind(srcbeg, srcp - srcbeg);
            lex->toktype = LifetimeToken;
            lex->srcp = srcp;
            return;
        }
        // This is not a lifetime variable. Reset to try it as a character literal
        srcp = lex->tokp;
    }

    // Obtain a single character/unicode (possibly escaped)
    int isUnicode = 0;
    if (*srcp == '\\') {
        isUnicode = *(srcp + 1) == 'u' || *(srcp + 1) == 'U';
        srcp = lexScanEscape(srcp, &lex->val.uintlit);
    }
    else
        lex->val.uintlit = *srcp++;

    // If following character is end quote, return as integer literal
    if (*srcp == '\'')
    {
        srcp++;
        if (*srcp == 'u') {
            lex->langtype = (INode*)u32Type;
            srcp++;
        }
        else
            lex->langtype = isUnicode || lex->val.uintlit >= 0x100 ? (INode*)u32Type : (INode*)u8Type;
        lex->toktype = IntLitToken;
        lex->srcp = srcp;
        return;
    }

    // Not a recognizable token. Skip forward, error and pretend we have a char literal anyway
    while (*srcp && *srcp != '\n') {
        if (*srcp == '\'') {
            ++srcp;
            break;
        }
        ++srcp;
    }
    errorMsgLex(ErrorBadTok, "Invalid lifetime or too-long character literal");
    lex->langtype = (INode*)u8Type;
    lex->toktype = IntLitToken;
    lex->srcp = srcp;
}

void lexScanString(char *srcp) {
    uint64_t uchar;
    lex->tokp = srcp++;

    // Conservatively count the size of the string
    uint32_t srclen = 0;
    while (*srcp && *srcp != '"') {
        srclen++;
        srcp++;
        if (*srcp == '\\' && *(srcp + 1) == '"') {
            srclen++;
            srcp += 2;
        }
    }

    // Build string literal
    char *newp = memAllocStr(NULL, srclen);
    srclen = 0;
    lex->val.strlit = newp;
    srcp = lex->tokp+1;
    while (*srcp != '"' && *srcp) {
        // discard all control chars, including spaces after new-line
        if ((unsigned char)*srcp < ' ') {
            if (*srcp++ == '\n') {
                ++lex->linenbr;
                lex->linep = srcp;
                while (*srcp <= ' ' && *srcp)
                    ++srcp;
            }
            continue;
        }

        // Copy over next byte or an escaped character
        if (*srcp != '\\') {
            *newp++ = *srcp++; // Works for utf8-encoded characters as well
            srclen++;
        }
        else {
            // Handle escaped character(s), including unicode
            int isUnicode = *(srcp + 1) == 'u' || *(srcp + 1) == 'U';
            srcp = lexScanEscape(srcp, &uchar);
            if (!isUnicode || uchar < 0x80) {
                *newp++ = (unsigned char)uchar;
                srclen++;
            }
            else if (uchar<0x800) {
                *newp++ = 0xC0 | (unsigned char)(uchar >> 6);
                *newp++ = 0x80 | (uchar & 0x3f);
                srclen+=2;
            }
            else if (uchar<0x10000) {
                *newp++ = 0xE0 | (unsigned char)(uchar >> 12);
                *newp++ = 0x80 | ((uchar >> 6) & 0x3F);
                *newp++ = 0x80 | (uchar & 0x3f);
                srclen+=3;
            }
            else if (uchar<0x110000) {
                *newp++ = 0xF0 | (unsigned char)(uchar >> 18);
                *newp++ = 0x80 | ((uchar >> 12) & 0x3F);
                *newp++ = 0x80 | ((uchar >> 6) & 0x3F);
                *newp++ = 0x80 | (uchar & 0x3f);
                srclen+=4;
            }
        }
    }
    *newp = '\0';  // Backstop with null to be careful
    if (*srcp == '"')
        srcp++;        // Move past terminating " character

    lex->strlen = srclen;  // Count of all characters of string, except final null char.
    lex->toktype = StringLitToken;
    lex->srcp = srcp;
}

// Convert ascii float number to double float
double lexToFloat(char *srcp, char *srcend) {
    // Copy ascii literal to number, stripping out underscores
    char number[1000];
    if (srcend - srcp > 999)
        srcend = srcp + 999; // avoid overflow
    char *nbrp = number;
    while (srcp < srcend) {
        if (*srcp != '_')
            *nbrp++ = *srcp;
        ++srcp;
    }
    *nbrp = '\0';

    return atof(number);
}

/** Tokenize an integer or floating point number */
void lexScanNumber(char *srcp) {

    char *srcbeg;        // Pointer to the start of the token
    uint64_t base;        // Radix for integer (10 or 16)
    uint64_t intval;    // Calculated integer value for integer literal
    uint64_t digit;     // Value of the digit being accumulated
    char isFloat;        // nonzero when number token is a float, 'e' when in exponent
    char overflow;      // nonzero once the digits no longer fit in intval

    lex->tokp = srcbeg = srcp;

    // A leading zero may indicate a non-base 10 number
    base = 10;
    if (*srcp=='0' && (*(srcp+1)=='x' || *(srcp+1)=='X')) {
        base = 16;
        srcp += 2;
    }

    // Validate and process remaining numeric digits
    isFloat = '\0';
    overflow = '\0';
    intval = 0;
    while (1) {
        // Only one exponent allowed. In a hex literal 'e' and 'E' are digits,
        // so only 'p' or 'P' can begin its exponent
        if (isFloat!='e' && ((base==10 && (*srcp=='e' || *srcp=='E')) || *srcp=='p' || *srcp=='P')) {
            isFloat = 'e';
            if (*++srcp == '-' || *srcp == '+')
                srcp++;
            continue;
        }
        // Handle characters in a suspected integer
        // Decimal point means it is floating point after all
        if (*srcp=='.') {
            // However, double periods is not floating point, but that subsequent token is range op
            if (*(srcp+1)=='.')
                break;
            srcp++;
            isFloat = '.';
            continue;
        }
        // Extract a number digit value from the character
        if (*srcp>='0' && *srcp<='9')
            digit = *srcp++ - '0';
        else if (*srcp=='_') {
            srcp++;
            continue;
        }
        else if (base==16 && *srcp>='A' && *srcp<='F')
            digit = *srcp++ - 'A'+10;
        else if (base==16 && *srcp>='a' && *srcp<='f')
            digit = *srcp++ - 'a'+10;
        else
            break;
        // Accumulate the digit unless it would carry past 64 bits. The digits
        // are still consumed so the token ends where it should, and an integer
        // that overflowed is reported once, below, once its suffix is known.
        if (intval > (UINT64_MAX - digit) / base)
            overflow = '\1';
        else if (!overflow)
            intval = intval*base + digit;
    }

    // Process number's explicit type as part of the token
    if (*srcp=='d') {
        isFloat = 'd';
        srcp++;
        lex->langtype = (INode*)f64Type;
    } else if (*srcp=='f') {
        isFloat = 'f';
        lex->langtype = (INode*)f32Type;
        if (*(++srcp)=='6' && *(srcp+1)=='4') {
            lex->langtype = (INode*)f64Type;
            srcp += 2;
        }
        else if (*srcp=='3' && *(srcp+1)=='2')
            srcp += 2;
    } else if (*srcp=='i') {
        lex->langtype = (INode*)i32Type;
        if (*(++srcp)=='8') {        
            srcp++; lex->langtype = (INode*)i8Type;
        } else if (*srcp=='1' && *(srcp+1)=='6') {
            srcp += 2; lex->langtype = (INode*)i16Type;
        } else if (*srcp=='3' && *(srcp+1)=='2') {
            srcp += 2;
        } else if (*srcp=='6' && *(srcp+1)=='4') {
            srcp += 2; lex->langtype = (INode*)i64Type;
        } else if (strncmp(srcp, "size", 4)==0) {
            srcp += 4; lex->langtype = (INode*)isizeType;
        }
    } else if (*srcp=='u') {
        lex->langtype = (INode*)u32Type;
        if (*(++srcp)=='8') {        
            srcp++; lex->langtype = (INode*)u8Type;
        } else if (*srcp=='1' && *(srcp+1)=='6') {
            srcp += 2; lex->langtype = (INode*)u16Type;
        } else if (*srcp=='3' && *(srcp+1)=='2') {
            srcp += 2;
        } else if (*srcp=='6' && *(srcp+1)=='4') {
            srcp += 2; lex->langtype = (INode*)u64Type;
        } else if (strncmp(srcp, "size", 4)==0) {
            srcp += 4; lex->langtype = (INode*)usizeType;
        }
    }
    else
        lex->langtype = isFloat ? (INode*)f32Type : unknownType;

    // Set value and type
    if (isFloat) {
        lex->val.floatlit = lexToFloat(srcbeg, srcp);
        lex->toktype = FloatLitToken;
    }
    else {
        if (overflow)
            errorMsgLex(ErrorLitOverflow, "Integer literal '%.*s' does not fit in 64 bits", (int)(srcp - srcbeg), srcbeg);
        lex->val.uintlit = intval;
        lex->toktype = IntLitToken;
    }
    lex->srcp = srcp;
}

/** Tokenize an identifier or reserved token. Returns 0 when the word was
 * reported and dropped instead, with lex->srcp where scanning resumes: only a
 * '@' or '#' word that names nothing is. */
int lexScanIdent(char *srcp) {
    char *srcbeg = srcp;    // Pointer to the start of the token
    lex->tokp = srcbeg;
    srcp += utf8ByteSkip(srcp);  // Skip past already accepted first character
    while (1) {
        switch (*srcp) {

        // Allow digit, letter or underscore in token
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        case 'a': case 'b': case 'c': case 'd': case 'e':
        case 'f': case 'g': case 'h': case 'i': case 'j':
        case 'k': case 'l': case 'm': case 'n': case 'o':
        case 'p': case 'q': case 'r': case 's': case 't':
        case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
        case 'A': case 'B': case 'C': case 'D': case 'E':
        case 'F': case 'G': case 'H': case 'I': case 'J':
        case 'K': case 'L': case 'M': case 'N': case 'O':
        case 'P': case 'Q': case 'R': case 'S': case 'T':
        case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
        case '_':
            srcp++;
            break;

        default:
            // Allow unicode letters in identifier name
            if (utf8IsLetter(srcp)) {
                srcp += utf8ByteSkip(srcp);
            }
            else {
                INode *identNode;
                // Find identifier token in name table and preserve info about it
                // Substitute token type when identifier is a keyword
                lex->val.ident = nametblFind(srcbeg, srcp-srcbeg);
                identNode = (INode*)lex->val.ident->node;
                if (identNode && identNode->tag == KeywordTag) {
                    lex->toktype = identNode->flags;
                    // A reserved word has no syntax to parse. Report it where it
                    // was written, then release the name so the rest of the
                    // compile treats it as the ordinary identifier the author
                    // meant. Releasing it also reports each reserved word once,
                    // at its first appearance, rather than at every use.
                    if (lex->toktype == ReservedToken) {
                        lex->srcp = srcp;
                        errorMsgLex(ErrorReserved,
                            "'%s' is reserved for a language feature that is not implemented yet. Rename it.",
                            &lex->val.ident->namestr);
                        lex->val.ident->node = NULL;
                        lex->toktype = IdentToken;
                        return 1;
                    }
                }
                else if (identNode && identNode->tag == PermTag)
                    lex->toktype = PermToken;
                // Every attribute is a keyword ('@move', '@opaque', '@unsized'),
                // so a '@' word that reaches here names none. It is reported
                // and dropped, and what follows it is read as though it were
                // absent.
                else if (*srcbeg == '@') {
                    if (strcmp(&lex->val.ident->namestr, "@samesize") == 0)
                        errorMsgLex(ErrorUnkAttr,
                            "'@samesize' is not an attribute: an enum is same-size by default, and '@unsized' declines it");
                    else
                        errorMsgLex(ErrorUnkAttr, "'%s' is not a Cone attribute",
                            &lex->val.ident->namestr);
                    lex->srcp = srcp;
                    return 0;
                }
                // '#' is held for metaprogramming. A '#' word is reported and
                // dropped with the rest of its line, so that what is written
                // after it ('#if x') is not reported as well.
                else if (*srcbeg == '#') {
                    errorMsgLex(ErrorReserved,
                        "'%s': '#' is reserved for metaprogramming, which is not implemented yet",
                        &lex->val.ident->namestr);
                    while (*srcp && *srcp != '\n' && *srcp != '\x1a')
                        srcp++;
                    lex->srcp = srcp;
                    return 0;
                }
                else
                    lex->toktype = IdentToken;
                lex->srcp = srcp;
                return 1;
            }
        }
    }
}

/** Tokenize an identifier or reserved token */
void lexScanTickedIdent(char *srcp) {
    char *srcbeg = srcp++;    // Pointer to the start of the token
    lex->tokp = srcbeg;

    // Look for closing backtick, but not past end of line
    while (*srcp != '`' && *srcp && *srcp != '\n' && *srcp != '\x1a')
        srcp++;
    if (*srcp != '`') {
        errorMsgLex(ErrorBadTok, "Back-ticked identifier requires closing backtick");
        srcp = srcbeg + 2;
    }

    // Find identifier token in name table and preserve info about it
    lex->val.ident = nametblFind(srcbeg+1, srcp - srcbeg - 1);
    lex->toktype = IdentToken;
    lex->srcp = srcp+1;
}

// Skip over nested block comment. Every line inside it is counted, so the
// first diagnostic after the comment names the right line.
char *lexBlockComment(char *srcp) {
    int nest = 1;
    while (*srcp) {
        if (*srcp == '\n')
            srcp = lexNewLine(srcp);
        else if (*srcp == '*' && *(srcp + 1) == '/') {
            if (--nest == 0)
                return srcp+2;
            ++srcp; ++srcp;
        }
        else if (*srcp == '/' && *(srcp + 1) == '*') {
            ++nest;
            ++srcp; ++srcp;
        }
        // ignore tokens inside line comment
        else if (*srcp == '/' && *(srcp + 1) == '/') {
            srcp += 2;
            while (*srcp && *srcp != '\n')
                ++srcp;
        }
        // ignore tokens inside string literal
        else if (*srcp == '"') {
            ++srcp;
            while (*srcp && *srcp != '"') {
                if (*srcp == '\n')
                    srcp = lexNewLine(srcp);
                else if (*srcp == '\\' && *(srcp + 1))
                    srcp += 2;
                else
                    ++srcp;
            }
            if (*srcp)
                ++srcp;
        }
        else
            ++srcp;
    }
    return srcp;
}

// Shortcut macro for return a punctuation token
#define lexReturnPuncTok(tok, skip) { \
    lex->toktype = tok; \
    lex->tokp = srcp; \
    lex->srcp = srcp + (skip); \
    return; \
}

// Decode next token from the source into new lex->token
void lexNextTokenx() {
    char *srcp;
    srcp = lex->srcp;
    lex->prevend = srcp;
    lex->prevlinep = lex->linep;
    lex->prevlinenbr = lex->linenbr;
    while (1) {
        switch (*srcp) {

        // Numeric literal (integer or float)
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            lexScanNumber(srcp);
            return;

        // ' ' - single character surrounded with single quotes
        case '\'':
            lexScanChar(srcp);
            return;

        // " " - string surrounded with double quotes
        case '"':
            lexScanString(srcp);
            return;

        // Identifier
        case 'a': case 'b': case 'c': case 'd': case 'e':
        case 'f': case 'g': case 'h': case 'i': case 'j':
        case 'k': case 'l': case 'm': case 'n': case 'o':
        case 'p': case 'q': case 'r': case 's': case 't':
        case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
        case 'A': case 'B': case 'C': case 'D': case 'E':
        case 'F': case 'G': case 'H': case 'I': case 'J':
        case 'K': case 'L': case 'M': case 'N': case 'O':
        case 'P': case 'Q': case 'R': case 'S': case 'T':
        case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
        case '_':
            lexScanIdent(srcp);
            return;

        // An attribute; or a '@' or '#' word naming nothing, reported and skipped
        case '#': case '@':
            if (lexScanIdent(srcp))
                return;
            srcp = lex->srcp;
            break;

        // backtick enclosed identifiers
        case '`':
            lexScanTickedIdent(srcp);
            return;

        // A range operator is two or three periods. Only a match's range
        // pattern reads one today (refmatch.html); an integer literal already
        // stops at '..', so '0..3' is a range and not a float.
        case '.':
            if (*(srcp + 1) == '.') {
                if (*(srcp + 2) == '.')
                    lexReturnPuncTok(EllipsisToken, 3);
                lexReturnPuncTok(DotDotToken, 2);
            }
            lexReturnPuncTok(DotToken, 1);
        case ',': lexReturnPuncTok(CommaToken, 1);
        case '~': lexReturnPuncTok(TildeToken, 1);

        case '+': 
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(PlusEqToken, 2);
            }
            else if (*(srcp + 1) == '+') {
                lexReturnPuncTok(IncrToken, 2);
            }
            else if (*(srcp + 1) == '[' && *(srcp + 2) == ']') {
                lexReturnPuncTok(PlusArrayRefToken, 3);
            }
            else if (*(srcp + 1) == '<') {
                lexReturnPuncTok(PlusVirtRefToken, 2);
            }
            else {
                lexReturnPuncTok(PlusToken, 1);
            }

        case '-': 
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(MinusEqToken, 2);
            }
            else if (*(srcp + 1) == '-') {
                lexReturnPuncTok(DecrToken, 2);
            }
            else {
                lexReturnPuncTok(DashToken, 1);
            }

        case '*': 
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(MultEqToken, 2);
            }
            else {
                lexReturnPuncTok(StarToken, 1);
            }

        case '%': 
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(RemEqToken, 2);
            }
            else {
                lexReturnPuncTok(PercentToken, 1);
            }

        case '^': 
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(XorEqToken, 2);
            }
            else {
                lexReturnPuncTok(CaretToken, 1);
            }

        case '&': 
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(AndEqToken, 2);
            }
            else if (*(srcp + 1) == '[' && *(srcp + 2) == ']') {
                lexReturnPuncTok(ArrayRefToken, 3);
            }
            else if (*(srcp + 1) == '<') {
                lexReturnPuncTok(VirtRefToken, 2);
            }
            else {
                lexReturnPuncTok(AmperToken, 1);
            }

        case '|': 
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(OrEqToken, 2);
            }
            else {
                lexReturnPuncTok(BarToken, 1);
            }

        // '=', '==' and '==='
        case '=':
            if (*(srcp + 1) == '=')    {
                if (*(srcp + 2) == '=') {
                    lexReturnPuncTok(SameToken, 3);
                }
                else {
                    lexReturnPuncTok(EqToken, 2);
                }
            }
            else {
                lexReturnPuncTok(AssgnToken, 1);
            }

        // '!', '!=' and '!=='
        case '!':
            if (*(srcp + 1) == '=') {
                if (*(srcp + 2) == '=') {
                    lexReturnPuncTok(NotSameToken, 3);
                }
                else {
                    lexReturnPuncTok(NeToken, 2);
                }
            }
            else {
                lexReturnPuncTok(NotToken, 1);
            }

        // '<' and '<='
        case '<':
            if (*(srcp + 1) == '-') {
                lexReturnPuncTok(LessDashToken, 2);
            }
            else if (*(srcp + 1) == '=') {
                if (*(srcp + 2) == '>') {
                    lexReturnPuncTok(SwapToken, 3);
                }
                else {
                    lexReturnPuncTok(LeToken, 2);
                }
            }
            else if (*(srcp + 1) == '<') {
                if (*(srcp + 2) == '=') {
                    lexReturnPuncTok(ShlEqToken, 3);
                }
                else {
                    lexReturnPuncTok(ShlToken, 2);
                }
            }
            else {
                lexReturnPuncTok(LtToken, 1);
            }

        // '>' and '>='
        case '>':
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(GeToken, 2);
            }
            else if (*(srcp + 1) == '>') {
                if (*(srcp + 2) == '=') {
                    lexReturnPuncTok(ShrEqToken, 3);
                }
                else {
                    lexReturnPuncTok(ShrToken, 2);
                }
            }
            else {
                lexReturnPuncTok(GtToken, 1);
            }

        // '?.' is held for None propagation (refoption.html). It is reported
        // and read as '.', so the member it reaches is parsed as it was meant.
        case '?':
            if (*(srcp + 1) == '.') {
                lex->tokp = srcp;
                errorMsgLex(ErrorReserved,
                    "'?.' is reserved for None propagation, which is not implemented yet");
                lexReturnPuncTok(DotToken, 2);
            }
            else
                lexReturnPuncTok(QuesToken, 1);
        case '[': 
            lexReturnPuncTok(LBracketToken, 1);
        case ']': 
            lexReturnPuncTok(RBracketToken, 1);
        case '(': 
            lexReturnPuncTok(LParenToken, 1);
        case ')': 
            lexReturnPuncTok(RParenToken, 1);

        // ':' and ':='
        case ':':
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(LAssgnToken, 2);
            }
            else {
                lexReturnPuncTok(ColonToken, 1);
            }

        // ';'
        case ';':
            lexReturnPuncTok(SemiToken, 1);

        case '{': 
            lexReturnPuncTok(LCurlyToken, 1);
        case '}': 
            lexReturnPuncTok(RCurlyToken, 1);
        
        // '/' or '//' or '/*'
        case '/':
            // Line comment: '//'
            if (*(srcp+1)=='/') {
                srcp += 2;
                while (*srcp && *srcp!='\n' && *srcp!='\x1a')
                    srcp++;
            }
            // Block comment, nested: '/*'
            else if (*(srcp + 1) == '*') {
                srcp = lexBlockComment(srcp+2);
            }
            // '/' operator (e.g., division)
            else if (*(srcp + 1) == '=') {
                lexReturnPuncTok(DivEqToken, 2);
            }
            else
                lexReturnPuncTok(SlashToken, 1);
            break;

        // Ignore white space
        case ' ': case '\t':
            srcp++;
            break;

        // Ignore carriage return
        case '\r':
            srcp++;
            break;

        // Handle new line
        case '\n':
            srcp = lexNewLine(srcp);
            break;

        // End-of-file
        case '\0': case '\x1a':
            lexReturnPuncTok(EofToken, 0);

        // Bad character
        default:
            {
                if (utf8IsLetter(srcp)) {
                    // Treat unicode character as the start of an identifier
                    lexScanIdent(srcp);
                    return;
                }
                else {
                    lex->tokp = srcp;
                    errorMsgLex(ErrorBadTok, "Bad character '%.*s' starting unknown token", utf8ByteSkip(srcp), srcp);
                    srcp += utf8ByteSkip(srcp);
                }
            }
        }
    }
}

// Obtain next token (and time how long it takes)
void lexNextToken() {
    timerBegin(LexTimer);
    lexNextTokenx();
    timerBegin(ParseTimer);
}

// Is the token after the current one the keyword 'word'? A look at the source
// text and nothing more: no token is lexed, so nothing is reported twice and the
// lexer stays where it was. Only white space may come between; a comment there
// hides the keyword. The grammar needs this in one place -- a 'pub' after a
// declaration, which is a fold clause's 'pub use' or else the start of the next
// statement after a missing ';' -- and one keyword is all it ever looks for.

// Whether the text at srcp is the keyword 'word'. The word must end where the
// keyword does, not run on into a longer name
static int lexIsWordAt(char *srcp, char *word) {
    size_t len = strlen(word);
    if (strncmp(srcp, word, len) != 0)
        return 0;
    char after = srcp[len];
    return !(isalnum((unsigned char)after) || after == '_' || (after & 0x80));
}

int lexNextIsWord(char *word) {
    char *srcp = lex->srcp;
    while (*srcp == ' ' || *srcp == '\t' || *srcp == '\r' || *srcp == '\n')
        srcp++;
    return lexIsWordAt(srcp, word);
}

// Pass over the white space and comments in front of a token, on text that is
// not a block: nothing is counted, since there is no block to count it in
static char *lexSkipTrivia(char *srcp) {
    while (1) {
        if (*srcp == ' ' || *srcp == '\t' || *srcp == '\r' || *srcp == '\n')
            srcp++;
        else if (*srcp == '/' && srcp[1] == '/') {
            while (*srcp && *srcp != '\n' && *srcp != '\x1a')
                srcp++;
        }
        else if (*srcp == '/' && srcp[1] == '*') {
            // Nested, and blind to what is inside a line comment or a string,
            // as lexBlockComment is
            int nest = 1;
            srcp += 2;
            while (*srcp && nest > 0) {
                if (*srcp == '*' && srcp[1] == '/') {
                    --nest;
                    srcp += 2;
                }
                else if (*srcp == '/' && srcp[1] == '*') {
                    ++nest;
                    srcp += 2;
                }
                else if (*srcp == '/' && srcp[1] == '/') {
                    while (*srcp && *srcp != '\n')
                        ++srcp;
                }
                else if (*srcp == '"') {
                    ++srcp;
                    while (*srcp && *srcp != '"')
                        srcp += (*srcp == '\\' && srcp[1]) ? 2 : 1;
                    if (*srcp)
                        ++srcp;
                }
                else
                    ++srcp;
            }
        }
        else
            return srcp;
    }
}

// Does this source's first statement begin 'mod' or 'pub mod'? The folder sweep
// asks it of every file it finds before any file is parsed, because the answer
// decides which module the file is: one that opens with a 'mod' declaration is a
// module of its own. It is read off the text, so nothing is lexed twice and
// nothing about the file's first tokens is reported ahead of its parse
int lexOpensWithMod(char *src) {
    char *srcp = lexSkipTrivia(src);
    if (lexIsWordAt(srcp, "pub"))
        srcp = lexSkipTrivia(srcp + 3);
    return lexIsWordAt(srcp, "mod");
}
