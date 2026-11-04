/** Handling for size-of nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>

// Create a new sizeof node
SizeofNode *newSizeofNode() {
    SizeofNode *node;
    newNode(node, SizeofNode, SizeofTag);
    node->vtype = (INode*)usizeType;
    node->what = SizeofSize;
    node->field = NULL;
    return node;
}

// Clone sizeof
INode *cloneSizeofNode(CloneState *cstate, SizeofNode *node) {
    SizeofNode *newnode;
    newnode = memAllocBlk(sizeof(SizeofNode));
    memcpy(newnode, node, sizeof(SizeofNode));
    newnode->type = cloneNode(cstate, node->type);
    return (INode *)newnode;
}

// Serialize sizeof
void sizeofPrint(SizeofNode *node) {
    inodeFprint(node->what == SizeofAlign ? "(alignof, " : node->what == SizeofOffset ? "(offsetof, " : "(sizeof, ");
    inodePrintNode(node->type);
    if (node->what == SizeofOffset)
        inodeFprint(", %s", &node->field->namesym->namestr);
    inodeFprint(")");
}

// Name resolution of sizeof node
void sizeofNameRes(NameResState *pstate, SizeofNode *node) {
    inodeNameRes(pstate, &node->type);
}

// Type check sizeof node
void sizeofTypeCheck(TypeCheckState *pstate, SizeofNode *node) {
    if (itypeTypeCheck(pstate, &node->type) == 0)
        return;
    if (node->what == SizeofSize || node->what == SizeofAlign) {
        char what[200];
        char name[128];
        name[0] = '\0';
        itypeSpellCat(name, sizeof(name), node->type, 0);
        snprintf(what, sizeof(what), "%s(%s)", node->what == SizeofSize ? "#size" : "#align", name);
        sizeofLayoutRefused((INode*)node, node->type, what);
    }
}

int sizeofLayoutRefused(INode *errnode, INode *type, const char *what) {
    if (!isTypeNode(type) || !structLayoutUnsettled(type))
        return 0;
    INode *root = NULL;
    char *cause = itypeNoSizeCause(type, &root);
    char name[128];
    name[0] = '\0';
    itypeSpellCat(name, sizeof(name), type, 0);
    errorMsgNode(errnode, ErrorMetaLayout,
        "'%s' asks about the layout of '%s' while that layout is still being settled, which would answer by the order the declarations happen to be laid out in%s%s.",
        what, name, cause ? ": " : "", cause ? cause : "");
    return 1;
}
