/** Declaration spans: where each declaration sits in its source text
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"
#include "../shared/fileio.h"

#include <stdio.h>
#include <string.h>

// Append a span to the list '*listp' points to, making the list if it is NULL
DclSpan *dclSpanAdd(DclSpans **listp, INode *node, Lexer *lexer, char *start, char *kw, char *end, uint16_t kind) {
    DclSpans *list = *listp;
    if (list == NULL) {
        list = (DclSpans*)memAllocBlk(sizeof(DclSpans));
        list->items = NULL;
        list->count = list->avail = 0;
        *listp = list;
    }
    if (list->count == list->avail) {
        list->avail = list->avail ? list->avail * 2 : 16;
        DclSpan **items = (DclSpan**)memAllocBlk(list->avail * sizeof(DclSpan*));
        for (uint32_t i = 0; i < list->count; ++i)
            items[i] = list->items[i];
        list->items = items;
    }
    DclSpan *span = (DclSpan*)memAllocBlk(sizeof(DclSpan));
    span->node = node;
    span->lexer = lexer;
    span->start = start;
    span->kw = kw;
    span->body = span->bodyend = span->nameend = NULL;
    span->end = end;
    span->kind = kind;
    span->typed = 0;
    span->members = NULL;
    list->items[list->count++] = span;
    return span;
}

// The 1-based line and column of a position in a span's file
void dclSpanLineCol(Lexer *lexer, char *pos, uint32_t *line, uint32_t *col) {
    uint32_t ln = 1;
    char *linep = lexer->source;
    for (char *p = lexer->source; p < pos && *p; ++p) {
        if (*p == '\n') {
            ++ln;
            linep = p + 1;
        }
    }
    *line = ln;
    *col = (uint32_t)(pos - linep) + 1;
}

static void dclSpanPrintPos(Lexer *lexer, char *label, char *pos) {
    if (pos == NULL)
        return;
    uint32_t line, col;
    dclSpanLineCol(lexer, pos, &line, &col);
    printf("  %s %u:%u", label, line, col);
}

// What a span covers, as a word and a name
static void dclSpanPrintWhat(DclSpan *span) {
    INode *node = span->node;
    switch (span->kind) {
    case SpanModLine: printf("mod"); return;
    case SpanImport:
        printf("import");
        if (node && ((ImportNode*)node)->module && ((ImportNode*)node)->module->namesym)
            printf(" %s", &((ImportNode*)node)->module->namesym->namestr);
        return;
    case SpanUse: printf("use"); return;
    case SpanExternBlock: printf("extern-block"); return;
    case SpanOther: printf("other"); return;
    default: break;
    }
    if (node == NULL) {
        printf("member");
        return;
    }
    char *word;
    switch (node->tag) {
    case FnDclTag: word = "fn"; break;
    case VarDclTag: word = "global"; break;
    case FieldDclTag: word = (node->flags & IsMixin) ? "mixin" : "field"; break;
    case StructTag: word = (node->flags & EnumType) ? "enum" : (node->flags & TraitType) ? "trait" : "struct"; break;
    case AliasDclTag: word = "typedef"; break;
    case ConstDclTag: word = "const"; break;
    case MacroDclTag: word = "macro"; break;
    case ModTraitTag: word = "mod-trait"; break;
    default: word = "node"; break;
    }
    Name *name = inodeGetName(node);
    if (name && name != anonName)
        printf("%s %s", word, &name->namestr);
    else
        printf("%s", word);
}

static void dclSpanPrintList(DclSpans *list, int depth) {
    if (list == NULL)
        return;
    for (uint32_t i = 0; i < list->count; ++i) {
        DclSpan *span = list->items[i];
        uint32_t line, col;
        dclSpanLineCol(span->lexer, span->start, &line, &col);
        char *url = span->lexer->url ? span->lexer->url : "";
        char *base = url + fileFolder(url);
        printf("%*s%s:%u:%u ", depth * 2, "", base, line, col);
        dclSpanPrintWhat(span);
        dclSpanPrintPos(span->lexer, "kw", span->kw);
        dclSpanPrintPos(span->lexer, "body", span->body);
        dclSpanPrintPos(span->lexer, "bodyend", span->bodyend);
        if (span->node && span->node->tag == VarDclTag) {
            dclSpanPrintPos(span->lexer, "nameend", span->nameend);
            printf("  %s", span->typed ? "typed" : "inferred");
        }
        dclSpanPrintPos(span->lexer, "end", span->end);
        printf("\n");
        dclSpanPrintList(span->members, depth + 1);
        if (span->node && span->node->tag == StructTag)
            dclSpanPrintList(((StructNode*)span->node)->spans, depth + 1);
    }
}

// Print each span of a module to stdout, and each member span of its types
void dclSpanPrintModule(ModuleNode *mod) {
    printf("spans of module %s\n", mod->namesym ? &mod->namesym->namestr : "?");
    dclSpanPrintList(mod->spans, 1);
}
