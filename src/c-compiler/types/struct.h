/** AST handling for record-based types with properties
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef struct_h
#define struct_h

// This is a reusable structure for named types having properties
// (e.g., struct, trait, interface, alloc, etc.)
// It holds:
// - properties. An ordered list of nodes for variable-like elements 
//     that hold typed data. All/most nodes are named and findable.
// - methods. A name-mapped node hash for methods.
//     A method may be a FnTuple, a list of methods with same name.
//     Note: the namespace is comprised of properties + methods
//       with no duplicate names between them
// - subtypes. An ordered list of nodes for its traits/interfaces
typedef struct StructAstNode {
	MethodTypeAstHdr;
} StructAstNode;

StructAstNode *newStructNode(Name *namesym);
void structPrint(StructAstNode *node);
void structPass(PassState *pstate, StructAstNode *name);
int structEqual(StructAstNode *node1, StructAstNode *node2);
int structCoerces(StructAstNode *to, StructAstNode *from);

#endif