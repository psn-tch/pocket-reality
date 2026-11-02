#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
	/* Touches libc, soft float, 64-bit maths and the clock. */
	volatile double d = 355.0 / 113.0;
	volatile long long big = 1234567890123LL;
	struct timespec ts;
	char *p = malloc(1 << 20);

	memset(p, 0x5a, 1 << 20);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("hello from sh3: argc=%d d=%.5f big/7=%lld mem=%x\n", argc, d, big / 7, p[4096]);
	free(p);
	return 0;
}
