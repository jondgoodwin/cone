/** stdio - Standard library i/o
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>

// Each value is written by one of these to a C stream: the print functions
// below to stdout, the eprint functions to stderr, the same text either way

static void streamUtf8(FILE *stream, uint64_t code) {
	char result[6];
	char *p = &result[0];

	if (code<0x80)
		*p++ = (unsigned char) code;
	else if (code<0x800) {
		*p++ = 0xC0 | (unsigned char)(code >> 6);
		*p++ = 0x80 | (code & 0x3f);
	}
	else if (code<0x10000) {
		*p++ = 0xE0 | (unsigned char)(code >> 12);
		*p++ = 0x80 | ((code >> 6) & 0x3F);
		*p++ = 0x80 | (code & 0x3f);
	}
	else if (code<0x110000) {
		*p++ = 0xF0 | (unsigned char)(code >> 18);
		*p++ = 0x80 | ((code >> 12) & 0x3F);
		*p++ = 0x80 | ((code >> 6) & 0x3F);
		*p++ = 0x80 | (code & 0x3f);
	}
	*p = '\0';
	fprintf(stream, "%s", result);
}

// Standard output, which the C library buffers

void printStr(char *p, size_t len) {
	fwrite(p, len, 1, stdout);
}

void printCStr(char *p) {
    printf("%s", p);
}

void printInt(int64_t nbr) {
	printf("%"PRId64, nbr);
}

void printUInt(uint64_t nbr) {
    printf("%"PRIu64, nbr);
}

void printFloat(double nbr) {
	printf("%g", nbr);
}

void printChar(uint64_t code) {
	streamUtf8(stdout, code);
}

void printFlush(void) {
	fflush(stdout);
}

// Standard error, which C never fully buffers (the Windows runtime not at all)

void eprintStr(char *p, size_t len) {
	fwrite(p, len, 1, stderr);
}

void eprintCStr(char *p) {
	fprintf(stderr, "%s", p);
}

void eprintInt(int64_t nbr) {
	fprintf(stderr, "%"PRId64, nbr);
}

void eprintUInt(uint64_t nbr) {
	fprintf(stderr, "%"PRIu64, nbr);
}

void eprintFloat(double nbr) {
	fprintf(stderr, "%g", nbr);
}

void eprintChar(uint64_t code) {
	streamUtf8(stderr, code);
}

void eprintFlush(void) {
	fflush(stderr);
}
