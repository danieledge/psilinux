/* bigimage.c - a program whose image is big (BIG KB of bss): for finding
 * how big a program the runner can load */
#include <stdio.h>
#ifndef BIG
#define BIG 3000
#endif
static char space[BIG * 1024];
int main(void) { space[0] = 1; space[sizeof space - 1] = 2; printf("big %d KB ok\n", BIG); return 0; }
