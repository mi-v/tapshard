/* TOTP (RFC 6238) + Base32 decode — public API */

#ifndef TOTP_H
#define TOTP_H

#include <stdint.h>
#include <stddef.h>

/* Initialize the crypto backend (call once before generating codes). */
int totp_init(void);

/* Decode RFC 4648 Base32 (tolerant of lowercase, separators and padding).
 * Returns number of bytes written to @out, or -1 on error. */
int base32_decode(const char *b32, uint8_t *out, size_t out_size);

/* Generate a TOTP code for @time (Unix seconds).
 * Writes a zero-padded decimal string (length @digits) to @out.
 * Returns 0 on success, negative on error. */
int totp_generate(const uint8_t *secret, size_t secret_len,
		  uint64_t time, uint32_t period, uint8_t digits,
		  char *out, size_t out_size);

/* Self-test against RFC 6238 Appendix B vectors. Returns 0 on success. */
int totp_selftest(void);

#endif /* TOTP_H */
