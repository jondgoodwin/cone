/* The C side of the sdl package's layout test.

   main() prints the size and alignment of every struct the package binds,
   and the offset of each of its fields, as SDL3's own headers declare them,
   in the order layout.cone prints them from the Cone declarations;
   layout.py writes both lists, layout.inc for this file. What this prints is
   layout.out, so 'congo test' checks the Cone layout against the C
   compiler's layout of SDL's structs. To make layout.out again, from a
   Visual Studio x64 prompt, with SDL3's development kit at <SDL3>:

     python layout.py && cl /nologo /I<SDL3>\include layout.c && layout.exe > layout.out

   It includes only headers, so it links nothing of SDL's.
*/
#include <stdio.h>
#include <stddef.h>
#include <SDL3/SDL.h>

#define S(t) printf("%s size %zu align %zu\n", #t, sizeof(SDL_##t), _Alignof(SDL_##t))
#define O(t, f, name) printf("%s.%s %zu\n", #t, name, offsetof(SDL_##t, f))

int main(void) {
#include "layout.inc"
  return 0;
}
