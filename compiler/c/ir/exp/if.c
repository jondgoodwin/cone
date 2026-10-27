/** Handling for if nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <stdlib.h>

// Create a new If node
IfNode *newIfNode() {
    IfNode *ifnode;
    newNode(ifnode, IfNode, IfTag);
    ifnode->condblk = newNodes(4);
    ifnode->vtype = unknownType;
    return ifnode;
}

// Clone if
INode *cloneIfNode(CloneState *cstate, IfNode *node) {
    IfNode *newnode;
    newnode = memAllocBlk(sizeof(IfNode));
    memcpy(newnode, node, sizeof(IfNode));
    newnode->condblk = cloneNodes(cstate, node->condblk);
    return (INode *)newnode;
}

// Serialize an if statement
void ifPrint(IfNode *ifnode) {
    INode **nodesp;
    uint32_t cnt;
    int firstflag = 1;

    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        if (firstflag) {
            inodeFprint("if ");
            firstflag = 0;
            inodePrintNode(*nodesp);
        }
        else {
            inodePrintIndent();
            if (*nodesp == elseCond)
                inodeFprint("else");
            else {
                inodeFprint("else if ");
                inodePrintNode(*nodesp);
            }
        }
        inodePrintNL();
        inodePrintNode(*(++nodesp));
        cnt--;
    }
}

// Recursively strip 'returns' out of all block-ends in 'if' (see returnPass)
void ifRemoveReturns(IfNode *ifnode) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        INode **laststmt;
        cnt--; nodesp++;
        laststmt = &nodesLast(((BlockNode*)*nodesp)->stmts);
        if ((*laststmt)->tag == ReturnTag)
            *laststmt = ((BreakRetNode*)*laststmt)->exp;
        if ((*laststmt)->tag == IfTag)
            ifRemoveReturns((IfNode*)*laststmt);
    }
}

// Does a type-checked block end by jumping away, so give its 'if' no value?
int ifBlockJumps(BlockNode *blk) {
    if (blk->tag != BlockTag || blk->stmts == NULL || blk->stmts->used == 0)
        return 0;
    uint16_t last = nodesLast(blk->stmts)->tag;
    return last == ReturnTag || last == BreakTag || last == ContinueTag;
}

// Where a path through a type-checked block goes on past its end, or NULL when every
// path leaves: the block ends in a jump (ifBlockJumps), or in an 'if' or 'match' with
// an 'else' each of whose blocks leaves, or in a plain block that leaves, written or
// wrapped around a 'match' or a bound 'if' (the block holding its matched value). The 'else' of
// 'let Some(n) = x else {...};' is held to this, as Rust holds it to diverge. A loop ends
// when its condition fails or a 'break' leaves it, so it is a path that goes on. '*why'
// says what the spot returned lacks. A match left without an 'else' has had its missing
// cases reported (ifMatchCover), so it is not reported again.
static INode *ifBlockFallsOut(BlockNode *blk, char **why) {
    if (ifBlockJumps(blk))
        return NULL;
    // The statement the type check adds after a last statement that gives no value
    uint32_t used = blk->stmts == NULL ? 0 : blk->stmts->used;
    if (used > 0 && nodesLast(blk->stmts)->tag == BlockRetTag)
        --used;
    if (used == 0) {
        *why = "this block is empty";
        return (INode*)blk;
    }
    INode *last = nodesGet(blk->stmts, used - 1);
    if (last->tag == BlockTag && !(((BlockNode*)last)->flags & FlagLoop))
        return ifBlockFallsOut((BlockNode*)last, why);
    if (last->tag == IfTag) {
        IfNode *ifnode = (IfNode*)last;
        int hasElse = 0;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(ifnode->condblk, cnt, nodesp)) {
            if (*nodesp == elseCond)
                hasElse = 1;
            cnt--; nodesp++;
            if ((*nodesp)->tag != BlockTag)
                continue;
            INode *out = ifBlockFallsOut((BlockNode*)*nodesp, why);
            if (out != NULL)
                return out;
        }
        if (hasElse || (ifnode->flags & (FlagIfMatch | FlagIfMatchTail)))
            return NULL;
        *why = "this 'if' has no 'else', so the path where its conditions are false goes on past it";
        return last;
    }
    if (last->tag == BlockTag) {
        *why = "this loop ends when its condition fails or a 'break' leaves it, and the path goes on past it";
        return last;
    }
    *why = "the path goes on past this, its last statement";
    return last;
}

// Does every path through a type-checked 'if' jump away?
int ifAllPathsJump(IfNode *ifnode) {
    int hasElse = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        if (*nodesp == elseCond)
            hasElse = 1;
        cnt--; nodesp++;
        if (!ifBlockJumps((BlockNode *)*nodesp))
            return 0;
    }
    return hasElse;
}

// if node name resolution
void ifNameRes(NameResState *pstate, IfNode *ifnode) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        inodeNameRes(pstate, nodesp);
    }
}

// Are these two checked expressions reads of the same place: one variable, or the
// same field of the same place, or a dereference of one, or an element at the same
// name or literal? (A match on a place reads the place as written in each of its
// tests, parseMatchSubject.)
static int ifSamePlace(INode *a, INode *b) {
    if (a == b)
        return 1;
    if (a == NULL || b == NULL || a->tag != b->tag)
        return 0;
    if (isNameUseNode(a))
        return ((NameUseNode*)a)->dclnode != NULL && ((NameUseNode*)a)->dclnode == ((NameUseNode*)b)->dclnode;
    switch (a->tag) {
    case DerefTag:
        return ifSamePlace(((StarNode*)a)->vtexp, ((StarNode*)b)->vtexp);
    case FldAccessTag:
    {
        INode *fa = ((FnCallNode*)a)->methfld;
        INode *fb = ((FnCallNode*)b)->methfld;
        if (fa == NULL || fb == NULL || fa->tag != fb->tag)
            return 0;
        if (isNameUseNode(fa) ? (((NameUseNode*)fa)->dclnode == NULL || ((NameUseNode*)fa)->dclnode != ((NameUseNode*)fb)->dclnode)
            : (fa->tag != ULitTag || ((ULitNode*)fa)->uintlit != ((ULitNode*)fb)->uintlit))
            return 0;
        return ifSamePlace(((FnCallNode*)a)->objfn, ((FnCallNode*)b)->objfn);
    }
    case ArrIndexTag:
    {
        Nodes *ia = ((FnCallNode*)a)->args;
        Nodes *ib = ((FnCallNode*)b)->args;
        if (ia == NULL || ib == NULL || ia->used != 1 || ib->used != 1)
            return 0;
        INode *xa = nodesGet(ia, 0);
        INode *xb = nodesGet(ib, 0);
        if (xa->tag != xb->tag)
            return 0;
        if (xa->tag == ULitTag ? ((ULitNode*)xa)->uintlit != ((ULitNode*)xb)->uintlit : !ifSamePlace(xa, xb))
            return 0;
        return ifSamePlace(((FnCallNode*)a)->objfn, ((FnCallNode*)b)->objfn);
    }
    default:
        return 0;
    }
}

// Do two 'is' conditions test the same value? A match on a value it holds writes
// one scrutinee node into every condition, so a match written in source shares
// it. A clone of the match -- a generic instance's, or a macro expansion's -- copies
// that node once per condition, so there two uses of the one variable the
// desugaring declared are what the same scrutinee looks like. A match on a place
// reads the place in each condition, which are reads of the same place.
static int ifSameScrutinee(INode *a, INode *b) {
    return ifSamePlace(a, b);
}

// Does this 'is' test name this variant: the variant itself, or a trait pattern's
// variants that comply?
static int ifTestsVariant(CastNode *isnode, INode *variant) {
    if (isnode->variants == NULL)
        return itypeGetDerefTypeDcl(isnode->typ) == variant;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(isnode->variants, cnt, nodesp)) {
        if (*nodesp == variant)
            return 1;
    }
    return 0;
}

// Detect when all closed variants are matched, and turn last match into 'else'
// We know the condition is an 'is' node
void ifExhaustCheck(IfNode *ifnode, CastNode *condition) {
    StructNode *strnode = (StructNode*)iexpGetDerefTypeDcl(condition->exp);

    // Exhaustiveness check only on closed variants against a base trait
    if (strnode->tag != StructTag || !(strnode->flags & TraitType) ||
        strnode->basetrait != NULL || 0 == (strnode->flags & (HasTagField | SameSize)))
        return;

    // We need to detect that all possible variants are accounted for
    uint32_t lowestcnt = ifnode->condblk->used;
    INode **varnodesp;
    uint32_t varcnt;
    for (nodesFor(strnode->derived, varcnt, varnodesp)) {
        // Look for 'is' check on same value against *varnodesp
        int found = 0;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(ifnode->condblk, cnt, nodesp)) {
            if ((*nodesp)->tag == IsTag) {
                CastNode *isnode = (CastNode*)*nodesp;
                // A later test not yet type checked may name a variant it has not
                // been bound to yet. The check runs again as each test is checked,
                // and the last one sees every pattern bound.
                // A pattern that named nothing to narrow to has been reported,
                // and was left unchecked, so it may not be a type: it matches no
                // variant. A value alone not yet checked may still become '=='.
                if ((isnode->flags & FlagMatchValue) || castPatternPending(isnode->typ)
                    || !isTypeNode(isnode->typ))
                    ;
                else if (ifSameScrutinee(isnode->exp, condition->exp) && ifTestsVariant(isnode, *varnodesp)) {
                    found = 1;
                    if (cnt < lowestcnt)
                        lowestcnt = cnt;
                    break;
                }
            }
            ++nodesp; --cnt;
        }
        if (found == 0)
            return;
    }
    // Turn last 'is' condition into 'else'
    if (lowestcnt == 2)
        nodesGet(ifnode->condblk, ifnode->condblk->used - 2) = elseCond;
}

// ---- What the cases of a match cover ----
//
// A match covers what simple, clear logic can show it covers, and the writer says the
// rest with 'else' (possibly that it cannot happen). The values that can be counted are
// an enum's variants and a bool's two; a match on several values, 'match a, b', counts
// the combinations of its columns. A number or character written out is counted only to
// find one named twice: a column of them counts the values the cases name, and one more
// for every other value, which only '_' or 'else' takes, so they never cover a match.
// Every other test (a comparison, a range, text equal to a value) matches some values
// and says which of them only by running.
//
// Each case is a row of cells, one for each value matched: '_' is a wild cell, a variant
// (or '|' of variants, or 'true' / 'false', or a number or character written out) the
// set of values it names, and anything else an "other" cell. A row with an "other" cell is not counted toward covering, or
// toward making a later case unreachable. A guarded case is not counted toward covering.

#define MatchCellWild  0    // takes any value
#define MatchCellSet   1    // takes the values it names: has[k] for the column's k-th value
#define MatchCellOther 2    // a test this logic cannot count

typedef struct {
    int kind;
    uint8_t *has;           // MatchCellSet: one byte for each value of the column's type
} MatchCell;

typedef struct {
    StructNode *enumdcl;    // the closed enum whose variants the column counts; NULL for a bool or no type yet
    int isbool;             // the column counts true and false
    uint32_t nvalues;       // how many values the column counts; 0 for a type it cannot count
    int isint;              // the column counts the numbers the cases name, and one more for the rest
    int intsigned;          // ... and they are signed
    uint64_t *ints;         // ... in the order they are first named
    uint32_t nints;
} MatchCol;

typedef struct {
    MatchCell *cells;       // one for each column
    int pure;               // no cell is "other": the row is counted
    int guarded;            // the case has an 'if' guard
} MatchRow;

// An enum whose variants are all known, and so can be counted
static StructNode *matchClosedEnum(INode *dcl) {
    if (dcl == NULL || dcl->tag != StructTag)
        return NULL;
    StructNode *strnode = (StructNode*)dcl;
    if (!(strnode->flags & TraitType) || strnode->basetrait != NULL || 0 == (strnode->flags & (HasTagField | SameSize))
        || strnode->derived == NULL)
        return NULL;
    return strnode;
}

// Is this the test of a number or character written out, 'matched == 5'? That is what
// 'case 5' and 'case ==5' make. Gives the value, and whether the matched type is signed.
static int matchIntTest(INode *cond, uint64_t *value, int *isSigned) {
    if (cond->tag != FnCallTag)
        return 0;
    FnCallNode *call = (FnCallNode*)cond;
    if (!(call->flags & FlagOperator) || call->methfld != NULL || call->objfn == NULL || !isNameUseNode(call->objfn)
        || call->args == NULL || call->args->used != 2)
        return 0;
    INode *callee = ((NameUseNode*)call->objfn)->dclnode;
    if (callee == NULL || callee->tag != FnDclTag || ((FnDclNode*)callee)->namesym != eqName)
        return 0;
    INode *matched = nodesGet(call->args, 0);
    INode *lit = nodesGet(call->args, 1);
    if (lit->tag != ULitTag || (lit->flags & (FlagMatchAny | FlagUnkType)) || !isExpNode(matched))
        return 0;
    INode *typedcl = iexpGetDerefTypeDcl(matched);
    if (typedcl == NULL || typedcl == (INode*)boolType || iexpGetDerefTypeDcl(lit) != typedcl
        || (typedcl->tag != IntNbrTag && typedcl->tag != UintNbrTag && typedcl != (INode*)charType))
        return 0;
    *value = ((ULitNode*)lit)->uintlit;
    *isSigned = typedcl->tag == IntNbrTag;
    return 1;
}

// Note the numbers a case names in a column (matchIntCell counts them)
static void matchCollectInts(INode *cond, MatchCol *col) {
    if (cond->tag == OrLogicTag) {
        matchCollectInts(((LogicNode*)cond)->lexp, col);
        matchCollectInts(((LogicNode*)cond)->rexp, col);
        return;
    }
    uint64_t value;
    int isSigned;
    if (!matchIntTest(cond, &value, &isSigned))
        return;
    for (uint32_t k = 0; k < col->nints; ++k)
        if (col->ints[k] == value)
            return;
    col->ints = realloc(col->ints, sizeof(uint64_t) * (col->nints + 1));
    col->ints[col->nints++] = value;
    col->isint = 1;
    col->intsigned = isSigned;
    col->nvalues = col->nints + 1;
}

// A set of one number of a column of numbers
static MatchCell matchIntCell(MatchCol *col, uint64_t value) {
    MatchCell cell;
    cell.kind = MatchCellOther;
    cell.has = NULL;
    if (!col->isint)
        return cell;
    uint32_t k = 0;
    while (col->ints[k] != value)
        ++k;
    cell.kind = MatchCellSet;
    cell.has = memAllocBlk(col->nvalues);
    memset(cell.has, 0, col->nvalues);
    cell.has[k] = 1;
    return cell;
}

// A set of one value of a column's type, which gives the column that type if it has none yet.
// A value of another type than the column's is a test that cannot be counted.
static MatchCell matchValueCell(MatchCol *col, StructNode *enumdcl, int isbool, uint32_t nvalues, uint32_t k) {
    MatchCell cell;
    cell.kind = MatchCellOther;
    cell.has = NULL;
    if (col->nvalues == 0) {
        col->enumdcl = enumdcl;
        col->isbool = isbool;
        col->nvalues = nvalues;
    }
    else if (col->enumdcl != enumdcl || col->isbool != isbool)
        return cell;
    cell.kind = MatchCellSet;
    cell.has = memAllocBlk(col->nvalues);
    memset(cell.has, 0, col->nvalues);
    cell.has[k] = 1;
    return cell;
}

// The cell a pattern, as type checked, makes. *unsure is set when the pattern was left
// unchecked by an error, so that what the match covers is not to be judged.
static MatchCell matchClassify(INode *cond, MatchCol *col, int *unsure) {
    MatchCell cell;
    cell.kind = MatchCellOther;
    cell.has = NULL;
    switch (cond->tag) {
    case ULitTag:
        if (cond->flags & FlagMatchAny)
            cell.kind = MatchCellWild;
        break;
    case OrLogicTag:
    {
        MatchCell left = matchClassify(((LogicNode*)cond)->lexp, col, unsure);
        MatchCell right = matchClassify(((LogicNode*)cond)->rexp, col, unsure);
        if (left.kind == MatchCellWild || right.kind == MatchCellWild)
            cell.kind = MatchCellWild;
        else if (left.kind == MatchCellSet && right.kind == MatchCellSet) {
            for (uint32_t k = 0; k < col->nvalues; ++k)
                left.has[k] |= right.has[k];
            cell = left;
        }
        break;
    }
    case IsTag:
    {
        CastNode *isnode = (CastNode*)cond;
        if ((isnode->flags & FlagMatchValue) || castPatternPending(isnode->typ) || !isTypeNode(isnode->typ)) {
            *unsure = 1;
            break;
        }
        StructNode *enumdcl = matchClosedEnum(iexpGetDerefTypeDcl(isnode->exp));
        if (enumdcl == NULL)
            break;
        // A variant, or the variants a trait pattern names [topic 7]: tag tests, so they count
        // as the variants they test
        uint8_t *tested = memAllocBlk(enumdcl->derived->used);
        memset(tested, 0, enumdcl->derived->used);
        if (castIsVariants(isnode, enumdcl, tested) == 0)
            break;
        uint32_t k = 0;
        while (!tested[k])
            ++k;
        cell = matchValueCell(col, enumdcl, 0, enumdcl->derived->used, k);
        if (cell.kind == MatchCellSet)
            for (++k; k < enumdcl->derived->used; ++k)
                cell.has[k] = tested[k];
        return cell;
    }
    case FnCallTag:
    {
        // 'v == true' and 'v == false' on a bool, which 'case true' and 'case ==true' make
        FnCallNode *call = (FnCallNode*)cond;
        // Type check has made the comparison a call of the '==' it found, the matched
        // value its first argument
        if (!(call->flags & FlagOperator) || call->methfld != NULL || call->objfn == NULL || !isNameUseNode(call->objfn)
            || call->args == NULL || call->args->used != 2)
            break;
        INode *callee = ((NameUseNode*)call->objfn)->dclnode;
        if (callee == NULL || callee->tag != FnDclTag || ((FnDclNode*)callee)->namesym != eqName)
            break;
        INode *matched = nodesGet(call->args, 0);
        INode *value = nodesGet(call->args, 1);
        if (value->tag != ULitTag || (value->flags & FlagMatchAny) || ((ULitNode*)value)->uintlit > 1
            || iexpGetDerefTypeDcl(value) != (INode*)boolType || iexpGetDerefTypeDcl(matched) != (INode*)boolType) {
            // A number or character written out
            uint64_t number;
            int isSigned;
            if (matchIntTest(cond, &number, &isSigned))
                return matchIntCell(col, number);
            break;
        }
        return matchValueCell(col, NULL, 1, 2, (uint32_t)((ULitNode*)value)->uintlit);
    }
    default:
        break;
    }
    return cell;
}

// The node a case is reported at: its first pattern
static INode *matchCaseStart(INode *cond) {
    while (cond->tag == OrLogicTag || (cond->tag == AndLogicTag && (cond->flags & (FlagMatchRow | FlagMatchGuard))))
        cond = ((LogicNode*)cond)->lexp;
    return cond;
}

// The columns of a case's condition, left to right: a row joins them with '&&'
static int matchFlattenRow(INode *cond, INode **out, int max) {
    if (cond->tag == AndLogicTag && (cond->flags & FlagMatchRow)) {
        int n = matchFlattenRow(((LogicNode*)cond)->lexp, out, max);
        if (n < max)
            out[n++] = ((LogicNode*)cond)->rexp;
        return n;
    }
    out[0] = cond;
    return 1;
}

// Keep the rows (listed by index) whose cell in column c takes value k, or any value (k < 0)
static int matchFilter(MatchRow **rows, int *idx, int n, int c, int k, int *out) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        MatchCell *cell = &rows[idx[i]]->cells[c];
        if (cell->kind == MatchCellWild || (k >= 0 && cell->has[k]))
            out[m++] = idx[i];
    }
    return m;
}

// Do these counted rows cover every combination of the columns from c on? When they do
// not, witness holds the values of a combination they leave out (-1: any).
static int matchCovers(MatchRow **rows, int *idx, int n, int c, int ncols, MatchCol *cols, int *witness) {
    if (n == 0) {
        for (int j = c; j < ncols; ++j)
            witness[j] = -1;
        return 0;
    }
    while (c < ncols) {
        int allwild = 1;
        for (int i = 0; i < n && allwild; ++i)
            allwild = rows[idx[i]]->cells[c].kind == MatchCellWild;
        if (!allwild)
            break;
        witness[c++] = -1;
    }
    if (c == ncols)
        return 1;
    int *next = malloc(sizeof(int) * n);
    int covered = 1;
    for (uint32_t k = 0; k < cols[c].nvalues && covered; ++k) {
        int m = matchFilter(rows, idx, n, c, (int)k, next);
        witness[c] = (int)k;
        covered = matchCovers(rows, next, m, c + 1, ncols, cols, witness);
    }
    free(next);
    return covered;
}

// Is every combination the pure row r takes taken by the counted rows before it?
static int matchUseless(MatchRow **rows, int *idx, int n, MatchRow *r, int c, int ncols, MatchCol *cols) {
    if (n == 0)
        return 0;
    while (c < ncols) {
        int allwild = r->cells[c].kind == MatchCellWild;
        for (int i = 0; i < n && allwild; ++i)
            allwild = rows[idx[i]]->cells[c].kind == MatchCellWild;
        if (!allwild)
            break;
        ++c;
    }
    if (c == ncols)
        return 1;
    int *next = malloc(sizeof(int) * n);
    int useless = 1;
    for (uint32_t k = 0; k < cols[c].nvalues && useless; ++k) {
        if (r->cells[c].kind == MatchCellSet && !r->cells[c].has[k])
            continue;
        int m = matchFilter(rows, idx, n, c, (int)k, next);
        useless = matchUseless(rows, next, m, r, c + 1, ncols, cols);
    }
    free(next);
    return useless;
}

// The name of a column's k-th value, as a pattern writes it
static void matchValueName(MatchCol *col, int k, char *buf, size_t size) {
    if (k < 0 || (col->isint && (uint32_t)k >= col->nints))
        snprintf(buf, size, "_");
    else if (col->isint)
        snprintf(buf, size, col->intsigned ? "%lld" : "%llu", (long long)col->ints[k]);
    else if (col->isbool)
        snprintf(buf, size, k ? "true" : "false");
    else {
        Name *name = ((StructNode*)nodesGet(col->enumdcl->derived, k))->namesym;
        snprintf(buf, size, "%s", &name->namestr);
    }
}

// Append to a message, never past its end
static void matchAppend(char *msg, size_t size, const char *text) {
    size_t used = strlen(msg);
    if (used + 1 < size)
        snprintf(msg + used, size - used, "%s", text);
}

#define MatchCoverUnsure     (-1)   // not judged: an error was reported in the match
#define MatchCoverMissing    0      // does not cover every value, and said so
#define MatchCoverComplete   1      // covers every value, by its cases or its 'else'

// Check what the cases of a type checked match cover [topic 5 and the exhaustiveness
// principle]: every enum and bool value, and the combinations of 'match a, b', or an
// 'else'; and that no case is one that can never run. A match whose cases cover everything
// has its last case turned into the 'else' (the value of a match as an expression needs
// no other).
static int ifMatchCover(IfNode *ifnode, int clean) {
    if (!clean)
        return MatchCoverUnsure;
    uint32_t narms = ifnode->condblk->used / 2;
    if (narms == 0) {
        errorMsgNode((INode*)ifnode, ErrorMatchNotExhaustive,
            "This match has no cases, so it covers nothing: add the cases, or an 'else {}'.");
        return MatchCoverMissing;
    }

    // Each case as a row
    int ncols = 0;
    INode *colconds[MatchMaxColumns];
    for (uint32_t a = 0; a < narms && ncols == 0; ++a) {
        INode *cond = nodesGet(ifnode->condblk, a * 2);
        if (cond == elseCond)
            continue;
        if (cond->tag == AndLogicTag && (cond->flags & FlagMatchGuard))
            cond = ((LogicNode*)cond)->lexp;
        ncols = matchFlattenRow(cond, colconds, MatchMaxColumns);
    }
    MatchCol cols[MatchMaxColumns];
    memset(cols, 0, sizeof(cols));
    // The numbers the cases name, in each column of numbers
    for (uint32_t a = 0; a < narms; ++a) {
        INode *cond = nodesGet(ifnode->condblk, a * 2);
        if (cond == elseCond)
            continue;
        if (cond->tag == AndLogicTag && (cond->flags & FlagMatchGuard))
            cond = ((LogicNode*)cond)->lexp;
        if (matchFlattenRow(cond, colconds, MatchMaxColumns) == ncols)
            for (int c = 0; c < ncols; ++c)
                matchCollectInts(colconds[c], &cols[c]);
    }
    MatchRow **rows = malloc(sizeof(MatchRow*) * narms);
    int haselse = 0, unsure = 0;
    uint32_t nrows = 0;
    for (uint32_t a = 0; a < narms && !haselse && !unsure; ++a) {
        INode *cond = nodesGet(ifnode->condblk, a * 2);
        MatchRow *row = malloc(sizeof(MatchRow));
        row->cells = malloc(sizeof(MatchCell) * (ncols ? ncols : 1));
        row->guarded = 0;
        row->pure = 1;
        rows[nrows++] = row;
        if (cond == elseCond) {
            haselse = 1;
            for (int c = 0; c < ncols; ++c)
                row->cells[c].kind = MatchCellWild;
            continue;
        }
        if (cond->tag == AndLogicTag && (cond->flags & FlagMatchGuard)) {
            row->guarded = 1;
            cond = ((LogicNode*)cond)->lexp;
        }
        if (matchFlattenRow(cond, colconds, MatchMaxColumns) != ncols) {
            unsure = 1;
            break;
        }
        for (int c = 0; c < ncols; ++c) {
            row->cells[c] = matchClassify(colconds[c], &cols[c], &unsure);
            if (row->cells[c].kind == MatchCellOther)
                row->pure = 0;
        }
    }
    int result = MatchCoverUnsure;
    if (!unsure) {
        // Report a case that can never run
        int *counted = malloc(sizeof(int) * nrows);
        int ncounted = 0;
        int reported = 0;
        for (uint32_t i = 0; i < nrows; ++i) {
            MatchRow *row = rows[i];
            INode *armcond = nodesGet(ifnode->condblk, i * 2);
            if (row->pure && ncounted > 0 && matchUseless(rows, counted, ncounted, row, 0, ncols, cols)) {
                INode *where = armcond == elseCond ? nodesGet(ifnode->condblk, i * 2 + 1) : matchCaseStart(armcond);
                // The 'else' that holds the cases after one binding with a function's pattern is that case
                INode *tail = where->tag == BlockTag && ((BlockNode*)where)->stmts && ((BlockNode*)where)->stmts->used
                    ? nodesLast(((BlockNode*)where)->stmts) : NULL;
                if (armcond == elseCond && tail && tail->tag == IfTag && (tail->flags & FlagIfMatchTail)) {
                    errorMsgNode(tail, ErrorMatchUnreachable, "This case can never run: the cases before it cover every value.");
                    reported = 1;
                }
                // (a match with a function's pattern must end in 'else' whatever the other cases cover)
                else if (armcond == elseCond && !(ifnode->flags & FlagIfMatchUsing)) {
                    errorMsgNode(where, ErrorMatchUnreachable, "This 'else' can never run: the cases before it cover every value.");
                    reported = 1;
                }
                else if (armcond != elseCond) {
                    errorMsgNode(where, ErrorMatchUnreachable, "This case can never run: the cases before it match every value it matches.");
                    reported = 1;
                }
            }
            if (row->pure && !row->guarded)
                counted[ncounted++] = (int)i;
        }
        int witness[MatchMaxColumns];
        for (int c = 0; c < MatchMaxColumns; ++c)
            witness[c] = -1;
        int covered = haselse || matchCovers(rows, counted, ncounted, 0, ncols, cols, witness);
        if (covered)
            result = MatchCoverComplete;
        else {
            result = MatchCoverMissing;
            char msg[400] = "";
            if (ncols == 1 && cols[0].nvalues > 0 && !cols[0].isint) {
                // The values no counted case takes
                matchAppend(msg, sizeof(msg), "This match does not cover ");
                int missing = 0;
                for (uint32_t k = 0; k < cols[0].nvalues; ++k) {
                    int taken = 0;
                    for (int i = 0; i < ncounted && !taken; ++i)
                        taken = rows[counted[i]]->cells[0].kind == MatchCellWild || rows[counted[i]]->cells[0].has[k];
                    if (taken)
                        continue;
                    char name[80];
                    matchValueName(&cols[0], (int)k, name, sizeof(name));
                    if (missing++)
                        matchAppend(msg, sizeof(msg), ", ");
                    matchAppend(msg, sizeof(msg), name);
                }
                matchAppend(msg, sizeof(msg), ": add a case for each, or end the match with 'else {}'.");
            }
            else if (ncols > 1) {
                matchAppend(msg, sizeof(msg), "This match does not cover every combination: no case takes (");
                for (int c = 0; c < ncols; ++c) {
                    char name[80];
                    matchValueName(&cols[c], witness[c], name, sizeof(name));
                    if (c)
                        matchAppend(msg, sizeof(msg), ", ");
                    matchAppend(msg, sizeof(msg), name);
                }
                matchAppend(msg, sizeof(msg), "), '_' being any value. Add a case for it, or end the match with 'else {}'.");
            }
            else
                matchAppend(msg, sizeof(msg), "This match does not cover every value, and nothing but an enum's variants and true and false can be counted: end it with 'else {}', which may say that no other value can happen.");
            if (!reported)
                errorMsgNode((INode*)ifnode, ErrorMatchNotExhaustive, "%s", msg);
        }
        // Covered by the cases alone: the last of them stands for the rest
        if (covered && !haselse)
            nodesGet(ifnode->condblk, ifnode->condblk->used - 2) = elseCond;
        free(counted);
    }
    for (uint32_t i = 0; i < nrows; ++i) {
        free(rows[i]->cells);
        free(rows[i]);
    }
    free(rows);
    for (int c = 0; c < ncols; ++c)
        free(cols[c].ints);
    return result;
}

// What is known of the names' variants after an 'if': where its branches meet, each
// that does not leave holds what it ended with, and a missing 'else' is a path that
// went by every condition false. Nothing live at all means nothing after it runs.
static NarrowEnv *ifNarrowAfter(NarrowEnv *armends, int livearms, NarrowEnv *pathenv, int fallthrough) {
    if (fallthrough)
        return livearms ? narrowJoin(armends, pathenv) : pathenv;
    return livearms ? armends : pathenv;
}

// Is this branch one whose value is a number literal with no suffix, so that the branches
// that are not literals may type it?
static int ifArmIsLiteral(INode *arm) {
    if (arm->tag != BlockTag || ifBlockJumps((BlockNode *)arm) || ((BlockNode *)arm)->stmts->used == 0)
        return 0;
    return litIsUntypedNumber(nodesLast(((BlockNode *)arm)->stmts));
}

// An 'if' or 'match' whose value goes to no place that names its type: a branch that
// is a number literal with no suffix takes the type of the branches that are not,
// as a literal beside an operator takes the other operand's. 'if c {0.} else {d}' is
// d's type, an f32 say, and 'match n { case 1 {0} case 2 {1} else {b} }' is the u8 b is.
// This applies when every branch that is no such literal has the one same number type;
// where they differ, or there are none, the branches are joined as ever and literals
// alone are the default of their kind. A float literal beside integer branches is not
// taken up, and is refused as it has been.
static void ifLiteralArmsFollow(IfNode *ifnode, INode **maybeType, TypeCompare *match) {
    int floats = 0;
    INode **nodesp;
    uint32_t cnt;
    // The type the branches that are no literal came to in common, if there is one
    INode *common = *maybeType == unknownType ? NULL : itypeGetTypeDcl(*maybeType);
    int usable = common != NULL
        && (common->tag == IntNbrTag || common->tag == UintNbrTag || common->tag == FloatNbrTag)
        && common != (INode*)boolType && common != (INode*)charType;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        ++nodesp; --cnt;
        if (ifArmIsLiteral(*nodesp) && nodesLast(((BlockNode *)*nodesp)->stmts)->tag == FLitTag)
            ++floats;
    }
    // A float literal beside integer branches is not taken up, nor is a branch
    // that is no number: the literals are joined with the others as ever
    int follow = common != NULL && usable && !(floats && common->tag != FloatNbrTag);
    if (!follow) {
        // The branches are joined in the order written, as if none had waited. The ones that
        // are no literal were joined without a complaint (else it was made, and stands), so a
        // mismatch is the literals' to bring, and is reported at the branch that brings it
        if (*match == NoMatch)
            return;
        TypeCompare joined = EqMatch;
        *maybeType = unknownType;
        for (nodesFor(ifnode->condblk, cnt, nodesp)) {
            ++nodesp; --cnt;
            if ((*nodesp)->tag != BlockTag || ifBlockJumps((BlockNode *)*nodesp))
                continue;
            switch (iexpMultiInfer(unknownType, maybeType, nodesp)) {
            case NoMatch: joined = NoMatch; break;
            case ConvSubtype: if (joined != NoMatch) joined = ConvSubtype; break;
            case CastSubtype: if (joined != NoMatch) joined = CastSubtype; break;
            default: break;
            }
        }
        *match = joined;
        return;
    }
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        ++nodesp; --cnt;
        if (!ifArmIsLiteral(*nodesp))
            continue;
        BlockNode *blk = (BlockNode *)*nodesp;
        if (litAdoptNumberType(&nodesLast(blk->stmts), common))
            blk->vtype = common;
    }
}

// Type check the if statement node
// - Every conditional expression must be a bool
// - else can only be last
// All branches must resolve to either the expected type or the same inferred supertype
// Coercion is performed on branches as needed to accomplish this, or errors result
void ifTypeCheck(TypeCheckState *pstate, IfNode *ifnode, INode *expectType) {
    int hasElse = 0;
    TypeCompare match = EqMatch;
    INode *maybeType = unknownType;
    INode **nodesp;
    uint32_t cnt;
    int condErrors = 0;     // a condition (a match's pattern) was reported
    // Flow typing: each condition is checked knowing the ones before it were false, and
    // its branch knowing it was true (narrow.h)
    NarrowEnv *pathenv = narrowSave();  // what holds going into the next condition
    NarrowEnv *armends = NULL;          // where the branches that do not leave end, met
    int livearms = 0;
    int direct = narrowIsStmt((INode*)ifnode);  // a statement of its block
    char *leavewhy = NULL;                      // what a let-else's 'else' lacks (unused here)
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        narrowRestore(pathenv);

        // Validate conditional node
        int errorsBefore = errors;
        inodeTypeCheckAny(pstate, nodesp);
        if (errors != errorsBefore)
            condErrors = 1;
        // Detect when all closed variants are matched, and turn last match into 'else'.
        // The cases of a match are judged together, once all are checked (ifMatchCover)
        if ((*nodesp)->tag == IsTag && !(ifnode->flags & FlagIfMatch))
            ifExhaustCheck(ifnode, (CastNode*)*nodesp);

        if (*nodesp != elseCond) {
            if (0 == iexpCoerce(nodesp, (INode*)boolType)) {
                // A case that tests with a function's pattern, 'case using f()', wants a bool; an Option is bound
                INode *given = isExpNode(*nodesp) ? iexpGetTypeDcl(*nodesp) : NULL;
                if ((ifnode->flags & (FlagIfMatch | FlagIfMatchTail)) && given && given->tag == StructTag && ((StructNode*)given)->namesym == optionName)
                    errorMsgNode(*nodesp, ErrorUsingForm,
                        "A case that only tests with a function's pattern, 'case using f()', needs a function giving a bool, and this one gives an Option. An Option is bound: 'case let x using f()'.");
                else
                    errorMsgNode(*nodesp, ErrorInvType, "Conditional expression requires true/false value (possibly via implicit .isTrue).");
            }
            NarrowEnv *aftercond = narrowSave();
            narrowAssume(*nodesp, 0);
            pathenv = narrowSave();
            narrowRestore(aftercond);
            narrowAssume(*nodesp, 1);
        }
        else {
            hasElse = 1;
            if (cnt > 2) {
                errorMsgNode(*(nodesp + 1), ErrorInvType, "match on everything should be last.");
            }
        }

        // Validate the node performed when previous condition is true
        ++nodesp; --cnt;
        BlockNode *armblk = (*nodesp)->tag == BlockTag ? (BlockNode *)*nodesp : NULL;
        if (armblk)
            narrowAnchorArm(armblk, (uint16_t)(pstate->scope + 1));
        uint32_t killmark = narrowKillMark();
        int armtyped = iexpTypeCheckCoerce(pstate, expectType, nodesp);
        if (armblk)
            narrowInsertHeads(armblk);
        if (ifBlockJumps((BlockNode *)*nodesp)) {
            // A branch that leaves the loop takes its changes with it; one that
            // continues the loop carries them to its next pass
            if (nodesLast(((BlockNode *)*nodesp)->stmts)->tag != ContinueTag)
                narrowKillDrop(killmark);
        }
        // The 'else' of a let-else that leaves on every path, the last of them through an
        // inner 'if' or block, is no path out either, so the variant is known after it, as
        // after one that ends in a jump. Its changes are kept, as a 'continue' keeps them,
        // since some of its paths may continue a loop.
        else if ((ifnode->flags & FlagIfLetElse) && *(nodesp - 1) == elseCond && armblk
            && ifBlockFallsOut(armblk, &leavewhy) == NULL)
            ;
        else {
            NarrowEnv *end = narrowSave();
            armends = livearms++ ? narrowJoin(armends, end) : end;
        }
        if (!armtyped) {
            errorMsgNode(*nodesp, ErrorInvType, "Expression does not match expected type.");
            match = NoMatch;
        }
        else if (!isExpNode(*nodesp)) {
            match = NoMatch;
        }
        // A block that jumps away -- a 'return', or a call that does not
        // return -- gives the 'if' no value, so has no say in its type
        else if (ifBlockJumps((BlockNode *)*nodesp))
            ;
        // A branch that is a literal waits for the others to say its type (ifLiteralArmsFollow)
        else if (expectType == unknownType && ifArmIsLiteral(*nodesp))
            ;
        else {
            switch (iexpMultiInfer(expectType, &maybeType, nodesp)) {
            case NoMatch:
                match = NoMatch;
                break;
            case CastSubtype:
                if (match != NoMatch)
                    match = CastSubtype;
                break;
            case ConvSubtype:
                if (match != NoMatch)
                    match = ConvSubtype;
                break;
            default:
                break;
            }
        }
    }

    // A match must cover every value it can be shown to, or end in 'else'; a match whose
    // cases cover them all ends in one
    if (ifnode->flags & FlagIfMatch) {
        int cover = ifMatchCover(ifnode, !condErrors);
        if (cover == MatchCoverMissing) {
            narrowRestore(ifNarrowAfter(armends, livearms, pathenv, 1));
            return;
        }
        if (cover == MatchCoverComplete)
            hasElse = 1;
    }
    narrowRestore(ifNarrowAfter(armends, livearms, pathenv, !hasElse));
    narrowAnchorAfter((INode*)ifnode, pstate->scope, direct);

    // 'let Ok(n) = r else {...};' goes on to take the variant its test found, so its
    // 'else' must leave on every path (ifBlockFallsOut), and the error points at the
    // path that does not, saying what it lacks
    if ((ifnode->flags & FlagIfLetElse) && ifnode->condblk->used >= 2 && !condErrors) {
        INode *elseblk = nodesGet(ifnode->condblk, ifnode->condblk->used - 1);
        char *why = NULL;
        INode *out = elseblk->tag == BlockTag ? ifBlockFallsOut((BlockNode *)elseblk, &why) : NULL;
        if (out == elseblk)
            errorMsgNode(elseblk, ErrorLetElse,
                "The 'else' of a binding like this must leave, and %s. End it with 'return', 'break', 'continue' or a call that does not return, such as 'panic(...)': otherwise the binding after it would take a variant that was not there.", why);
        else if (out != NULL)
            errorMsgNode(out, ErrorLetElse,
                "The 'else' of a binding like this must leave on every path, and here %s. End that path with 'return', 'break', 'continue' or a call that does not return, such as 'panic(...)': otherwise the binding after it would take a variant that was not there.", why);
    }

    if (expectType == noCareType)
        return;

    // All paths must return a typed value. Absence of an 'else' clause makes that impossible
    if (!hasElse) {
        errorMsgNode((INode*)ifnode, ErrorNoElse, "if requires an 'else' clause (or exhaustive matches) to return a value");
        return;
    }

    // When expectType specified, all branches have been coerced (or not w/ errors)
    if (expectType != unknownType)
        ifnode->vtype = expectType;
    else {
        // A branch that is a literal follows the others' number type
        ifLiteralArmsFollow(ifnode, &maybeType, &match);
        // If expected type is unknown, set the inferred type
        ifnode->vtype = maybeType;

        //  If coercion is needed for some blocks, perform them as needed
        if (match == ConvSubtype || match == CastSubtype) {
            for (nodesFor(ifnode->condblk, cnt, nodesp)) {
                ++nodesp; --cnt;
                // Since generation requires this node to be a block,
                // perform coercion on the last statement
                BlockNode *blk = (BlockNode *)*nodesp;
                if (!ifBlockJumps(blk))
                    iexpCoerce(&nodesLast(blk->stmts), maybeType);
            }
        }
    }

    // The 'if' gives whichever arm's value runs, so a borrowed reference lives
    // only as long as its shortest-lived arm's, not its first's or the type
    // expected of it
    uint16_t narrowest = 0;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        ++nodesp; --cnt;
        if (!ifBlockJumps((BlockNode *)*nodesp))
            narrowest = iexpNarrowerScope(narrowest, *nodesp);
    }
    ifnode->vtype = iexpNarrowestType(ifnode->vtype, (INode*)ifnode, narrowest);
}

// Perform data flow analysis on an if expression
//
// Each branch is walked from the state its conditions leave, and the branches
// join after it (flowVarJoin): a variable moved, hollowed or initialized on any
// branch that did not jump away is so after the 'if' as far as its use goes. A
// missing 'else' is a path of its own. The first condition runs on every path;
// each later one, and each branch, on some only (flowDepth).
void ifFlow(FlowState *fstate, IfNode **ifnodep) {
    IfNode *ifnode = *ifnodep;
    INode **nodesp;
    uint32_t cnt;
    int first = 1;
    int haselse = 0;
    uint32_t mark = 0;
    // Only a path that changed something is kept; the join counts the others
    FlowVarPath *paths = NULL;
    uint32_t live = 0;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        if (*nodesp == elseCond)
            haselse = 1;
        else {
            if (!first)
                ++flowDepth;
            flowLoadValue(fstate, nodesp);
            if (!first)
                --flowDepth;
        }
        if (first) {
            mark = flowVarLogMark();
            first = 0;
        }
        nodesp++; cnt--;
        uint32_t condmark = flowVarLogPos();
        ++flowDepth;
        blockFlow(fstate, (BlockNode**)nodesp);
        --flowDepth;
        if (!fstate->jumped) {
            if (flowVarLogPos() > mark)
                paths = flowVarPathTake(mark, paths);
            ++live;
        }
        flowVarRollback(condmark);
    }
    if (!haselse) {
        if (flowVarLogPos() > mark)
            paths = flowVarPathTake(mark, paths);
        ++live;
    }
    flowVarRollback(mark);
    flowVarJoin(paths, live);
    fstate->jumped = live == 0;
}
