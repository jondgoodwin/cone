/** Expression generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../parser/lexer.h"
#include "../shared/error.h"
#include "../coneopts.h"
#include "../ir/nametbl.h"
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
LLVMTypeRef _genlType(GenState *gen, char *name, INode *typ) {
	switch (typ->tag) {
	case IntNbrTag: case UintNbrTag:
	{
		switch (((NbrNode*)typ)->bits) {
		case 1: return LLVMInt1TypeInContext(gen->context);
		case 8: return LLVMInt8TypeInContext(gen->context);
		case 16: return LLVMInt16TypeInContext(gen->context);
		case 32: return LLVMInt32TypeInContext(gen->context);
		case 64: return LLVMInt64TypeInContext(gen->context);
		}
	}
	case FloatNbrTag:
	{
		switch (((NbrNode*)typ)->bits) {
		case 32: return LLVMFloatTypeInContext(gen->context);
		case 64: return LLVMDoubleTypeInContext(gen->context);
		}
	}

	case VoidTag:
		return LLVMVoidTypeInContext(gen->context);

	case RefTag: case PtrTag:
	{
		LLVMTypeRef pvtype = genlType(gen, ((PtrNode *)typ)->pvtype);
		return LLVMPointerType(pvtype, 0);
	}

	case FnSigTag:
	{
		// Build typeref from function signature
		FnSigNode *fnsig = (FnSigNode*)typ;
		LLVMTypeRef *param_types = (LLVMTypeRef *)memAllocBlk(fnsig->parms->used * sizeof(LLVMTypeRef));
		LLVMTypeRef *parm = param_types;
		INode **nodesp;
		uint32_t cnt;
		for (nodesFor(fnsig->parms, cnt, nodesp)) {
			assert((*nodesp)->tag == VarDclTag);
			*parm++ = genlType(gen, ((ITypedNode *)*nodesp)->vtype);
		}
		return LLVMFunctionType(genlType(gen, fnsig->rettype), param_types, fnsig->parms->used, 0);
	}

	case StructTag:
	case AllocTag:
	{
		// Build typeref from struct
		StructNode *strnode = (StructNode*)typ;
        INode **nodesp;
        uint32_t cnt;
        uint32_t propcount = 0;
        for (imethnodesFor(&strnode->methprops, cnt, nodesp)) {
            if ((*nodesp)->tag == VarDclTag)
                ++propcount;
        }
        LLVMTypeRef *prop_types = (LLVMTypeRef *)memAllocBlk(propcount * sizeof(LLVMTypeRef));
		LLVMTypeRef *property = prop_types;
		for (imethnodesFor(&strnode->methprops, cnt, nodesp)) {
			if ((*nodesp)->tag == VarDclTag)
    			*property++ = genlType(gen, ((ITypedNode *)*nodesp)->vtype);
		}
		LLVMTypeRef structype = LLVMStructCreateNamed(gen->context, name);
		LLVMStructSetBody(structype, prop_types, propcount, 0);
		return structype;
	}

	case ArrayTag:
	{
		ArrayNode *anode = (ArrayNode*)typ;
		return LLVMArrayType(genlType(gen, anode->elemtype), anode->size);
	}

	default:
		assert(0 && "Invalid vtype to generate");
		return NULL;
	}
}

// Generate a type value
LLVMTypeRef genlType(GenState *gen, INode *typ) {
	char *name = "";
	if (typ->tag == TypeNameUseTag || typ->tag == AllocTag) {
		// with vtype name use, we can memoize type value and give it a name
		NamedTypeNode *dclnode = typ->tag==AllocTag? (NamedTypeNode*)typ : (NamedTypeNode*)((NameUseNode*)typ)->dclnode;
		if (dclnode->llvmtype)
			return dclnode->llvmtype;

		// Also process the type's methods and functions
		LLVMTypeRef typeref = dclnode->llvmtype = _genlType(gen, &dclnode->namesym->namestr, (INode*)dclnode);
		if (isMethodType(dclnode)) {
            IMethodNode *tnode = (IMethodNode*)dclnode;
            INode **nodesp;
            uint32_t cnt;
            // Declare just method names first, enabling forward references
            for (imethnodesFor(&tnode->methprops, cnt, nodesp)) {
                if ((*nodesp)->tag == FnDclTag)
                    genlGloFnName(gen, (FnDclNode*)*nodesp);
            }
			// Now generate the code for each method
            for (imethnodesFor(&tnode->methprops, cnt, nodesp)) {
                if ((*nodesp)->tag == FnDclTag)
                    genlFn(gen, (FnDclNode*)*nodesp);
			}
		}
		return typeref;
	}
	else
		return _genlType(gen, "", typ);
}

LLVMValueRef genlSizeof(GenState *gen, INode *vtype) {
	unsigned long long size = LLVMABISizeOfType(gen->datalayout, genlType(gen, vtype));
	if (vtype->tag == AllocTag) {
		if (LLVMPointerSize(gen->datalayout) == 32)
			size = (size + 3) & 0xFFFFFFFC;
		else
			size = (size + 7) & 0xFFFFFFFFFFFFFFF8;
	}
	return LLVMConstInt(genlType(gen, (INode*)usizeType), size, 0);
}

// Generate an if statement
LLVMValueRef genlIf(GenState *gen, IfNode *ifnode) {
	LLVMBasicBlockRef endif;
	LLVMBasicBlockRef nextif;
	INode *vtype;
	int i, phicnt, count;
	LLVMValueRef *blkvals;
	LLVMBasicBlockRef *blks;
	INode **nodesp;
	uint32_t cnt;

	// If we are returning a value in each block, set up space for phi info
	vtype = iexpGetTypeDcl(ifnode->vtype);
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
		LLVMValueRef blkval = genlBlock(gen, (BlockNode*)*(nodesp + 1));
		int16_t lastStmttype = nodesLast(((BlockNode*)*(nodesp + 1))->stmts)->tag;
		if (lastStmttype != ReturnTag && lastStmttype != BreakTag && lastStmttype != ContinueTag) {
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
LLVMValueRef genlGetIntrinsicFn(GenState *gen, char *fnname, NameUseNode *fnuse) {
	LLVMValueRef fn = LLVMGetNamedFunction(gen->module, fnname);
	if (!fn)
		fn = LLVMAddFunction(gen->module, fnname, genlType(gen, iexpGetTypeDcl((INode*)fnuse->dclnode)));
	return fn;
}

// Generate a function call, including special intrinsics
LLVMValueRef genlFnCall(GenState *gen, FnCallNode *fncall) {

	// Get Valuerefs for all the parameters to pass to the function
	LLVMValueRef *fnargs = (LLVMValueRef*)memAllocBlk(fncall->args->used * sizeof(LLVMValueRef*));
	LLVMValueRef *fnarg = fnargs;
	INode **nodesp;
	uint32_t cnt;
	for (nodesFor(fncall->args, cnt, nodesp))
		*fnarg++ = genlExpr(gen, *nodesp);

	// Handle call when we have a pointer to a function
	if (fncall->objfn->tag == DerefTag) {
		return LLVMBuildCall(gen->builder, genlExpr(gen, ((DerefNode*)fncall->objfn)->exp), fnargs, fncall->args->used, "");
	}

	// A function call may be to an intrinsic, or to program-defined code
	NameUseNode *fnuse = (NameUseNode *)fncall->objfn;
    VarDclNode *fndcl = (VarDclNode *)fnuse->dclnode;
	switch (fndcl->value? fndcl->value->tag : BlockTag) {
	case BlockTag: {
		return LLVMBuildCall(gen->builder, fndcl->llvmvar, fnargs, fncall->args->used, "");
	}
	case IntrinsicTag: {
		NbrNode *nbrtype = (NbrNode *)iexpGetTypeDcl(*nodesNodes(fncall->args));
		uint16_t nbrtag = nbrtype->tag;

		// Floating point intrinsics
		if (nbrtag == FloatNbrTag) {
			switch (((IntrinsicNode *)fndcl->value)->intrinsicFn) {
			case NegIntrinsic: return LLVMBuildFNeg(gen->builder, fnargs[0], "");
			case AddIntrinsic: return LLVMBuildFAdd(gen->builder, fnargs[0], fnargs[1], "");
			case SubIntrinsic: return LLVMBuildFSub(gen->builder, fnargs[0], fnargs[1], "");
			case MulIntrinsic: return LLVMBuildFMul(gen->builder, fnargs[0], fnargs[1], "");
			case DivIntrinsic: return LLVMBuildFDiv(gen->builder, fnargs[0], fnargs[1], "");
			case RemIntrinsic: return LLVMBuildFRem(gen->builder, fnargs[0], fnargs[1], "");
			// Comparison
			case EqIntrinsic: return LLVMBuildFCmp(gen->builder, LLVMRealOEQ, fnargs[0], fnargs[1], "");
			case NeIntrinsic: return LLVMBuildFCmp(gen->builder, LLVMRealONE, fnargs[0], fnargs[1], "");
			case LtIntrinsic: return LLVMBuildFCmp(gen->builder, LLVMRealOLT, fnargs[0], fnargs[1], "");
			case LeIntrinsic: return LLVMBuildFCmp(gen->builder, LLVMRealOLE, fnargs[0], fnargs[1], "");
			case GtIntrinsic: return LLVMBuildFCmp(gen->builder, LLVMRealOGT, fnargs[0], fnargs[1], "");
			case GeIntrinsic: return LLVMBuildFCmp(gen->builder, LLVMRealOGE, fnargs[0], fnargs[1], "");
			// Intrinsic functions
			case SqrtIntrinsic: 
			{
				char *fnname = nbrtype->bits == 32 ? "llvm.sqrt.f32" : "llvm.sqrt.f64";
				return LLVMBuildCall(gen->builder, genlGetIntrinsicFn(gen, fnname, fnuse), fnargs, fncall->args->used, "");
			}
			}
		}
		// Integer intrinsics
		else {
			switch (((IntrinsicNode *)fndcl->value)->intrinsicFn) {
			
			// Arithmetic
			case NegIntrinsic: return LLVMBuildNeg(gen->builder, fnargs[0], "");
			case AddIntrinsic: return LLVMBuildAdd(gen->builder, fnargs[0], fnargs[1], "");
			case SubIntrinsic: return LLVMBuildSub(gen->builder, fnargs[0], fnargs[1], "");
			case MulIntrinsic: return LLVMBuildMul(gen->builder, fnargs[0], fnargs[1], "");
			case DivIntrinsic:
				if (nbrtag == IntNbrTag)
					return LLVMBuildSDiv(gen->builder, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildUDiv(gen->builder, fnargs[0], fnargs[1], "");
			case RemIntrinsic:
				if (nbrtag == IntNbrTag)
					return LLVMBuildSRem(gen->builder, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildURem(gen->builder, fnargs[0], fnargs[1], "");
			
			// Comparison
			case EqIntrinsic: return LLVMBuildICmp(gen->builder, LLVMIntEQ, fnargs[0], fnargs[1], "");
			case NeIntrinsic: return LLVMBuildICmp(gen->builder, LLVMIntNE, fnargs[0], fnargs[1], "");
			case LtIntrinsic:
				if (nbrtag == IntNbrTag)
					return LLVMBuildICmp(gen->builder, LLVMIntSLT, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntULT, fnargs[0], fnargs[1], "");
			case LeIntrinsic:
				if (nbrtag == IntNbrTag)
					return LLVMBuildICmp(gen->builder, LLVMIntSLE, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntULE, fnargs[0], fnargs[1], "");
			case GtIntrinsic:
				if (nbrtag == IntNbrTag)
					return LLVMBuildICmp(gen->builder, LLVMIntSGT, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntUGT, fnargs[0], fnargs[1], "");
			case GeIntrinsic:
				if (nbrtag == IntNbrTag)
					return LLVMBuildICmp(gen->builder, LLVMIntSGE, fnargs[0], fnargs[1], "");
				else
					return LLVMBuildICmp(gen->builder, LLVMIntUGE, fnargs[0], fnargs[1], "");

			// Bitwise
			case NotIntrinsic: return LLVMBuildNot(gen->builder, fnargs[0], "");
			case AndIntrinsic: return LLVMBuildAnd(gen->builder, fnargs[0], fnargs[1], "");
			case OrIntrinsic: return LLVMBuildOr(gen->builder, fnargs[0], fnargs[1], "");
			case XorIntrinsic: return LLVMBuildXor(gen->builder, fnargs[0], fnargs[1], "");
			case ShlIntrinsic: return LLVMBuildShl(gen->builder, fnargs[0], fnargs[1], "");
			case ShrIntrinsic:
				if (nbrtag == IntNbrTag)
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
LLVMValueRef genlCast(GenState *gen, CastNode* node) {
	NbrNode *fromtype = (NbrNode *)iexpGetTypeDcl(node->exp);
	NbrNode *totype = (NbrNode *)iexpGetTypeDcl(node->vtype);

	// Casting a number to Bool means false if zero and true otherwise
	if (totype == boolType) {
		INode *vtype = iexpGetTypeDcl(node->exp);
		if (fromtype->tag == FloatNbrTag)
			return LLVMBuildFCmp(gen->builder, LLVMRealONE, genlExpr(gen, node->exp), LLVMConstNull(genlType(gen, vtype)), "");
		else
			return LLVMBuildICmp(gen->builder, LLVMIntNE, genlExpr(gen, node->exp), LLVMConstNull(genlType(gen, vtype)), "");
	}

	// Handle number to number casts, depending on relative size and encoding format
	switch (totype->tag) {

	case UintNbrTag:
		if (fromtype->tag == FloatNbrTag)
			return LLVMBuildFPToUI(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
		else if (totype->bits < fromtype->bits)
			return LLVMBuildTrunc(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
		else if (totype->bits > fromtype->bits)
			return LLVMBuildZExt(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
		else
			return LLVMBuildBitCast(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");

	case IntNbrTag:
		if (fromtype->tag == FloatNbrTag)
			return LLVMBuildFPToSI(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
		else if (totype->bits < fromtype->bits)
			return LLVMBuildTrunc(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
		else if (totype->bits > fromtype->bits) {
			if (fromtype->tag == IntNbrTag)
				return LLVMBuildSExt(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
			else
				return LLVMBuildZExt(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
		}
		else
			return LLVMBuildBitCast(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");

	case FloatNbrTag:
		if (fromtype->tag == IntNbrTag)
			return LLVMBuildSIToFP(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
		else if (fromtype->tag == UintNbrTag)
			return LLVMBuildUIToFP(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
		else if (totype->bits < fromtype->bits)
			return LLVMBuildFPTrunc(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
		else if (totype->bits > fromtype->bits)
			return LLVMBuildFPExt(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");
		else
			return genlExpr(gen, node->exp);

	case RefTag: case PtrTag:
		return LLVMBuildBitCast(gen->builder, genlExpr(gen, node->exp), genlType(gen, (INode*)totype), "");

	default:
		assert(0 && "Unknown type to cast to");
		return NULL;
	}
}

// Generate not
LLVMValueRef genlNot(GenState *gen, LogicNode* node) {
	return LLVMBuildXor(gen->builder, genlExpr(gen, node->lexp), LLVMConstInt(LLVMInt1TypeInContext(gen->context), 1, 0), "not");
}

// Generate and, or
LLVMValueRef genlLogic(GenState *gen, LogicNode* node) {
	LLVMBasicBlockRef logicblks[2], logicphi;
	LLVMValueRef logicvals[2];

	// Set up basic blocks
	logicblks[0] = LLVMGetInsertBlock(gen->builder);
	logicphi = genlInsertBlock(gen, node->tag==AndLogicTag? "andphi" : "orphi");
	logicblks[1] = genlInsertBlock(gen, node->tag==AndLogicTag? "andrhs" : "orrhs");

	// Generate left-hand condition and conditional branch
	logicvals[0] = genlExpr(gen, node->lexp);
	if (node->tag==OrLogicTag)
		LLVMBuildCondBr(gen->builder, logicvals[0], logicphi, logicblks[1]);
	else
		LLVMBuildCondBr(gen->builder, logicvals[0], logicblks[1], logicphi);

	// Generate right-hand condition and branch to phi
	LLVMPositionBuilderAtEnd(gen->builder, logicblks[1]);
	logicvals[1] = genlExpr(gen, node->rexp);
	LLVMBuildBr(gen->builder, logicphi);

	// Generate phi
	LLVMPositionBuilderAtEnd(gen->builder, logicphi);
	LLVMValueRef phi = LLVMBuildPhi(gen->builder, genlType(gen, (INode*)boolType), "logicval");
	LLVMAddIncoming(phi, logicvals, logicblks, 2);
	return phi;
}

// Generate local variable
LLVMValueRef genlLocalVar(GenState *gen, VarDclNode *var) {
	assert(var->tag == VarDclTag);
	LLVMValueRef val = NULL;
	var->llvmvar = LLVMBuildAlloca(gen->builder, genlType(gen, var->vtype), &var->namesym->namestr);
	if (var->value)
		LLVMBuildStore(gen->builder, (val = genlExpr(gen, var->value)), var->llvmvar);
	return val;
}

// Generate an lval pointer
LLVMValueRef genlLval(GenState *gen, INode *lval) {
	switch (lval->tag) {
	case VarNameUseTag:
		return ((VarDclNode*)((NameUseNode *)lval)->dclnode)->llvmvar;
	case DerefTag:
		return genlExpr(gen, ((DerefNode *)lval)->exp);
	case FnCallTag:
	{
		FnCallNode *fncall = (FnCallNode *)lval;
		VarDclNode *flddcl = (VarDclNode*)((NameUseNode*)fncall->methprop)->dclnode;
		return LLVMBuildStructGEP(gen->builder, genlLval(gen, fncall->objfn), flddcl->index, &flddcl->namesym->namestr);
	}
	}
	return NULL;
}

// Generate a term
LLVMValueRef genlExpr(GenState *gen, INode *termnode) {
    switch (termnode->tag) {
    case ULitTag:
        return LLVMConstInt(genlType(gen, ((ULitNode*)termnode)->vtype), ((ULitNode*)termnode)->uintlit, 0);
    case FLitTag:
        return LLVMConstReal(genlType(gen, ((ULitNode*)termnode)->vtype), ((FLitNode*)termnode)->floatlit);
    case StrLitTag:
    {
        char *strlit = ((SLitNode *)termnode)->strlit;
        uint32_t size = strlen(strlit) + 1;
        LLVMValueRef sglobal = LLVMAddGlobal(gen->module, LLVMArrayType(LLVMInt8TypeInContext(gen->context), size), "string");
        LLVMSetLinkage(sglobal, LLVMInternalLinkage);
        LLVMSetGlobalConstant(sglobal, 1);
        LLVMSetInitializer(sglobal, LLVMConstStringInContext(gen->context, strlit, size, 1));
        return LLVMBuildStructGEP(gen->builder, sglobal, 0, "");
    }
    case VarNameUseTag:
    {
        VarDclNode *vardcl = (VarDclNode*)((NameUseNode *)termnode)->dclnode;
        return LLVMBuildLoad(gen->builder, vardcl->llvmvar, &vardcl->namesym->namestr);
    }
    case FnCallTag:
    {
        FnCallNode *fncall = (FnCallNode *)termnode;
        // Property access (no arguments will be present)
        if (fncall->methprop) {
            VarDclNode *flddcl = (VarDclNode*)((NameUseNode*)fncall->methprop)->dclnode;
            return LLVMBuildExtractValue(gen->builder, genlExpr(gen, fncall->objfn), flddcl->index, &flddcl->namesym->namestr);
        }
        else {
            INode *objtype = ((ITypedNode *)fncall->objfn)->vtype;
            if (objtype->tag == FnSigTag)
                return genlFnCall(gen, fncall);
            else if (objtype->tag == ArrayTag) {
                LLVMValueRef indexes[2];
                indexes[0] = LLVMConstInt(LLVMInt32TypeInContext(gen->context), 0, 0);
                indexes[1] = genlExpr(gen, nodesGet(fncall->args,0));
                LLVMValueRef arrayptr = ((VarDclNode*)((NameUseNode *)fncall->objfn)->dclnode)->llvmvar; // assume nameuse!
                LLVMValueRef gep = LLVMBuildGEP(gen->builder, arrayptr, indexes, 2, "");
                return LLVMBuildLoad(gen->builder, gep, "");
            }
            else
                assert(0 && "Unknown type of fncall node");
        }
    }
	case AssignTag:
	{
		LLVMValueRef val;
		AssignNode *node = (AssignNode*)termnode;
		LLVMBuildStore(gen->builder, (val = genlExpr(gen, node->rval)), genlLval(gen, node->lval));
		return val;
	}
	case SizeofTag:
		return genlSizeof(gen, ((SizeofNode*)termnode)->type);
	case CastTag:
		return genlCast(gen, (CastNode*)termnode);
	case AddrTag:
	{
		AddrNode *anode = (AddrNode*)termnode;
		RefNode *reftype = (RefNode *)anode->vtype;
		if (reftype->alloc == voidType) {
			assert(anode->exp->tag == VarNameUseTag);
			NameUseNode *var = (NameUseNode*)anode->exp;
			return ((VarDclNode*)var->dclnode)->llvmvar;
		}
		else
			return genlExpr(gen, anode->exp);
	}
	case DerefTag:
		return LLVMBuildLoad(gen->builder, genlExpr(gen, ((DerefNode*)termnode)->exp), "deref");
	case OrLogicTag: case AndLogicTag:
		return genlLogic(gen, (LogicNode*)termnode);
	case NotLogicTag:
		return genlNot(gen, (LogicNode*)termnode);
	case VarDclTag:
		return genlLocalVar(gen, (VarDclNode*)termnode); break;
	case BlockTag:
		return genlBlock(gen, (BlockNode*)termnode); break;
	case IfTag:
		return genlIf(gen, (IfNode*)termnode); break;
	default:
		printf("Unknown node to genlExpr!");
		return NULL;
	}
}
