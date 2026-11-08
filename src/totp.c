/*
 * TOTP (RFC 6238) with HMAC-SHA1 (RFC 4226 dynamic truncation)
 * and RFC 4648 Base32 decoding, via the PSA crypto API.
 */

#include <string.h>

#include <zephyr/sys/util.h>
#include <psa/crypto.h>

#include "totp.h"
#include "secure_zero.h"

#define SHA1_DIGEST_LEN 20

static int crypto_ready;

int totp_init(void)
{
	if (crypto_ready) {
		return 0;
	}

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -1;
	}

	crypto_ready = 1;
	return 0;
}

int base32_decode(const char *b32, uint8_t *out, size_t out_size)
{
	uint32_t buf = 0;
	int bits = 0;
	int result;
	size_t n = 0;
	const char *p = b32;

	while (*p) {
		char c = *p++;
		int v;

		if (c == '=') {
			break; /* padding */
		}
		if (c == ' ' || c == '-' || c == '\n' || c == '\r' ||
		    c == '\t') {
			continue; /* skip separators */
		}
		if (c >= 'A' && c <= 'Z') {
			v = c - 'A';
		} else if (c >= 'a' && c <= 'z') {
			v = c - 'a';
		} else if (c >= '2' && c <= '7') {
			v = c - '2' + 26;
		} else if (c == '0') {
			v = 14; /* tolerate 0 -> O */
		} else if (c == '1') {
			v = 8; /* tolerate 1 -> I */
		} else {
			secure_zero(out, out_size);
			result = -1;
			goto out; /* invalid character */
		}

		buf = (buf << 5) | (uint32_t)v;
		bits += 5;
		if (bits >= 8) {
			bits -= 8;
			if (n >= out_size) {
				secure_zero(out, out_size);
				result = -1;
				goto out;
			}
			out[n++] = (uint8_t)((buf >> bits) & 0xFF);
		}
	}

	result = (int)n;
out:
	secure_zero(&buf, sizeof(buf));
	return result;
}

int totp_generate(const uint8_t *secret, size_t secret_len,
		  uint64_t time, uint32_t period, uint8_t digits,
		  char *out, size_t out_size)
{
	uint8_t counter_bytes[8];
	uint8_t mac[SHA1_DIGEST_LEN];
	size_t mac_len = 0;
	uint64_t counter;
	psa_key_attributes_t attrs;
	mbedtls_svc_key_id_t key_id;
	psa_status_t status;
	uint32_t bin;
	uint32_t mod = 1;
	uint32_t code;
	int i;
	int result = -1;

	if (totp_init() != 0) {
		return -1;
	}

	if (period == 0 || digits == 0 || digits > 9 ||
	    out_size < (size_t)digits + 1) {
		return -1;
	}

	counter = time / period;
	for (i = 7; i >= 0; i--) {
		counter_bytes[i] = (uint8_t)(counter & 0xFF);
		counter >>= 8;
	}

	attrs = psa_key_attributes_init();
	psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
	psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_1));

	status = psa_import_key(&attrs, secret, secret_len, &key_id);
	if (status != PSA_SUCCESS) {
		goto out;
	}

	status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_1),
				 counter_bytes, sizeof(counter_bytes),
				 mac, sizeof(mac), &mac_len);

	psa_destroy_key(key_id);

	if (status != PSA_SUCCESS || mac_len != SHA1_DIGEST_LEN) {
		goto out;
	}

	/* Dynamic truncation (RFC 4226 5.3). */
	i = mac[SHA1_DIGEST_LEN - 1] & 0x0f;
	bin = ((uint32_t)(mac[i] & 0x7f) << 24) |
	      ((uint32_t)mac[i + 1] << 16) |
	      ((uint32_t)mac[i + 2] << 8) |
	      ((uint32_t)mac[i + 3]);

	for (i = 0; i < digits; i++) {
		mod *= 10;
	}
	code = bin % mod;

	/* Zero-padded decimal. */
	for (i = digits - 1; i >= 0; i--) {
		out[i] = (char)('0' + (code % 10));
		code /= 10;
	}
	out[digits] = '\0';
	result = 0;

out:
	secure_zero(counter_bytes, sizeof(counter_bytes));
	secure_zero(mac, sizeof(mac));
	secure_zero(&bin, sizeof(bin));
	secure_zero(&code, sizeof(code));
	return result;
}

/* RFC 6238 Appendix B test vectors. */
static const struct {
	uint64_t time;
	const char *code6;
	const char *code8;
} test_vectors[] = {
	{ 59ULL,          "287082",   "94287082" },
	{ 1111111109ULL,  "081804",   "07081804" },
	{ 1111111111ULL,  "050471",   "14050471" },
	{ 1234567890ULL,  "005924",   "89005924" },
	{ 2000000000ULL,  "279037",   "69279037" },
	{ 20000000000ULL, "353130",   "65353130" },
};

int totp_selftest(void)
{
	static const char b32[] = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
	static const char expected[] = "12345678901234567890";
	uint8_t secret[20];
	uint8_t overflow[1] = { 0xa5 };
	char code[16];
	int n;
	size_t i;

	if (totp_init() != 0) {
		return -1;
	}
	if (base32_decode("AAAA", overflow, sizeof(overflow)) >= 0 ||
	    overflow[0] != 0) {
		return -1;
	}

	n = base32_decode(b32, secret, sizeof(secret));
	if (n != (int)sizeof(secret) ||
	    memcmp(secret, expected, sizeof(secret)) != 0) {
		return -1;
	}

	for (i = 0; i < ARRAY_SIZE(test_vectors); i++) {
		if (totp_generate(secret, sizeof(secret),
				  test_vectors[i].time, 30, 6,
				  code, sizeof(code)) != 0 ||
		    strcmp(code, test_vectors[i].code6) != 0) {
			return -1;
		}
		if (totp_generate(secret, sizeof(secret),
				  test_vectors[i].time, 30, 8,
				  code, sizeof(code)) != 0 ||
		    strcmp(code, test_vectors[i].code8) != 0) {
			return -1;
		}
	}

	return 0;
}
