// The speechkernels package's shared shader code: the argument block every kernel takes (sixteen 32-bit words, read
// as unsigned integers or, through asfloat, floats; each kernel's header says what its words are) and the index fold.
// A module, with no entry points: tools/shaders/shaders.py compiles the shaders that import it.

struct Args {
    uint v[16];
};

// The number of a workgroup in a dispatch folded into two dimensions: groups across, then as many rows of them as it
// takes; the group's number is y * groupsX + x
uint foldedGroup(uint3 group, uint groupsX) {
    return group.y * groupsX + group.x;
}
