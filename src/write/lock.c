#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lock.h"
#include "monocypher.h"

#define MAGIC "PKTLOCK1"
#define MAGIC_LEN 8
#define OFF_BLOCKS 8
#define OFF_PASSES 12
#define OFF_SALT 16
#define OFF_CHECK 32
#define OFF_NONCE 48
#define OFF_MAC 72

#define PASSES 3
#define TRIAL_BLOCKS 32
#define TARGET_MS 2000
#define MIN_BLOCKS 256		/* the floor, even if the clock says otherwise */
#define MAX_BLOCKS 4096		/* 4 MB of a 14 MB machine */
#define FILE_MAX_BLOCKS 8192	/* anything larger is a damaged header */
#define FILE_MAX_PASSES 8

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int random_bytes(uint8_t *out, size_t len)
{
	int fd = open("/dev/urandom", O_RDONLY);
	size_t got = 0;

	if (fd < 0)
		return -1;
	while (got < len) {
		ssize_t n = read(fd, out + got, len - got);

		if (n <= 0)
			break;
		got += n;
	}
	close(fd);
	return got == len ? 0 : -1;
}

static int derive(struct lock *lock, const char *password)
{
	crypto_argon2_config config = { CRYPTO_ARGON2_ID, lock->blocks, lock->passes, 1 };
	crypto_argon2_inputs inputs = { (const uint8_t *)password, lock->salt,
					strlen(password), sizeof lock->salt };
	uint8_t out[48];
	void *work = malloc((size_t)lock->blocks * 1024);

	if (!work)
		return -1;
	crypto_argon2(out, sizeof out, work, config, inputs, crypto_argon2_no_extras);
	memcpy(lock->key, out, 32);
	memcpy(lock->check, out + 32, 16);
	crypto_wipe(out, sizeof out);
	crypto_wipe(work, (size_t)lock->blocks * 1024);
	free(work);
	return 0;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ll + ts.tv_nsec / 1000000;
}

/* The Jornada's speed sets the cost, about two seconds to open a story. */
static uint32_t pick_blocks(void)
{
	struct lock trial;
	long long start, spent;
	long long blocks;

	memset(&trial, 0, sizeof trial);
	trial.blocks = TRIAL_BLOCKS;
	trial.passes = PASSES;
	start = now_ms();
	if (derive(&trial, "trial"))
		return MIN_BLOCKS;
	spent = now_ms() - start;
	crypto_wipe(&trial, sizeof trial);
	if (spent < 1)
		spent = 1;
	blocks = TRIAL_BLOCKS * TARGET_MS / spent;
	if (blocks < MIN_BLOCKS)
		return MIN_BLOCKS;
	return blocks > MAX_BLOCKS ? MAX_BLOCKS : blocks;
}

int lock_is_locked(const void *file, size_t len)
{
	return len >= MAGIC_LEN && !memcmp(file, MAGIC, MAGIC_LEN);
}

int lock_new(struct lock *lock, const char *password)
{
	memset(lock, 0, sizeof *lock);
	lock->blocks = pick_blocks();
	lock->passes = PASSES;
	if (random_bytes(lock->salt, sizeof lock->salt) || derive(lock, password))
		return -1;
	lock->ready = 1;
	lock->on = 1;
	return 0;
}

int lock_try(struct lock *lock, const char *password, const uint8_t *file, size_t len)
{
	uint32_t blocks, passes;

	if (len < LOCK_HEADER)
		return LOCK_DAMAGED;
	blocks = get32(file + OFF_BLOCKS);
	passes = get32(file + OFF_PASSES);
	if (blocks < 8 || blocks > FILE_MAX_BLOCKS || !passes || passes > FILE_MAX_PASSES)
		return LOCK_DAMAGED;
	/* A story and its side files share a salt, so this costs one derivation. */
	if (!lock->ready || blocks != lock->blocks || passes != lock->passes ||
	    memcmp(lock->salt, file + OFF_SALT, sizeof lock->salt)) {
		lock->blocks = blocks;
		lock->passes = passes;
		memcpy(lock->salt, file + OFF_SALT, sizeof lock->salt);
		if (derive(lock, password))
			return LOCK_DAMAGED;
		lock->ready = 1;
	}
	return crypto_verify16(lock->check, file + OFF_CHECK) ? LOCK_WRONG : LOCK_OK;
}

void lock_retry(struct lock *lock)
{
	lock->ready = 0;
}

int lock_open(const struct lock *lock, const uint8_t *file, size_t len, uint8_t *out)
{
	if (len < LOCK_HEADER)
		return LOCK_DAMAGED;
	return crypto_aead_unlock(out, file + OFF_MAC, lock->key, file + OFF_NONCE, file,
				  OFF_NONCE, file + LOCK_HEADER, len - LOCK_HEADER) ?
		LOCK_DAMAGED : LOCK_OK;
}

int lock_seal(const struct lock *lock, const void *plain, size_t len, uint8_t *out)
{
	memcpy(out, MAGIC, MAGIC_LEN);
	put32(out + OFF_BLOCKS, lock->blocks);
	put32(out + OFF_PASSES, lock->passes);
	memcpy(out + OFF_SALT, lock->salt, sizeof lock->salt);
	memcpy(out + OFF_CHECK, lock->check, sizeof lock->check);
	/* Random, so a save never reuses a nonce under the same key. */
	if (random_bytes(out + OFF_NONCE, 24))
		return -1;
	crypto_aead_lock(out + LOCK_HEADER, out + OFF_MAC, lock->key, out + OFF_NONCE, out,
			 OFF_NONCE, plain, len);
	return 0;
}

int lock_matches(const struct lock *lock, const char *password)
{
	struct lock test = *lock;
	int same;

	if (derive(&test, password))
		return 0;
	same = !crypto_verify16(test.check, lock->check);
	crypto_wipe(&test, sizeof test);
	return same;
}

void lock_forget(struct lock *lock)
{
	crypto_wipe(lock, sizeof *lock);
}
