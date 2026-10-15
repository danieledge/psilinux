/* hello.c - the first PsiLinux program */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	printf("Hello, world from Linux on a Psion! (pid %d, %d args)\n", (int)getpid(), argc);
	return 0;
}
