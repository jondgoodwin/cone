/** AST handling for expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef expr_h
#define expr_h

// Variations on assignment
enum AssignType {
	NormalAssign
};

// Assignment node
typedef struct AssignAstNode {
	TypedAstHdr;
	AstNode *lval;
	AstNode *rval;
	int16_t assignType;
} AssignAstNode;

// Unary operator (e.g., negative)
typedef struct UnaryAstNode {
	TypedAstHdr;
	AstNode *expnode;
} UnaryAstNode;

AssignAstNode *newAssignAstNode(int16_t assigntype, AstNode *lval, AstNode *rval);
void assignPrint(AssignAstNode *node);
void assignPass(AstPass *pstate, AssignAstNode *node);

#endif