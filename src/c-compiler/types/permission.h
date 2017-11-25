/** Permission types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef permission_h
#define permission_h

#include "type.h"

struct AstNode;

// Permission type info
typedef struct PermTypeAstNode {
	AstNodeHdr;
	uint8_t ptype;
	struct AstNode *locker;
} PermTypeAstNode;

// Permission types
enum PermType {
	MutPerm,
	MmutPerm,
	ImmPerm,
	ConstPerm,
	ConstxPerm,
	MutxPerm,
	IdPerm,
	LockPerm
};

// Permission type flags
// - Each true flag indicates a helpful capability the permission makes possible
// - All false flags essentially deny those capabilities to the permission
#define MayRead 0x01		// If on, the contents may be read
#define MayWrite 0x02		// If on, the contents may be mutated
// If on, another live alias may be created able to read or mutate the contents. 
// If off, any new alias turns off use of the old alias for the lifetime of the new alias.
#define MayAlias 0x04		
#define MayAliasWrite 0x08	// If on, another live alias may be created able to write the contents.
#define RaceSafe 0x10		// If on, a reference may be shared with or sent to another thread.
// If on, lifetime-constrained interior references may be borrowed from this alias.
#define MayIntRef 0x20
// If on, no locks are needed to read or mutate the contents. 
// If off, the permission's designated locking mechanism must be wrapped around all content access.
#define IsLockless 0x40

void permInit();

#endif