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
#define ITypedNodeHdr \
	INodeHdr; \
	INode *vtype

// Castable structure for all typed nodes
typedef struct ITypedNode {
    ITypedNodeHdr;
} ITypedNode;

// Return node's type's declaration node
// (Note: only use after it has been type-checked)
INode *iexpGetTypeDcl(INode *node);

// Return node's type's declaration node
// and when it is a a pointer/ref, get its type declaration node
// (Note: only use after it has been type-checked)
INode *iexpGetDerefTypeDcl(INode *node);

// Can from's value be coerced to to's value type?
// This might inject a 'cast' node in front of the 'from' node with non-matching numbers
int iexpCoerces(INode *to, INode **from);

// Retrieve the permission flags for the node
uint16_t iexpGetPermFlags(INode *node);

#endif