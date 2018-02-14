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
#include <string.h>
#include <assert.h>

// Generate a LLVMTypeRef from a basic type definition node
LLVMTypeRef _genlType(genl_t *gen, char *name, AstNode *typ) {
	switch (typ->asttype) {
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

	case RefType: case PtrType:
	{
		LLVMTypeRef pvtype = genlType(gen, ((PtrAstNode *)typ)->pvtype);
		return LLVMPointerType(pvtype, 0);
	}

	case FnSig:
	{
		// Build typeref from function signature
		FnSigAstNode *fnsig = (FnSigAstNode*)typ;
		LLVMTypeRef *param_types = (LLVMTypeRef *)memAllocBlk(fnsig->parms->used * sizeof(LLVMTypeRef));
		LLVMTypeRef *parm = param_types;
		SymNode *nodesp;
		uint32_t cnt;
		for (inodesFor(fnsig->parms, cnt, nodesp)) {
			assert(nodesp->node->asttype == VarNameDclNode);
			*parm++ = genlType(gen, ((TypedAstNode *)nodesp->node)->vtype);
		}
		return LLVMFunctionType(genlType(gen, fnsig->rettype), param_types, fnsig->parms->used, 0);
	}

	case StructType:
	case AllocType:
	{
		// Build typeref from struct
		StructAstNode *strnode = (StructAstNode*)typ;
		LLVMTypeRef *field_types = (LLVMTypeRef *)memAllocBlk(strnode->fields->used * sizeof(LLVMTypeRef));
		LLVMTypeRef *field = field_types;
		SymNode *nodesp;
		uint32_t cnt;
		for (inodesFor(strnode->fields, cnt, nodesp)) {
			assert(nodesp->node->asttype == VarNameDclNode);
			*field++ = genlType(gen, ((TypedAstNode *)nodesp->node)->vtype);
		}
		LLVMTypeRef structype = LLVMStructCreateNamed(gen->context, name);
		LLVMStructSetBody(structype, field_types, strnode->fields->used, 0);
		return structype;
	}

	case ArrayType:
	{
		ArrayAstNode *anode = (ArrayAstNode*)typ;
		return LLVMArrayType(genlType(gen, anode->elemtype), anode->size);
	}

	default:
		assert(0 && "Invalid vtype to generate");
		return NULL;
	}
}

// Generate a type value
LLVMTypeRef genlType(genl_t *gen, AstNode *typ) {
	char *name = "";
	if (typ->asttype == VtypeNameUseNode) {
		// with vtype name use, we can memoize type value and give it a name
		NameDclAstNode *dclnode = ((NameUseAstNode*)typ)->dclnode;
		if (dclnode->llvmvar)
			return (LLVMTypeRef)dclnode->llvmvar;

		LLVMTypeRef typeref = (LLVMTypeRef)(dclnode->llvmvar = (LLVMValueRef)_genlType(gen, &dclnode->namesym->namestr, dclnode->value));
		AstNode **nodesp;
		uint32_t cnt;
		TypeAstNode *tnode = (TypeAstNode*)dclnode->value;
		if (tnode->methods) {
			for (nodesFor(tnode->methods, cnt, nodesp)) {
				NameDclAstNode *fnnode = (NameDclAstNode*)(*nodesp);
				assert(fnnode->asttype == VarNameDclNode);
				if (fnnode->value->asttype != OpCodeNode) {
					genlGloVarName(gen, fnnode);
					genlFn(gen, fnnode);
				}
			}
		}
		return typeref;
	}
	else
		return _genlType(gen, "", typ);
}

// Generate an if statement
LLVMValueRef genlIf(genl_t *gen, IfAstNode *ifnode) {
	LLVMBasicBlockRef endif;
	LLVMBasicBlockRef nextif;
	AstNode *vtype;
	int i, phicnt, count;
	LLVMValueRef *blkvals;
	LLVMBasicBlockRef *blks;
	AstNode **nodesp;
	uint32_t cnt;

	// If we are returning a value in each block, set up space for phi info
	vtype = typeGetVtype(ifnode->vtype);
	count = ifnode->condblk->used / 2;
	i = phicnt = 0;
	if (vtype != voidType) {
		blkvals = memAllocBlk(count * sizeof(LLVMValueRef));
		blks = memAllocBlk(count * sizeof(LLVMBasicBlockRef));
	}

	endif = genlInsertBlock(gen, "endif");
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

		// Generate this condition's code block, along with jump to endif if block does not end with a return
		LLVMValueRef blkval = genlBlock(gen, (BlockAstNode*)*(nodesp + 1));
		int16_t lastStmtAsttype = nodesLast(((BlockAstNode*)*(nodesp + 1))->stmts)->asttype;
		if (lastStmtAsttype != ReturnNode && lastStmtAsttype != BreakNode && lastStmtAsttype != ContinueNode) {
			LLVMBuildBr(gen->builder, endif);
			// Remember value and block if needed for phi merge
			if (vtype != voidType) {
				blkvals[phicnt] = blkval;
				blks[phicnt++] = ablk;
			}
		}

		LLVMPositionBuilderAtEnd(gen->builder, nextif);
		cnt--; nodesp++; i++;
	}

	// Merge point at end of if. Create merged phi value if needed.
	if (phicnt) {
		LLVMValueRef phi = LLVMBuildPhi(gen->builder, genlType(gen, vtype), "ifval");
		LLVMAddIncoming(phi, blkvals, blks, phicnt);
		return phi;
	}

	return NULL;
}

// Obtain value ref for a specific named intrinsic function
LLVMValueRef genlGetIntrinsicFn(genl_t *gen, char *fnname, NameUseAstNode *fnuse) {
	LLVMValueRef fn = LLVMGetNamedFunction(gen->module, fnname);
	if (!fn)
		fn = LLVMAddFunction(gen->module, fnname, genlType(gen, typeGetVtype((AstNode*)fnuse->dclnode)));
	return fn;
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

	// Handle call when we have a pointer to a function
	if (fncall->fn->asttype == DerefNode) {
		return LLVMBuildCall(gen->builder, genlExpr(gen, ((DerefAstNode*)fncall->fn)->exp), fnargs, fncall->parms->used, "");
	}

	// A function call may be to an internal op code, or to program-defined code
	NameUseAstNode *fnuse = (NameUseAstNode *)fncall->fn;
	switch (fnuse->dclnode->value? fnuse->dclnode->value->asttype : BlockNode) {
	case BlockNode: {
		char *fnname = fnuse->dclnode->guname? fnuse->dclnode->guname : &fnuse->dclnode->namesym->namestr;
		return LLVMBuildCall(gen->builder, LLVMGetNamedFunction(gen->module, fnname), fnargs, fncall->parms->used, "");
	}
	case OpCodeNode: {
		NbrAstNode *nbrtype = (NbrAstNode *)typeGetVtype(*nodesNodes(fncall->parms));
		int16_t nbrasttype = nbrtype->asttype;

		// Floating point op codes
		if (nbrasttype == FloatNbrType) {
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
			// Intrinsic functions
			case SqrtOpCode: 
			{
				char *fnname = nbrtype->bits == 32 ? "llvm.sqrt.f32" : "llvm.sqrt.f64";
				return LLVMBuildCall(gen->builder, genlGetIntrinsicFn(gen, fnname, fnuse), fnargs, fncall->parms->used, "");
			}
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
				if (nbrasttype == IntNbrType)
					return LLVMBuildSDiv(gen->builder, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildUDiv(gen->builder, fnargs[0], fnargs[1], "");
			case RemOpCode:
				if (nbrasttype == IntNbrType)
					return LLVMBuildSRem(gen->builder, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildURem(gen->builder, fnargs[0], fnargs[1], "");
			
			// Comparison
			case EqOpCode: return LLVMBuildICmp(gen->builder, LLVMIntEQ, fnargs[0], fnargs[1], "");
			case NeOpCode: return LLVMBuildICmp(gen->builder, LLVMIntNE, fnargs[0], fnargs[1], "");
			case LtOpCode:
				if (nbrasttype == IntNbrType)
					return LLVMBuildICmp(gen->builder, LLVMIntSLT, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntULT, fnargs[0], fnargs[1], "");
			case LeOpCode:
				if (nbrasttype == IntNbrType)
					return LLVMBuildICmp(gen->builder, LLVMIntSLE, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntULE, fnargs[0], fnargs[1], "");
			case GtOpCode:
				if (nbrasttype == IntNbrType)
					return LLVMBuildICmp(gen->builder, LLVMIntSGT, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntUGT, fnargs[0], fnargs[1], "");
			case GeOpCode:
				if (nbrasttype == IntNbrType)
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
				if (nbrasttype == IntNbrType)
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

	case FloatNbrType:
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

	case RefType: case PtrType:
		return LLVMBuildBitCast(gen->builder, genlExpr(gen, node->exp), genlType(gen, (AstNode*)totype), "");

	default:
		assert(0 && "Unknown type to cast to");
		return NULL;
	}
}

// Generate not
LLVMValueRef genlNot(genl_t *gen, LogicAstNode* node) {
	return LLVMBuildXor(gen->builder, genlExpr(gen, node->lexp), LLVMConstInt(LLVMInt1TypeInContext(gen->context), 1, 0), "not");
}

// Generate and, or
LLVMValueRef genlLogic(genl_t *gen, LogicAstNode* node) {
	LLVMBasicBlockRef logicblks[2], logicphi;
	LLVMValueRef logicvals[2];

	// Set up basic blocks
	logicblks[0] = LLVMGetInsertBlock(gen->builder);
	logicphi = genlInsertBlock(gen, node->asttype==AndLogicNode? "andphi" : "orphi");
	logicblks[1] = genlInsertBlock(gen, node->asttype==AndLogicNode? "andrhs" : "orrhs");

	// Generate left-hand condition and conditional branch
	logicvals[0] = genlExpr(gen, node->lexp);
	if (node->asttype==OrLogicNode)
		LLVMBuildCondBr(gen->builder, logicvals[0], logicphi, logicblks[1]);
	else
		LLVMBuildCondBr(gen->builder, logicvals[0], logicblks[1], logicphi);

	// Generate right-hand condition and branch to phi
	LLVMPositionBuilderAtEnd(gen->builder, logicblks[1]);
	logicvals[1] = genlExpr(gen, node->rexp);
	LLVMBuildBr(gen->builder, logicphi);

	// Generate phi
	LLVMPositionBuilderAtEnd(gen->builder, logicphi);
	LLVMValueRef phi = LLVMBuildPhi(gen->builder, genlType(gen, (AstNode*)boolType), "logicval");
	LLVMAddIncoming(phi, logicvals, logicblks, 2);
	return phi;
}

// Generate local variable
LLVMValueRef genlLocalVar(genl_t *gen, NameDclAstNode *var) {
	assert(var->asttype == VarNameDclNode);
	LLVMValueRef val = NULL;
	var->llvmvar = LLVMBuildAlloca(gen->builder, genlType(gen, var->vtype), &var->namesym->namestr);
	if (var->value)
		LLVMBuildStore(gen->builder, (val = genlExpr(gen, var->value)), var->llvmvar);
	return val;
}

// Generate an lval pointer
LLVMValueRef genlLval(genl_t *gen, AstNode *lval) {
	switch (lval->asttype) {
	case VarNameUseNode:
		return ((NameUseAstNode *)lval)->dclnode->llvmvar;
	case DerefNode:
		return genlExpr(gen, ((DerefAstNode *)lval)->exp);
	case ElementNode:
	{
		ElementAstNode *elem = (ElementAstNode *)lval;
		NameDclAstNode *flddcl = ((NameUseAstNode*)elem->element)->dclnode;
		return LLVMBuildStructGEP(gen->builder, genlLval(gen, elem->owner), flddcl->index, &flddcl->namesym->namestr);
	}
	}
	return NULL;
}

// Generate a term
LLVMValueRef genlExpr(genl_t *gen, AstNode *termnode) {
	switch (termnode->asttype) {
	case ULitNode:
		return LLVMConstInt(genlType(gen, ((ULitAstNode*)termnode)->vtype), ((ULitAstNode*)termnode)->uintlit, 0);
	case FLitNode:
		return LLVMConstReal(genlType(gen, ((ULitAstNode*)termnode)->vtype), ((FLitAstNode*)termnode)->floatlit);
	case SLitNode:
	{
		char *strlit = ((SLitAstNode *)termnode)->strlit;
		uint32_t size = strlen(strlit)+1;
		LLVMValueRef sglobal = LLVMAddGlobal(gen->module, LLVMArrayType(LLVMInt8TypeInContext(gen->context), size), "string");
		LLVMSetLinkage(sglobal, LLVMInternalLinkage);
		LLVMSetGlobalConstant(sglobal, 1);
		LLVMSetInitializer(sglobal, LLVMConstStringInContext(gen->context, strlit, size, 1));
		return LLVMBuildStructGEP(gen->builder, sglobal, 0, "");
	}
	case VarNameUseNode:
	{
		NameDclAstNode *vardcl = ((NameUseAstNode *)termnode)->dclnode;
		return LLVMBuildLoad(gen->builder, vardcl->llvmvar, &vardcl->namesym->namestr);
	}
	case FnCallNode:
		return genlFnCall(gen, (FnCallAstNode*)termnode);
	case AssignNode:
	{
		LLVMValueRef val;
		AssignAstNode *node = (AssignAstNode*)termnode;
		LLVMBuildStore(gen->builder, (val = genlExpr(gen, node->rval)), genlLval(gen, node->lval));
		return val;
	}
	case CastNode:
		return genlCast(gen, (CastAstNode*)termnode);
	case AddrNode:
	{
		AddrAstNode *anode = (AddrAstNode*)termnode;
		assert(anode->exp->asttype == VarNameUseNode);
		NameUseAstNode *var = (NameUseAstNode*)anode->exp;
		return var->dclnode->llvmvar;
	}
	case DerefNode:
		return LLVMBuildLoad(gen->builder, genlExpr(gen, ((DerefAstNode*)termnode)->exp), "deref");
	case ElementNode:
	{
		ElementAstNode *elem = (ElementAstNode*)termnode;
		NameDclAstNode *flddcl = ((NameUseAstNode*)elem->element)->dclnode;
		return LLVMBuildExtractValue(gen->builder, genlExpr(gen, elem->owner), flddcl->index, &flddcl->namesym->namestr);
	}
	case OrLogicNode: case AndLogicNode:
		return genlLogic(gen, (LogicAstNode*)termnode);
	case NotLogicNode:
		return genlNot(gen, (LogicAstNode*)termnode);
	case VarNameDclNode:
		return genlLocalVar(gen, (NameDclAstNode*)termnode); break;
	case BlockNode:
		return genlBlock(gen, (BlockAstNode*)termnode); break;
	case IfNode:
		return genlIf(gen, (IfAstNode*)termnode); break;
	default:
		printf("Unknown AST node to genlExpr!");
		return NULL;
	}
}
