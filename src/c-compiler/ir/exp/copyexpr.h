/** AST handling for expression nodes that might do copy/move
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef copyexpr_h
#define copyexpr_h

// Variations on assignment
enum AssignType {
	NormalAssign
};

// Assignment node
typedef struct AssignAstNode {
	TypedAstHdr;
	INode *lval;
	INode *rval;
	int16_t assignType;
} AssignAstNode;

typedef struct AddrAstNode {
	TypedAstHdr;
	INode *exp;
} AddrAstNode;

AssignAstNode *newAssignAstNode(int16_t assigntype, INode *lval, INode *rval);
void assignPrint(AssignAstNode *node);
void assignPass(PassState *pstate, AssignAstNode *node);

AddrAstNode *newAddrAstNode();
void addrPrint(AddrAstNode *node);
void addrPass(PassState *pstate, AddrAstNode *node);

#endif