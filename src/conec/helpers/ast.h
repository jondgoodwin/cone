/** AST encoding definitions: AstNode, AstType
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef ast_h
#define ast_h

#include "stdint.h"

typedef struct AstNode AstNode;

// AstNode encodes every type of token or AST node in a fixed-size structure.
// A single structure converges both purposes, as most tokens are proto-AST nodes.
struct AstNode {
	// The "value" information held by the node
	union {
		double floatlit;
		uint64_t uintlit;
		void *val;
	} v;
	AstNode *typeinfo;
	// Token/Node's location in the source program
	// Type information about the Token/AST Node
	short int type;			// AstType
	uint16_t flags;
};

// All the possible types for an AstNode
enum AstType {
	EofNode,		// End-of-file
	LitNode,		// Literal
	NbrAstTypes
};

#endif