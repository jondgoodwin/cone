/** Generic node handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"
#include "../parser/lexer.h"
#include "../shared/fileio.h"
#include "../shared/error.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

// Copy lexer info over
void inodeLexCopy(INode *new, INode *old) {
    new->instnode = old->instnode;
    new->lexer = old->lexer;
    new->linenbr = old->linenbr;
    new->linep = old->linep;
    new->srcp = old->srcp;
}

// State for inodePrint
FILE *irfile;
int irIndent=0;
int irIsNL = 1;

// Output a string to irfile
void inodeFprint(char *str, ...) {
    va_list argptr;
    va_start(argptr, str);
    vfprintf(irfile, str, argptr);
    va_end(argptr);
    irIsNL = 0;
}

// Print new line character
void inodePrintNL() {
    if (!irIsNL)
        fputc('\n', irfile);
    irIsNL = 1;
}

// Output a line's beginning indentation
void inodePrintIndent() {
    int cnt;
    for (cnt = 0; cnt<irIndent; cnt++)
        fprintf(irfile, (cnt & 3) == 0 ? "| " : "  ");
    irIsNL = 0;
}

// Increment indentation
void inodePrintIncr() {
    irIndent++;
}

// Decrement indentation
void inodePrintDecr() {
    irIndent--;
}

// Serialize a specific node
void inodePrintNode(INode *node) {
    // A name use prints as its name, whatever it has resolved to
    if (isNameUseNode(node)) {
        nameUsePrint((NameUseNode *)node);
        return;
    }
    switch (node->tag) {
    case ProgramTag:
        pgmPrint((ProgramNode *)node); break;
    case ModuleTag:
        modPrint((ModuleNode *)node); break;
    case ModTraitTag:
        modTraitPrint((ModTraitNode *)node); break;
    case FnDclTag:
        fnDclPrint((FnDclNode *)node); break;
    case FnOverloadDclTag:
        fnOverloadDclPrint((FnOverloadDclNode *)node); break;
    case VarDclTag:
        varDclPrint((VarDclNode *)node); break;
    case ConstDclTag:
        constDclPrint((ConstDclNode *)node); break;
    case AliasDclTag:
        aliasDclPrint((AliasDclNode *)node); break;
    case FieldDclTag:
        fieldDclPrint((FieldDclNode *)node); break;
    case ImportTag:
        importPrint((ImportNode *)node); break;
    case ModUseTag:
        modUsePrint((ModUseNode *)node); break;
    case BlockTag:
        blockPrint((BlockNode *)node); break;
    case IfTag:
        ifPrint((IfNode *)node); break;
    case BreakTag:
        inodeFprint("break"); break;
    case ContinueTag:
        inodeFprint("continue"); break;
    case BlockRetTag:
    case ReturnTag:
        returnPrint((BreakRetNode *)node); break;
    case AssignTag:
        assignPrint((AssignNode *)node); break;
    case SwapTag:
        swapPrint((SwapNode *)node); break;
    case VTupleTag:
        vtuplePrint((TupleNode *)node); break;
    case FnCallTag:
    case FldAccessTag:
    case ArrIndexTag:
        fnCallPrint((FnCallNode *)node); break;
    case SizeofTag:
        sizeofPrint((SizeofNode *)node); break;
    case CastTag:  case IsTag:
        castPrint((CastNode *)node); break;
    case DerefTag:
        derefPrint((StarNode *)node); break;
    case ArrayBorrowTag:
    case BorrowTag:
        borrowPrint((RefNode *)node); break;
    case ArrayAllocTag:
    case AllocateTag:
        allocatePrint((RefNode *)node); break;
    case NotLogicTag: case OrLogicTag: case AndLogicTag:
        logicPrint((LogicNode *)node); break;
    case NilLitTag:
        nilLitPrint((NilLitNode *)node); break;
    case ULitTag:
        ulitPrint((ULitNode *)node); break;
    case FLitTag:
        flitPrint((FLitNode *)node); break;
    case TypeLitTag:
        typeLitPrint((FnCallNode *)node); break;
    case StringLitTag:
        slitPrint((SLitNode *)node); break;
    case FnSigTag:
        fnSigPrint((FnSigNode *)node); break;
    case RefTag: case VirtRefTag:
        refPrint((RefNode *)node); break;
    case ArrayRefTag:
        arrayRefPrint((RefNode *)node); break;
    case ArrayDerefTag:
        arrayDerefPrint((RefNode *)node); break;
    case PtrTag:
        ptrPrint((StarNode *)node); break;
    case StructTag:
        structPrint((StructNode *)node); break;
    case EnumTag:
        enumPrint((EnumNode *)node); break;
    case ArrayTag:
    case ArrayLitTag:
        arrayPrint((ArrayNode *)node); break;
    case IntNbrTag: case UintNbrTag: case FloatNbrTag:
        nbrTypePrint((NbrNode *)node); break;
    case PermTag:
        permPrint((PermNode *)node); break;
    case LifetimeTag:
        lifePrint((LifetimeNode *)node); break;
    case BorrowRegTag:
        inodeFprint("borrow"); break;
    case TTupleTag:
        ttuplePrint((TupleNode *)node); break;
    case AbsenceTag:
    case UnknownTag:
    case VoidTag:
        voidPrint((VoidTypeNode *)node); break;
    case NamedValTag:
        namedValPrint((NamedValNode *)node); break;
    case RefCountTag:
    {
        RefCountNode *anode = (RefCountNode *)node;
        inodeFprint("(refcount ");
        if (anode->counts == NULL)
            inodeFprint("%d ", (int)anode->amt);
        else {
            int16_t count = anode->amt;
            int16_t *countp = anode->counts;
            while (count--)
                inodeFprint("%d ", (int)*countp++);
        }
        inodePrintNode(anode->exp);
        inodeFprint(")");
        break;
    }
    case HollowTag:
    {
        HollowNode *hnode = (HollowNode *)node;
        inodeFprint("(hollow %s ", &hnode->var->namesym->namestr);
        if (hnode->exp)
            inodePrintNode(hnode->exp);
        inodeFprint(")");
        break;
    }
    case MacroDclTag:
        macroPrint((MacroDclNode *)node); break;
    case GenVarDclTag:
        gVarDclPrint((GenVarDclNode *)node); break;

    default:
        inodeFprint("**** UNKNOWN NODE ****");
    }
}

// Serialize the program's IR to dir+srcfn
void inodePrint(char *dir, char *srcfn, INode *pgmnode) {
    // Name the dump after the source compiled. The program node's own lexer is
    // the empty "init" pseudo-source the lexer starts on, so every compile used
    // to overwrite one init.ast.
    irfile = fopen(fileMakePath(dir, srcfn, "ast"), "wb");
    inodePrintNode(pgmnode);
    fclose(irfile);
}

// Dispatch a node walk for the current semantic analysis pass
// - pstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
void inodeNameRes(NameResState *pstate, INode **node) {
    // Every name use that reaches this walk resolves here. A member name never
    // does: fnCallNameRes leaves the call's member slot alone, because selecting
    // the member needs the receiver's type, and fnCallTypeCheck selects it.
    if (isNameUseNode(*node)) {
        nameUseNameRes(pstate, (NameUseNode **)node);
        return;
    }
    switch ((*node)->tag) {
    case ProgramTag:
        pgmNameRes(pstate, (ProgramNode*)*node); break;
    case ModuleTag:
        modNameRes(pstate, (ModuleNode*)*node); break;
    case ModTraitTag:
        modTraitNameRes(pstate, (ModTraitNode*)*node); break;
    case FnDclTag:
        fnDclNameRes(pstate, (FnDclNode *)*node); break;
    case VarDclTag:
        varDclNameRes(pstate, (VarDclNode *)*node); break;
    case ConstDclTag:
        constDclNameRes(pstate, (ConstDclNode *)*node); break;
    case FieldDclTag:
        fieldDclNameRes(pstate, (FieldDclNode *)*node); break;
    // A folded name's target is a member name, bound where the fold that made
    // the alias is expanded, exactly as a call's member slot is left alone here.
    // A type alias's target is a type expression, which is resolved.
    case AliasDclTag:
        aliasDclNameRes(pstate, (AliasDclNode *)*node); break;
    case TypeLitTag:
        typeLitNameRes(pstate, (FnCallNode *)*node); break;
    case ImportTag:
        importNameRes(pstate, (ImportNode *)*node); break;
    case BlockTag:
        blockNameRes(pstate, (BlockNode *)*node); break;
    case IfTag:
        ifNameRes(pstate, (IfNode *)*node); break;
    case BreakTag:
        breakNameRes(pstate, (BreakRetNode *)*node); break;
    case ContinueTag:
        continueNameRes(pstate, (BreakRetNode *)*node); break;
    case ReturnTag:
        returnNameRes(pstate, (BreakRetNode *)*node); break;
    case AssignTag:
        assignNameRes(pstate, (AssignNode *)*node); break;
    case SwapTag:
        swapNameRes(pstate, (SwapNode *)*node); break;
    case FnCallTag:
        fnCallNameRes(pstate, (FnCallNode **)node); break;
    case SizeofTag:
        sizeofNameRes(pstate, (SizeofNode *)*node); break;
    case CastTag:  case IsTag:
        castNameRes(pstate, (CastNode *)*node); break;
    case NotLogicTag:
        logicNotNameRes(pstate, (LogicNode *)*node); break;
    case OrLogicTag: case AndLogicTag:
        logicNameRes(pstate, (LogicNode *)*node); break;
    case NamedValTag:
        namedValNameRes(pstate, (NamedValNode *)*node); break;
    case NilLitTag:
    case ULitTag:
    case FLitTag:
    case StringLitTag:
        litNameRes(pstate, (IExpNode *)*node); break;

    case FnSigTag:
        fnSigNameRes(pstate, (FnSigNode *)*node); break;
    case RefTag:
    case VirtRefTag:
        refNameRes(pstate, (RefNode *)*node); break;
    case ArrayRefTag:
        arrayRefNameRes(pstate, (RefNode *)*node); break;
    case StarTag:
        ptrNameRes(pstate, (StarNode *)*node); break;
    case StructTag:
        structNameRes(pstate, (StructNode *)*node); break;
    case EnumTag:
        enumNameRes(pstate, (EnumNode *)*node); break;
    case ArrayTag:
        arrayNameRes(pstate, (ArrayNode *)*node); break;
    case TupleTag:
        ttupleNameRes(pstate, (TupleNode *)*node); break;
    case QuesTag:
        allocateQuesNameRes(pstate, (FnCallNode **)node); break;
    case BorrowRegTag:
        break;

    case MacroDclTag:
        macroNameRes(pstate, (MacroDclNode *)*node); break;
    case GenVarDclTag:
        gVarDclNameRes(pstate, (GenVarDclNode *)*node); break;

    case IntNbrTag: case UintNbrTag: case FloatNbrTag:
    case PermTag:
    case AbsenceTag:
    case UnknownTag:
    case VoidTag:
        break;
    // The owning module or type name resolves every concrete FnDclNode candidate,
    // so walking the overload node's candidates again would process them twice
    case FnOverloadDclTag:
        break;
    default:
        errorUnreachable(*node, "a node name resolution has no case for");
        break;
    }
}

// Dispatch a node walk for the type check pass
// - pstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
// - expectType is the type expected of an expression node (or unknownType/noCareType)
void inodeTypeCheck(TypeCheckState *pstate, INode **node, INode *expectType) {

    // A declaration is type checked once, however many places reach it. This
    // pass lowers and replaces nodes, so a second walk of one corrupts it; the
    // marks are a correctness requirement rather than an optimization.
    //
    // Type nodes are laid out the first time they are referenced, so that we
    // know everything we need about managing their values (infectious
    // constraints have to be inferred from the fields they compose). A type's
    // members are checked later, once no layout is in flight (structLayoutExit).
    //
    // A generic instantiation is a type but not a type declaration: this pass
    // replaces it with the instance it names, and the instance carries these
    // marks. Marking the instantiation itself would strand a mark on a node the
    // walk abandoned, and a second walk of the same node -- a match pattern and
    // the variable it declares share one -- would then read as a recursive type.
    if (((isTypeNode(*node) && (*node)->tag != FnCallTag)) || (*node)->tag == ModuleTag
        || (*node)->tag == ModTraitTag) {
        if ((*node)->flags & TypeChecked)
            return;
        // Under analysis and reached again. Its identity is established, which
        // is what a type question needs; only a *size* question has no answer
        // yet, and that is asked where a value is held rather than here. This
        // is what makes a linked list expressible: 'next &S' asks S for its
        // identity, and the reference answers the size on its own behalf.
        if ((*node)->flags & TypeChecking)
            return;
        (*node)->flags |= TypeChecking;
    }
    else if (inodeIsDcl(*node)) {
        if ((*node)->flags & TypeChecked)
            return;
        // Under analysis and reached again. Its own type was established before
        // it began anything that could refer back to it, so the caller reads
        // that from the node and there is nothing left to do here. Unreachable
        // until a use demands a declaration rather than reading it, and it is
        // what lets two functions call each other.
        if ((*node)->flags & TypeChecking)
            return;
        (*node)->flags |= TypeChecking;
    }

    // A type that holds values by value is a layout in flight until its check
    // ends, and no type's members are checked while one is (structLayoutExit).
    // A reference answers its own size and a signature has none, so neither
    // counts. See compiler/c/doc/phases/type-check.md, "Layout before members".
    int layout = (*node)->tag == StructTag || (*node)->tag == ArrayTag || (*node)->tag == TTupleTag;
    if (layout)
        structLayoutEnter();

    // A resolved name is checked as what its declaration is: a type, a value,
    // or a macro to expand. A member name never arrives here: the call it
    // belongs to selects the member against the receiver's type.
    if (isNameUseNode(*node)) {
        if (isTypeNode(*node))
            nameUseTypeCheckType(pstate, (NameUseNode **)node);
        else if (isExpNode(*node))
            nameUseTypeCheck(pstate, (NameUseNode **)node);
        else if (nameUseNames(*node, MacroDclTag))
            macroNameTypeCheck(pstate, (NameUseNode **)node);
        else {
            errorUnreachable(*node, "a node type check has no case for");
            return;
        }
    }
    else switch ((*node)->tag) {
    case ProgramTag:
        pgmTypeCheck(pstate, (ProgramNode *)*node); break;
    case ModuleTag:
        modTypeCheck(pstate, (ModuleNode*)*node); break;
    case ModTraitTag:
        modTraitTypeCheck(pstate, (ModTraitNode*)*node); break;
    case FnDclTag:
        fnDclTypeCheck(pstate, (FnDclNode *)*node); break;
    case VarDclTag:
        varDclTypeCheck(pstate, (VarDclNode *)*node); break;
    case ConstDclTag:
        constDclTypeCheck(pstate, (ConstDclNode *)*node); break;
    case FieldDclTag:
        fieldDclTypeCheck(pstate, (FieldDclNode *)*node); break;
    // A folded name has nothing of its own to check; what it stands for is
    // checked as itself, by whatever reaches it through the alias. A type alias
    // owns the type expression it stands for, so that is checked here.
    case AliasDclTag:
        aliasDclTypeCheck(pstate, (AliasDclNode *)*node); break;
    case ImportTag:
        importTypeCheck(pstate, (ImportNode *)*node); break;
    case ArrayLitTag:
        arrayLitTypeCheck(pstate, (ArrayNode *)*node); break;
    case BlockTag:
        blockTypeCheck(pstate, (BlockNode *)*node, expectType); break;
    case IfTag:
        ifTypeCheck(pstate, (IfNode *)*node, expectType); break;
    case BreakTag:
        breakTypeCheck(pstate, (BreakRetNode *)*node); break;
    case ContinueTag:
        continueTypeCheck(pstate, (BreakRetNode *)*node); break;
    case ReturnTag:
        returnTypeCheck(pstate, (BreakRetNode *)*node); break;
    case AssignTag:
        assignTypeCheck(pstate, (AssignNode *)*node); break;
    case SwapTag:
        swapTypeCheck(pstate, (SwapNode *)*node); break;
    // A call already lowered to a field access or an index by an earlier check
    // is complete: its type is set and its parts were checked. It is reached
    // again when a macro method's receiver, lowered before the expansion cloned
    // it into the body, is checked as part of that body.
    case FldAccessTag:
    case ArrIndexTag:
        break;
    case VTupleTag:
        vtupleTypeCheck(pstate, (TupleNode *)*node); break;
    case FnCallTag:
        fnCallTypeCheck(pstate, (FnCallNode **)node); break;
    case SizeofTag:
        sizeofTypeCheck(pstate, (SizeofNode *)*node); break;
    case CastTag:
        castTypeCheck(pstate, (CastNode *)*node); break;
    case DerefTag:
        derefTypeCheck(pstate, (StarNode *)*node); break;
    case ArrayBorrowTag:
    case BorrowTag:
        borrowTypeCheck(pstate, (RefNode **)node); break;
    case AllocateTag:
    case ArrayAllocTag:
        allocateTypeCheck(pstate, (RefNode **)node); break;
    case NotLogicTag:
        logicNotTypeCheck(pstate, (LogicNode *)*node); break;
    case OrLogicTag: case AndLogicTag:
        logicTypeCheck(pstate, (LogicNode *)*node); break;
    case IsTag:
        castIsTypeCheck(pstate, (CastNode *)*node); break;
    case NamedValTag:
        namedValTypeCheck(pstate, (NamedValNode *)*node); break;
    case NilLitTag:
    case ULitTag:
    case FLitTag:
        litTypeCheck(pstate, node, expectType); break;

    case FnSigTag:
        fnSigTypeCheck(pstate, (FnSigNode *)*node); break;
    case RefTag:
        refTypeCheck(pstate, (RefNode *)*node); break;
    case VirtRefTag:
        refvirtTypeCheck(pstate, (RefNode *)*node); break;
    case ArrayRefTag:
        arrayRefTypeCheck(pstate, (RefNode *)*node); break;
    case PtrTag:
        ptrTypeCheck(pstate, (StarNode *)*node); break;
    case StructTag:
        structTypeCheck(pstate, (StructNode *)*node); break;
    case EnumTag:
        enumTypeCheck(pstate, (EnumNode *)*node); break;
    case ArrayTag:
        arrayTypeCheck(pstate, (ArrayNode *)*node); break;
    case TTupleTag:
        ttupleTypeCheck(pstate, (TupleNode *)*node); break;
    case BorrowRegTag:
    case PermTag:
        break;

    case MacroDclTag:
        macroTypeCheck(pstate, (MacroDclNode *)*node); break;
    case GenVarDclTag:
        gVarDclTypeCheck(pstate, (GenVarDclNode *)*node); break;

    case StringLitTag:
        slitTypeCheck(pstate, (SLitNode*)*node); break;

    case IntNbrTag: case UintNbrTag: case FloatNbrTag:
    case AbsenceTag:
    case UnknownTag:
    case VoidTag:
        break;
    // The owning module or type type checks every concrete FnDclNode candidate,
    // so only the set's candidate signatures are compared here
    case FnOverloadDclTag:
        fnOverloadDclTypeCheck(pstate, (FnOverloadDclNode *)*node); break;
    default:
        errorUnreachable(*node, "a node type check has no case for");
        return;
    }

    // Confirm the declaration has been type checked. *node may have been replaced by
    // now -- an instantiation leaves behind the instance it named, which is a
    // declaration and does take the mark.
    if (((isTypeNode(*node) && (*node)->tag != FnCallTag)) || (*node)->tag == ModuleTag
            || (*node)->tag == ModTraitTag || inodeIsDcl(*node)) {
        (*node)->flags |= TypeChecked;
    }

    if (layout)
        structLayoutExit();
}


// Perform a node walk for the current semantic analysis pass (w/ no type expected)
// - pstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
void inodeTypeCheckAny(TypeCheckState *pstate, INode **pgm) {
    inodeTypeCheck(pstate, pgm, unknownType);
}

// Obtain name from a named node
Name *inodeGetName(INode *node) {
    switch (node->tag) {
    // Non-type Declarations
    case FnDclTag:
        return ((FnDclNode*)node)->namesym;
    case FnOverloadDclTag:
        return ((FnOverloadDclNode*)node)->namesym;
    case VarDclTag:
        return ((VarDclNode*)node)->namesym;
    case FieldDclTag:
        return ((FieldDclNode*)node)->namesym;
    case ConstDclTag:
        return ((ConstDclNode*)node)->namesym;
    case AliasDclTag:
        return ((AliasDclNode*)node)->namesym;
    case MacroDclTag:
        return ((MacroDclNode*)node)->namesym;
    case GenVarDclTag:
        return ((GenVarDclNode *)node)->namesym;

    // Type declarations
    case LifetimeTag:
        return ((LifetimeNode*)node)->namesym;
    case StructTag:
        return ((StructNode*)node)->namesym;
    case ModuleTag:
        return ((ModuleNode*)node)->namesym;
    case ModTraitTag:
        return ((ModTraitNode*)node)->namesym;
    case IntNbrTag:
        return ((NbrNode*)node)->namesym;
    case UintNbrTag:
        return ((NbrNode*)node)->namesym;
    case FloatNbrTag:
        return ((NbrNode*)node)->namesym;
    case PermTag:
        return ((PermNode*)node)->namesym;
    case EnumTag:
        return ((EnumNode*)node)->namesym;
    default:
        errorUnreachable(node, "a request for the name of a node that has none");
        return NULL;
    }
}

// Obtain the declaration facts of a node that declares a symbol the object file
// can carry, or NULL if it declares none. This is the only place that knows
// which node kinds those are.
DclInfo *inodeGetDclInfo(INode *node) {
    switch (node->tag) {
    case FnDclTag:
        return &((FnDclNode*)node)->dclinfo;
    case VarDclTag:
        return &((VarDclNode*)node)->dclinfo;
    case StructTag:
        return &((StructNode*)node)->dclinfo;
    case ModuleTag:
        return &((ModuleNode*)node)->dclinfo;
    case ModTraitTag:
        return &((ModTraitNode*)node)->dclinfo;
    default:
        return NULL;
    }
}

// Obtain the module or type node a declaration lives in, or NULL if it has none
INode *inodeGetOwner(INode *node) {
    DclInfo *info = inodeGetDclInfo(node);
    return info ? info->owner : NULL;
}

// Is this a declaration that carries its own analysis marks?
// Type declarations and modules are handled separately: they are reached as
// types rather than as declarations, and a type's mark means laid out.
int inodeIsDcl(INode *node) {
    switch (node->tag) {
    case FnDclTag:
    case VarDclTag:
    case FieldDclTag:
    case ConstDclTag:
        return 1;
    default:
        return 0;
    }
}

// Determine whether a named node is private: not declared 'pub'. A declaration
// that carries DclInfo answers from its DclPrivate bit, written from the flag
// once when it joined its namespace. A node that carries none (a field, const,
// macro, typedef, overload name or generic parameter) answers from the flag.
int inodeIsPrivate(INode *node) {
    DclInfo *dclinfo = inodeGetDclInfo(node);
    if (dclinfo)
        return (dclinfo->facts & DclPrivate) != 0;
    return (node->flags & FlagPub) == 0;
}

// Determine whether a declaration is a member reached through a receiver: a
// field, a method, a macro method, or an overload set whose candidates are
// methods. A static function or macro declared in a type is not one.
int inodeIsMember(INode *node) {
    // An alias answers for what it stands for; one not yet bound answers from
    // its own flag, which says what it was made to stand for
    if (node->tag == AliasDclTag) {
        INode *target = aliasDclResolve(node);
        if (target == NULL)
            return (node->flags & FlagMethFld) != 0;
        node = target;
    }
    switch (node->tag) {
    case FieldDclTag:
    case FnDclTag:
    case MacroDclTag:
        return (node->flags & FlagMethFld) != 0;
    case FnOverloadDclTag: {
        Nodes *overloads = ((FnOverloadDclNode*)node)->overloads;
        return overloads->used > 0 && (nodesGet(overloads, 0)->flags & FlagMethFld) != 0;
    }
    default:
        return 0;
    }
}

// Determine whether an earlier diagnostic already marked this node as bad.
// A check that would complain about such a node has nothing new to report.
int inodeIsError(INode *node) {
    return isExpNode(node) && ((IExpNode*)node)->vtype == errorType;
}

// What a tag is, since its number says nothing: the group it belongs to,
// whether it declares a named item, and whether it is a type that supports
// methods.
typedef struct NodeTagFacts {
    NodeGroup group;
    int named;      // Declares a named item (a name use names one, and is not one)
    int method;     // A type that supports methods
} NodeTagFacts;

// Every tag in enum NodeTags needs a row here. A tag with none is a statement
// that declares nothing and carries no methods, and nothing says so.
static NodeTagFacts nodeTagFacts[NodeTagCount] = {
    [ProgramTag] = {StmtGroup, 0, 0},
    [KeywordTag] = {StmtGroup, 0, 0},

    [IntrinsicTag] = {StmtGroup, 0, 0},
    [ReturnTag] = {StmtGroup, 0, 0},
    [BlockRetTag] = {StmtGroup, 0, 0},
    [BreakTag] = {StmtGroup, 0, 0},
    [ContinueTag] = {StmtGroup, 0, 0},
    [SwapTag] = {StmtGroup, 0, 0},
    [ImportTag] = {StmtGroup, 0, 0},
    [ModUseTag] = {StmtGroup, 0, 0},

    // A name use is in no group of its own: it answers for what it names
    [NameUseTag] = {StmtGroup, 0, 0},
    [TupleTag] = {StmtGroup, 0, 0},
    [StarTag] = {StmtGroup, 0, 0},

    [ModuleTag] = {StmtGroup, 1, 0},
    [ModTraitTag] = {StmtGroup, 1, 0},
    [FnDclTag] = {StmtGroup, 1, 0},
    [FnOverloadDclTag] = {StmtGroup, 1, 0},
    [VarDclTag] = {StmtGroup, 1, 0},
    [FieldDclTag] = {StmtGroup, 1, 0},
    [ConstDclTag] = {StmtGroup, 1, 0},
    [AliasDclTag] = {StmtGroup, 1, 0},

    [NilLitTag] = {ExpGroup, 0, 0},
    [ULitTag] = {ExpGroup, 0, 0},
    [FLitTag] = {ExpGroup, 0, 0},
    [StringLitTag] = {ExpGroup, 0, 0},
    [ArrayLitTag] = {ExpGroup, 0, 0},
    [TypeLitTag] = {ExpGroup, 0, 0},
    [VTupleTag] = {ExpGroup, 0, 0},
    [AssignTag] = {ExpGroup, 0, 0},
    [FnCallTag] = {ExpGroup, 0, 0},
    [ArrIndexTag] = {ExpGroup, 0, 0},
    [FldAccessTag] = {ExpGroup, 0, 0},
    [SizeofTag] = {ExpGroup, 0, 0},
    [CastTag] = {ExpGroup, 0, 0},
    [BorrowTag] = {ExpGroup, 0, 0},
    [ArrayBorrowTag] = {ExpGroup, 0, 0},
    [AllocateTag] = {ExpGroup, 0, 0},
    [ArrayAllocTag] = {ExpGroup, 0, 0},
    [DerefTag] = {ExpGroup, 0, 0},
    [NotLogicTag] = {ExpGroup, 0, 0},
    [OrLogicTag] = {ExpGroup, 0, 0},
    [AndLogicTag] = {ExpGroup, 0, 0},
    [IsTag] = {ExpGroup, 0, 0},
    [BlockTag] = {ExpGroup, 0, 0},
    [IfTag] = {ExpGroup, 0, 0},
    [RefCountTag] = {ExpGroup, 0, 0},
    [HollowTag] = {ExpGroup, 0, 0},
    [NamedValTag] = {ExpGroup, 0, 0},
    [AbsenceTag] = {ExpGroup, 0, 0},

    [FnSigTag] = {TypeGroup, 0, 0},
    [ArrayTag] = {TypeGroup, 0, 0},
    [RefTag] = {TypeGroup, 0, 0},
    [ArrayRefTag] = {TypeGroup, 0, 0},
    [VirtRefTag] = {TypeGroup, 0, 0},
    [ArrayDerefTag] = {TypeGroup, 0, 0},
    [PtrTag] = {TypeGroup, 0, 0},
    [TTupleTag] = {TypeGroup, 0, 0},
    [VoidTag] = {TypeGroup, 0, 0},
    [QuesTag] = {TypeGroup, 0, 0},
    [BorrowRegTag] = {TypeGroup, 0, 0},
    [UnknownTag] = {TypeGroup, 0, 0},

    [EnumTag] = {TypeGroup, 1, 0},
    [LifetimeTag] = {TypeGroup, 1, 0},

    [IntNbrTag] = {TypeGroup, 1, 1},
    [UintNbrTag] = {TypeGroup, 1, 1},
    [FloatNbrTag] = {TypeGroup, 1, 1},
    [StructTag] = {TypeGroup, 1, 1},
    [PermTag] = {TypeGroup, 1, 1},

    [MacroDclTag] = {MetaGroup, 1, 0},
    [GenVarDclTag] = {MetaGroup, 1, 0},
};

// The group a node belongs to: StmtGroup, ExpGroup, TypeGroup or MetaGroup.
// For every node but a name use the tag is the node's characteristic, so the
// tag table is the answer. A name use stands for whatever it names, so it is
// asked of the declaration at the end of its chain of names rather than of the
// use itself: that is what lets a name reached through an alias answer.
static NodeGroup inodeGroup(INode *node) {
    if (isNameUseNode(node))
        return nameUseGroup((NameUseNode*)node);
    return nodeTagFacts[node->tag].group;
}

int inodeIsExp(INode *node) {
    return inodeGroup(node) == ExpGroup;
}

// An instantiation of a generic type, 'Box[i64]', is a call node until type
// check replaces it with the instance it names, and is a type all the while
int inodeIsType(INode *node) {
    return inodeGroup(node) == TypeGroup || itypeIsGenericType(node);
}

int inodeIsMeta(INode *node) {
    return inodeGroup(node) == MetaGroup;
}

// In a generic's template a use of a type parameter is not a type -- it is a
// meta node, the same declaration a macro's parameter is, and those may be
// given values -- so name resolution's type-or-value votes, which ask
// isTypeNode of an operand, cast a provisional answer on it that the
// instance's clone takes again. Is this such an operand: a use of a generic
// parameter, or a form whose own vote was cast on one? A vote whose losing
// side is an error ('?', a tuple mixing types and values) asks this before
// refusing, so as not to refuse what substitution may yet make a type.
int inodeIsProvisionalType(INode *node) {
    switch (node->tag) {
    case NameUseTag:
        return nameUseNames(node, GenVarDclTag);
    case BorrowTag:
    case AllocateTag:
    case ArrayBorrowTag:
    case ArrayAllocTag:
        return inodeIsProvisionalType(((RefNode*)node)->vtexp);
    case DerefTag:
        return inodeIsProvisionalType(((StarNode*)node)->vtexp);
    case ArrayLitTag: {
        Nodes *elems = ((ArrayNode*)node)->elems;
        return elems->used > 0 && inodeIsProvisionalType(nodesGet(elems, 0));
    }
    case VTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)node)->elems, cnt, nodesp))
            if (!isTypeNode(*nodesp) && !inodeIsProvisionalType(*nodesp))
                return 0;
        return 1;
    }
    default:
        return 0;
    }
}

// Does this node declare a named item? A name use is not one, whatever it names.
int inodeIsNamed(INode *node) {
    return nodeTagFacts[node->tag].named;
}

// Is this a type that supports methods? An unlowered generic instantiation is a
// type, but it is a call node until type check replaces it with the instance
// that holds the methods, so it is not one of these yet.
int inodeIsMethodType(INode *node) {
    return isTypeNode(node) && nodeTagFacts[node->tag].method;
}