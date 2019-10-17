/** The Type pass
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

// Dispatch a node walk for the type pass.
void typePass(TypePass *pstate, INode *node) {
    switch (node->tag) {
    case ModuleTag:
        modTypePass(pstate, (ModuleNode*)node); break;
    case StructTag:
        structTypePass(pstate, (StructNode *)node); break;
    default:
        break;
    }
}

void doTypePass(ModuleNode *mod) {
    TypePass pstate;
    typePass(&pstate, (INode*)mod);
}


