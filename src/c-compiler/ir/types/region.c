/** Region type handling - region is a specialized struct
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

AllocNode *newRegionNodeStr(Name *namesym) {
    AllocNode *allocnode;
    newNode(allocnode, AllocNode, RegionTag);
    allocnode->namesym = namesym;
    namesym->node = (INode*)allocnode;
    return allocnode;
}

int isRegion(INode *region, Name *namesym) {
    region = itypeGetTypeDcl(region);
    if (region->tag == RegionTag) {
        AllocNode *rnode = (AllocNode*)region;
        return rnode->namesym == namesym;
    }
    return 0;
}

int regionIsMove(INode *region) {
    return isRegion(region, soName);
}
