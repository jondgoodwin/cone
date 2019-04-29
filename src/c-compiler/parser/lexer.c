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

#include <stdio.h>

// Global lexer state
Lexer *lex = NULL;        // Current lexer

// Inject a new source stream into the lexer
void lexInject(char *url, char *src) {
    Lexer *prev;

    // Obtain next lexer block via link chain or allocation
    prev = lex;
    if (lex == NULL)
        lex = (Lexer*) memAllocBlk(sizeof(Lexer));
    else if (lex->next == NULL) {
        lex->next = (Lexer*) memAllocBlk(sizeof(Lexer));
        lex = lex->next;
    }
    else
        lex = lex->next; // Re-use an old lexer block
    lex->next = NULL;
    lex->prev = prev;

    // Skip over UTF8 Byte-order mark (BOM = U+FEFF) at start of source, if there
    if (*src=='\xEF' && *(src+1)=='\xBB' && *(src+2)=='\xBF')
        src += 3;

    // Initialize lexer's source info
    lex->url = url;
    lex->fname = fileName(url);
    lex->source = src;

    // Initialize lexer context
    lex->srcp = lex->tokp = lex->linep = src;
    lex->linenbr = 1;
    lex->flags = 0;
    lex->nbrcurly = 0;
    lex->nbrtoks = 0;
    lex->indentch = '\0';
    lex->inject = 0;
    lex->curindent = 0;
    lex->indentlvl = 0;
    lex->indents[0] = 0;

    // Prime the pump with the first token
    lexNextToken();
}

// Inject a new source stream into the lexer
void lexInjectFile(char *url) {
    char *src;
    char *fn;
    timerBegin(LoadTimer);
    // Load specified source file
    src = fileLoadSrc(lex? lex->url : NULL, url, &fn);
    if (!src)
        errorExit(ExitNF, "Cannot find or read source file %s", url);

    timerBegin(ParseTimer);
    lexInject(fn, src);
}

// Restore previous lexer's stream
void lexPop() {
    if (lex)
        lex = lex->prev;
}

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
            errorMsgLex(ErrorBadTok, "Invalid hexadecimal character '%c'", *srcp);
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
    case '\0': *charval = '\0'; return ++srcp;
    case 'x': return lexHexDigits(2, ++srcp, charval);
    case 'u': return lexHexDigits(4, ++srcp, charval);
    case 'U': return lexHexDigits(8, ++srcp, charval);
    default:
        errorMsgLex(ErrorBadTok, "Invalid escape sequence '%c'", *srcp);
        *charval = *srcp++;
        return srcp;
    }
}

/** Tokenize a character */
void lexScanChar(char *srcp) {
    lex->tokp = srcp++;
    if (*srcp == '\\')
        srcp = lexScanEscape(srcp, &lex->val.uintlit);
    else
        lex->val.uintlit = *srcp++;
    if (*srcp == '\'')
        srcp++;
    else
        errorMsgLex(ErrorBadTok, "Only one character allowed in character literal");
    if (*srcp == 'u') {
        lex->langtype = (INode*)u32Type;
        srcp++;
    }
    else
        lex->langtype = lex->val.uintlit >= 0x100? (INode*)u32Type : (INode*)u8Type;
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
    while (*srcp != '"') {
        if (*srcp == '\\')
            srcp = lexScanEscape(srcp, &uchar);
        else
            uchar = *srcp++;
        if (uchar < 0x80) {
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
    *newp = '\0';
    srcp++;
    srclen++;

    lex->langtype = (INode*)newArrayNodeTyped(srclen, (INode*)u8Type);
    lex->toktype = StrLitToken;
    lex->srcp = srcp;
}

/** Tokenize an integer or floating point number */
void lexScanNumber(char *srcp) {

    char *srcbeg;        // Pointer to the start of the token
    uint64_t base;        // Radix for integer (10 or 16)
    uint64_t intval;    // Calculated integer value for integer literal
    char isFloat;        // nonzero when number token is a float, 'e' when in exponent

    lex->tokp = srcbeg = srcp;

    // A leading zero may indicate a non-base 10 number
    base = 10;
    if (*srcp=='0' && (*(srcp+1)=='x' || *(srcp+1)=='X')) {
        base = 16;
        srcp += 2;
    }

    // Validate and process remaining numeric digits
    isFloat = '\0';
    intval = 0;
    while (1) {
        // Only one exponent allowed
        if (isFloat!='e' && (*srcp=='e' || *srcp=='E' || *srcp=='p' || *srcp=='P')) {
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
            intval = intval*base + *srcp++ - '0';
        else if (*srcp=='_')
            srcp++;
        else if (base==16) {
            if (*srcp>='A' && *srcp<='F')
                intval = (intval<<4) + *srcp++ - 'A'+10;
            else if (*srcp>='a' && *srcp<='f')
                intval = (intval<<4) + *srcp++ - 'a'+10;
            else
                break;
        }
        else
            break;
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
        lex->langtype = (INode*)(isFloat ? f32Type : i32Type);

    // Set value and type
    if (isFloat) {
        lex->val.floatlit = atof(srcbeg);
        lex->toktype = FloatLitToken;
    }
    else {
        lex->val.uintlit = intval;
        lex->toktype = IntLitToken;
    }
    lex->srcp = srcp;
}

/** Tokenize an identifier or reserved token */
void lexScanIdent(char *srcp) {
    char *srcbeg = srcp++;    // Pointer to the start of the token
    lex->tokp = srcbeg;
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
            if (utf8IsLetter(srcp))
                srcp += utf8ByteSkip(srcp);
            else {
                INode *identNode;
                // Find identifier token in name table and preserve info about it
                // Substitute token type when identifier is a keyword
                lex->val.ident = nametblFind(srcbeg, srcp-srcbeg);
                identNode = (INode*)lex->val.ident->node;
                if (identNode && identNode->tag == KeywordTag)
                    lex->toktype = identNode->flags;
                else if (identNode && identNode->tag == PermTag)
                    lex->toktype = PermToken;
                else
                    lex->toktype = IdentToken;
                lex->srcp = srcp;
                return;
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

// Skip over nested block comment
char *lexBlockComment(char *srcp) {
    int nest = 1;
    while (*srcp) {
        if (*srcp == '*' && *(srcp + 1) == '/') {
            if (--nest == 0)
                return srcp+2;
            ++srcp;
        }
        else if (*srcp == '/' && *(srcp + 1) == '*') {
            ++nest;
            ++srcp;
        }
        // ignore tokens inside line comment
        else if (*srcp == '/' && *(srcp + 1) == '/') {
            srcp += 2;
            while (*srcp && *srcp++ != '\n');
        }
        // ignore tokens inside string literal
        else if (*srcp == '"') {
            ++srcp;
            while (*srcp && *srcp++ != '"') {
                if (*(srcp - 1) == '\\' && *srcp == '"')
                    srcp++;
            }
        }
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

// Inject tokens for off-side rule indentation
// Returns 1 if token is injected, 0 otherwise
// - Same indentation -  and not continuation
int lexInjectToken() {
    // Inject '{' if indentation increases
    if (lex->curindent > lex->indents[lex->indentlvl]) {
        if (lex->indentlvl >= LEX_MAX_INDENTS)
            errorExit(ExitIndent, "Too many indent levels in source file.");
        lex->indents[++lex->indentlvl] = lex->curindent;
        lex->inject = 0;
        lex->toktype = LCurlyToken;
        return 1;
    }
    // inject ';' if nbrtoks > 1 (unfinished statement)
    if (lex->nbrtoks > 1) {
        lex->nbrtoks = 0;
        lex->toktype = SemiToken;
        return 1;
    }
    // Inject '}' if indentation decreases
    if (lex->curindent < lex->indents[lex->indentlvl]) {
        --lex->indentlvl;
        lex->nbrtoks = 0;
        lex->toktype = RCurlyToken;
        return 1;
    }
    lex->inject = 0;
    return 0;
}

// Decode next token from the source into new lex->token
void lexNextTokenx() {
    // Inject tokens, if needed based on current line's indentation
    if (lex->inject && lexInjectToken())
        return;

    char *srcp;
    srcp = lex->srcp;
    lex->nbrtoks++;
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
        case '$':
            lexScanIdent(srcp);
            return;

        // '_' or identifier that starts with underscore
        case '_':
            if (utf8IsLetter(srcp+1) || *(srcp + 1) == '_' || *(srcp + 1) == '$' || (*(srcp+1)>='0' && *(srcp+1)<='9'))
                lexScanIdent(srcp);
            else {
                lex->toktype = UnderscoreToken;
                lex->srcp = ++srcp;
            }
            return;

        // backtick enclosed identifiers
        case '`':
            lexScanTickedIdent(srcp);
            return;

        case '.': lexReturnPuncTok(DotToken, 1);
        case ',': lexReturnPuncTok(CommaToken, 1);
        case '~': lexReturnPuncTok(TildeToken, 1);

        case '+': 
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(PlusEqToken, 2);
            }
            else if (*(srcp + 1) == '+') {
                lexReturnPuncTok(IncrToken, 2);
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

        // '=' and '=='
        case '=':
            if (*(srcp + 1) == '=')    {
                lexReturnPuncTok(EqToken, 2);
            }
            else {
                lexReturnPuncTok(AssgnToken, 1);
            }

        // '!' and '!='
        case '!':
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(NeToken, 2);
            }
            else {
                lexReturnPuncTok(NotToken, 1);
            }

        // '<' and '<='
        case '<':
            if (*(srcp + 1) == '=') {
                lexReturnPuncTok(LeToken, 2);
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
                if (*(srcp + 1) == '=') {
                    lexReturnPuncTok(ShrEqToken, 3);
                }
                else {
                    lexReturnPuncTok(ShrToken, 2);
                }
            }
            else {
                lexReturnPuncTok(GtToken, 1);
            }

        case '?': lexReturnPuncTok(QuesToken, 1);
        case '[': 
            lex->nbrcurly++;
            lexReturnPuncTok(LBracketToken, 1);
        case ']': 
            if (lex->nbrcurly > 0) --lex->nbrcurly;
            lexReturnPuncTok(RBracketToken, 1);
        case '(': 
            lex->nbrcurly++;
            lexReturnPuncTok(LParenToken, 1);
        case ')': 
            if (lex->nbrcurly > 0) --lex->nbrcurly;
            lexReturnPuncTok(RParenToken, 1);

        // ':' and '::'
        case ':':
            if (*(srcp + 1) == ':') {
                lexReturnPuncTok(DblColonToken, 2);
            }
            else {
                lexReturnPuncTok(ColonToken, 1);
            }

        // ';'
        case ';':
            lex->nbrtoks = 0;
            lexReturnPuncTok(SemiToken, 1);

        case '{': 
            lex->nbrcurly++;
            lexReturnPuncTok(LCurlyToken, 1);
        case '}': 
            if (lex->nbrcurly > 0) --lex->nbrcurly;
            lex->nbrtoks = 0;
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

        // Handle line continuation in off-side mode
        case '\\':
            ++srcp;
            if (lex->nbrcurly == 0) {
                // Skip to end of line
                while (*srcp && *srcp != '\n' && *srcp != '\x1a')
                    srcp++;
                // Skip over new line
                if (*srcp == '\n') {
                    srcp++;
                    lex->linep = srcp;
                    lex->linenbr++;
                }
            }
            break;

        // Handle new line
        case '\n':
            srcp++;
            lex->linep = srcp;
            lex->linenbr++;
            // In off-side mode
            if (lex->nbrcurly == 0) {
                // Count line's indentation
                lex->curindent = 0;
                while (1) {
                    if (*srcp == '\r')
                        srcp++;
                    else if (*srcp == ' ' || *srcp == '\t') {
                        if (lex->indentch == '\0')
                            lex->indentch = *srcp;
                        if (*srcp != lex->indentch) {
                            lex->tokp = lex->srcp = srcp;
                            if (*srcp == ' ')
                                errorMsgLex(WarnIndent, "Inconsistent indentation - using space where tab is expected.");
                            else
                                errorMsgLex(WarnIndent, "Inconsistent indentation - using tab where space is expected.");
                        }
                        srcp++;
                        lex->curindent++;
                    }
                    else
                        break;
                }
                // If line continuation, skip over and don't inject
                if (*srcp == '\\')
                    ++srcp;
                // For non-blank, non-comment line in off-side mode, inject token if needed
                else if (*srcp != '\n' && !(*srcp == '/' && *(srcp + 1) == '/')) {
                    lex->inject = 1;
                    lex->tokp = lex->srcp = srcp;
                    if (lexInjectToken())
                        return;
                }
            }
            break;

        // End-of-file
        case '\0': case '\x1a':
            if (lex->nbrcurly == 0) {
                // For off-side, pretend eof is a new line to force needed injections
                lex->inject = 1;
                lex->curindent = 0;
                lex->tokp = lex->srcp = srcp;
                if (lexInjectToken())
                    return;
            }
            lexReturnPuncTok(EofToken, 0);

        // Bad character
        default:
            {
                lex->tokp = srcp;
                errorMsgLex(ErrorBadTok, "Bad character '%c' starting unknown token", *srcp);
                srcp += utf8ByteSkip(srcp);
            }
        }
    }
}

void lexNextToken() {
    timerBegin(LexTimer);
    lexNextTokenx();
    timerBegin(ParseTimer);
}
