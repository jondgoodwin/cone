/** Global Type Table.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "typetbl.h"
#include "memory.h"

#include <stdio.h>
#include <assert.h>

// Public globals
size_t gTypeTblInitSize = 4096;     // Initial maximum number of unique types (must be power of 2)
unsigned int gTypeTblUtil = 50;     // % utilization that triggers doubling of table

// Private globals
static TypeTblEntry *gTypeTable = NULL;    // The type table array
static size_t gTypeTblAvail = 0;           // Number of allocated type table slots (power of 2)
static size_t gTypeTblCeil = 0;            // Ceiling that triggers table growth
static size_t gTypeTblUsed = 0;            // Number of type table slots used

/** Modulo operation that calculates primary table entry from name's hash.
 * 'size' is always a power of 2 */
#define nameHashMod(hash, size) \
    (assert(((size)&((size)-1))==0), (size_t) ((hash) & ((size)-1)) )

/** Calculate index into name table for a name using linear probing
 * The table's slot at index is either empty or matches the provided name/hash
 */
#define typetblFindSlot(tblp, hash, type) \
{ \
    size_t tbli; \
    for (tbli = nameHashMod(hash, gTypeTblAvail);;) { \
        tblp = &gTypeTable[tbli]; \
        if (tblp->type==NULL || (tblp->hash == hash && itypeIsSame(tblp->type, type))) \
            break; \
        tbli = nameHashMod(tbli + 1, gTypeTblAvail); \
    } \
}

/** Grow the type table, by either creating it or doubling its size */
void typetblGrow() {
    size_t oldTblAvail;
    TypeTblEntry *oldTable;
    size_t newTblMem;
    size_t oldslot;

    // Preserve old table info
    oldTable = gTypeTable;
    oldTblAvail = gTypeTblAvail;

    // Allocate and initialize new name table
    gTypeTblAvail = oldTblAvail==0? gTypeTblInitSize : oldTblAvail<<1;
    gTypeTblCeil = (gTypeTblUtil * gTypeTblAvail) / 100;
    newTblMem = gTypeTblAvail * sizeof(TypeTblEntry);
    gTypeTable = (TypeTblEntry*) memAllocBlk(newTblMem);
    memset(gTypeTable, 0, newTblMem); // Fill with NULL pointers & 0s

    // Copy existing name slots to re-hashed positions in new table
    for (oldslot=0; oldslot < oldTblAvail; oldslot++) {
        TypeTblEntry *oldslotp, *newslotp;
        oldslotp = &oldTable[oldslot];
        if (oldslotp->type) {
            INode *type = oldslotp->type;
            size_t hash = oldslotp->hash;
            typetblFindSlot(newslotp, hash, type);
            newslotp->hash = oldslotp->hash;
            newslotp->type = oldslotp->type;
            newslotp->normal = oldslotp->normal;
        }
    }
    // memFreeBlk(oldTable);
}

/** Get pointer to type's normalized metadata in Global Type Table matching type. 
 * For unknown type, this allocates memory for the metadata and adds it to type table. */
void *typetblFind(INode *type, void *(*allocfn)()) {
    TypeTblEntry *slotp;

    // Hash provide string into table
    size_t hash = itypeHash(type);
    typetblFindSlot(slotp, hash, type);

    // If not already a name, allocate memory for string and add to table
    if (slotp->type == NULL) {
        // Double table if it has gotten too full
        if (++gTypeTblUsed >= gTypeTblCeil) {
            typetblGrow();
            typetblFindSlot(slotp, hash, type);
        }

        // Allocate and populate type info
        slotp->type = type;
        slotp->hash = hash;
        slotp->normal = allocfn();
    }
    return slotp->normal;
}

// Return size of unused space for name table
size_t typetblUnused() {
    return (gTypeTblAvail-gTypeTblUsed)*sizeof(TypeTblEntry);
}

// Initialize name table
void typetblInit() {
    typetblGrow();
}
