/** The report of unconstrained operations ('conec --report-trust')
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// After type checking, one line per site where the program does what the
// compiler cannot check, printed as
//     <kind> <file>:<line> <enclosing function or type> [<what>]
// Kinds: deref, ref-from-ptr, intrinsic, extern, reinterpret, reinterpret-perm
// (an 'as' between references that changes only the permission), sendable. A
// generic template also prints 'generic <file>:<line> <name> [<instances>]',
// which is no site. It
// refuses nothing and changes nothing; it reads the checked tree.
//
// A generic is analysed only as instances, so its body is read from them; a
// site is kept once by its text, which every instance of one template shares
// (an instance keeps the template's source position and name).
//
// Not reported, because a pointer does no harm until it reaches memory: the
// expressions of pointer type that are not themselves a site (making, storing,
// passing, returning, comparing a pointer, pointer arithmetic, casts between
// pointer types). They are only counted, once per source position, as
// 'unreported-pointer-operations'.

typedef struct { char **lines; size_t used, max; } LineSet;
static LineSet sites, ptrops;
static char *curFn = "<module>";
static char *curOwner = NULL;
static INode *suppressed = NULL;    // a deref or index already reported as part of a reference made from a pointer

static void addLine(LineSet *set, char *text) {
    if (set->used == set->max) {
        set->max = set->max ? set->max * 2 : 1024;
        set->lines = (char **)realloc(set->lines, set->max * sizeof(char *));
    }
    set->lines[set->used++] = text;
}

static char *copyText(char *text) {
    size_t len = strlen(text) + 1;
    char *copy = (char *)malloc(len);
    memcpy(copy, text, len);
    return copy;
}

static char *spellFile(INode *node) {
    static char buf[1024];
    char *url = (node->lexer && node->lexer->url) ? node->lexer->url : "?";
    size_t i = 0;
    for (; url[i] && i < sizeof(buf) - 1; ++i)
        buf[i] = url[i] == '\\' ? '/' : url[i];
    buf[i] = '\0';
    // Text read from a generated include file is named as the package's source file it was copied
    // from (a line mark, relative to the package's src/); the include file is added after a '#',
    // which says which package it is
    if (node->lexer && (node->lexer->flags & LexLineMarks) && node->lexer->path && i < sizeof(buf) - 2) {
        buf[i++] = '#';
        for (char *p = node->lexer->path; *p && i < sizeof(buf) - 1; ++p)
            buf[i++] = *p == '\\' ? '/' : *p;
        buf[i] = '\0';
    }
    return buf;
}

static char *spellFn(void) {
    static char buf[512];
    if (curOwner)
        snprintf(buf, sizeof(buf), "%s.%s", curOwner, curFn);
    else
        snprintf(buf, sizeof(buf), "%s", curFn);
    return buf;
}

static void report(char *kind, INode *node, char *what) {
    char text[2048];
    int n = snprintf(text, sizeof(text), "%s %s:%u %s", kind, spellFile(node), (unsigned)node->linenbr, spellFn());
    if (what)
        snprintf(text + n, sizeof(text) - n, " [%s]", what);
    addLine(&sites, copyText(text));
}

static INode *typeOf(INode *exp) {
    if (exp == NULL || !isExpNode(exp) || ((IExpNode*)exp)->vtype == NULL)
        return NULL;
    return itypeGetTypeDcl(((IExpNode*)exp)->vtype);
}

static int isPtrExp(INode *exp) {
    INode *type = typeOf(exp);
    return type && type->tag == PtrTag;
}

// A place reached through a raw pointer: '*p', or 'p[i]'
static int isPtrPlace(INode *node) {
    if (node->tag == DerefTag)
        return isPtrExp(((StarNode*)node)->vtexp);
    if (node->tag == ArrIndexTag)
        return isPtrExp(((FnCallNode*)node)->objfn);
    return 0;
}

static void walk(INode *node);

static void walkNodes(Nodes *nodes) {
    if (nodes == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(nodes, cnt, nodesp))
        walk(*nodesp);
}

static void walkNodeList(NodeList *list) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(list, cnt, nodesp))
        walk(*nodesp);
}

// A generic's line: where its template is, and how many instances this program made
static void reportGeneric(INode *template, char *name, uint32_t instances) {
    char text[2048];
    char *owner = curOwner;
    snprintf(text, sizeof(text), "generic %s:%u %s%s%s [%u]", spellFile(template), (unsigned)template->linenbr,
        owner ? owner : "", owner ? "." : "", name, (unsigned)instances);
    addLine(&sites, copyText(text));
}

static uint32_t walkInstances(GenericInfo *info, INode *template) {
    uint32_t instances = 0;
    if (info->memonodes == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(info->memonodes, cnt, nodesp)) {
        nodesp++; cnt--;
        if (*nodesp != template) {
            ++instances;
            walk(*nodesp);
        }
    }
    return instances;
}

// Whether a type holds a raw pointer by value, directly or through a field
static int holdsPointer(INode *type, int depth) {
    if (type != NULL && type->tag == DerefTag)   // '*T' in a template, before it becomes a pointer type
        return 1;
    type = itypeGetTypeDcl(type);
    if (type == NULL || depth > 8)
        return 0;
    if (type->tag == PtrTag)
        return 1;
    if (type->tag == ArrayTag) {
        ArrayNode *arr = (ArrayNode*)type;
        return arr->elems && arr->elems->used > 0 && holdsPointer(nodesGet(arr->elems, 0), depth + 1);
    }
    if (type->tag == StructTag) {
        StructNode *str = (StructNode*)type;
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&str->fields, cnt, nodesp)) {
            if (((FieldDclNode*)*nodesp)->vtype && holdsPointer(((FieldDclNode*)*nodesp)->vtype, depth + 1))
                return 1;
        }
    }
    return 0;
}

// The function a call names, if it is a plain function: the call's callee is its
// name use, or the member of a method call
static FnDclNode *calleeOf(FnCallNode *call) {
    INode *names[2] = { call->objfn, call->methfld };
    for (int i = 0; i < 2; ++i) {
        INode *n = names[i];
        if (n && isNameUseNode(n)) {
            INode *dcl = ((NameUseNode*)n)->dclnode;
            if (dcl && dcl->tag == FnDclTag)
                return (FnDclNode*)dcl;
        }
    }
    return NULL;
}

// A package's function as its generated include file declares it: 'extern' with no body, though the
// package defines it. Only a C-named declaration there is a foreign function.
static int declaredByInclude(FnDclNode *fn) {
    return fn->lexer && (fn->lexer->flags & LexLineMarks) && !(fn->dclinfo.facts & DclCName);
}

static void walkCall(FnCallNode *call) {
    FnDclNode *fn = calleeOf(call);
    if (fn) {
        char *name = fn->namesym ? &fn->namesym->namestr : "?";
        char *trust = intrinsicTrustName(fn);
        if (trust)
            report(strncmp(trust, "sliceFromParts", 14) == 0 ? "ref-from-ptr" : "intrinsic", (INode*)call, trust);
        else if ((fn->flags & FlagExtern) && fn->value == NULL
                 && !declaredByInclude(fn))
            report("extern", (INode*)call, name);
    }
    walk(call->objfn);
    walkNodes(call->args);
}

// A reference made of a place: '&*p', '&p[i]', '&p.field' through a pointer
static void walkBorrow(RefNode *ref) {
    INode *place = ref->vtexp;
    while (place && place->tag == FldAccessTag)
        place = ((FnCallNode*)place)->objfn;
    if (place && isPtrPlace(place)) {
        report("ref-from-ptr", (INode*)ref, NULL);
        suppressed = place;
    }
    walk(ref->vtexp);
}

static void walkCast(CastNode *cast) {
    INode *from = typeOf(cast->exp);
    INode *to = itypeGetTypeDcl(cast->vtype);
    if (from && to && (cast->flags & FlagWrittenAs) && !(cast->flags & FlagConvert)) {
        int toref = to->tag == RefTag || to->tag == ArrayRefTag;
        if (from->tag == PtrTag && toref)
            report("ref-from-ptr", (INode*)cast, NULL);
        else if (from->tag != PtrTag && to->tag != PtrTag) {
            // A reference made another permission of the same referent is counted on its own
            int refs = (from->tag == RefTag || from->tag == ArrayRefTag || from->tag == VirtRefTag)
                && (to->tag == RefTag || to->tag == ArrayRefTag || to->tag == VirtRefTag);
            int permonly = refs && from->tag == to->tag
                && itypeIsSame(((RefNode*)from)->vtexp, ((RefNode*)to)->vtexp)
                && !permIsSame(((RefNode*)from)->perm, ((RefNode*)to)->perm);
            report(permonly ? "reinterpret-perm" : "reinterpret", (INode*)cast, NULL);
        }
    }
    walk(cast->exp);
}

static void walkFn(FnDclNode *fn) {
    char *svfn = curFn;
    curFn = fn->namesym ? &fn->namesym->namestr : "?";
    walk(fn->value);
    curFn = svfn;
}

// A struct declaring Sendable that holds a raw pointer, directly or through a field's type
static void reportSendable(StructNode *str, char *name) {
    if (str->flags & TraitType)
        return;
    // A template's trait list holds the names it was written with, an instance's the traits
    int declares = 0;
    if (str->traits) {
        INode **traitp;
        uint32_t tcnt;
        for (nodesFor(str->traits, tcnt, traitp))
            if (*traitp == (INode*)sendableTrait || itypeGetTypeDcl(*traitp) == (INode*)sendableTrait)
                declares = 1;
    }
    if (!declares)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&str->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        if (field->vtype && holdsPointer(field->vtype, 0)) {
            char text[2048];
            snprintf(text, sizeof(text), "sendable %s:%u %s [%s]", spellFile((INode*)str),
                (unsigned)str->linenbr, name, field->namesym ? &field->namesym->namestr : "?");
            addLine(&sites, copyText(text));
            break;
        }
    }
}

static void walkStruct(StructNode *str) {
    char *svowner = curOwner;
    char *name = str->namesym ? &str->namesym->namestr : "?";
    if (str->genericinfo != NULL) {
        // The template's own declaration is read for Sendable (a field of type *T holds a
        // pointer whatever T is); its methods only through the instances
        reportSendable(str, name);
        reportGeneric((INode*)str, name, walkInstances(str->genericinfo, (INode*)str));
        return;
    }
    reportSendable(str, name);
    curOwner = name;
    walkNodeList(&str->nodelist);
    walkNodeList(&str->fields);
    curOwner = svowner;
}

static void walk(INode *node) {
    if (node == NULL)
        return;
    // Expressions of pointer type, one per source position
    if (isExpNode(node) && node->tag != BlockTag && node->lexer && isPtrExp(node)) {
        char text[1536];
        snprintf(text, sizeof(text), "%s\t%u:%d:%s", spellFile(node), (unsigned)node->linenbr,
            (int)(node->srcp - node->linep), spellFn());
        addLine(&ptrops, copyText(text));
    }

    switch (node->tag) {
    case ProgramTag:
        walkNodes(((ProgramNode*)node)->modules); break;
    case ModuleTag:
        walkNodes(((ModuleNode*)node)->nodes); break;
    case FnDclTag:
        if (((FnDclNode*)node)->genericinfo != NULL) {
            FnDclNode *fn = (FnDclNode*)node;
            uint32_t made = walkInstances(fn->genericinfo, node);
            reportGeneric(node, fn->namesym ? &fn->namesym->namestr : "?", made);
        } else
            walkFn((FnDclNode*)node);
        break;
    case FnOverloadDclTag:
        walkNodes(((FnOverloadDclNode*)node)->overloads); break;
    case VarDclTag:
        walk(((VarDclNode*)node)->value); break;
    case ConstDclTag:
        walk(((ConstDclNode*)node)->value); break;
    case FieldDclTag:
        walk(((FieldDclNode*)node)->value); break;
    case StructTag:
        walkStruct((StructNode*)node); break;
    case BlockTag:
        walkNodes(((BlockNode*)node)->stmts); break;
    case IfTag:
        walkNodes(((IfNode*)node)->condblk); break;
    case BreakTag: case ContinueTag: case BlockRetTag: case ReturnTag:
        walk(((BreakRetNode*)node)->exp); break;
    case AssignTag:
        walk(((AssignNode*)node)->lval);
        walk(((AssignNode*)node)->rval);
        break;
    case SwapTag:
        walk(((SwapNode*)node)->lval);
        walk(((SwapNode*)node)->rval);
        break;
    case VTupleTag:
        walkNodes(((TupleNode*)node)->elems); break;
    case ArrayLitTag:
        walkNodes(((ArrayNode*)node)->dimens);
        walkNodes(((ArrayNode*)node)->elems);
        break;
    case ArrIndexTag:
        if (node != suppressed && isPtrPlace(node))
            report("deref", node, NULL);
        walk(((FnCallNode*)node)->objfn);
        walkNodes(((FnCallNode*)node)->args);
        break;
    case FnCallTag:
        walkCall((FnCallNode*)node); break;
    case FldAccessTag: case TypeLitTag:
        walk(((FnCallNode*)node)->objfn);
        walkNodes(((FnCallNode*)node)->args);
        break;
    case CastTag:
        walkCast((CastNode*)node); break;
    case IsTag:
        walk(((CastNode*)node)->exp); break;
    case DerefTag:
        if (node != suppressed && isPtrPlace(node))
            report("deref", node, NULL);
        walk(((StarNode*)node)->vtexp);
        break;
    case BorrowTag: case ArrayBorrowTag:
        walkBorrow((RefNode*)node); break;
    case AllocateTag:
        walk(((RefNode*)node)->vtexp); break;
    case NotLogicTag:
        walk(((LogicNode*)node)->lexp); break;
    case OrLogicTag: case AndLogicTag:
        walk(((LogicNode*)node)->lexp);
        walk(((LogicNode*)node)->rexp);
        break;
    case AwaitTag:
        walk(((AwaitNode*)node)->exp); break;
    case YieldTag:
        walk(((YieldNode*)node)->exp); break;
    case DestructTag:
        walk(((DestructNode*)node)->exp); break;
    case NamedValTag:
        walk(((NamedValNode*)node)->val); break;
    case OfEntryTag: case FillEntryTag: case PairEntryTag: case EachEntryTag: case YieldEntryTag:
        walk(((EntryNode*)node)->first);
        walk(((EntryNode*)node)->val);
        break;
    case RefCountTag:
        walk(((RefCountNode*)node)->exp); break;
    case HollowTag:
        walk(((HollowNode*)node)->exp); break;
    case DropFlagTag:
        walk(((DropFlagNode*)node)->release); break;
    case TempTag:
        walk(((TempNode*)node)->exp); break;
    default:
        break;
    }
}

static int cmpLines(const void *a, const void *b) {
    return strcmp(*(char * const *)a, *(char * const *)b);
}

// Sort a set and drop repeats; returns how many lines remain
static size_t sortUnique(LineSet *set) {
    if (set->used == 0)
        return 0;
    qsort(set->lines, set->used, sizeof(char *), cmpLines);
    size_t out = 1;
    for (size_t i = 1; i < set->used; ++i)
        if (strcmp(set->lines[i], set->lines[out - 1]) != 0)
            set->lines[out++] = set->lines[i];
    set->used = out;
    return out;
}

void trustReport(INode *pgm) {
    walk(pgm);
    sortUnique(&sites);
    for (size_t i = 0; i < sites.used; ++i)
        printf("%s\n", sites.lines[i]);
    // The pointer expressions that are not sites, counted by file
    size_t total = sortUnique(&ptrops);
    for (size_t i = 0; i < ptrops.used; ) {
        size_t len = strcspn(ptrops.lines[i], "\t");
        size_t j = i;
        while (j < ptrops.used && strncmp(ptrops.lines[j], ptrops.lines[i], len + 1) == 0)
            ++j;
        printf("pointer-operations %.*s %u\n", (int)len, ptrops.lines[i], (unsigned)(j - i));
        i = j;
    }
    printf("unreported-pointer-operations %u\n", (unsigned)total);
    fflush(stdout);
}
