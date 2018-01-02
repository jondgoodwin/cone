/** Expression generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../parser/lexer.h"
#include "../shared/error.h"
#include "../coneopts.h"
#include "../shared/symbol.h"
#include "../shared/fileio.h"
#include "genllvm.h"

#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Transforms/Scalar.h>

#include <stdio.h>
#include <assert.h>

// Generate a type value
LLVMTypeRef genlType(genl_t *gen, AstNode *typ) {
	switch (typ->asttype) {
	// If it's a name declaration (e.g., parm), resolve it to the actual type info
	case VarNameDclNode:
		return genlType(gen, ((NameDclAstNode *)typ)->vtype);
	// If it's a name, resolve it to the actual type info
	case VtypeNameUseNode:
		return genlType(gen, ((NameUseAstNode *)typ)->dclnode->value);
	case IntNbrType: case UintNbrType:
	{
		switch (((NbrAstNode*)typ)->bits) {
		case 1: return LLVMInt1TypeInContext(gen->context);
		case 8: return LLVMInt8TypeInContext(gen->context);
		case 16: return LLVMInt16TypeInContext(gen->context);
		case 32: return LLVMInt32TypeInContext(gen->context);
		case 64: return LLVMInt64TypeInContext(gen->context);
		}
	}
	case FloatNbrType:
	{
		switch (((NbrAstNode*)typ)->bits) {
		case 32: return LLVMFloatTypeInContext(gen->context);
		case 64: return LLVMDoubleTypeInContext(gen->context);
		}
	}
	case VoidType:
		return LLVMVoidTypeInContext(gen->context);

	default:
		return NULL;
	}
}

// Generate an if statement
LLVMValueRef genlIf(genl_t *gen, IfAstNode *ifnode) {
	LLVMBasicBlockRef endif;
	LLVMBasicBlockRef nextif;
	AstNode *vtype;
	int i, count;
	LLVMValueRef *blkvals;
	LLVMBasicBlockRef *blks;
	AstNode **nodesp;
	uint32_t cnt;

	// If we are returning a value in each block, set up space for phi info
	vtype = typeGetVtype(ifnode->vtype);
	count = ifnode->condblk->used / 2;
	i = 0;
	if (vtype != voidType) {
		blkvals = memAllocBlk(count * sizeof(LLVMValueRef));
		blks = memAllocBlk(count * sizeof(LLVMBasicBlockRef));
	}

	endif = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "endif");
	LLVMBasicBlockRef nextMemBlk;
	for (nodesFor(ifnode->condblk, cnt, nodesp)) {

		// Set up block for next condition (or endif if this is last condition)
		if (i + 1 < count)
			nextif = nextMemBlk = LLVMInsertBasicBlockInContext(gen->context, endif, "ifnext");
		else
			nextif = endif;

		// Set up this condition's statement block and then conditionally jump to it or next condition
		LLVMBasicBlockRef ablk;
		if (*nodesp != voidType) {
			ablk = LLVMInsertBasicBlockInContext(gen->context, nextif, "ifblk");
			LLVMBuildCondBr(gen->builder, genlExpr(gen, *nodesp), ablk, nextif);
			LLVMPositionBuilderAtEnd(gen->builder, ablk);
		}
		else
			ablk = nextMemBlk;

		// Generate this condition's code block, along with jump to endif
		LLVMValueRef blkval = genlBlock(gen, (BlockAstNode*)*(nodesp + 1));
		LLVMBuildBr(gen->builder, endif);

		// Remember value and block if needed for phi merge
		if (vtype != voidType) {
			blkvals[i] = blkval;
			blks[i] = ablk;
		}

		LLVMPositionBuilderAtEnd(gen->builder, nextif);
		cnt--; nodesp++; i++;
	}

	// Merge point at end of if. Create merged phi value if needed.
	if (vtype != voidType) {
		LLVMValueRef phi = LLVMBuildPhi(gen->builder, genlType(gen, vtype), "ifval");
		LLVMAddIncoming(phi, blkvals, blks, count);
		return phi;
	}

	return NULL;
}

// Generate a function call, including special op codes
LLVMValueRef genlFnCall(genl_t *gen, FnCallAstNode *fncall) {

	// Get Valuerefs for all the parameters to pass to the function
	LLVMValueRef *fnargs = (LLVMValueRef*)memAllocBlk(fncall->parms->used * sizeof(LLVMValueRef*));
	LLVMValueRef *fnarg = fnargs;
	AstNode **nodesp;
	uint32_t cnt;
	for (nodesFor(fncall->parms, cnt, nodesp))
		*fnarg++ = genlExpr(gen, *nodesp);

	// A function call may be to an internal op code, or to program-defined code
	NameUseAstNode *fnuse = (NameUseAstNode *)fncall->fn;
	switch (fnuse->dclnode->value->asttype) {
	case BlockNode: {
		char *fnname = &fnuse->dclnode->namesym->namestr;
		return LLVMBuildCall(gen->builder, LLVMGetNamedFunction(gen->module, fnname), fnargs, fncall->parms->used, "");
	}
	case OpCodeNode: {
		int16_t nbrtype = typeGetVtype(*nodesNodes(fncall->parms))->asttype;

		// Floating point op codes
		if (nbrtype == FloatNbrType) {
			switch (((OpCodeAstNode *)fnuse->dclnode->value)->opcode) {
			case NegOpCode: return LLVMBuildFNeg(gen->builder, fnargs[0], "");
			case AddOpCode: return LLVMBuildFAdd(gen->builder, fnargs[0], fnargs[1], "");
			case SubOpCode: return LLVMBuildFSub(gen->builder, fnargs[0], fnargs[1], "");
			case MulOpCode: return LLVMBuildFMul(gen->builder, fnargs[0], fnargs[1], "");
			case DivOpCode: return LLVMBuildFDiv(gen->builder, fnargs[0], fnargs[1], "");
			case RemOpCode: return LLVMBuildFRem(gen->builder, fnargs[0], fnargs[1], "");
			// Comparison
			case EqOpCode: return LLVMBuildFCmp(gen->builder, LLVMRealOEQ, fnargs[0], fnargs[1], "");
			case NeOpCode: return LLVMBuildFCmp(gen->builder, LLVMRealONE, fnargs[0], fnargs[1], "");
			case LtOpCode: return LLVMBuildFCmp(gen->builder, LLVMRealOLT, fnargs[0], fnargs[1], "");
			case LeOpCode: return LLVMBuildFCmp(gen->builder, LLVMRealOLE, fnargs[0], fnargs[1], "");
			case GtOpCode: return LLVMBuildFCmp(gen->builder, LLVMRealOGT, fnargs[0], fnargs[1], "");
			case GeOpCode: return LLVMBuildFCmp(gen->builder, LLVMRealOGE, fnargs[0], fnargs[1], "");
			}
		}
		// Integer op codes
		else {
			switch (((OpCodeAstNode *)fnuse->dclnode->value)->opcode) {
			
			// Arithmetic
			case NegOpCode: return LLVMBuildNeg(gen->builder, fnargs[0], "");
			case AddOpCode: return LLVMBuildAdd(gen->builder, fnargs[0], fnargs[1], "");
			case SubOpCode: return LLVMBuildSub(gen->builder, fnargs[0], fnargs[1], "");
			case MulOpCode: return LLVMBuildMul(gen->builder, fnargs[0], fnargs[1], "");
			case DivOpCode:
				if (nbrtype == IntNbrType)
					return LLVMBuildSDiv(gen->builder, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildUDiv(gen->builder, fnargs[0], fnargs[1], "");
			case RemOpCode:
				if (nbrtype == IntNbrType)
					return LLVMBuildSRem(gen->builder, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildURem(gen->builder, fnargs[0], fnargs[1], "");
			
			// Comparison
			case EqOpCode: return LLVMBuildICmp(gen->builder, LLVMIntEQ, fnargs[0], fnargs[1], "");
			case NeOpCode: return LLVMBuildICmp(gen->builder, LLVMIntNE, fnargs[0], fnargs[1], "");
			case LtOpCode:
				if (nbrtype == IntNbrType)
					return LLVMBuildICmp(gen->builder, LLVMIntSLT, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntULT, fnargs[0], fnargs[1], "");
			case LeOpCode:
				if (nbrtype == IntNbrType)
					return LLVMBuildICmp(gen->builder, LLVMIntSLE, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntULE, fnargs[0], fnargs[1], "");
			case GtOpCode:
				if (nbrtype == IntNbrType)
					return LLVMBuildICmp(gen->builder, LLVMIntSGT, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntUGT, fnargs[0], fnargs[1], "");
			case GeOpCode:
				if (nbrtype == IntNbrType)
					return LLVMBuildICmp(gen->builder, LLVMIntSGE, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntUGE, fnargs[0], fnargs[1], "");

			// Bitwise
			case NotOpCode: return LLVMBuildNot(gen->builder, fnargs[0], "");
			case AndOpCode: return LLVMBuildAnd(gen->builder, fnargs[0], fnargs[1], "");
			case OrOpCode: return LLVMBuildOr(gen->builder, fnargs[0], fnargs[1], "");
			case XorOpCode: return LLVMBuildXor(gen->builder, fnargs[0], fnargs[1], "");
			case ShlOpCode: return LLVMBuildShl(gen->builder, fnargs[0], fnargs[1], "");
			case ShrOpCode:
				if (nbrtype == IntNbrType)
					return LLVMBuildAShr(gen->builder, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildLShr(gen->builder, fnargs[0], fnargs[1], "");
			}
		}
	}
	default:
		assert(0 && "invalid type of function call");
		return NULL;
	}
}

// Generate a cast (value conversion)
LLVMValueRef genlCast(genl_t *gen, CastAstNode* node) {
	NbrAstNode *fromtype = (NbrAstNode *)typeGetVtype(node->exp);
	NbrAstNode *totype = (NbrAstNode *)typeGetVtype(node->vtype);

	// Casting a number to Bool means false if zero and true otherwise
	if (totype == boolType) {
		AstNode *vtype = typeGetVtype(node->exp);
		if (fromtype->asttype == FloatNbrType)
			return LLVMBuildFCmp(gen->builder, LLVMRealONE, genlExpr(gen, node->exp), LLVMConstNull(genlType(gen, vtype)), "");
		else
			return LLVMBuildICmp(gen->builder, LLVMIntNE, genlExpr(gen, node->exp), LLVMConstNull(genlType(gen, vtype)), "");
	}

	// Handle number to number casts, depending on relative size and encoding format
	switch (totype->asttype) {

	case UintNbrType:
		if (fromtype->asttype == FloatNbrType)
			return LLVMBuildFPToUI(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		else if (totype->bits < fromtype->bits)
			return LLVMBuildTrunc(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		else if (totype->bits > fromtype->bits)
			return LLVMBuildZExt(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		else
			return LLVMBuildBitCast(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");

	case IntNbrType:
		if (fromtype->asttype == FloatNbrType)
			return LLVMBuildFPToSI(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		else if (totype->bits < fromtype->bits)
			return LLVMBuildTrunc(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		else if (totype->bits > fromtype->bits) {
			if (fromtype->asttype == IntNbrType)
				return LLVMBuildSExt(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
			else
				return LLVMBuildZExt(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		}
		else
			return LLVMBuildBitCast(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");

	default:
		if (fromtype->asttype == IntNbrType)
			return LLVMBuildSIToFP(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		else if (fromtype->asttype == UintNbrType)
			return LLVMBuildUIToFP(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		else if (totype->bits < fromtype->bits)
			return LLVMBuildFPTrunc(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		else if (totype->bits > fromtype->bits)
			return LLVMBuildFPExt(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");
		else
			return genlExpr(gen, node->exp);
	}
}

// Generate a return statement
LLVMValueRef genlReturn(genl_t *gen, ReturnAstNode *node) {
	if (node->exp != voidType)
		LLVMBuildRet(gen->builder, genlExpr(gen, node->exp));
	else
		LLVMBuildRetVoid(gen->builder);
	return NULL;
}

// Generate local variable
LLVMValueRef genlLocalVar(genl_t *gen, NameDclAstNode *var) {
	assert(var->asttype == VarNameDclNode);
	LLVMValueRef val = NULL;
	var->llvmvar = LLVMBuildAlloca(gen->builder, genlType(gen, (AstNode*)var), &var->namesym->namestr);
	if (var->value)
		LLVMBuildStore(gen->builder, (val = genlExpr(gen, var->value)), var->llvmvar);
	return val;
}

// Generate a block's statements
LLVMValueRef genlBlock(genl_t *gen, BlockAstNode *blk) {
	AstNode **nodesp;
	uint32_t cnt;
	LLVMValueRef lastval;
	for (nodesFor(blk->stmts, cnt, nodesp))
		lastval = genlExpr(gen, *nodesp);
	return lastval;
}

// Generate a term
LLVMValueRef genlExpr(genl_t *gen, AstNode *termnode) {
	switch (termnode->asttype) {
	case ULitNode:
		return LLVMConstInt(genlType(gen, ((ULitAstNode*)termnode)->vtype), ((ULitAstNode*)termnode)->uintlit, 0);
	case FLitNode:
		return LLVMConstReal(genlType(gen, ((ULitAstNode*)termnode)->vtype), ((FLitAstNode*)termnode)->floatlit);
	case VarNameUseNode:
	{
		// Load from a variable. If pointer, do a load otherwise assume it is the (immutable) value
		LLVMValueRef varval = ((NameUseAstNode *)termnode)->dclnode->llvmvar;
		if (LLVMGetTypeKind(LLVMTypeOf(varval)) == LLVMPointerTypeKind) {
			char *name = &((NameUseAstNode *)termnode)->dclnode->namesym->namestr;
			return LLVMBuildLoad(gen->builder, varval, name);
		}
		else
			return varval;
	}
	case FnCallNode:
		return genlFnCall(gen, (FnCallAstNode*)termnode);
	case AssignNode:
	{
		LLVMValueRef val;
		AssignAstNode *node = (AssignAstNode*)termnode;
		NameDclAstNode *lvalvar = ((NameUseAstNode *)node->lval)->dclnode;
		LLVMBuildStore(gen->builder, (val = genlExpr(gen, node->rval)), lvalvar->llvmvar);
		return val;
	}
	case CastNode:
		return genlCast(gen, (CastAstNode*)termnode);
	case VarNameDclNode:
		return genlLocalVar(gen, (NameDclAstNode*)termnode); break;
	case ReturnNode:
		return genlReturn(gen, (ReturnAstNode*)termnode); break;
	case IfNode:
		return genlIf(gen, (IfAstNode*)termnode); break;
	default:
		printf("Unknown AST node to genlExpr!");
		return NULL;
	}
}
