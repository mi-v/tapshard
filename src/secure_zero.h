/* Sensitive-memory clearing that the compiler must not optimize away. */

#ifndef SECURE_ZERO_H
#define SECURE_ZERO_H

#include <stddef.h>
#include <stdint.h>

static inline void secure_zero(void *ptr, size_t len)
{
	volatile uint8_t *p = ptr;

	while (len-- > 0) {
		*p++ = 0;
	}
}

#endif /* SECURE_ZERO_H */
