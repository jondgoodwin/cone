/** AST handling for expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef expr_h
#define expr_h

// Unary operator (e.g., negative)
typedef struct UnaryAstNode {
	TypedAstHdr;
	AstNode *expnode;
} UnaryAstNode;

#endif