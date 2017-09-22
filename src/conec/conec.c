/** Main program file
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "conec.h"
#include "fileio.h"

#include <stdio.h>
#include <stdlib.h>

void main(int argv, char **argc) {
	char *filestr;

	// Output compiler name and release level
	puts(CONE_RELEASE);

	if (argv<2) {
		puts("Specify a Cone program to compile");
		exit(1);
	}

	filestr = fileLoad(argc[1]);
	if (filestr) {
		printf("I wish I knew how to compile, but here is the source!\n\n%s", filestr);
	}

	getchar();
}