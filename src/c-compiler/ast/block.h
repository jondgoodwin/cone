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

// Function block
typedef struct FnImplAstNode {
	NamedAstHdr;
	AstNode *value;	// May be NULL if not initialized
} FnImplAstNode;

// Block
typedef struct BlockAstNode {
	BasicAstHdr;
	Nodes *nodes;
	// Dict *locals;
	uint16_t scope;
} BlockAstNode;

// Statement expressions (incl. return, yield, ...)
typedef struct StmtExpAstNode {
	BasicAstHdr;
	AstNode *exp;
} StmtExpAstNode;

PgmAstNode *newPgmNode();
void pgmPrint(int indent, PgmAstNode *pgm);
void pgmPass(AstPass *pstate, PgmAstNode *pgm);

FnImplAstNode *newFnImplNode(Symbol *name, AstNode *sig);
void fnImplPrint(int indent, FnImplAstNode *fn);
void fnImplPass(AstPass *pstate, FnImplAstNode *node);

BlockAstNode *newBlockNode();
void blockPrint(int indent, BlockAstNode *blk);
void blockPass(AstPass *pstate, BlockAstNode *node);

StmtExpAstNode *newStmtExpNode();
void stmtExpPrint(int indent, StmtExpAstNode *node);
void stmtExpPass(AstPass *pstate, StmtExpAstNode *node);

StmtExpAstNode *newReturnNode();
void returnPrint(int indent, StmtExpAstNode *node);
void returnPass(AstPass *pstate, StmtExpAstNode *node);

#endif