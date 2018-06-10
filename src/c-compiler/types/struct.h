/** AST handling for record-based types with fields
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef struct_h
#define struct_h

// This is a reusable structure for named types having fields
// (e.g., struct, trait, interface, alloc, etc.)
// It holds:
// - fields. An ordered list of nodes for variable-like elements 
//     that hold typed data. All/most nodes are named and findable.
// - methods. A name-mapped node hash for methods.
//     A method may be a FnTuple, a list of methods with same name.
//     Note: the namespace is comprised of fields + methods
//       with no duplicate names between them
// - subtypes. An ordered list of nodes for its traits/interfaces
typedef struct FieldsAstNode {
	TypeAstHdr;
	Nodes *fields;
} FieldsAstNode;

FieldsAstNode *newStructNode();
void structPrint(FieldsAstNode *node);
void structPass(PassState *pstate, FieldsAstNode *name);
int structEqual(FieldsAstNode *node1, FieldsAstNode *node2);
int structCoerces(FieldsAstNode *to, FieldsAstNode *from);

#endif