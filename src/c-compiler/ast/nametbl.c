/** Names and Global Name Table.
 * @file
 *
 * The lexer generates symbolic, hashed tokens to speed up parsing matches (== vs. strcmp).
 * Every name is immutable and uniquely hashed based on its string characters.
 *
 * All names are hashed and stored in the global name table.
 * The name's table entry points to an allocated block that holds its current "value", computed hash and c-string.
 *
 * Dan Bernstein's djb algorithm is the hash function for strings, being fast and effective
 * The name table uses open addressing (vs. chaining) with quadratic probing (no Robin Hood).
 * The name table starts out large, but will double in size whenever it gets close to full.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "nametbl.h"
#include "memory.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>

// Public globals
size_t gNames = 16384;	// Initial maximum number of unique names (must be power of 2)
unsigned int gNameTblUtil = 80;	// % utilization that triggers doubling of table

// Private globals
Name **gNameTable = NULL;	// The name table array
size_t gNameTblAvail = 0;	// Number of allocated name table slots (power of 2)
size_t gNameTblCeil = 0;		// Ceiling that triggers table growth
size_t gNameTblUsed = 0;		// Number of name table slots used

/** String hash function (djb: Dan Bernstein)
 * Ref: http://www.cse.yorku.ca/~oz/hash.html
 * Ref: http://www.partow.net/programming/hashfunctions/#AvailableHashFunctions
 * Would this help?: hash ^= hash >> 11; // Extra randomness for long names?
 */
#define nameHashFn(hash, strp, strl) \
{ \
	char *p; \
	size_t len = strl; \
	p = strp; \
	hash = 5381; \
	while (len--) \
		hash = ((hash << 5) + hash) ^ ((size_t)*p++); \
}

/** Modulo operation that calculates primary table entry from name's hash.
 * 'size' is always a power of 2 */
#define nameHashMod(hash, size) \
	(assert(((size)&((size)-1))==0), (size_t) ((hash) & ((size)-1)) )

/** Calculate index into name table for a name using quadratic probing
 * The table's slot at index is either empty or matches the provided name/hash
 * http://stackoverflow.com/questions/2348187/moving-from-linear-probing-to-quadratic-probing-hash-collisons/2349774#2349774
 */
#define nameFindSlot(tblp, hash, strp, strl) \
{ \
	size_t tbli, step; \
	for (tbli = nameHashMod(hash, gNameTblAvail), step = 1;; ++step) { \
		Name *slot; \
		slot = gNameTable[tbli]; \
		if (slot==NULL || (slot->hash == hash && slot->namesz == strl && strncmp(strp, &slot->namestr, strl)==0)) \
			break; \
		tbli = nameHashMod(tbli + step, gNameTblAvail); \
	} \
	tblp = &gNameTable[tbli]; \
}

/** Grow the name table, by either creating it or doubling its size */
void nameGrow() {
	size_t oldTblAvail;
	Name **oldTable;
	size_t newTblMem;
	size_t oldslot;

	// Preserve old table info
	oldTable = gNameTable;
	oldTblAvail = gNameTblAvail;

	// Allocate and initialize new name table
	gNameTblAvail = oldTblAvail==0? gNames : oldTblAvail<<1;
	gNameTblCeil = (gNameTblUtil * gNameTblAvail) / 100;
	newTblMem = gNameTblAvail * sizeof(Name*);
	gNameTable = (Name**) memAllocBlk(newTblMem);
	memset(gNameTable, 0, newTblMem); // Fill with NULL pointers & 0s

	// Copy existing name slots to re-hashed positions in new table
	for (oldslot=0; oldslot < oldTblAvail; oldslot++) {
		Name **oldslotp, **newslotp;
		oldslotp = &oldTable[oldslot];
		if (*oldslotp) {
			char *strp;
			size_t strl, hash;
			Name *oldnamep = *oldslotp;
			strp = &oldnamep->namestr;
			strl = oldnamep->namesz;
			hash = oldnamep->hash;
			nameFindSlot(newslotp, hash, strp, strl);
			*newslotp = *oldslotp;
		}
	}
	// memFreeBlk(oldTable);
}

/** Get pointer to SymId for the name's string. 
 * For unknown name, this allocates memory for the string (SymId) and adds it to name table. */
Name *nameFind(char *strp, size_t strl) {
	size_t hash;
	Name **slotp;

	// Hash provide string into table
	nameHashFn(hash, strp, strl);
	nameFindSlot(slotp, hash, strp, strl);

	// If not already a name, allocate memory for string and add to table
	if (*slotp == NULL) {
		Name *newname;
		// Double table if it has gotten too full
		if (++gNameTblUsed >= gNameTblCeil)
			nameGrow();

		// Allocate and populate name info
		*slotp = newname = memAllocBlk(sizeof(Name) + strl);
		memcpy(&newname->namestr, strp, strl);
		(&newname->namestr)[strl] = '\0';
		newname->hash = hash;
		newname->namesz = (unsigned char)strl;
		newname->node = NULL;		// Node not yet known
	}
	return *slotp;
}

// Return size of unused space for name table
size_t nameUnused() {
	return (gNameTblAvail-gNameTblUsed)*sizeof(Name*);
}

// Initialize name table
void nameInit() {
	nameGrow();
}

// Hook a node into global name table, such that its owner can withdraw it later
void nameHook(Namespace2 *namespace, NamedAstNode *namenode, Name *name) {
	namenode->hooklink = namespace->nameslink; // Add to owner's hook list
	namespace->nameslink = (NamedAstNode*)namenode;
	namenode->prevname = name->node; // Latent unhooker
	name->node = (NamedAstNode*)namenode;
}

// Hook all names in inodes into global name table
void nameHookAll(Namespace2 *namespace, Inodes *inodes) {
	SymNode *nodesp;
	uint32_t cnt;
	for (inodesFor(inodes, cnt, nodesp))
		nameHook(namespace, nodesp->node, nodesp->name);
}

// Unhook all of an owner's names from global name table (LIFO)
void nameUnhookAll(Namespace2 *namespace) {
	NamedAstNode *node = namespace->nameslink;
	while (node) {
		NamedAstNode *next = node->hooklink;
		node->namesym->node = node->prevname;
		node->prevname = NULL;
		node->hooklink = NULL;
		node = next;
	}
	namespace->nameslink = NULL;
}
