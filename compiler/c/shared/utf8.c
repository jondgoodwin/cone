/** UTF8 Helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "utf8.h"
#include <ctype.h>

/** How many bytes the character at src is made of, or 0 if it is malformed.
 *
 * A lead byte declares a length, and the bytes after it have to agree: each
 * must be a continuation byte. Reading the declared length without checking is
 * what let one stray byte consume the source after it -- 0xF0 announces four
 * bytes whatever follows, so scanning an identifier swallowed the space, the
 * '=' and whatever came next, and the diagnostic landed on a later line with
 * nothing wrong with it.
 *
 * 0xF8 through 0xFF lead nothing: UTF-8 characters are at most four bytes.
 * Reading up to the declared length is safe at the end of source, because the
 * terminating '\0' is not a continuation byte and stops the walk.
 */
static int utf8SeqLen(const char *src) {
    unsigned char lead = (unsigned char)*src;
    int nbytes;
    if (lead < 0x80)
        return 1;
    else if ((lead & 0xE0) == 0xC0) nbytes = 2;
    else if ((lead & 0xF0) == 0xE0) nbytes = 3;
    else if ((lead & 0xF8) == 0xF0) nbytes = 4;
    else return 0;   // a continuation byte alone, or one no character may start with

    for (int i = 1; i < nbytes; ++i) {
        if (((unsigned char)src[i] & 0xC0) != 0x80)
            return 0;   // the sequence its lead promised is not there
    }
    return nbytes;
}

int utf8ByteSkip(const char *src) {
    if (*src == '\0' || *src == '\x1A')
        return 0;
    int nbytes = utf8SeqLen(src);
    return nbytes ? nbytes : 1;
}

int utf8IsMultibyte(const char *src) {
    return utf8SeqLen(src) > 1;
}

/** Return the current unicode character whose UTF-8 bytes start at lex->bytepos */
uint32_t utf8GetCode(const char *src) {
    int nbytes = utf8SeqLen(src);
    uint32_t chr;

    if (nbytes == 0)
        return 0;   // malformed: the caller has already reported the byte
    else if (nbytes == 1) return (uint32_t)((unsigned char)*src & 0x7F);
    else if (nbytes == 2) chr = (unsigned char)*src & 0x1F;
    else if (nbytes == 3) chr = (unsigned char)*src & 0x0F;
    else chr = (unsigned char)*src & 0x07;

    // utf8SeqLen has established that this many continuation bytes follow
    while (--nbytes) {
        src++;
        chr = (chr << 6) + ((unsigned char)*src & 0x3F);
    }
    return chr;
}

// Return true if unicode is a letter
int utf8IsLetter(const char* srcp) {
    return utf8IsMultibyte(srcp) || isalpha((unsigned char)*srcp);
}
