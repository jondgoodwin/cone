/** The discriminant type of a closed variant type
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef enum_h
#define enum_h

// The type of an enum's discriminant: an unsigned integer wide enough to hold
// every variant's tag number, carrying no arithmetic of its own.
//
// It is not the enum. An enum is a StructNode -- see nodes/struct.md -- and this
// is the type of the one field the compiler synthesizes at position 0 of it. An
// author may place that field explicitly, for alignment, by writing '_ tag'.
typedef struct EnumNode {
    INsTypeNodeHdr;
    Name *namesym;
    INode *underlying;     // The integer type the enum declared, or NULL for none
    uint8_t bytes;         // Width in bytes: 1, 2, 4 or 8
    uint8_t fixedwidth;    // Set when 'underlying' pinned the width, so nothing widens it
} EnumNode;

// Create a new discriminant type node
EnumNode *newEnumNode();

// Serialize an enum node
void enumPrint(EnumNode *node);

// Name resolution of a discriminant type
void enumNameRes(NameResState *pstate, EnumNode *node);

// Type check a discriminant type
void enumTypeCheck(TypeCheckState *pstate, EnumNode *node);

// The number of bytes needed to hold tag value 'maxtag'
uint8_t enumBytesFor(uint32_t maxtag);

#endif
