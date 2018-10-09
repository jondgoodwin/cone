/** Handling for block nodes
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

BlockNode *newBlockNode();
void blockPrint(BlockNode *blk);
void blockPass(PassState *pstate, BlockNode *node);

// Perform data flow analysis
void blockFlow(FlowState *fstate, BlockNode **blknode, int copyflag);

#endif