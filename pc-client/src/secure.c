#include "secure.h"

#include <stdio.h>
#include <string.h>

#include "../third_party/monocypher/monocypher-ed25519.h"
#include "../third_party/monocypher/monocypher.h"

static const char INFO[] = "SoftKVM v1 session";

void sk_wipe(void *p, size_t n) { crypto_wipe(p, n); }

void sk_keypair_complete(SkKeyPair *kp) { crypto_x25519_public_key(kp->pub, kp->priv); }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

size_t sk_hello_encode(const SkHello *h, uint8_t *out, size_t cap) {
    size_t nl = strlen(h->name), vl = strlen(h->version);
    if (nl > 255) nl = 255;
    if (vl > 255) vl = 255;
    size_t need = 4 + 4 + 64 + 1 + nl + 1 + vl + 1;
    if (need > cap) return 0;
    size_t o = 0;
    memcpy(out, "SKVM", 4), o += 4;
    put16(out + o, h->proto), o += 2;
    put16(out + o, h->status), o += 2;
    memcpy(out + o, h->static_pub, 32), o += 32;
    memcpy(out + o, h->eph_pub, 32), o += 32;
    out[o++] = (uint8_t)nl;
    memcpy(out + o, h->name, nl), o += nl;
    out[o++] = (uint8_t)vl;
    memcpy(out + o, h->version, vl), o += vl;
    out[o++] = h->platform;
    return o;
}

int sk_hello_decode(const uint8_t *in, size_t len, SkHello *h) {
    memset(h, 0, sizeof *h);
    if (len < 4 + 4 + 64 + 3 || memcmp(in, "SKVM", 4) != 0) return -1;
    size_t o = 4;
    h->proto = get16(in + o), o += 2;
    h->status = get16(in + o), o += 2;
    memcpy(h->static_pub, in + o, 32), o += 32;
    memcpy(h->eph_pub, in + o, 32), o += 32;
    size_t n = in[o++];
    if (o + n >= len) return -1;
    memcpy(h->name, in + o, n), o += n;
    n = in[o++];
    if (o + n >= len) return -1;
    memcpy(h->version, in + o, n), o += n;
    h->platform = in[o];
    return 0;
}

void sk_client_keys(SkCipher *c, const uint8_t *client_hello, size_t chlen, const uint8_t *host_hello,
                    size_t hhlen, const SkKeyPair *client_static, const SkKeyPair *client_eph,
                    const SkHello *host) {
    uint8_t ikm[96];
    crypto_x25519(ikm, client_eph->priv, host->eph_pub);          // ee
    crypto_x25519(ikm + 32, client_static->priv, host->eph_pub);  // se: client static × host ephemeral
    crypto_x25519(ikm + 64, client_eph->priv, host->static_pub);  // es: client ephemeral × host static

    // salt = SHA-512(ClientHello ‖ HostHello)
    uint8_t salt[64];
    crypto_sha512_ctx sh;
    crypto_sha512_init(&sh);
    crypto_sha512_update(&sh, client_hello, chlen);
    crypto_sha512_update(&sh, host_hello, hhlen);
    crypto_sha512_final(&sh, salt);

    uint8_t okm[68];
    crypto_sha512_hkdf(okm, sizeof okm, ikm, sizeof ikm, salt, sizeof salt, (const uint8_t *)INFO,
                       sizeof INFO - 1);
    memset(c, 0, sizeof *c);
    memcpy(c->send_key, okm, 32);       // client → host
    memcpy(c->recv_key, okm + 32, 32);  // host → client
    uint32_t code = (uint32_t)okm[64] | (uint32_t)okm[65] << 8 | (uint32_t)okm[66] << 16 | (uint32_t)okm[67] << 24;
    code %= 1000000;
    snprintf(c->code, sizeof c->code, "%03u %03u", code / 1000, code % 1000);
    crypto_wipe(ikm, sizeof ikm);
    crypto_wipe(okm, sizeof okm);
}

static void nonce_for(uint64_t ctr, uint8_t n[12]) {
    memset(n, 0, 4);
    for (int i = 0; i < 8; i++) n[4 + i] = (uint8_t)(ctr >> (8 * i));
}

size_t sk_seal(SkCipher *c, const uint8_t *plain, size_t len, uint8_t *out) {
    uint32_t rl = (uint32_t)(len + SK_TAG);
    out[0] = (uint8_t)rl, out[1] = (uint8_t)(rl >> 8), out[2] = (uint8_t)(rl >> 16), out[3] = (uint8_t)(rl >> 24);
    uint8_t nonce[12];
    nonce_for(c->send_ctr++, nonce);
    crypto_aead_ctx ctx;  // fresh context per record = standard RFC 8439 ChaCha20-Poly1305
    crypto_aead_init_ietf(&ctx, c->send_key, nonce);
    crypto_aead_write(&ctx, out + 4, out + 4 + len, out, 4, plain, len);
    crypto_wipe(&ctx, sizeof ctx);
    return 4 + len + SK_TAG;
}

long sk_open(SkCipher *c, const uint8_t header[4], uint8_t *body, size_t n) {
    if (n < SK_TAG) return -1;
    size_t len = n - SK_TAG;
    uint8_t nonce[12];
    nonce_for(c->recv_ctr, nonce);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, c->recv_key, nonce);
    int r = crypto_aead_read(&ctx, body, body + len, header, 4, body, len);
    crypto_wipe(&ctx, sizeof ctx);
    if (r != 0) return -1;
    c->recv_ctr++;
    return (long)len;
}

void sk_device_id(const uint8_t pub[32], char out[17]) {
    uint8_t h[64];
    crypto_sha512(h, pub, 32);
    for (int i = 0; i < 8; i++) snprintf(out + i * 2, 3, "%02x", h[i]);
}
