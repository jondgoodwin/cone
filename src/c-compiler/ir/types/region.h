/** region type handling. A region is a specialized struct
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef region_h
#define region_h

typedef struct {
    INsTypeNodeHdr;
} AllocNode;

AllocNode *newRegionNodeStr(Name *namesym);

int isRegion(INode *region, Name *namesym);

int regionIsMove(INode *region);

#endif
