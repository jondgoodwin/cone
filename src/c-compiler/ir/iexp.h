/** Expression nodes that return a typed value
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef iexp_h
#define iexp_h

// Typed Node header, offering access to the node's type info
// - vtype is the value type for an expression (e.g., 'i32')
#define IExpNodeHdr \
    INodeHdr; \
    INode *vtype

// Castable structure for all typed nodes
typedef struct IExpNode {
    IExpNodeHdr;
} IExpNode;

// Return node's type's declaration node
// (Note: only use after it has been type-checked)
INode *iexpGetTypeDcl(INode *node);

// Return node's type's declaration node
// and when it is a a pointer/ref, get its type declaration node
// (Note: only use after it has been type-checked)
INode *iexpGetDerefTypeDcl(INode *node);

// Bidirectional type checking between 'from' and 'to' nodes
// Evaluate whether the 'from' node will type check to 'to' node's type
// - If 'to' type is unspecified, infer it from 'from' type
// - If 'from' node is 'if', 'block', 'loop', set it to 'to' node's type
// - Otherwise, check that types match.
//   If match requires a coercion, a 'cast' node will be inject ahead of the 'from' node
// return 1 for success, 0 for fail
int iexpChkType(TypeCheckState *pstate, INode **to, INode **from);

// Can from's value be coerced to to's value type?
// This might inject a 'cast' node in front of the 'from' node with non-matching numbers
int iexpCoerces(INode *to, INode **from);

// Are types the same (no coercion)
int iexpSameType(INode *to, INode **from);

// Retrieve the permission flags for the node
uint16_t iexpGetPermFlags(INode *node);

#endif