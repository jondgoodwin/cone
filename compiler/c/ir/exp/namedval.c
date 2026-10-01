/** Handling for named value nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new named value node
NamedValNode *newNamedValNode(INode *name) {
    NamedValNode *node;
    newNode(node, NamedValNode, NamedValTag);
    node->vtype = unknownType;
    node->name = name;
    return node;
}

// Clone namedval
INode *cloneNamedValNode(CloneState *cstate, NamedValNode *node) {
    NamedValNode *newnode;
    newnode = memAllocBlk(sizeof(NamedValNode));
    memcpy(newnode, node, sizeof(NamedValNode));
    newnode->name = cloneNode(cstate, node->name);
    newnode->val = cloneNode(cstate, node->val);
    return (INode *)newnode;
}

// Serialize named value node
void namedValPrint(NamedValNode *node) {
    inodePrintNode(node->name);
    inodeFprint(": ");
    inodePrintNode(node->val);
}

// Name resolution of named value node
void namedValNameRes(NameResState *pstate, NamedValNode *node) {
    inodeNameRes(pstate, &node->val);
}

// Type check named value node. The value is wanted as what the named value is:
// a type literal's field, once the field is known (fnCallTypeCheck).
void namedValTypeCheck(TypeCheckState *pstate, NamedValNode *node, INode *expectType) {
    inodeTypeCheck(pstate, &node->val, expectType);
    if (!isExpNode(node->val)) {
        errorMsgNode(node->val, ErrorNotTyped, "Expected a typed expression.");
        return;
    }
    node->vtype = ((IExpNode*)node->val)->vtype;
}

// The parser wraps 'name: value' wherever an argument list is written, because
// it cannot tell 'new Point(x: 1)' from 'f(x: 1)'. Only a struct's literal --
// its implicit init's construction, or a variant's brackets -- matches the
// name to anything (typelit.c), so every other use is refused here rather than
// left to reach flow analysis and generation, which know the node only there.
int namedValRefuseArgs(Nodes *args, char *what) {
    int found = 0;
    INode **argsp;
    uint32_t cnt;
    if (args == NULL)
        return 0;
    for (nodesFor(args, cnt, argsp)) {
        NamedValNode *arg = (NamedValNode*)*argsp;
        if (arg->tag != NamedValTag)
            continue;
        found = 1;
        if (arg->name->tag == NameUseTag)
            errorMsgNode((INode*)arg, ErrorNamedArg,
                "Named arguments are not supported in %s; pass `%s` by position. Only a struct's implicit init, such as `new Point(x: 1)`, takes values by name.",
                what, &((NameUseNode*)arg->name)->namesym->namestr);
        else
            errorMsgNode((INode*)arg, ErrorNamedArg,
                "Named arguments are not supported in %s. Only a struct's implicit init, such as `new Point(x: 1)`, takes values by name.", what);
    }
    return found;
}
