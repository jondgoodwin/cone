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

// Gate trigger: a call storing through a '&mut X' argument beside another
// argument carrying a borrow -- so a call of two arguments or more, one of
// them a reference
static inline void flowGateCall(FlowState *fstate, Nodes *args) {
    if (args == NULL || args->used < 2 || !flowGateOpen(fstate, FlowGateStore))
        return;
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(args, cnt, argsp)) {
        uint16_t tag = flowGateNamed(((IExpNode *)*argsp)->vtype)->tag;
        if (tag == RefTag || tag == AliasDclTag) {
            flowGateCallAsk(fstate, args);
            return;
        }
    }
}

// An operand of a call or a literal was just walked: while the rest are, a
// borrow it made waits, and the variable it borrows is remembered
static inline void flowGateOperand(FlowState *fstate, INode *operand) {
    if ((operand->tag == BorrowTag || operand->tag == ArrayBorrowTag || operand->tag == CastTag)
        && flowGateOpen(fstate, FlowGateInCall))
        flowGateOperandAsk(fstate, operand);
}

#endif
