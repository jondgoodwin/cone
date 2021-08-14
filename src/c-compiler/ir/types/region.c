/** Region type handling - region is a specialized struct
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

int isRegion(INode *region, Name *namesym) {
    region = itypeGetTypeDcl(region);
    if (region->tag == StructTag) {
        StructNode *rnode = (StructNode*)region;
        return rnode->namesym == namesym;
    }
    return 0;
}

int regionIsMove(INode *region) {
    return isRegion(region, soName);
}
