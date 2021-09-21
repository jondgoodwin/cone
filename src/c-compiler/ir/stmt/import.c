/** import node helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new Import node
ImportNode *newImportNode() {
    ImportNode *node;
    newNode(node, ImportNode, ImportTag);
    node->module = NULL;
    node->foldall = 0;
    return node;
}

// Serialize a import node
void importPrint(ImportNode *node) {
    inodeFprint("import %s", &node->module->namesym->namestr);
    if (node->foldall)
        inodeFprint("::*");
}

// Name resolution of the import node
void importNameRes(NameResState *pstate, ImportNode *node) {
}

// Type check the import node
void importTypeCheck(TypeCheckState *pstate, ImportNode *node) {
}
