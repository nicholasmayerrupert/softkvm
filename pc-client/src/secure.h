#pragma once
// SoftKVM handshake and record encryption (client side). Portable C; see docs/PROTOCOL.md.

#include <stddef.h>
#include <stdint.h>

#define SK_PROTO_VERSION 2
#define SK_MAX_HELLO 4096
#define SK_TAG 16

enum { SK_PLATFORM_WINDOWS = 1, SK_PLATFORM_MAC = 2, SK_PLATFORM_LINUX = 3 };
enum { SK_STATUS_OK = 0, SK_STATUS_VERSION_MISMATCH = 1 };

typedef struct {
    uint8_t priv[32];
    uint8_t pub[32];
} SkKeyPair;

typedef struct {
    uint16_t proto, status;
    uint8_t static_pub[32];
    uint8_t eph_pub[32];
    char name[256];
    char version[256];
    uint8_t platform;
} SkHello;

typedef struct {
    uint8_t send_key[32];
    uint8_t recv_key[32];
    uint64_t send_ctr, recv_ctr;
    char code[8];  // 6-digit pairing code, "123 456"
} SkCipher;

/// Derives the public key for kp->priv.
void sk_keypair_complete(SkKeyPair *kp);

size_t sk_hello_encode(const SkHello *h, uint8_t *out, size_t cap);
int sk_hello_decode(const uint8_t *in, size_t len, SkHello *h);

/// Client-side key derivation from both hello bodies (without their length prefixes).
void sk_client_keys(SkCipher *c, const uint8_t *client_hello, size_t chlen, const uint8_t *host_hello,
                    size_t hhlen, const SkKeyPair *client_static, const SkKeyPair *client_eph,
                    const SkHello *host);

/// Encrypts one message into `out` (needs 4 + len + 16 bytes). Returns the record size.
size_t sk_seal(SkCipher *c, const uint8_t *plain, size_t len, uint8_t *out);

/// Decrypts a record body (ciphertext ‖ tag, n bytes) in place. `header` is its 4-byte length.
/// Returns the plaintext length, or -1 if authentication fails.
long sk_open(SkCipher *c, const uint8_t header[4], uint8_t *body, size_t n);

/// v2 commitment to a ClientHello: "SKVM", u16 proto, u16 0, SHA-512(hello). Returns 72.
size_t sk_commit(const uint8_t *client_hello, size_t len, uint8_t out[72]);

/// Short stable id for a public key: 16 hex chars + NUL.
void sk_device_id(const uint8_t pub[32], char out[17]);

void sk_wipe(void *p, size_t n);
