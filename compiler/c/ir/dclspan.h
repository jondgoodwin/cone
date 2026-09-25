/** Declaration spans: where each declaration sits in its source text
 *
 * The include-file generator copies a package's own source text rather than
 * printing the IR (compiler/c/doc/nodes/module.md, "Generating the include
 * file"), so it needs to know where each declaration starts and ends, where
 * the 'pub' before it is, and where its body or value begins. A node's own
 * position is only where its name is, so the parser records the rest here: one
 * span per module-level statement, kept in a side list on the module, and one
 * per member of a type's braces, kept on the type. Nothing else reads them.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef dclspan_h
#define dclspan_h

// What kind of statement a span covers
enum DclSpanKind {
    SpanModLine,      // The module's 'mod' line
    SpanImport,       // An 'import'
    SpanUse,          // A standalone 'use'
    SpanDcl,          // A declaration: fn, global, type, typedef, const, macro, module trait
    SpanExternBlock,  // 'extern { ... }': its items are its members, each a SpanDcl
    SpanMember,       // A member of a type's braces that is not a declaration of its own:
                      // a field, a line of variants, a sibling 'use'
    SpanOther         // A statement that made no node: a retired 'include', one refused
};

// One statement's extent. Each pointer is into 'lexer->source'.
typedef struct DclSpan {
    INode *node;        // The node the statement made, or NULL
    Lexer *lexer;       // The file it is written in
    char *start;        // Its first token: the 'pub' before it, where one is written
    char *kw;           // Its first token after 'pub': where 'extern' is written in
    char *body;         // A function's body, from its '{', or a global's value, from its
                        // '='; NULL where it has none
    char *bodyend;      // Just past that body or value's last token
    char *nameend;      // A global: just past its name, where an inferred type is written
    char *end;          // Just past its last token, its ';' or '}' included
    uint16_t kind;      // DclSpanKind
    uint16_t typed;     // A global: its type is written
    struct DclSpans *members; // An extern block's items; NULL for anything else
} DclSpan;

// A list of spans, in the order written
typedef struct DclSpans {
    DclSpan **items;
    uint32_t count;
    uint32_t avail;
} DclSpans;

// Append a span to the list '*listp' points to, making the list if it is NULL
DclSpan *dclSpanAdd(DclSpans **listp, INode *node, Lexer *lexer, char *start, char *kw, char *end, uint16_t kind);

// The 1-based line and column of a position in a span's file
void dclSpanLineCol(Lexer *lexer, char *pos, uint32_t *line, uint32_t *col);

// Print each span of a module to stdout, and each member span of its types:
// the debug switch '--spans', which measures what the parser recorded
void dclSpanPrintModule(ModuleNode *mod);

#endif
