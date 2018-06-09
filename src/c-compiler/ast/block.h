/** AST handling for block nodes: Program, FnImpl, Block, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef block_h
#define block_h

// Block
typedef struct BlockAstNode {
	TypedAstHdr;
	Namespace2 namespace;
	Nodes *stmts;
	uint16_t scope;
} BlockAstNode;

// If statement
typedef struct IfAstNode {
	TypedAstHdr;
	Nodes *condblk;
} IfAstNode;

// While statement
typedef struct WhileAstNode {
	BasicAstHdr;
	AstNode *condexp;
	AstNode *blk;
} WhileAstNode;

// Return/yield statement
typedef struct ReturnAstNode {
	BasicAstHdr;
	AstNode *exp;
} ReturnAstNode;

// The various intrinsic functions supported by IntrinsicAstNode
enum IntrinsicFn {
	// Arithmetic
	NegIntrinsic,
	AddIntrinsic,
	SubIntrinsic,
	MulIntrinsic,
	DivIntrinsic,
	RemIntrinsic,

	// Comparison
	EqIntrinsic,
	NeIntrinsic,
	LtIntrinsic,
	LeIntrinsic,
	GtIntrinsic,
	GeIntrinsic,

	// Bitwise
	NotIntrinsic,
	AndIntrinsic,
	OrIntrinsic,
	XorIntrinsic,
	ShlIntrinsic,
	ShrIntrinsic,

	// Intrinsic functions
	SqrtIntrinsic
};

// An internal operation (e.g., add). 
// Used as an alternative to FnDcl->value = Block within a function declaration.
typedef struct IntrinsicAstNode {
	BasicAstHdr;
	int16_t intrinsicFn;
} IntrinsicAstNode;

BlockAstNode *newBlockNode();
void blockPrint(BlockAstNode *blk);
void blockPass(PassState *pstate, BlockAstNode *node);

IfAstNode *newIfNode();
void ifPrint(IfAstNode *ifnode);
void ifPass(PassState *pstate, IfAstNode *ifnode);

WhileAstNode *newWhileNode();
void whilePrint(WhileAstNode *wnode);
void whilePass(PassState *pstate, WhileAstNode *wnode);

void breakPass(PassState *pstate, AstNode *node);

IntrinsicAstNode *newIntrinsicNode(int16_t intrinsicFn);

ReturnAstNode *newReturnNode();
void returnPrint(ReturnAstNode *node);
void returnPass(PassState *pstate, ReturnAstNode *node);

#endif