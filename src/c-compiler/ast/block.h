/** AST handling for block nodes: Program, FnImpl, Block, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef block_h
#define block_h

// Program
typedef struct PgmAstNode {
	BasicAstHdr;
	Nodes *nodes;
} PgmAstNode;

// Block
typedef struct BlockAstNode {
	TypedAstHdr;
	Inodes *locals;
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

// The various op codes supported by OpCodeAstNode
enum OpCode {
	// Arithmetic
	NegOpCode,
	AddOpCode,
	SubOpCode,
	MulOpCode,
	DivOpCode,
	RemOpCode,

	// Comparison
	EqOpCode,
	NeOpCode,
	LtOpCode,
	LeOpCode,
	GtOpCode,
	GeOpCode,

	// Bitwise
	NotOpCode,
	AndOpCode,
	OrOpCode,
	XorOpCode,
	ShlOpCode,
	ShrOpCode
};

// An internal operation (e.g., add). 
// Used as an alternative to FnDcl->value = Block within a function declaration.
typedef struct OpCodeAstNode {
	BasicAstHdr;
	int16_t opcode;
} OpCodeAstNode;

PgmAstNode *newPgmNode();
void pgmPrint(PgmAstNode *pgm);
void pgmPass(AstPass *pstate, PgmAstNode *pgm);

BlockAstNode *newBlockNode();
void blockPrint(BlockAstNode *blk);
void blockPass(AstPass *pstate, BlockAstNode *node);

IfAstNode *newIfNode();
void ifPrint(IfAstNode *ifnode);
void ifPass(AstPass *pstate, IfAstNode *ifnode);

WhileAstNode *newWhileNode();
void whilePrint(WhileAstNode *wnode);
void whilePass(AstPass *pstate, WhileAstNode *wnode);

void breakPass(AstPass *pstate, AstNode *node);

OpCodeAstNode *newOpCodeNode(int16_t opcode);

ReturnAstNode *newReturnNode();
void returnPrint(ReturnAstNode *node);
void returnPass(AstPass *pstate, ReturnAstNode *node);

#endif