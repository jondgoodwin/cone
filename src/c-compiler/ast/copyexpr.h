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
	AstNode *lval;
	AstNode *rval;
	int16_t assignType;
} AssignAstNode;

typedef struct AddrAstNode {
	TypedAstHdr;
	AstNode *exp;
} AddrAstNode;

void handleCopy(PassState *pstate, AstNode *node);

AssignAstNode *newAssignAstNode(int16_t assigntype, AstNode *lval, AstNode *rval);
void assignPrint(AssignAstNode *node);
void assignPass(PassState *pstate, AssignAstNode *node);

AddrAstNode *newAddrAstNode();
void addrPrint(AddrAstNode *node);
void addrPass(PassState *pstate, AddrAstNode *node);

#endif