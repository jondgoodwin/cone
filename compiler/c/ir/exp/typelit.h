/** Handling for type literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef list_h
#define list_h

// Serialize a type literal
void typeLitPrint(FnCallNode *node);

// Name resolution of the literal node
void typeLitNameRes(NameResState *pstate, FnCallNode *lit);

// Reorder the literal's field values to the same order as the type's fields
// Also prevent the specification of a value for a private field outside the type's methods
int typeLitStructReorder(FnCallNode *arrlit, StructNode *strnode, int private);

// Check the type literal node
void typeLitTypeCheck(TypeCheckState *pstate, FnCallNode *lit);

// 'new Point(1, 2)': select the init the arguments call for, and lower the
// construction to the struct's literal (its implicit init) or to a call of a
// declared init, which fills the value in place. 'new Rc[mut, Node](1)', and
// 'trynew', allocate it in a region: an AllocateTag node holding the value's
// construction.
void typeLitNewCheck(TypeCheckState *pstate, FnCallNode **nodep);

// Is this type-checked value one 'new' constructs: a construction, or a struct
// that is not a variant written in brackets?
int typeLitIsConstruction(INode *node);

// Check the value a number's 'from' converts; 0 when it does not convert
int typeLitNbrFromCheck(FnCallNode *conv, INode *type);

// Is the type literal actually a literal?
int typeLitIsLiteral(FnCallNode *node);

// Perform data flow analysis on a type literal's field values
void typeLitFlow(FlowState *fstate, FnCallNode **nodep);

#endif
