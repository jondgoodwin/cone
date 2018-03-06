/** Hashed name table
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef name_h
#define name_h

typedef struct NamedAstNode NamedAstNode;	// ../ast/ast.h

#include <stdlib.h>
#include <stddef.h>

/* *****************************************************
 * Names and Global Name Table - name.c
 * *****************************************************/

// Configurable size for name table and % utilization trigger
size_t gNames;		// Initial maximum number of unique names (must be power of 2)
unsigned int gNameTblUtil;	// % utilization that triggers doubling of table

// Name info (a name-unique, unmovable allocated block in memory
typedef struct Name {
	NamedAstNode *node;	// AST node currently assigned to name
	size_t hash;	// Name's computed hash
	unsigned char namesz;	// Number of characters in the name (<=255)
	char namestr;	// First byte of name's string (the rest follow)
} Name;

// Get pointer to Name struct for the name's string in the name table 
// For unknown name, it allocates memory for the string and adds it to name table.
Name *nameFind(char *strp, size_t strl);

size_t nameUnused();
	
// Initialize the name table with reserved names
void nameInit();

#endif