/** Handling for block nodes: Program, FnImpl, Block, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef block_h
#define block_h

// Block is a ordered sequence of executable statements within a function.
// It is also a local execution context/namespace owning local variables.
// Local variables are uniquely named and cannot be forward referenced.
typedef struct BlockNode {
	ITypedNodeHdr;
	Nodes *stmts;
    uint16_t scope;
} BlockNode;

// If statement
typedef struct IfNode {
	ITypedNodeHdr;
	Nodes *condblk;
} IfNode;

// While statement
typedef struct WhileNode {
	INodeHdr;
	INode *condexp;
	INode *blk;
} WhileNode;

// Return/yield statement
typedef struct ReturnNode {
	INodeHdr;
	INode *exp;
} ReturnNode;

// The various intrinsic functions supported by IntrinsicNode
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
typedef struct IntrinsicNode {
	INodeHdr;
	int16_t intrinsicFn;
} IntrinsicNode;

BlockNode *newBlockNode();
void blockPrint(BlockNode *blk);
void blockPass(PassState *pstate, BlockNode *node);

IfNode *newIfNode();
void ifPrint(IfNode *ifnode);
void ifPass(PassState *pstate, IfNode *ifnode);

WhileNode *newWhileNode();
void whilePrint(WhileNode *wnode);
void whilePass(PassState *pstate, WhileNode *wnode);

void breakPass(PassState *pstate, INode *node);

IntrinsicNode *newIntrinsicNode(int16_t intrinsicFn);

ReturnNode *newReturnNode();
void returnPrint(ReturnNode *node);
void returnPass(PassState *pstate, ReturnNode *node);

#endif