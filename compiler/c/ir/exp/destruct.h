/** Handling for field destructuring nodes, 'x.(a, b)'
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef destruct_h
#define destruct_h

// 'x.(a, b)': the named fields of a struct (or the numbered elements of a tuple,
// 'x.(0, 2)') taken from the value 'exp'. One name gives that field's value; two
// or more give a tuple of them, in the order written.
//
// The parser builds the node; type check decides what it is. Where the fields are
// read as 'x.a' reads one (through a reference, or from a place that stays whole)
// it is lowered to those field accesses and the node is gone. What is left, and
// reaches flow and generation, takes the value apart: 'exp' is the whole value
// (moved, if it is a move type), the named fields are its result, and the fields
// not named die with the statement.
typedef struct DestructNode {
    IExpNodeHdr;
    INode *exp;         // The value whose fields are taken
    Nodes *names;       // As written: a member name use, or a number for a tuple's element
    uint32_t *index;    // After type check: for each name, its field's index in the struct or element in the tuple
} DestructNode;

DestructNode *newDestructNode(INode *exp);

// Clone a field destructuring
INode *cloneDestructNode(CloneState *cstate, DestructNode *node);

void destructPrint(DestructNode *node);

// Name resolution of a field destructuring: its value; the names are members
void destructNameRes(NameResState *pstate, DestructNode *node);

// Type check a field destructuring. May replace the node with the field accesses
// it stands for.
void destructTypeCheck(TypeCheckState *pstate, INode **nodep);

#endif
