/** Hashed table for normalizing equivalent structural types to single definition
 *  For use by LLVM generation and for preserving useful metadata.
 *
 *  Nominal types are uniquely defined, with a name being used to refer to the type's singular definition
 *  Structural types (references/pointers, arrays, tuples) redefine the type each time,
 *     which forces type matching to compare all elements for equivalence
 *  This global Structural Type table allows us to collapse all equivalent definitions
 *  for a structural type into a single definitional object.
 *  - This speeds up creation and reuse of the LLVM type
 *  - This single definition can hold constructed metadata for the type, particularly the 
 *    field and method table for region-managed references (alloc, free, deref, etc.)
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef typetbl_h
#define typetbl_h

#include "ir.h"

#include <stddef.h>

typedef struct {
    INode *type;             // Type node to compare against
    size_t hash;             // Type's computed hash
    void *normal;            // Normalized type metadata
} TypeTblEntry;

// Global Type Table configuration variables
extern size_t gTypeTblInitSize;      // Initial maximum number of unique types (must be power of 2)
extern unsigned int gTypeTblUtil;    // % utilization that triggers doubling of table

// Allocate and initialize the global type table
void typetblInit();

// Get pointer to the type's normal metadata for the structural type in the global type table 
// For an unknown type, it allocates memory for the metadata and adds it to type table.
void *typetblFind(INode *type, void *(*allocfn)());

// Return how many bytes have been allocated for global type table but not yet used
size_t typetblUnused();

#endif
