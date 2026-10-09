/** Contents after '<-': the entries of an append list, and its lowering
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef contents_h
#define contents_h

// One entry on the right of '<-' that is not a plain value:
// - OfEntryTag:   'n of x', n values, x evaluated for each
// - FillEntryTag: 'fill x', x appended until the collection is full
// - PairEntryTag: 'k: v', a key and its value, given to a two-value append
// - EachEntryTag: 'each src', or 'each x in src if c yield v': a loop (an
//   'each' block, built by parseEach) that appends what it yields
// - YieldEntryTag: the value a pass of that loop appends, the last statement of
//   its body ('k: v' makes it a pair). Not written on its own: parseEach makes
//   it from the 'yield', or, for 'each src', as the pass variable.
// The parser builds one only on the right of '<-' (parseEntry), and type check
// lowers it into appends there (contentsLower); one reached anywhere else is
// refused (entryTypeCheck).
typedef struct EntryNode {
    IExpNodeHdr;
    INode *first;          // 'n' of 'n of x', 'k' of 'k: v' (also of a yield); the loop's outer block of an each; NULL for 'fill x'
    INode *val;            // 'x', or 'v'; NULL for an each
    INode *recv;           // yield: the variable holding the receiver, set by the '<-' that lowers the loop (contentsEach)
    uint16_t nest;         // each: how many blocks deeper than name resolution finds it the lowering puts the loop
    uint16_t drain;        // yield: appends a copy of each item that comes borrowed ('<- each src')
} EntryNode;

EntryNode *newEntryNode(uint16_t tag, INode *first);
INode *cloneEntryNode(CloneState *cstate, EntryNode *node);
void entryPrint(EntryNode *node);
void entryNameRes(NameResState *pstate, EntryNode *node);
void entryTypeCheck(TypeCheckState *pstate, INode **nodep);

#define isEntryNode(node) ((node)->tag == OfEntryTag || (node)->tag == FillEntryTag || (node)->tag == PairEntryTag \
    || (node)->tag == EachEntryTag || (node)->tag == YieldEntryTag)

// Is this '<-' one contentsLower takes apart: contents after a construction,
// several entries, or an entry that is not a plain value?
int contentsIsAppend(FnCallNode *node);

// Lower such a '<-' into the appends it stands for, and type check them
void contentsLower(TypeCheckState *pstate, FnCallNode **nodep);

#endif
