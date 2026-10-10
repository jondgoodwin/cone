/** Handling for generic variable declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new generic variable declaraction node
GenVarDclNode *newGVarDclNode(Name *namesym) {
    GenVarDclNode *var;
    newNode(var, GenVarDclNode, GenVarDclTag);
    var->vtype = NULL;
    var->namesym = namesym;
    var->annot = NULL;
    var->valtype = NULL;
    return var;
}

// Serialize a generic variable node
void gVarDclPrint(GenVarDclNode *name) {
    inodeFprint("%s", &name->namesym->namestr);
    if (name->annot) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(name->annot, cnt, nodesp)) {
            inodeFprint(cnt == name->annot->used ? " " : " + ");
            inodePrintNode(*nodesp);
        }
    }
}

// Perform name resolution. What the annotation names is resolved here, with
// every parameter before it hooked; what it means is decided by the
// declaration's genericConstraintsNameRes, once all of them are.
void gVarDclNameRes(NameResState *pstate, GenVarDclNode *var) {
    nametblHookNode(var->namesym, (INode*)var);
    if (var->annot) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(var->annot, cnt, nodesp))
            inodeNameRes(pstate, nodesp);
    }
}

// Type check 
void gVarDclTypeCheck(TypeCheckState *pstate, GenVarDclNode *var) {
}
