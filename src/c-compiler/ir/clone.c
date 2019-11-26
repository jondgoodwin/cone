/** The cloning pass
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>

// Deep copy a node
INode *cloneNode(CloneState *cstate, INode **nodep) {
    INode *node;
    switch ((*nodep)->tag) {
    case ULitTag:
        node = cloneULitNode((ULitNode *)*nodep);
        break;
    default:
        assert(0 && "Do not know how to clone a node of this type");
    }
    node->instnode = cstate->instnode;
    return node;
}
