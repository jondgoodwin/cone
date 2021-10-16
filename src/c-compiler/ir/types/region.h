/** region type handling. A region is a specialized struct
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef region_h
#define region_h

int isRegion(INode *region, Name *namesym);

void regionAllocTypeCheck(INode *region);

#endif
