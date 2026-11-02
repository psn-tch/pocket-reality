/*
 * Password locked files.
 *
 * A locked file is a header followed by the story, encrypted with XChaCha20 and
 * Poly1305 under a key made from the password with Argon2id. The header keeps a
 * value derived from the password, which tells a wrong password from a damaged
 * file. There is no way back from a forgotten password.
 */
#ifndef LOCK_H
#define LOCK_H

#include <stddef.h>
#include <stdint.h>

#define LOCK_HEADER 88
#define LOCK_MAX_PASSWORD 64

enum { LOCK_OK, LOCK_WRONG, LOCK_DAMAGED };

struct lock {
	uint8_t key[32];
	uint8_t check[16];
	uint8_t salt[16];
	uint32_t blocks;
	uint32_t passes;
	int ready;		/* key belongs to the password last tried */
	int on;			/* saves are encrypted with key */
};

int lock_is_locked(const void *file, size_t len);

/* Fresh salt and key for a new password. Returns 0 on success. */
int lock_new(struct lock *lock, const char *password);

/* Tests a password against a locked file. Forget the key with lock_retry. */
int lock_try(struct lock *lock, const char *password, const uint8_t *file, size_t len);
void lock_retry(struct lock *lock);

/* out holds len - LOCK_HEADER bytes. Returns LOCK_OK or LOCK_DAMAGED. */
int lock_open(const struct lock *lock, const uint8_t *file, size_t len, uint8_t *out);

/* out holds LOCK_HEADER + len bytes. Returns 0 on success. */
int lock_seal(const struct lock *lock, const void *plain, size_t len, uint8_t *out);

/* Returns 1 if password is the one this lock was made from. */
int lock_matches(const struct lock *lock, const char *password);

void lock_forget(struct lock *lock);

#endif
