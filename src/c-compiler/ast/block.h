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
	BasicAstHdr;
	Inodes *locals;
	Nodes *stmts;
	uint16_t scope;
} BlockAstNode;

// Statement expressions (incl. return, yield, ...)
typedef struct StmtExpAstNode {
	BasicAstHdr;
	AstNode *exp;
} StmtExpAstNode;

PgmAstNode *newPgmNode();
void pgmPrint(PgmAstNode *pgm);
void pgmPass(AstPass *pstate, PgmAstNode *pgm);

BlockAstNode *newBlockNode();
void blockPrint(BlockAstNode *blk);
void blockPass(AstPass *pstate, BlockAstNode *node);

StmtExpAstNode *newStmtExpNode();
void stmtExpPrint(StmtExpAstNode *node);
void stmtExpPass(AstPass *pstate, StmtExpAstNode *node);

StmtExpAstNode *newReturnNode();
void returnPrint(StmtExpAstNode *node);
void returnPass(AstPass *pstate, StmtExpAstNode *node);

#endif