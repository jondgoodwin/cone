/** The flow gate's triggers, as inline tests
 * @file
 *
 * The gate (flow.h, FlowGate) is asked at nearly every node of every function,
 * and most functions it never marks. Asked through calls that resolved each type
 * first, it cost flow 25-30% on code holding no borrow, where the walk itself
 * spends some 20ns a node. So each trigger here is an inline test that looks
 * through one name use to the declaration and dismisses the common case -- a
 * number, a struct already known to carry no borrow, a call whose arguments are
 * no references, an operand that is no borrow -- and asks the whole question out
 * of line (flow.c) only when the answer may matter.
 *
 * It reads the node types, so ir.h includes it after every node header.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef flowgate_h
#define flowgate_h

// Is the gate still to decide this trigger? At -V 2 every trigger is asked
// until it fires, to count each; otherwise the first one settles the gate.
#define flowGateOpen(fstate, trigger) \
    (flowGateCountAll ? !((fstate)->gate & (trigger)) : !(fstate)->gate)

// What a type expression names, looking through one name use
static inline INode *flowGateNamed(INode *type) {
    if (type->tag == NameUseTag && ((NameUseNode *)type)->dclnode)
        return ((NameUseNode *)type)->dclnode;
    return type;
}

// Is this type certainly free of borrows, by a look at what it names: a
// number, void, or a struct already known to carry none? Anything else is asked.
static inline int flowGateCarriesNone(INode *type) {
    INode *dcl = flowGateNamed(type);
    switch (dcl->tag) {
    case IntNbrTag:
    case UintNbrTag:
    case FloatNbrTag:
    case VoidTag:
        return 1;
    case StructTag:
        return ((StructNode *)dcl)->carriesborrow == CarriesBorrowNo;
    default:
        return 0;
    }
}

// Gate trigger: a local declared with this type
static inline void flowGateHolder(FlowState *fstate, INode *type) {
    if (flowGateOpen(fstate, FlowGateHolder) && type && !flowGateCarriesNone(type)
        && itypeCarriesBorrow(type))
        fstate->gate |= FlowGateHolder;
}

// Gate trigger: a place assigned or swapped. A local named whole was asked
// about at its declaration, and '_' stores nothing.
static inline void flowGateAssigned(FlowState *fstate, INode *lval) {
    if (lval->tag == NameUseTag) {
        INode *dcl = ((NameUseNode *)lval)->dclnode;
        if (dcl == NULL || (dcl->tag == VarDclTag && ((VarDclNode *)dcl)->scope >= 2
                && !(dcl->flags & FlagStatic)))
            return;
    }
    flowGateHolder(fstate, ((IExpNode *)lval)->vtype);
}

// Gate trigger: a value a return, break or block end hands out
static inline void flowGateResult(FlowState *fstate, INode *exp) {
    if (flowGateOpen(fstate, FlowGateResult) && exp->tag != NilLitTag) {
        INode *type = ((IExpNode *)exp)->vtype;
        if (type && !flowGateCarriesNone(type))
            flowGateResultAsk(fstate, type);
    }
}

// Gate trigger: a call storing through a '&mut X' argument, or a struct
// holding one, beside another argument carrying a borrow -- so a call of two
// arguments or more, one of them a reference or a struct that may hold one
static inline void flowGateCall(FlowState *fstate, Nodes *args) {
    if (args == NULL || args->used < 2 || !flowGateOpen(fstate, FlowGateStore))
        return;
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(args, cnt, argsp)) {
        INode *named = flowGateNamed(((IExpNode *)*argsp)->vtype);
        if (named->tag == RefTag || named->tag == AliasDclTag
            || (named->tag == StructTag && ((StructNode *)named)->carriesborrow != CarriesBorrowNo)) {
            flowGateCallAsk(fstate, args);
            return;
        }
    }
}

// Gate trigger: a method lending a borrow from a value that may change shape,
// reached through a reference, in a function that also makes a call with a
// writable argument. Asked even when another trigger has fired, for it also
// says the callees' bodies are checked before the walk (fnDclTypeCheck). A call
// whose result holds no borrow is dismissed by its type.
static inline void flowGateShape(FlowState *fstate, FnCallNode *call) {
    if (fstate->gate & FlowGateShape)
        return;
    // A call with a writable argument matters once a borrow is lent before it (a
    // borrow held across a loop is a holder, which gates the function itself)
    if (fstate->shapelend) {
        // A borrow of a 'var' global is asked of every call, not only one with a writable argument
        if (fstate->shapeglobal)
            fstate->gate |= FlowGateShape;
        else {
            if (!fstate->shapewrite)
                flowGateShapeWrite(fstate, call->args);
            // (a later borrow lent from a global)
            if (!(fstate->gate & FlowGateShape) && call->vtype && !flowGateCarriesNone(call->vtype))
                flowGateShapeAsk(fstate, (INode *)call);
        }
    }
    else if (call->vtype && !flowGateCarriesNone(call->vtype))
        flowGateShapeAsk(fstate, (INode *)call);
}

// An operand of a call or a literal is about to be walked. One that is itself a
// call or a literal whose value may carry a borrow may carry one its own operands
// made, of a variable the later operands may name ('both(s.view(), &mut s)',
// 'two(first(&mut b), &mut b)'): what its operands lend is kept waiting
// (flowGateOperandsEnd) until the call or literal around it is made. One whose
// value carries none ('len(s.view())') lends nothing past itself.
static inline void flowGateOperandNext(FlowState *fstate, INode *operand) {
    while (operand->tag == CastTag)
        operand = ((CastNode *)operand)->exp;
    if ((operand->tag == FnCallTag || operand->tag == TypeLitTag || operand->tag == ArrayLitTag
            || operand->tag == VTupleTag)
        && flowGateOpen(fstate, FlowGateInCall)
        && ((IExpNode *)operand)->vtype && !flowGateCarriesNone(((IExpNode *)operand)->vtype))
        fstate->inflightkeep = 1;
}

// An operand of a call or a literal was just walked: while the rest are, a
// borrow it made waits, and the variable it borrows is remembered
static inline void flowGateOperand(FlowState *fstate, INode *operand) {
    fstate->inflightkeep = 0;
    if ((operand->tag == BorrowTag || operand->tag == ArrayBorrowTag || operand->tag == CastTag
            || operand->tag == ArrIndexTag)
        && flowGateOpen(fstate, FlowGateInCall))
        flowGateOperandAsk(fstate, operand);
}

// Gate trigger: a reference narrowed from an enum to one of its variants through
// a reference other references share (castSumNarrows). Whether the variant could
// change while the narrowed reference is used is the loan walk's to say, through
// the same name and through others, so the function is walked
static inline void flowGateNarrow(FlowState *fstate, INode *cast) {
    if (castSumNarrows(cast))
        fstate->gate |= FlowGateShape;
}

// Gate trigger: a place stored into, in a function that has lent a borrow of a
// value that may change shape: the store could replace what the borrow is into
// (a list or string whole, an owning element of one, an enum), through another
// name for it (reshape.h). A local named whole has no other name; a value that is
// no struct (a number, a reference) is no collection or enum.
static inline void flowGateShapeStore(FlowState *fstate, INode *lval) {
    if (!fstate->shapelend || (fstate->gate & FlowGateShape))
        return;
    if (lval->tag == NameUseTag || lval->tag == VTupleTag)
        return;
    INode *type = ((IExpNode *)lval)->vtype;
    if (type && flowGateNamed(type)->tag == StructTag)
        fstate->gate |= FlowGateShape;
}

// Gate trigger: a conversion, which may make an owning virtual reference of
// a value holding a borrow ('Uni[Trait]' from a 'Uni[H]'): a number's
// conversion is dismissed by its operand's type
static inline void flowGateBoxed(FlowState *fstate, INode *cast) {
    if ((cast->flags & FlagConvert) && flowGateOpen(fstate, FlowGateBoxed)
        && !flowGateCarriesNone(((IExpNode *)((CastNode *)cast)->exp)->vtype))
        flowGateBoxedAsk(fstate, cast);
}

// Gate trigger: the value of a struct or variant literal's field at 'i',
// which may hold a borrow given to a field holding only ''static' ones: a
// value that is no reference nor a struct that may hold one is dismissed
static inline void flowGateStatic(FlowState *fstate, INode *lit, uint32_t i, INode *val) {
    if (flowGateOpen(fstate, FlowGateStatic) && !flowGateCarriesNone(((IExpNode *)val)->vtype))
        flowGateStaticAsk(fstate, lit, i, val);
}

#endif
