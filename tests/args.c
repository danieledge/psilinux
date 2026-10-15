/* args.c - prints its arguments, environment size and stdin's first line */
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv, char **envp)
{
	int i, n = 0;
	char line[128];
	for (i = 0; i < argc; i++) printf("argv[%d] = \"%s\" (%d bytes)\n", i, argv[i], (int)strlen(argv[i]));
	while (envp[n]) n++;
	printf("%d environment strings\n", n);
	if (argc > 1 && !strcmp(argv[1], "-r") && fgets(line, sizeof line, stdin)) printf("stdin: %s", line);
	return 0;
}
