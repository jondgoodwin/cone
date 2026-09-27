/** The discriminant type of a closed variant type
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef enum_h
#define enum_h

// The type of an enum's discriminant: an integer wide enough to hold every
// variant's tag number, signed when the enum declared a signed type or, declaring
// none, holds a negative value, and carrying no arithmetic of its own.
//
// It is not the enum. An enum is a StructNode -- see compiler/c/doc/nodes/struct.md -- and this
// is the type of the one field the compiler synthesizes at position 0 of it. An
// author may place that field explicitly, for alignment, by writing '_ tag'.
//
// This node is SHARED rather than cloned (clone.c), so an enum, every variant of
// it and every enum that extends it read one of these. That is what makes it the
// place for a layout fact the whole family must agree on: the tag's width.
typedef struct EnumNode {
    INsTypeNodeHdr;
    Name *namesym;
    INode *underlying;     // The integer type the enum declared, or NULL for none
    uint8_t bytes;         // Width in bytes: 1, 2, 4 or 8
    uint8_t fixedwidth;    // Set when 'underlying' pinned the width, so nothing widens it
    uint8_t issigned;      // Set when the tag values read signed: 'underlying' is, or a value is negative
} EnumNode;

// Create a new discriminant type node
EnumNode *newEnumNode();

// Serialize an enum node
void enumPrint(EnumNode *node);

// Name resolution of a discriminant type
void enumNameRes(NameResState *pstate, EnumNode *node);

// Type check a discriminant type
void enumTypeCheck(TypeCheckState *pstate, EnumNode *node);

// Does a variant's tag value fit an integer 'bits' wide, signed or not?
int enumTagFits(struct StructNode *variant, unsigned int bits, int issigned);

// The values an integer 'bits' wide holds, "from A to B", written into 'buf'
// (at least 64 bytes), which is returned
char *enumRangeText(unsigned int bits, int issigned, char *buf);

#endif
