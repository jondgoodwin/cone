/** Handling for cast nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef cast_h
#define cast_h

// Cast to another type
typedef struct CastNode {
    IExpNodeHdr;
    INode *exp;
    INode *typ;
} CastNode;

#define FlagConvert 0x8000  // Cast should convert instead of re-cast (default)

// Create node for recasting to a new type without conversion
CastNode *newRecastNode(INode *exp, INode *type);

// Create node for converting exp to a new type
CastNode *newConvCastNode(INode *exp, INode *type);

// Clone cast
INode *cloneCastNode(CloneState *cstate, CastNode *node);

// Create a new cast node
CastNode *newIsNode(INode *exp, INode *type);

void castPrint(CastNode *node);

// Name resolution of cast node
void castNameRes(AnalysisState *pstate, CastNode *node);

// Answer whether a value of fromtype may be converted to Bool
int castConvertsToBool(INode *fromtype);

// Type check cast node:
// - reinterpret cast types must be same size
// - Ensure type can be safely converted to target type
void castTypeCheck(AnalysisState *pstate, CastNode *node);

// Analyze type comparison (is) node
void castIsTypeCheck(AnalysisState *pstate, CastNode *node);

#endif
