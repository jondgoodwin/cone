/** A statement that throws a value away: see discard.h
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Why dropping a value of this type is worth saying, for a type its declaration
// marks '@mustUse'. NULL where it is not marked. A variant is judged by its own declaration and its enum's: '@mustUse' on
// an enum covers its variants, which are what a value of it is built as.
// Only a value counts; a reference to a Result is no Result
static char *discardReason(INode *type) {
    if (type == NULL || type->tag != StructTag)
        return NULL;
    StructNode *snode = (StructNode*)type;
    StructNode *base = !(snode->flags & EnumType) && (snode->flags & HasTagField) ? structBaseTraitDcl(snode) : NULL;
    if (snode->mustuse || (base != NULL && base->mustuse))
        return "It is marked @mustUse: dropping one unseen is a mistake.";
    return NULL;
}

// Statements this one's value would have
static int discardIsValueStmt(INode *stmt) {
    switch (stmt->tag) {
    case FnCallTag:
    case AwaitTag:
    case BlockTag:
    case IfTag:
        return 1;
    default:
        return 0;
    }
}

// Where a warning has been given: a generic's body is checked once for each
// instance, and the reader wrote the statement once
static char **discardSaid = NULL;
static size_t discardSaidUsed = 0, discardSaidCap = 0;

static int discardAlreadySaid(INode *stmt) {
    for (size_t i = 0; i < discardSaidUsed; ++i) {
        if (discardSaid[i] == stmt->srcp)
            return 1;
    }
    if (discardSaidUsed == discardSaidCap) {
        discardSaidCap = discardSaidCap ? discardSaidCap * 2 : 64;
        discardSaid = realloc(discardSaid, discardSaidCap * sizeof(char*));
    }
    discardSaid[discardSaidUsed++] = stmt->srcp;
    return 0;
}

void discardCheck(INode *stmt) {
    if (stmt == NULL || !isExpNode(stmt) || !discardIsValueStmt(stmt))
        return;
    INode *vtype = ((IExpNode*)stmt)->vtype;
    if (vtype == NULL || vtype == errorType || vtype == unknownType || vtype == noCareType)
        return;
    INode *type = iexpGetTypeDcl(stmt);
    if (type == NULL || type == errorType || type == unknownType)
        return;

    const char *reason = NULL;
    // The answer of an operation awaited: it says whether it worked
    if (stmt->tag == AwaitTag && ((AwaitNode*)stmt)->awaitable != NULL) {
        if (type->tag == VoidTag)
            return;
        reason = "An operation's answer says whether it worked, a failure among what it may say, so a failure here goes unread.";
    }
    else {
        INode *base = type->tag == StructTag && !(type->flags & EnumType) && (type->flags & HasTagField)
            ? (INode*)structBaseTraitDcl((StructNode*)type) : NULL;
        char *core = propagateCoreName(type);
        if (core == NULL && base != NULL)
            core = propagateCoreName(base);
        if (core != NULL)
            reason = core[0] == 'R' ? "A failure among what it may hold goes unread."
                                    : "Whether it holds a value goes unread.";
        else
            reason = discardReason(type);
    }
    if (reason == NULL || discardAlreadySaid(stmt))
        return;

    char tname[256];
    propagateSpell(tname, sizeof(tname), type);
    const char *what = stmt->tag == AwaitTag ? "'await'" : stmt->tag == FnCallTag ? "call"
        : stmt->tag == BlockTag ? "block" : "statement";
    errorMsgNode(stmt, WarnDiscard,
        "The %s this %s gives is thrown away. %s Use it (bind it and match on it, or '?' it), or write '_ = ...;' to discard it on purpose.",
        tname, what, reason);
}
