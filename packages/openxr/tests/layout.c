/* The C side of the openxr package's layout test.

   The declarations come from Khronos's own open headers, openxr.h and
   openxr_platform.h (OpenXR 1.1.65, Apache 2.0), with the Vulkan structs
   XR_KHR_vulkan_enable2 names from vulkan.h. No header is kept in this
   repository: name the folders of a copy with /I.

   main() prints the size and alignment of every struct, and the offset of
   each of its fields, in the order layout.cone prints them from the Cone
   declarations; layout.py writes both lists, layout.inc for this file. What
   this prints is layout.out, so 'congo test' checks the Cone layout against
   the C compiler's. To make layout.out again, from a Visual Studio x64
   prompt, with the OpenXR loader package unpacked (its include folder holds
   openxr/openxr.h) and the Vulkan SDK installed:

     python layout.py
     cl /nologo /I<OpenXR include> /I<Vulkan SDK Include> layout.c
     layout.exe > layout.out
*/
#define XR_USE_GRAPHICS_API_VULKAN 1
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define S(t) printf("%s size %zu align %zu\n", #t, sizeof(Xr##t), _Alignof(Xr##t))
#define O(t, f) printf("%s.%s %zu\n", #t, #f, offsetof(Xr##t, f))

int main(void) {
#include "layout.inc"
  return 0;
}
