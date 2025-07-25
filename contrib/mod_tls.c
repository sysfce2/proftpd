/*
 * mod_tls - An RFC2228 SSL/TLS module for ProFTPD
 *
 * Copyright (c) 2000-2002 Peter 'Luna' Runestig <peter@runestig.com>
 * Copyright (c) 2002-2025 TJ Saunders <tj@castaglia.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modifi-
 * cation, are permitted provided that the following conditions are met:
 *
 *    o Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *
 *    o Redistributions in binary form must reproduce the above copyright no-
 *      tice, this list of conditions and the following disclaimer in the do-
 *      cumentation and/or other materials provided with the distribution.
 *
 *    o The names of the contributors may not be used to endorse or promote
 *      products derived from this software without specific prior written
 *      permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LI-
 * ABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUEN-
 * TIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEV-
 * ER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABI-
 * LITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  --- DO NOT DELETE BELOW THIS LINE ----
 *  $Libraries: -lssl -lcrypto$
 */

#include "conf.h"
#include "privs.h"
#include "mod_tls.h"

#ifdef PR_USE_CTRLS
# include "mod_ctrls.h"
#endif

/* Define if you have the LibreSSL library.
 *
 * Note that in LibreSSL-3.5.0, the structs became opaque, as they are in
 * OpenSSL-1.1.0, and thus these version-dependent macros became more
 * complex.
 */
#if defined(LIBRESSL_VERSION_NUMBER)
# define HAVE_LIBRESSL	1
#endif

/* Note that the openssl/ssl.h header is already included in mod_tls.h, so
 * we don't need to include it here.
*/

#include <openssl/err.h>
#include <openssl/conf.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/dh.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/ssl3.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#if OPENSSL_VERSION_NUMBER > 0x000907000L
# ifdef PR_USE_OPENSSL_ENGINE
#  include <openssl/engine.h>
# endif /* PR_USE_OPENSSL_ENGINE */
# ifdef PR_USE_OPENSSL_OCSP
#  include <openssl/ocsp.h>
# endif /* PR_USE_OPENSSL_OCSP */
#endif
#ifdef PR_USE_OPENSSL_ECC
# include <openssl/ec.h>
# include <openssl/ecdh.h>
#endif /* PR_USE_OPENSSL_ECC */

#ifdef HAVE_MLOCK
# include <sys/mman.h>
#endif

#define MOD_TLS_VERSION		"mod_tls/2.9.2"

/* Make sure the version of proftpd is as necessary. */
#if PROFTPD_VERSION_NUMBER < 0x0001030602
# error "ProFTPD 1.3.6rc2 or later required"
#endif

extern pr_response_t *resp_list, *resp_err_list;
extern session_t session;
extern xaset_t *server_list;
extern int ServerUseReverseDNS;

static const char *trace_channel = "tls";

static DH *get_dh(BIGNUM *p, BIGNUM *g) {
  DH *dh;

  dh = DH_new();
  if (dh == NULL) {
    return NULL;
  }

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
  if (DH_set0_pqg(dh, p, NULL, g) != 1) {
    pr_trace_msg(trace_channel, 3, "error setting DH p/q parameters: %s",
      ERR_error_string(ERR_get_error(), NULL));
    DH_free(dh);
    return NULL;
  }
#else
  dh->p = p;
  dh->g = g;
#endif /* OpenSSL 1.1.x/LibreSSL-3.5.x and later */

  return dh;
}

static X509 *read_cert(FILE *fh, SSL_CTX *ctx) {
  pem_password_cb *cb;
  void *cb_data;

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
  cb = SSL_CTX_get_default_passwd_cb(ctx);
  cb_data = SSL_CTX_get_default_passwd_cb_userdata(ctx);
#else
  cb = ctx->default_passwd_callback;
  cb_data = ctx->default_passwd_callback_userdata;
#endif /* OpenSSL-1.1.x/LibreSSL-3.5.x and later */

  return PEM_read_X509(fh, NULL, cb, cb_data);
}

static int get_pkey_type(EVP_PKEY *pkey) {
  int pkey_type;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESS)
  pkey_type = EVP_PKEY_base_id(pkey);
#else
  pkey_type = EVP_PKEY_type(pkey->type);
#endif /* OpenSSL 1.1.x and later */

  return pkey_type;
}

/* DH parameters.  These are generated using:
 *
 *  # openssl dhparam -2|-5 512|768|1024|1536|2048|3072|4096 -C
 *
 * These should be regenerated periodically by the mod_tls maintainer.
 * Last updated on 2025-04-17.
 */

/*
-----BEGIN DH PARAMETERS-----
MEYCQQD5M5sxqPFYS9cuIDNbUontBgEEP0nNzw+URM1cL4nWFC1DKA2XmxKrV2dF
pJR5Tvv4ugDhCsOgZ4HczjsMy6zjAgEC
-----END DH PARAMETERS-----
*/

static unsigned char dh512_p[] = {
  0xF9, 0x33, 0x9B, 0x31, 0xA8, 0xF1, 0x58, 0x4B, 0xD7, 0x2E, 0x20, 0x33,
  0x5B, 0x52, 0x89, 0xED, 0x06, 0x01, 0x04, 0x3F, 0x49, 0xCD, 0xCF, 0x0F,
  0x94, 0x44, 0xCD, 0x5C, 0x2F, 0x89, 0xD6, 0x14, 0x2D, 0x43, 0x28, 0x0D,
  0x97, 0x9B, 0x12, 0xAB, 0x57, 0x67, 0x45, 0xA4, 0x94, 0x79, 0x4E, 0xFB,
  0xF8, 0xBA, 0x00, 0xE1, 0x0A, 0xC3, 0xA0, 0x67, 0x81, 0xDC, 0xCE, 0x3B,
  0x0C, 0xCB, 0xAC, 0xE3
};

static unsigned char dh512_g[] = { 0x02 };

static DH *get_dh512(void) {
  BIGNUM *p, *g;

  p = BN_bin2bn(dh512_p, sizeof(dh512_p), NULL);
  g = BN_bin2bn(dh512_g, sizeof(dh512_g), NULL);
  if (p == NULL ||
      g == NULL) {
    return NULL;
  }

  return get_dh(p, g);
}

/*
-----BEGIN DH PARAMETERS-----
MGYCYQDaHgRO6KPA1t5P6L5LjjlZOo9HtaqNlVUWfM3k0cBh+iUp0DeBScdpEhR1
F+0z1LvbOxuF3d2yhUcx8EU02CWEfaJBRsE+JUPnqJghn/7vyp9QFlPAtiOz0Txc
/QbFVJsCAQU=
-----END DH PARAMETERS-----
*/

static unsigned char dh768_p[] = {
  0xDA, 0x1E, 0x04, 0x4E, 0xE8, 0xA3, 0xC0, 0xD6, 0xDE, 0x4F, 0xE8, 0xBE,
  0x4B, 0x8E, 0x39, 0x59, 0x3A, 0x8F, 0x47, 0xB5, 0xAA, 0x8D, 0x95, 0x55,
  0x16, 0x7C, 0xCD, 0xE4, 0xD1, 0xC0, 0x61, 0xFA, 0x25, 0x29, 0xD0, 0x37,
  0x81, 0x49, 0xC7, 0x69, 0x12, 0x14, 0x75, 0x17, 0xED, 0x33, 0xD4, 0xBB,
  0xDB, 0x3B, 0x1B, 0x85, 0xDD, 0xDD, 0xB2, 0x85, 0x47, 0x31, 0xF0, 0x45,
  0x34, 0xD8, 0x25, 0x84, 0x7D, 0xA2, 0x41, 0x46, 0xC1, 0x3E, 0x25, 0x43,
  0xE7, 0xA8, 0x98, 0x21, 0x9F, 0xFE, 0xEF, 0xCA, 0x9F, 0x50, 0x16, 0x53,
  0xC0, 0xB6, 0x23, 0xB3, 0xD1, 0x3C, 0x5C, 0xFD, 0x06, 0xC5, 0x54, 0x9B
};

static unsigned char dh768_g[] = { 0x05 };

static DH *get_dh768(void) {
  BIGNUM *p, *g;

  p = BN_bin2bn(dh768_p, sizeof(dh768_p), NULL);
  g = BN_bin2bn(dh768_g, sizeof(dh768_g), NULL);
  if (p == NULL ||
      g == NULL) {
    return NULL;
  }

  return get_dh(p, g);
}

/*
-----BEGIN DH PARAMETERS-----
MIGHAoGBAP2NO9bx296PCuRL0I2bgK3cCx0360i/hgT09yB2Bz2QDx8NB52hR2Wh
fH/O/i7B83ln0jSfBx1vo4iJxlMplvgCF26oRJh8pfPG8T+YGSHwXkycqyVqF0Hx
YCdqtqiKM+V+vo3FnOLu0XqLTLiGoZeVl2FV3E4waW/tcjnKG84rAgEC
-----END DH PARAMETERS-----
*/

static unsigned char dh1024_p[] = {
  0xFD, 0x8D, 0x3B, 0xD6, 0xF1, 0xDB, 0xDE, 0x8F, 0x0A, 0xE4, 0x4B, 0xD0,
  0x8D, 0x9B, 0x80, 0xAD, 0xDC, 0x0B, 0x1D, 0x37, 0xEB, 0x48, 0xBF, 0x86,
  0x04, 0xF4, 0xF7, 0x20, 0x76, 0x07, 0x3D, 0x90, 0x0F, 0x1F, 0x0D, 0x07,
  0x9D, 0xA1, 0x47, 0x65, 0xA1, 0x7C, 0x7F, 0xCE, 0xFE, 0x2E, 0xC1, 0xF3,
  0x79, 0x67, 0xD2, 0x34, 0x9F, 0x07, 0x1D, 0x6F, 0xA3, 0x88, 0x89, 0xC6,
  0x53, 0x29, 0x96, 0xF8, 0x02, 0x17, 0x6E, 0xA8, 0x44, 0x98, 0x7C, 0xA5,
  0xF3, 0xC6, 0xF1, 0x3F, 0x98, 0x19, 0x21, 0xF0, 0x5E, 0x4C, 0x9C, 0xAB,
  0x25, 0x6A, 0x17, 0x41, 0xF1, 0x60, 0x27, 0x6A, 0xB6, 0xA8, 0x8A, 0x33,
  0xE5, 0x7E, 0xBE, 0x8D, 0xC5, 0x9C, 0xE2, 0xEE, 0xD1, 0x7A, 0x8B, 0x4C,
  0xB8, 0x86, 0xA1, 0x97, 0x95, 0x97, 0x61, 0x55, 0xDC, 0x4E, 0x30, 0x69,
  0x6F, 0xED, 0x72, 0x39, 0xCA, 0x1B, 0xCE, 0x2B
};

static unsigned char dh1024_g[] = { 0x02 };

static DH *get_dh1024(void) {
  BIGNUM *p, *g;

  p = BN_bin2bn(dh1024_p, sizeof(dh1024_p), NULL);
  g = BN_bin2bn(dh1024_g, sizeof(dh1024_g), NULL);
  if (p == NULL ||
      g == NULL) {
    return NULL;
  }

  return get_dh(p, g);
}

/*
-----BEGIN DH PARAMETERS-----
MIHHAoHBANdLgd6x6/N0JAtvlbg1iWUZ7otEjuOFXs6D5UhG8YNg2suMFpAS3ZgE
aoatUcHbGCzWy7GHY0XdQO3XrKBuDNO/3W1Qpo+mJiTqBDWxWFPCkY2OFjQrEutU
Bv6FN1u0DUX+pipTJTyTipTvHpfupJ3uSzrsokrsWAILgD3E7QQeGiyylGXvxs6R
ouCe2Uv17FunYUDH5dXVuLUXYPt6bmlk41oJxIo9/3Fm1yXnMZhJ6+wt4N7jt1ac
Ji+ft9kdTwIBBQ==
-----END DH PARAMETERS-----
*/

static unsigned char dh1536_p[] = {
  0xD7, 0x4B, 0x81, 0xDE, 0xB1, 0xEB, 0xF3, 0x74, 0x24, 0x0B, 0x6F, 0x95,
  0xB8, 0x35, 0x89, 0x65, 0x19, 0xEE, 0x8B, 0x44, 0x8E, 0xE3, 0x85, 0x5E,
  0xCE, 0x83, 0xE5, 0x48, 0x46, 0xF1, 0x83, 0x60, 0xDA, 0xCB, 0x8C, 0x16,
  0x90, 0x12, 0xDD, 0x98, 0x04, 0x6A, 0x86, 0xAD, 0x51, 0xC1, 0xDB, 0x18,
  0x2C, 0xD6, 0xCB, 0xB1, 0x87, 0x63, 0x45, 0xDD, 0x40, 0xED, 0xD7, 0xAC,
  0xA0, 0x6E, 0x0C, 0xD3, 0xBF, 0xDD, 0x6D, 0x50, 0xA6, 0x8F, 0xA6, 0x26,
  0x24, 0xEA, 0x04, 0x35, 0xB1, 0x58, 0x53, 0xC2, 0x91, 0x8D, 0x8E, 0x16,
  0x34, 0x2B, 0x12, 0xEB, 0x54, 0x06, 0xFE, 0x85, 0x37, 0x5B, 0xB4, 0x0D,
  0x45, 0xFE, 0xA6, 0x2A, 0x53, 0x25, 0x3C, 0x93, 0x8A, 0x94, 0xEF, 0x1E,
  0x97, 0xEE, 0xA4, 0x9D, 0xEE, 0x4B, 0x3A, 0xEC, 0xA2, 0x4A, 0xEC, 0x58,
  0x02, 0x0B, 0x80, 0x3D, 0xC4, 0xED, 0x04, 0x1E, 0x1A, 0x2C, 0xB2, 0x94,
  0x65, 0xEF, 0xC6, 0xCE, 0x91, 0xA2, 0xE0, 0x9E, 0xD9, 0x4B, 0xF5, 0xEC,
  0x5B, 0xA7, 0x61, 0x40, 0xC7, 0xE5, 0xD5, 0xD5, 0xB8, 0xB5, 0x17, 0x60,
  0xFB, 0x7A, 0x6E, 0x69, 0x64, 0xE3, 0x5A, 0x09, 0xC4, 0x8A, 0x3D, 0xFF,
  0x71, 0x66, 0xD7, 0x25, 0xE7, 0x31, 0x98, 0x49, 0xEB, 0xEC, 0x2D, 0xE0,
  0xDE, 0xE3, 0xB7, 0x56, 0x9C, 0x26, 0x2F, 0x9F, 0xB7, 0xD9, 0x1D, 0x4F
};

static unsigned char dh1536_g[] = { 0x05 };

static DH *get_dh1536(void) {
  BIGNUM *p, *g;

  p = BN_bin2bn(dh1536_p, sizeof(dh1536_p), NULL);
  g = BN_bin2bn(dh1536_g, sizeof(dh1536_g), NULL);
  if (p == NULL ||
      g == NULL) {
    return NULL;
  }

  return get_dh(p, g);
}

/*
-----BEGIN DH PARAMETERS-----
MIIBCAKCAQEAivjznJh+4PWfaUytA8RyGLO6ycnq1VSg8lywLHL2m5WU+OgMeTgj
dyKZmV/YsBhrywgy66XAbXlBX01ncIY4YCbefKQ/KZc9tEgkO6NqXlrgudItmkQA
LkcdIG2ca4lt0MZQA6+Z7KkLHZv/aFeMbUpKyj38d6F0F4YYAEbzb95BtJ/ZCRzS
1iWeuxyfvrD3AxHjNiw85qgSmOIPoC2q7dnPUfktmo875P03lJf1GZo2nQ74LbHY
moPMI+oKAGHjthjYDRKs9NTqwtagnA0yR05sNzCTKUPWCsspelH5xD8WNn2o2yEu
c1tNSKB8L93TggWeQXrx7h9gFqagdktiQwIBAg==
-----END DH PARAMETERS-----
*/

static unsigned char dh2048_p[] = {
  0x8A, 0xF8, 0xF3, 0x9C, 0x98, 0x7E, 0xE0, 0xF5, 0x9F, 0x69, 0x4C, 0xAD,
  0x03, 0xC4, 0x72, 0x18, 0xB3, 0xBA, 0xC9, 0xC9, 0xEA, 0xD5, 0x54, 0xA0,
  0xF2, 0x5C, 0xB0, 0x2C, 0x72, 0xF6, 0x9B, 0x95, 0x94, 0xF8, 0xE8, 0x0C,
  0x79, 0x38, 0x23, 0x77, 0x22, 0x99, 0x99, 0x5F, 0xD8, 0xB0, 0x18, 0x6B,
  0xCB, 0x08, 0x32, 0xEB, 0xA5, 0xC0, 0x6D, 0x79, 0x41, 0x5F, 0x4D, 0x67,
  0x70, 0x86, 0x38, 0x60, 0x26, 0xDE, 0x7C, 0xA4, 0x3F, 0x29, 0x97, 0x3D,
  0xB4, 0x48, 0x24, 0x3B, 0xA3, 0x6A, 0x5E, 0x5A, 0xE0, 0xB9, 0xD2, 0x2D,
  0x9A, 0x44, 0x00, 0x2E, 0x47, 0x1D, 0x20, 0x6D, 0x9C, 0x6B, 0x89, 0x6D,
  0xD0, 0xC6, 0x50, 0x03, 0xAF, 0x99, 0xEC, 0xA9, 0x0B, 0x1D, 0x9B, 0xFF,
  0x68, 0x57, 0x8C, 0x6D, 0x4A, 0x4A, 0xCA, 0x3D, 0xFC, 0x77, 0xA1, 0x74,
  0x17, 0x86, 0x18, 0x00, 0x46, 0xF3, 0x6F, 0xDE, 0x41, 0xB4, 0x9F, 0xD9,
  0x09, 0x1C, 0xD2, 0xD6, 0x25, 0x9E, 0xBB, 0x1C, 0x9F, 0xBE, 0xB0, 0xF7,
  0x03, 0x11, 0xE3, 0x36, 0x2C, 0x3C, 0xE6, 0xA8, 0x12, 0x98, 0xE2, 0x0F,
  0xA0, 0x2D, 0xAA, 0xED, 0xD9, 0xCF, 0x51, 0xF9, 0x2D, 0x9A, 0x8F, 0x3B,
  0xE4, 0xFD, 0x37, 0x94, 0x97, 0xF5, 0x19, 0x9A, 0x36, 0x9D, 0x0E, 0xF8,
  0x2D, 0xB1, 0xD8, 0x9A, 0x83, 0xCC, 0x23, 0xEA, 0x0A, 0x00, 0x61, 0xE3,
  0xB6, 0x18, 0xD8, 0x0D, 0x12, 0xAC, 0xF4, 0xD4, 0xEA, 0xC2, 0xD6, 0xA0,
  0x9C, 0x0D, 0x32, 0x47, 0x4E, 0x6C, 0x37, 0x30, 0x93, 0x29, 0x43, 0xD6,
  0x0A, 0xCB, 0x29, 0x7A, 0x51, 0xF9, 0xC4, 0x3F, 0x16, 0x36, 0x7D, 0xA8,
  0xDB, 0x21, 0x2E, 0x73, 0x5B, 0x4D, 0x48, 0xA0, 0x7C, 0x2F, 0xDD, 0xD3,
  0x82, 0x05, 0x9E, 0x41, 0x7A, 0xF1, 0xEE, 0x1F, 0x60, 0x16, 0xA6, 0xA0,
  0x76, 0x4B, 0x62, 0x43
};

static unsigned char dh2048_g[] = { 0x02 };

static DH *get_dh2048(void) {
  BIGNUM *p, *g;

  p = BN_bin2bn(dh2048_p, sizeof(dh2048_p), NULL);
  g = BN_bin2bn(dh2048_g, sizeof(dh2048_g), NULL);
  if (p == NULL ||
      g == NULL) {
    return NULL;
  }

  return get_dh(p, g);
}

/*
-----BEGIN DH PARAMETERS-----
MIIBiAKCAYEA9iwjdWslO+8rOS/2mRVnXNeGc2WCrunqB04+tzlbzho881jDDLwd
EBMiP8fw1klEtsqjWf3BoFHPudpvd3sbHyANlBEO7no4f1oIKiHOwSLBh78lw99Q
tqO5RvRYxvM1YeE59M2Dh6hirKmnZSA6avU70W79n2Cj4y/MjaJwiawHZ9s8Y+VT
65JPULgcRs/yDoD2ZcvJktpTpgE4CHGz06YqCYA5TialYRcbEllq9S3DDQQA7rI6
SrWX1LHZTj2AV7PQJLXFn2u/B9Z7+SX76QVFwwe1oKrBCFGAckPpBC7BfZWETfMU
5GqTdhniK9Sw6O+VIbkD6Xt0x6rYDkb+7YYVU/vJLGeHO/J0eYoesEwD4fnaQh3/
6+wdZ9Ck50q3+rGjF+fSVoajMoQOXZq9EG2dKPrQtTW6BQvGksW6UF6wZz960xj0
/t6AN/KEFmpOfyNoiVjgRLvxvTjWn8pr0rmIuOzSXyuwgDa76a+CTXyqHCNX9AlL
IZgYtMTn+IJXAgEF
-----END DH PARAMETERS-----
*/

static unsigned char dh3072_p[] = {
  0xF6, 0x2C, 0x23, 0x75, 0x6B, 0x25, 0x3B, 0xEF, 0x2B, 0x39, 0x2F, 0xF6,
  0x99, 0x15, 0x67, 0x5C, 0xD7, 0x86, 0x73, 0x65, 0x82, 0xAE, 0xE9, 0xEA,
  0x07, 0x4E, 0x3E, 0xB7, 0x39, 0x5B, 0xCE, 0x1A, 0x3C, 0xF3, 0x58, 0xC3,
  0x0C, 0xBC, 0x1D, 0x10, 0x13, 0x22, 0x3F, 0xC7, 0xF0, 0xD6, 0x49, 0x44,
  0xB6, 0xCA, 0xA3, 0x59, 0xFD, 0xC1, 0xA0, 0x51, 0xCF, 0xB9, 0xDA, 0x6F,
  0x77, 0x7B, 0x1B, 0x1F, 0x20, 0x0D, 0x94, 0x11, 0x0E, 0xEE, 0x7A, 0x38,
  0x7F, 0x5A, 0x08, 0x2A, 0x21, 0xCE, 0xC1, 0x22, 0xC1, 0x87, 0xBF, 0x25,
  0xC3, 0xDF, 0x50, 0xB6, 0xA3, 0xB9, 0x46, 0xF4, 0x58, 0xC6, 0xF3, 0x35,
  0x61, 0xE1, 0x39, 0xF4, 0xCD, 0x83, 0x87, 0xA8, 0x62, 0xAC, 0xA9, 0xA7,
  0x65, 0x20, 0x3A, 0x6A, 0xF5, 0x3B, 0xD1, 0x6E, 0xFD, 0x9F, 0x60, 0xA3,
  0xE3, 0x2F, 0xCC, 0x8D, 0xA2, 0x70, 0x89, 0xAC, 0x07, 0x67, 0xDB, 0x3C,
  0x63, 0xE5, 0x53, 0xEB, 0x92, 0x4F, 0x50, 0xB8, 0x1C, 0x46, 0xCF, 0xF2,
  0x0E, 0x80, 0xF6, 0x65, 0xCB, 0xC9, 0x92, 0xDA, 0x53, 0xA6, 0x01, 0x38,
  0x08, 0x71, 0xB3, 0xD3, 0xA6, 0x2A, 0x09, 0x80, 0x39, 0x4E, 0x26, 0xA5,
  0x61, 0x17, 0x1B, 0x12, 0x59, 0x6A, 0xF5, 0x2D, 0xC3, 0x0D, 0x04, 0x00,
  0xEE, 0xB2, 0x3A, 0x4A, 0xB5, 0x97, 0xD4, 0xB1, 0xD9, 0x4E, 0x3D, 0x80,
  0x57, 0xB3, 0xD0, 0x24, 0xB5, 0xC5, 0x9F, 0x6B, 0xBF, 0x07, 0xD6, 0x7B,
  0xF9, 0x25, 0xFB, 0xE9, 0x05, 0x45, 0xC3, 0x07, 0xB5, 0xA0, 0xAA, 0xC1,
  0x08, 0x51, 0x80, 0x72, 0x43, 0xE9, 0x04, 0x2E, 0xC1, 0x7D, 0x95, 0x84,
  0x4D, 0xF3, 0x14, 0xE4, 0x6A, 0x93, 0x76, 0x19, 0xE2, 0x2B, 0xD4, 0xB0,
  0xE8, 0xEF, 0x95, 0x21, 0xB9, 0x03, 0xE9, 0x7B, 0x74, 0xC7, 0xAA, 0xD8,
  0x0E, 0x46, 0xFE, 0xED, 0x86, 0x15, 0x53, 0xFB, 0xC9, 0x2C, 0x67, 0x87,
  0x3B, 0xF2, 0x74, 0x79, 0x8A, 0x1E, 0xB0, 0x4C, 0x03, 0xE1, 0xF9, 0xDA,
  0x42, 0x1D, 0xFF, 0xEB, 0xEC, 0x1D, 0x67, 0xD0, 0xA4, 0xE7, 0x4A, 0xB7,
  0xFA, 0xB1, 0xA3, 0x17, 0xE7, 0xD2, 0x56, 0x86, 0xA3, 0x32, 0x84, 0x0E,
  0x5D, 0x9A, 0xBD, 0x10, 0x6D, 0x9D, 0x28, 0xFA, 0xD0, 0xB5, 0x35, 0xBA,
  0x05, 0x0B, 0xC6, 0x92, 0xC5, 0xBA, 0x50, 0x5E, 0xB0, 0x67, 0x3F, 0x7A,
  0xD3, 0x18, 0xF4, 0xFE, 0xDE, 0x80, 0x37, 0xF2, 0x84, 0x16, 0x6A, 0x4E,
  0x7F, 0x23, 0x68, 0x89, 0x58, 0xE0, 0x44, 0xBB, 0xF1, 0xBD, 0x38, 0xD6,
  0x9F, 0xCA, 0x6B, 0xD2, 0xB9, 0x88, 0xB8, 0xEC, 0xD2, 0x5F, 0x2B, 0xB0,
  0x80, 0x36, 0xBB, 0xE9, 0xAF, 0x82, 0x4D, 0x7C, 0xAA, 0x1C, 0x23, 0x57,
  0xF4, 0x09, 0x4B, 0x21, 0x98, 0x18, 0xB4, 0xC4, 0xE7, 0xF8, 0x82, 0x57
};

static unsigned char dh3072_g[] = { 0x05 };

static DH *get_dh3072(void) {
  BIGNUM *p, *g;

  p = BN_bin2bn(dh3072_p, sizeof(dh3072_p), NULL);
  g = BN_bin2bn(dh3072_g, sizeof(dh3072_g), NULL);
  if (p == NULL ||
      g == NULL) {
    return NULL;
  }

  return get_dh(p, g);
};

/*
-----BEGIN DH PARAMETERS-----
MIICCAKCAgEAjhmKPW6D+3ah6HBaIMznvUMeT1NtLwe06qJNkxh4/SxemwpetOUY
D0O9HWaU/07o1Q5ttbkuVpQjZToT2X0zMkjwjYzO7ERDW9JjVJpwSX7JxNNYRlS7
2swxCehr/IAOVCAs6zgpa2uJEHDmLlM01kQT5SlkfJn5ePohUicLGijDa+rrlIwl
wjTnitQK5C3Ux39bqkhJw9ROgGhkavtn4kTnZKduY5YleQngQI6JM7AlaewiEPPb
ZwnGlOAVuzBL/xXKbMPKIxTBsqdj0Uqs9y1ZKd2TtrXelx4tgUeFBsbDdDhbd+Ol
FIfewJ0yTwZJerkOdqrRXhqqDUt+xhIuaymoX2bRBQVLMwTrEa2Cbfh6EQib5m03
oRIHXpp8oVSsJ3LMQV2UzBLlIXk9/vcAn8pWq7Hd+JyF4CmKbNBj/YhF39y4+uO8
jVeDnxUWszJEIDSV+7ekE49k10d1lLPl2R1lPhY9O+PQci6J27B6J4lHeVYZTqbT
w4KNH14AGBfkGGCkXnZFbo94R1QHt/MZsWaszy6j51aGhUrOUfDnTAnuMvGr9oMV
VbpYqruar+o98kGfQWAdsGqjU8F+M2kXhwWi2S0NdzUERXMA0HD7guzL0vjQCaOs
6UXlJKekV9YnhKjcXqk782xKXjljAYleprUakmgZS8cqmGzjkYgYZkMCAQI=
-----END DH PARAMETERS-----
*/

static unsigned char dh4096_p[] = {
  0x8E, 0x19, 0x8A, 0x3D, 0x6E, 0x83, 0xFB, 0x76, 0xA1, 0xE8, 0x70, 0x5A,
  0x20, 0xCC, 0xE7, 0xBD, 0x43, 0x1E, 0x4F, 0x53, 0x6D, 0x2F, 0x07, 0xB4,
  0xEA, 0xA2, 0x4D, 0x93, 0x18, 0x78, 0xFD, 0x2C, 0x5E, 0x9B, 0x0A, 0x5E,
  0xB4, 0xE5, 0x18, 0x0F, 0x43, 0xBD, 0x1D, 0x66, 0x94, 0xFF, 0x4E, 0xE8,
  0xD5, 0x0E, 0x6D, 0xB5, 0xB9, 0x2E, 0x56, 0x94, 0x23, 0x65, 0x3A, 0x13,
  0xD9, 0x7D, 0x33, 0x32, 0x48, 0xF0, 0x8D, 0x8C, 0xCE, 0xEC, 0x44, 0x43,
  0x5B, 0xD2, 0x63, 0x54, 0x9A, 0x70, 0x49, 0x7E, 0xC9, 0xC4, 0xD3, 0x58,
  0x46, 0x54, 0xBB, 0xDA, 0xCC, 0x31, 0x09, 0xE8, 0x6B, 0xFC, 0x80, 0x0E,
  0x54, 0x20, 0x2C, 0xEB, 0x38, 0x29, 0x6B, 0x6B, 0x89, 0x10, 0x70, 0xE6,
  0x2E, 0x53, 0x34, 0xD6, 0x44, 0x13, 0xE5, 0x29, 0x64, 0x7C, 0x99, 0xF9,
  0x78, 0xFA, 0x21, 0x52, 0x27, 0x0B, 0x1A, 0x28, 0xC3, 0x6B, 0xEA, 0xEB,
  0x94, 0x8C, 0x25, 0xC2, 0x34, 0xE7, 0x8A, 0xD4, 0x0A, 0xE4, 0x2D, 0xD4,
  0xC7, 0x7F, 0x5B, 0xAA, 0x48, 0x49, 0xC3, 0xD4, 0x4E, 0x80, 0x68, 0x64,
  0x6A, 0xFB, 0x67, 0xE2, 0x44, 0xE7, 0x64, 0xA7, 0x6E, 0x63, 0x96, 0x25,
  0x79, 0x09, 0xE0, 0x40, 0x8E, 0x89, 0x33, 0xB0, 0x25, 0x69, 0xEC, 0x22,
  0x10, 0xF3, 0xDB, 0x67, 0x09, 0xC6, 0x94, 0xE0, 0x15, 0xBB, 0x30, 0x4B,
  0xFF, 0x15, 0xCA, 0x6C, 0xC3, 0xCA, 0x23, 0x14, 0xC1, 0xB2, 0xA7, 0x63,
  0xD1, 0x4A, 0xAC, 0xF7, 0x2D, 0x59, 0x29, 0xDD, 0x93, 0xB6, 0xB5, 0xDE,
  0x97, 0x1E, 0x2D, 0x81, 0x47, 0x85, 0x06, 0xC6, 0xC3, 0x74, 0x38, 0x5B,
  0x77, 0xE3, 0xA5, 0x14, 0x87, 0xDE, 0xC0, 0x9D, 0x32, 0x4F, 0x06, 0x49,
  0x7A, 0xB9, 0x0E, 0x76, 0xAA, 0xD1, 0x5E, 0x1A, 0xAA, 0x0D, 0x4B, 0x7E,
  0xC6, 0x12, 0x2E, 0x6B, 0x29, 0xA8, 0x5F, 0x66, 0xD1, 0x05, 0x05, 0x4B,
  0x33, 0x04, 0xEB, 0x11, 0xAD, 0x82, 0x6D, 0xF8, 0x7A, 0x11, 0x08, 0x9B,
  0xE6, 0x6D, 0x37, 0xA1, 0x12, 0x07, 0x5E, 0x9A, 0x7C, 0xA1, 0x54, 0xAC,
  0x27, 0x72, 0xCC, 0x41, 0x5D, 0x94, 0xCC, 0x12, 0xE5, 0x21, 0x79, 0x3D,
  0xFE, 0xF7, 0x00, 0x9F, 0xCA, 0x56, 0xAB, 0xB1, 0xDD, 0xF8, 0x9C, 0x85,
  0xE0, 0x29, 0x8A, 0x6C, 0xD0, 0x63, 0xFD, 0x88, 0x45, 0xDF, 0xDC, 0xB8,
  0xFA, 0xE3, 0xBC, 0x8D, 0x57, 0x83, 0x9F, 0x15, 0x16, 0xB3, 0x32, 0x44,
  0x20, 0x34, 0x95, 0xFB, 0xB7, 0xA4, 0x13, 0x8F, 0x64, 0xD7, 0x47, 0x75,
  0x94, 0xB3, 0xE5, 0xD9, 0x1D, 0x65, 0x3E, 0x16, 0x3D, 0x3B, 0xE3, 0xD0,
  0x72, 0x2E, 0x89, 0xDB, 0xB0, 0x7A, 0x27, 0x89, 0x47, 0x79, 0x56, 0x19,
  0x4E, 0xA6, 0xD3, 0xC3, 0x82, 0x8D, 0x1F, 0x5E, 0x00, 0x18, 0x17, 0xE4,
  0x18, 0x60, 0xA4, 0x5E, 0x76, 0x45, 0x6E, 0x8F, 0x78, 0x47, 0x54, 0x07,
  0xB7, 0xF3, 0x19, 0xB1, 0x66, 0xAC, 0xCF, 0x2E, 0xA3, 0xE7, 0x56, 0x86,
  0x85, 0x4A, 0xCE, 0x51, 0xF0, 0xE7, 0x4C, 0x09, 0xEE, 0x32, 0xF1, 0xAB,
  0xF6, 0x83, 0x15, 0x55, 0xBA, 0x58, 0xAA, 0xBB, 0x9A, 0xAF, 0xEA, 0x3D,
  0xF2, 0x41, 0x9F, 0x41, 0x60, 0x1D, 0xB0, 0x6A, 0xA3, 0x53, 0xC1, 0x7E,
  0x33, 0x69, 0x17, 0x87, 0x05, 0xA2, 0xD9, 0x2D, 0x0D, 0x77, 0x35, 0x04,
  0x45, 0x73, 0x00, 0xD0, 0x70, 0xFB, 0x82, 0xEC, 0xCB, 0xD2, 0xF8, 0xD0,
  0x09, 0xA3, 0xAC, 0xE9, 0x45, 0xE5, 0x24, 0xA7, 0xA4, 0x57, 0xD6, 0x27,
  0x84, 0xA8, 0xDC, 0x5E, 0xA9, 0x3B, 0xF3, 0x6C, 0x4A, 0x5E, 0x39, 0x63,
  0x01, 0x89, 0x5E, 0xA6, 0xB5, 0x1A, 0x92, 0x68, 0x19, 0x4B, 0xC7, 0x2A,
  0x98, 0x6C, 0xE3, 0x91, 0x88, 0x18, 0x66, 0x43
};

static unsigned char dh4096_g[] = { 0x02 };

static DH *get_dh4096(void) {
  BIGNUM *p, *g;

  p = BN_bin2bn(dh4096_p, sizeof(dh4096_p), NULL);
  g = BN_bin2bn(dh4096_g, sizeof(dh4096_g), NULL);
  if (p == NULL ||
      g == NULL) {
    return NULL;
  }

  return get_dh(p, g);
}

/* ASN1_BIT_STRING_cmp was renamed in 0.9.5 */
#if OPENSSL_VERSION_NUMBER < 0x00905100L
# define M_ASN1_BIT_STRING_cmp ASN1_BIT_STRING_cmp
#endif

#if defined(SSL_CTRL_SET_TLSEXT_TICKET_KEY_CB)
# define TLS_USE_SESSION_TICKETS
#endif

module tls_module;

struct tls_next_proto {
  const char *proto;
  unsigned char *encoded_proto;
  unsigned int encoded_protolen;
};

typedef struct tls_pkey_obj {
  struct tls_pkey_obj *next;
  pool *pool;

  size_t pkeysz;

  char *rsa_pkey;
  int rsa_passlen;
  void *rsa_pkey_ptr;

  char *dsa_pkey;
  int dsa_passlen;
  void *dsa_pkey_ptr;

#if defined(PR_USE_OPENSSL_ECC)
  char *ec_pkey;
  int ec_passlen;
  void *ec_pkey_ptr;
#endif /* PR_USE_OPENSSL_ECC */

  /* Used for stashing the password for a PKCS12 file, which should
   * contain a certificate.  Any passphrase for the private key for that
   * certificate should be in one of the above RSA/DSA buffers.
   */
  char *pkcs12_passwd;
  int pkcs12_passlen;
  void *pkcs12_passwd_ptr;

  unsigned int flags;
  unsigned int sid;
  const char *path;

} tls_pkey_t;

#define TLS_PKEY_USE_RSA		0x0100
#define TLS_PKEY_USE_DSA		0x0200
#define TLS_PKEY_USE_EC			0x0400

static tls_pkey_t *tls_pkey_list = NULL;
static unsigned int tls_npkeys = 0;

#define TLS_DEFAULT_CIPHER_SUITE	"DEFAULT:!ADH:!EXPORT:!DES"
#define TLS_DEFAULT_NEXT_PROTO		"ftp"

/* Module variables */
#if OPENSSL_VERSION_NUMBER > 0x000907000L && \
    defined(PR_USE_OPENSSL_ENGINE)
static const char *tls_crypto_device = NULL;
#endif /* PR_USE_OPENSSL_ENGINE */
static unsigned char tls_engine = FALSE;
static unsigned long tls_flags = 0UL, tls_opts = 0UL;
static pool *tls_pool = NULL;
static tls_pkey_t *tls_pkey = NULL;
static int tls_logfd = -1;
#if defined(PR_USE_OPENSSL_OCSP)
static int tls_stapling = FALSE;
static unsigned long tls_stapling_opts = 0UL;
# define TLS_STAPLING_OPT_NO_NONCE		0x0001
# define TLS_STAPLING_OPT_NO_VERIFY		0x0002
# define TLS_STAPLING_OPT_NO_FAKE_TRY_LATER	0x0004
static const char *tls_stapling_responder = NULL;

#define TLS_DEFAULT_STAPLING_TIMEOUT	10
static unsigned int tls_stapling_timeout = TLS_DEFAULT_STAPLING_TIMEOUT;
#endif

static char *tls_passphrase_provider = NULL;
#define TLS_PASSPHRASE_TIMEOUT		10
#define TLS_PASSPHRASE_FL_RSA_KEY	0x0001
#define TLS_PASSPHRASE_FL_DSA_KEY	0x0002
#define TLS_PASSPHRASE_FL_PKCS12_PASSWD	0x0004
#define TLS_PASSPHRASE_FL_EC_KEY	0x0008

#define TLS_PROTO_SSL_V3		0x0001
#define TLS_PROTO_TLS_V1		0x0002
#define TLS_PROTO_TLS_V1_1		0x0004
#define TLS_PROTO_TLS_V1_2		0x0008
#define TLS_PROTO_TLS_V1_3		0x0010

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
# define TLS_PROTO_DEFAULT		(TLS_PROTO_TLS_V1|TLS_PROTO_TLS_V1_1|TLS_PROTO_TLS_V1_2|TLS_PROTO_TLS_V1_3)
#elif OPENSSL_VERSION_NUMBER >= 0x10001000L
# define TLS_PROTO_DEFAULT		(TLS_PROTO_TLS_V1|TLS_PROTO_TLS_V1_1|TLS_PROTO_TLS_V1_2)
#else
# define TLS_PROTO_DEFAULT		(TLS_PROTO_TLS_V1)
#endif /* OpenSSL 1.0.1 or later */

static unsigned int tls_protocol = TLS_PROTO_DEFAULT;

/* This is used for e.g. "TLSProtocol ALL -SSLv3 ...". */
#define TLS_PROTO_ALL			(TLS_PROTO_SSL_V3|TLS_PROTO_TLS_V1|TLS_PROTO_TLS_V1_1|TLS_PROTO_TLS_V1_2|TLS_PROTO_TLS_V1_3)

#ifdef SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS
static int tls_ssl_opts = (SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_SINGLE_DH_USE)^SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;
#else
/* OpenSSL-0.9.6 and earlier (yes, it appears people still have these versions
 * installed) does not define the DONT_INSERT_EMPTY_FRAGMENTS option.
 */
static int tls_ssl_opts = SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_SINGLE_DH_USE;
#endif

static int tls_required_on_auth = 0;
static int tls_required_on_ctrl = 0;
static int tls_required_on_data = 0;
static unsigned char *tls_authenticated = NULL;

/* Define the minimum DH group length we allow (unless the AllowWeakDH
 * TLSOption is used).
 */
#define TLS_DH_MIN_LEN				2048

/* mod_tls session flags */
#define	TLS_SESS_ON_CTRL			0x0001
#define TLS_SESS_ON_DATA			0x0002
#define TLS_SESS_PBSZ_OK			0x0004
#define TLS_SESS_TLS_REQUIRED			0x0010
#define TLS_SESS_VERIFY_CLIENT_REQUIRED		0x0020
#define TLS_SESS_NO_PASSWD_NEEDED		0x0040
#define TLS_SESS_NEED_DATA_PROT			0x0100
#define TLS_SESS_CTRL_RENEGOTIATING		0x0200
#define TLS_SESS_DATA_RENEGOTIATING		0x0400
#define TLS_SESS_HAVE_CCC			0x0800
#define TLS_SESS_VERIFY_SERVER			0x1000
#define TLS_SESS_VERIFY_SERVER_NO_DNS		0x2000
#define TLS_SESS_VERIFY_CLIENT_OPTIONAL		0x4000

/* mod_tls option flags */
#define TLS_OPT_VERIFY_CERT_FQDN			0x0002
#define TLS_OPT_VERIFY_CERT_IP_ADDR			0x0004
#define TLS_OPT_ALLOW_DOT_LOGIN				0x0008
#define TLS_OPT_EXPORT_CERT_DATA			0x0010
#define TLS_OPT_STD_ENV_VARS				0x0020
#define TLS_OPT_ALLOW_PER_USER				0x0040
#define TLS_OPT_ENABLE_DIAGS				0x0080
#define TLS_OPT_NO_SESSION_REUSE_REQUIRED		0x0100
#define TLS_OPT_USE_IMPLICIT_SSL			0x0200
#define TLS_OPT_ALLOW_CLIENT_RENEGOTIATIONS		0x0400
#define TLS_OPT_VERIFY_CERT_CN				0x0800
#define TLS_OPT_NO_AUTO_ECDH				0x1000
#define TLS_OPT_ALLOW_WEAK_DH				0x2000
#define TLS_OPT_IGNORE_SNI				0x4000
#define TLS_OPT_ALLOW_WEAK_SECURITY			0x8000

/* mod_tls SSCN modes */
#define TLS_SSCN_MODE_SERVER				0
#define TLS_SSCN_MODE_CLIENT				1
static unsigned int tls_sscn_mode = TLS_SSCN_MODE_SERVER;

/* mod_tls cleanup flags */
#define TLS_CLEANUP_FL_SESS_INIT	0x0001

/* mod_tls OCSP constants */
#define TLS_OCSP_RESP_MAX_AGE_SECS	300

/* X509v3 OCSP "must staple" extensions (RFC 7633) */
#define TLS_X509V3_TLS_FEAT_OID_TEXT		"1.3.6.1.5.5.7.1.24"
#define TLS_X509V3_TLS_FEAT_STATUS_REQUEST 	{ 0x30, 0x03, 0x02, 0x01, 0x05 }
#define TLS_X509V3_TLS_FEAT_STATUS_REQUEST_V2	{ 0x30, 0x03, 0x02, 0x01, 0x17 }

static char *tls_cipher_suite = NULL;
#if OPENSSL_VERSION_NUMBER >= 0x10101000L && \
    defined(TLS1_3_VERSION)
static char *tlsv13_cipher_suite = NULL;
#endif /* TLS1_3_VERSION */

static char *tls_ca_file = NULL, *tls_ca_path = NULL, *tls_ca_chain = NULL;
static char *tls_crl_file = NULL, *tls_crl_path = NULL;
static char *tls_ec_cert_file = NULL, *tls_ec_key_file = NULL;
static void *tls_ecdh_curve = NULL;
static char *tls_dsa_cert_file = NULL, *tls_dsa_key_file = NULL;
static char *tls_pkcs12_file = NULL;
static char *tls_rsa_cert_file = NULL, *tls_rsa_key_file = NULL;
static char *tls_rand_file = NULL;
#if !defined(OPENSSL_NO_TLSEXT) && !defined(HAVE_LIBRESSL) && \
    OPENSSL_VERSION_NUMBER >= 0x10002000L
static char *tls_serverinfo_file = NULL;
#endif /* OPENSSL_NO_TLSEXT */
static int tls_use_next_protocol = TRUE;
static int tls_use_server_cipher_preference = TRUE;
static int tls_use_session_tickets = FALSE;

#if defined(PSK_MAX_PSK_LEN)
static pr_table_t *tls_psks = NULL;
# define TLS_MIN_PSK_LEN	20
#endif /* PSK support */

/* Timeout given for TLS handshakes.  The default is 5 minutes. */
#define TLS_DEFAULT_HANDSHAKE_TIMEOUT		300
static unsigned int tls_handshake_timeout = TLS_DEFAULT_HANDSHAKE_TIMEOUT;
static unsigned char tls_handshake_timed_out = FALSE;
static int tls_handshake_timer_id = -1;

/* Note: 9 is the default OpenSSL depth. */
#define TLS_DEFAULT_VERIFY_DEPTH	9
static int tls_verify_depth = TLS_DEFAULT_VERIFY_DEPTH;

#if OPENSSL_VERSION_NUMBER > 0x000907000L
/* No default renegotiation of control channel on TLS sessions. */
static int tls_ctrl_renegotiate_timeout = 0;

/* No default renegotiation of data channel on TLS sessions. */
static off_t tls_data_renegotiate_limit = 0;
static off_t tls_data_renegotiate_current = 0;

/* Timeout given for renegotiations to occur before the TLS session is
 * shutdown.  The default is 30 seconds.
 */
static int tls_renegotiate_timeout = 30;

/* Is client acceptance of a requested renegotiation required? */
static unsigned char tls_renegotiate_required = FALSE;
#endif

#define TLS_NETIO_NOTE		"mod_tls.SSL"

static pr_netio_t *tls_ctrl_netio = NULL;
static pr_netio_stream_t *tls_ctrl_rd_nstrm = NULL;
static pr_netio_stream_t *tls_ctrl_wr_nstrm = NULL;

static pr_netio_t *tls_data_netio = NULL;
static pr_netio_stream_t *tls_data_rd_nstrm = NULL;
static pr_netio_stream_t *tls_data_wr_nstrm = NULL;

static tls_sess_cache_t *tls_sess_cache = NULL;
static tls_ocsp_cache_t *tls_ocsp_cache = NULL;

#if defined(PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK)
/* These are used to ensure that TLSv1.3 session tickets are from the same
 * FTP session.
 */
static const void *tls_ctrl_ticket_appdata = NULL;
static size_t tls_ctrl_ticket_appdatasz = 0, tls_ctrl_ticket_appdata_len = 0;
static const void *tls_data_ticket_appdata = NULL;
static size_t tls_data_ticket_appdatasz = 0, tls_data_ticket_appdata_len = 0;
#endif /* PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK */

/* OpenSSL variables */
static SSL *ctrl_ssl = NULL;
static SSL_CTX *ssl_ctx = NULL;
static array_header *tls_tmp_dhs = NULL;
static RSA *tls_tmp_rsa = NULL;

static void tls_exit_ev(const void *event_data, void *user_data);

/* SSL/TLS support functions */
static void tls_closelog(void);
static void tls_end_sess(SSL *ssl, conn_t *conn, int flags);
#define TLS_SHUTDOWN_FL_BIDIRECTIONAL		0x0001

static void tls_fatal_error(long, int);
static const char *tls_get_errors(void);
static const char *tls_get_errors2(pool *p);
static char *tls_get_page(size_t sz, void **ptr);
static size_t tls_get_pagesz(void);
static int tls_get_passphrase(server_rec *s, const char *path,
  const char *prompt, char *buf, size_t bufsz, int flags);

static char *tls_get_subj_name(SSL *ssl);

/* Table-based session cache "provider" for SNI sessions only (Issue #924). */
static pr_table_t *tls_sni_sess_tab = NULL;

static void tls_lookup_all(server_rec *s);
static int tls_ctx_set_all(server_rec *s, SSL_CTX *ctx);
static int tls_ctx_set_ca_certs(SSL_CTX *ctx);
static int tls_ctx_set_certs(SSL_CTX *ctx, X509 **dsa_certs, X509 **ec_certs,
  X509 **rsa_certs);
static int tls_ctx_set_crls(SSL_CTX *ctx);
static int tls_ctx_set_renegotiations(SSL_CTX *ctx);
static int tls_ctx_set_session_cache(server_rec *s, SSL_CTX *ctx);
static int tls_ctx_set_session_id_context(server_rec *s, SSL_CTX *ctx);
static int tls_ctx_set_session_tickets(SSL_CTX *ctx);
static int tls_ctx_set_stapling(SSL_CTX *ctx);
static int tls_ctx_set_stapling_cache(server_rec *s, SSL_CTX *ctx);
static int tls_ssl_set_all(server_rec *s, SSL *ssl);

static int tls_openlog(void);
static int tls_seed_prng(void);
static int tls_sess_init(void);
static void tls_setup_environ(pool *p, SSL *ssl);
static void tls_setup_notes(pool *p, SSL *ssl);
static int tls_verify_cb(int ok, X509_STORE_CTX *ctx);
static int tls_verify_crl(int ok, X509_STORE_CTX *ctx);
static int tls_verify_ocsp(int ok, X509_STORE_CTX *ctx);
static char *tls_x509_name_oneline(X509_NAME *x509_name);

static int tls_readmore(int fd);
static int tls_writemore(int fd);

/* Session cache API */
static tls_sess_cache_t *tls_sess_cache_get_cache(const char *name);
static long tls_sess_cache_get_cache_mode(void);
static int tls_sess_cache_open(char *info, long timeout);
static int tls_sess_cache_close(void);
#if defined(PR_USE_CTRLS)
static int tls_sess_cache_clear(void);
static int tls_sess_cache_remove(void);
static int tls_sess_cache_status(pr_ctrls_t *ctrl, int flags);
#endif /* PR_USE_CTRLS */
static int tls_sess_cache_add_sess_cb(SSL *ssl, SSL_SESSION *ssl_session);
static SSL_SESSION *tls_sess_cache_get_sess_cb(SSL *, const unsigned char *,
  int, int *);
static void tls_sess_cache_delete_sess_cb(SSL_CTX *ssl,
  SSL_SESSION *ssl_session);

/* OCSP response cache API */
static tls_ocsp_cache_t *tls_ocsp_cache_get_cache(const char *name);
static int tls_ocsp_cache_open(char *info);
static int tls_ocsp_cache_close(void);
#if defined(PR_USE_CTRLS)
static int tls_ocsp_cache_clear(void);
static int tls_ocsp_cache_remove(void);
static int tls_ocsp_cache_status(pr_ctrls_t *ctrl, int flags);
#endif /* PR_USE_CTRLS */

#if defined(TLS_USE_SESSION_TICKETS)
/* Default maximum ticket key age: 12 hours */
static unsigned int tls_ticket_key_max_age = 43200;

/* Maximum number of session ticket keys: 25 (1 per hour, plus leeway) */
static unsigned int tls_ticket_key_max_count = 25;
static unsigned int tls_ticket_key_curr_count = 0;

struct tls_ticket_key {
  struct tls_ticket_key *next, *prev;

  /* Memory page pointer and size, for locking. */
  void *page_ptr;
  size_t pagesz;
  int locked;
  time_t created;

  /* 16 bytes for the key name, per OpenSSL implementation. */
  unsigned char key_name[16];
  unsigned char cipher_key[32];
  unsigned char hmac_key[32];
};

/* In-memory list of session ticket keys, newest key first.  Note that the
 * memory pages used for a ticket key will be mlock(2)'d into memory, where
 * possible.
 *
 * Ticket keys will be generated randomly, based on the timeout.  Expired
 * ticket keys will be destroyed when a new key is generated.  Tickets
 * encrypted with older keys will be renewed using the newest key.
 */
static xaset_t *tls_ticket_keys = NULL;
#endif /* TLS_USE_SESSION_TICKETS */

#if defined(PR_USE_CTRLS)
static pool *tls_act_pool = NULL;
static ctrls_acttab_t tls_acttab[];
#endif /* PR_USE_CTRLS */

static int tls_ctrl_need_init_handshake = TRUE;
static int tls_data_need_init_handshake = TRUE;

static const char *timing_channel = "timing";

static int tls_keyfile_check_cb(char *buf, int size, int rwflag,
    void *user_data) {
  buf[0] = '\0';
  return 0;
}

static const char *tls_get_fingerprint(pool *p, X509 *cert) {
  const EVP_MD *md = EVP_sha1();
  unsigned char fp[EVP_MAX_MD_SIZE];
  unsigned int fp_len = 0;
  char *fp_hex = NULL;

  if (X509_digest(cert, md, fp, &fp_len) != 1) {
    pr_trace_msg(trace_channel, 1,
      "error obtaining %s digest of X509 cert: %s", OBJ_nid2sn(EVP_MD_type(md)),
      tls_get_errors());
    errno = EINVAL;
    return NULL;
  }

  fp_hex = pr_str_bin2hex(p, fp, fp_len, 0);

  pr_trace_msg(trace_channel, 8,
    "%s fingerprint: %s", OBJ_nid2sn(EVP_MD_type(md)), fp_hex);
  return fp_hex;
}

static const char *get_pkey_typestr(int pkey_type) {
  const char *str = "unknown";

  switch (pkey_type) {
    case EVP_PKEY_RSA:
      str = "RSA";
      break;

    case EVP_PKEY_DSA:
      str = "DSA";
      break;

#ifdef PR_USE_OPENSSL_ECC
    case EVP_PKEY_EC:
      str = "EC";
      break;
#endif /* PR_USE_OPENSSL_EC */
  }

  return str;
}

static const char *tls_get_fingerprint_from_file(pool *p, const char *path,
    int expected_pkey_type, const char **errstr) {
  FILE *fh;
  X509 *cert = NULL;
  const char *fingerprint;

  fh = fopen(path, "rb");
  if (fh == NULL) {
    int xerrno = errno;

    *errstr = (const char *) pstrdup(p, strerror(xerrno));
    errno = xerrno;
    return NULL;
  }

  /* As the file may contain sensitive data, we do not want it lingering
   * around in stdio buffers.
   */
  (void) setvbuf(fh, NULL, _IONBF, 0);

  cert = PEM_read_X509(fh, &cert, NULL, NULL);
  (void) fclose(fh);

  if (cert == NULL) {
    const char *err_msg;

    err_msg = tls_get_errors2(p);
    *errstr = err_msg;

    pr_trace_msg(trace_channel, 1, "error obtaining X509 cert from '%s': %s",
      path, err_msg);
    errno = ENOENT;
    return NULL;
  }

  fingerprint = tls_get_fingerprint(p, cert);

  if (cert != NULL) {
    time_t now;
    const ASN1_TIME *cert_end_ts;
    EVP_PKEY *pkey;

    now = time(NULL);
#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    defined(HAVE_LIBRESSL)
    cert_end_ts = X509_get_notAfter(cert);
#else
    cert_end_ts = X509_get0_notAfter(cert);
#endif
    pkey = X509_get_pubkey(cert);

    if (pkey != NULL) {
      int pkey_type;

      pkey_type = get_pkey_type(pkey);
      EVP_PKEY_free(pkey);

      if (pkey_type != expected_pkey_type) {
        pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
          ": certificate '%s': expected %s certificate, found %s", path,
          get_pkey_typestr(expected_pkey_type), get_pkey_typestr(pkey_type));
      }
    }

    if (X509_cmp_time(cert_end_ts, &now) < 0) {
      BIO *bio;
      char *data = NULL;
      long datalen = 0;

      bio = BIO_new(BIO_s_mem());
      ASN1_TIME_print(bio, cert_end_ts);
      datalen = BIO_get_mem_data(bio, &data);
      if (data != NULL) {
        data[datalen] = '\0';
        *errstr = (const char *) pstrcat(p, "expired on ", data, NULL);

      } else {
        *errstr = "already expired";
      }

      BIO_free(bio);

      pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
        ": certificate '%s': %s",  path, *errstr);
    }
  }

  X509_free(cert);
  return fingerprint;
}

#if defined(PR_USE_OPENSSL_OCSP)
static const char *ocsp_get_responder_url(pool *p, X509 *cert) {
  STACK_OF(OPENSSL_STRING) *strs;
  char *ocsp_url = NULL;

  strs = X509_get1_ocsp(cert);
  if (strs != NULL) {
# if OPENSSL_VERSION_NUMBER >= 0x10000001L
    if (sk_OPENSSL_STRING_num(strs) > 0) {
      ocsp_url = pstrdup(p, sk_OPENSSL_STRING_value(strs, 0));
    }
# endif /* OpenSSL-1.0.0 and later */

    /* Yes, this says "email", but it Does The Right Thing(tm) for our needs. */
    X509_email_free(strs);
  }

  return ocsp_url;
}
#endif /* PR_USE_OPENSSL_OCSP */

static void tls_reset_state(void) {
  tls_engine = FALSE;
  tls_flags = 0UL;
  tls_opts = 0UL;

  if (tls_logfd >= 0) {
    (void) close(tls_logfd);
    tls_logfd = -1;
  }

  tls_cipher_suite = NULL;
# if OPENSSL_VERSION_NUMBER >= 0x10101000L && \
     defined(TLS1_3_VERSION)
  tlsv13_cipher_suite = NULL;
# endif /* TLS1_3_VERSION */
  tls_crl_file = NULL;
  tls_crl_path = NULL;
# if defined(PR_USE_OPENSSL_ENGINE)
  tls_crypto_device = NULL;
# endif /* PR_USE_OPENSSL_ENGINE */
  tls_dsa_cert_file = NULL;
  tls_dsa_key_file = NULL;
  tls_ec_cert_file = NULL;
  tls_ec_key_file = NULL;
  tls_pkcs12_file = NULL;
  tls_rsa_cert_file = NULL;
  tls_rsa_key_file = NULL;
  tls_rand_file = NULL;
#if defined(PSK_MAX_PSK_LEN)
  tls_psks = NULL;
#endif /* PSK_MAX_PSK_LEN */

#if defined(PR_USE_OPENSSL_OCSP)
  tls_stapling = FALSE;
  tls_stapling_opts = 0UL;
  tls_stapling_responder = NULL;
  tls_stapling_timeout = TLS_DEFAULT_STAPLING_TIMEOUT;
#endif /* PR_USE_OPENSSL_OCSP */

  tls_handshake_timeout = TLS_DEFAULT_HANDSHAKE_TIMEOUT;
  tls_handshake_timed_out = FALSE;
  tls_handshake_timer_id = -1;

  tls_verify_depth = TLS_DEFAULT_VERIFY_DEPTH;

  /* Note that we do NOT want to set the netio and nstrm pointers, ctrl
   * channel, to here.  Why not?  We reset this state when a HOST command
   * occurs -- but that does NOT cause the NetIO open() to be called again.
   * And only the NetIO open callback sets our netio/nstrm pointers; so we
   * leave those as they are (Bug#4370).
   */

  tls_data_netio = NULL;
  tls_data_rd_nstrm = NULL;
  tls_data_wr_nstrm = NULL;

  tls_tmp_dhs = NULL;
  tls_tmp_rsa = NULL;

  tls_ctrl_need_init_handshake = TRUE;
  tls_data_need_init_handshake = TRUE;

  tls_required_on_auth = 0;
  tls_required_on_ctrl = 0;
  tls_required_on_data = 0;

  tls_data_renegotiate_current = 0;
}

static void tls_info_cb(const SSL *ssl, int where, int ret) {
  const char *str = "(unknown)";
  int w;

  pr_signals_handle();

  w = where & ~SSL_ST_MASK;

  if (w & SSL_ST_CONNECT) {
    str = "connecting";

  } else if (w & SSL_ST_ACCEPT) {
    str = "accepting";

  } else {
    int ssl_state;

    ssl_state = SSL_get_state(ssl);
    switch (ssl_state) {
#ifdef SSL_ST_BEFORE
      case SSL_ST_BEFORE:
        str = "before";
        break;
#endif

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
      case TLS_ST_OK:
#else
      case SSL_ST_OK:
#endif /* OpenSSL-1.1.x and later */
        str = "ok";
        break;

#ifdef SSL_ST_RENEGOTIATE
      case SSL_ST_RENEGOTIATE:
        str = "renegotiating";
        break;
#endif

      default:
        break;
    }
  }

  if (where & SSL_CB_ACCEPT_LOOP) {
    int ssl_state;

    ssl_state = SSL_get_state(ssl);

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
    if (ssl_state == TLS_ST_SR_CLNT_HELLO) {
#else
    if (ssl_state == SSL3_ST_SR_CLNT_HELLO_A
# if LIBRESSL_VERSION_NUMBER < 0x4000000fL
     /* LibreSSL's ssl23.h was removed in 4.0.0 */
        || ssl_state == SSL23_ST_SR_CLNT_HELLO_A
# endif
        ) {
#endif /* OpenSSL-1.1.x and later */

      if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
        tls_log("[info] %s: %s", str, SSL_state_string_long(ssl));
      }

      /* If we have already completed our initial handshake, then this might
       * a session renegotiation.
       */
      if ((ssl == ctrl_ssl && !tls_ctrl_need_init_handshake) ||
          (ssl != ctrl_ssl && !tls_data_need_init_handshake)) {

        /* Yes, this is indeed a session renegotiation. If it's a
         * renegotiation that we requested, allow it.  If it is from a
         * data connection, allow it.  Otherwise, it's a client-initiated
         * renegotiation, and we probably don't want to allow it.
         */

        if (ssl == ctrl_ssl &&
            !(tls_flags & TLS_SESS_CTRL_RENEGOTIATING) &&
            !(tls_flags & TLS_SESS_DATA_RENEGOTIATING)) {

          if (!(tls_opts & TLS_OPT_ALLOW_CLIENT_RENEGOTIATIONS)) {
            tls_log("warning: client-initiated session renegotiation "
              "detected, aborting connection");
            pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
              ": client-initiated session renegotiation detected, "
              "aborting connection");

            tls_end_sess(ctrl_ssl, session.c, 0);
            pr_table_remove(tls_ctrl_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
            pr_table_remove(tls_ctrl_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
            ctrl_ssl = NULL;

            pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_CONFIG_ACL,
              "TLSOption AllowClientRenegotiations");
          }
        }
      }

#if OPENSSL_VERSION_NUMBER >= 0x009080cfL && \
    OPENSSL_VERSION_NUMBER < 0x10100000L
    } else if (ssl_state & SSL_ST_RENEGOTIATE) {
      if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
        tls_log("[info] %s: %s", str, SSL_state_string_long(ssl));
      }

      if ((ssl == ctrl_ssl && !tls_ctrl_need_init_handshake) ||
          (ssl != ctrl_ssl && !tls_data_need_init_handshake)) {

        if (ssl == ctrl_ssl &&
            !(tls_flags & TLS_SESS_CTRL_RENEGOTIATING) &&
            !(tls_flags & TLS_SESS_DATA_RENEGOTIATING)) {

          /* In OpenSSL-0.9.8l and later, SSL session renegotiations are
           * automatically disabled.  Thus if the admin has not explicitly
           * configured support for client-initiated renegotiations via the
           * AllowClientRenegotiations TLSOption, then we need to disconnect
           * the client here.  Otherwise, the client would hang (up to the
           * TLSTimeoutHandshake limit).  Since we know, right now, that the
           * handshake won't succeed, just drop the connection.
           */

          if (!(tls_opts & TLS_OPT_ALLOW_CLIENT_RENEGOTIATIONS)) {
            tls_log("warning: client-initiated session renegotiation detected, "
              "aborting connection");
            pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
              ": client-initiated session renegotiation detected, "
              "aborting connection");

            tls_end_sess(ctrl_ssl, session.c, 0);
            pr_table_remove(tls_ctrl_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
            pr_table_remove(tls_ctrl_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
            ctrl_ssl = NULL;

            pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_CONFIG_ACL,
              "TLSOption AllowClientRenegotiations");
          }
        }
      }
#endif
    }

    if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
      tls_log("[info] %s: %s", str, SSL_state_string_long(ssl));
    }

  } else if (where & SSL_CB_HANDSHAKE_START) {
    if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
      tls_log("[info] %s: %s (HANDSHAKE_START)", str,
        SSL_state_string_long(ssl));
    }

  } else if (where & SSL_CB_HANDSHAKE_DONE) {
    if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
      tls_log("[info] %s: %s (HANDSHAKE_DONE)", str,
        SSL_state_string_long(ssl));
    }

    /* ctrl_ssl is NULL if this is our initial ctrl SSL, and the handshake has
     * not be completed yet.
     */
    if (ctrl_ssl == NULL) {
      if (tls_ctrl_need_init_handshake == FALSE) {
        int reused;

        /* If this is an accepted renegotiation, log the possibly-changed
         * ciphersuite et al.
         */

        reused = SSL_session_reused((SSL *) ssl);
        tls_log("%s renegotiation accepted, using cipher %s (%d bits%s)",
          SSL_get_version(ssl), SSL_get_cipher_name(ssl),
          SSL_get_cipher_bits(ssl, NULL),
          reused > 0 ? ", resumed session" : "");
      }

      tls_ctrl_need_init_handshake = FALSE;

    } else {
      if (tls_data_need_init_handshake == FALSE) {
        /* If this is an accepted renegotiation, log the possibly-changed
         * ciphersuite et al.
         */
        tls_log("%s renegotiation accepted, using cipher %s (%d bits)",
          SSL_get_version(ssl), SSL_get_cipher_name(ssl),
          SSL_get_cipher_bits(ssl, NULL));
      }

      tls_data_need_init_handshake = FALSE;
    }

    /* Clear the flags set for server-requested renegotiations. */
    if (tls_flags & TLS_SESS_CTRL_RENEGOTIATING) {
      tls_flags &= ~TLS_SESS_CTRL_RENEGOTIATING;
    }

    if (tls_flags & ~TLS_SESS_DATA_RENEGOTIATING) {
      tls_flags &= ~TLS_SESS_DATA_RENEGOTIATING;
    }

  } else if (where & SSL_CB_LOOP) {
    if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
      tls_log("[info] %s: %s", str, SSL_state_string_long(ssl));
    }

  } else if (where & SSL_CB_ALERT) {
    if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
      str = (where & SSL_CB_READ) ? "reading" : "writing";
      tls_log("[info] %s: SSL/TLS alert %s: %s", str,
        SSL_alert_type_string_long(ret), SSL_alert_desc_string_long(ret));
    }

  } else if (where & SSL_CB_EXIT) {
    if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
      if (ret == 0) {
        tls_log("[info] %s: failed in %s: %s", str,
          SSL_state_string_long(ssl), tls_get_errors());

      } else if (ret < 0 &&
                 errno != 0 &&
                 errno != EAGAIN) {
        /* Ignore EAGAIN errors */
        tls_log("[info] %s: error in %s (errno %d: %s)",
          str, SSL_state_string_long(ssl), errno, strerror(errno));
      }
    }
  }
}

#if OPENSSL_VERSION_NUMBER > 0x000907000L
/* Tables needed for describing bits of the ClientHello. */

struct tls_label {
  int labelno;
  const char *label_name;
};

/* SSL record types */
static struct tls_label tls_record_type_labels[] = {
  { 20, "ChangeCipherSpec" },
  { 21, "Alert" },
  { 22, "Handshake" },
  { 23, "ApplicationData" },
  { 0, NULL }
};

/* SSL versions */
static struct tls_label tls_version_labels[] = {
  { 0x0002, "SSL 2.0" },
  { 0x0300, "SSL 3.0" },
  { 0x0301, "TLS 1.0" },
  { 0x0302, "TLS 1.1" },
  { 0x0303, "TLS 1.2" },
  { 0x0304, "TLS 1.3" },

  { 0, NULL }
};

/* Cipher suites.  These values come from:
 *   http://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#tls-parameters-4
 */
static struct tls_label tls_ciphersuite_labels[] = {
  { 0x0000, "SSL_NULL_WITH_NULL_NULL" },
  { 0x0001, "SSL_RSA_WITH_NULL_MD5" },
  { 0x0002, "SSL_RSA_WITH_NULL_SHA" },
  { 0x0003, "SSL_RSA_EXPORT_WITH_RC4_40_MD5" },
  { 0x0004, "SSL_RSA_WITH_RC4_128_MD5" },
  { 0x0005, "SSL_RSA_WITH_RC4_128_SHA" },
  { 0x0006, "SSL_RSA_EXPORT_WITH_RC2_CBC_40_MD5" },
  { 0x0007, "SSL_RSA_WITH_IDEA_CBC_SHA" },
  { 0x0008, "SSL_RSA_EXPORT_WITH_DES40_CBC_SHA" },
  { 0x0009, "SSL_RSA_WITH_DES_CBC_SHA" },
  { 0x000A, "SSL_RSA_WITH_3DES_EDE_CBC_SHA" },
  { 0x000B, "SSL_DH_DSS_EXPORT_WITH_DES40_CBC_SHA" },
  { 0x000C, "SSL_DH_DSS_WITH_DES_CBC_SHA" },
  { 0x000D, "SSL_DH_DSS_WITH_3DES_EDE_CBC_SHA" },
  { 0x000E, "SSL_DH_RSA_EXPORT_WITH_DES40_CBC_SHA" },
  { 0x000F, "SSL_DH_RSA_WITH_DES_CBC_SHA" },
  { 0x0010, "SSL_DH_RSA_WITH_3DES_EDE_CBC_SHA" },
  { 0x0011, "SSL_DHE_DSS_EXPORT_WITH_DES40_CBC_SHA" },
  { 0x0012, "SSL_DHE_DSS_WITH_DES_CBC_SHA" },
  { 0x0013, "SSL_DHE_DSS_WITH_3DES_EDE_CBC_SHA" },
  { 0x0014, "SSL_DHE_RSA_EXPORT_WITH_DES40_CBC_SHA" },
  { 0x0015, "SSL_DHE_RSA_WITH_DES_CBC_SHA" },
  { 0x0016, "SSL_DHE_RSA_WITH_3DES_EDE_CBC_SHA" },
  { 0x0017, "SSL_DH_anon_EXPORT_WITH_RC4_40_MD5" },
  { 0x0018, "SSL_DH_anon_WITH_RC4_128_MD5" },
  { 0x0019, "SSL_DH_anon_EXPORT_WITH_DES40_CBC_SHA" },
  { 0x001A, "SSL_DH_anon_WITH_DES_CBC_SHA" },
  { 0x001B, "SSL_DH_anon_WITH_3DES_EDE_CBC_SHA" },
  { 0x001D, "SSL_FORTEZZA_KEA_WITH_FORTEZZA_CBC_SHA" },
  { 0x001E, "SSL_FORTEZZA_KEA_WITH_RC4_128_SHA" },
  { 0x001F, "TLS_KRB5_WITH_3DES_EDE_CBC_SHA" },
  { 0x0020, "TLS_KRB5_WITH_RC4_128_SHA" },
  { 0x0021, "TLS_KRB5_WITH_IDEA_CBC_SHA" },
  { 0x0022, "TLS_KRB5_WITH_DES_CBC_MD5" },
  { 0x0023, "TLS_KRB5_WITH_3DES_EDE_CBC_MD5" },
  { 0x0024, "TLS_KRB5_WITH_RC4_128_MD5" },
  { 0x0025, "TLS_KRB5_WITH_IDEA_CBC_MD5" },
  { 0x0026, "TLS_KRB5_EXPORT_WITH_DES_CBC_40_SHA" },
  { 0x0027, "TLS_KRB5_EXPORT_WITH_RC2_CBC_40_SHA" },
  { 0x0028, "TLS_KRB5_EXPORT_WITH_RC4_40_SHA" },
  { 0x0029, "TLS_KRB5_EXPORT_WITH_DES_CBC_40_MD5" },
  { 0x002A, "TLS_KRB5_EXPORT_WITH_RC2_CBC_40_MD5" },
  { 0x002B, "TLS_KRB5_EXPORT_WITH_RC4_40_MD5" },
  { 0x002F, "TLS_RSA_WITH_AES_128_CBC_SHA" },
  { 0x0030, "TLS_DH_DSS_WITH_AES_128_CBC_SHA" },
  { 0x0031, "TLS_DH_RSA_WITH_AES_128_CBC_SHA" },
  { 0x0032, "TLS_DHE_DSS_WITH_AES_128_CBC_SHA" },
  { 0x0033, "TLS_DHE_RSA_WITH_AES_128_CBC_SHA" },
  { 0x0034, "TLS_DH_anon_WITH_AES_128_CBC_SHA" },
  { 0x0035, "TLS_RSA_WITH_AES_256_CBC_SHA" },
  { 0x0036, "TLS_DH_DSS_WITH_AES_256_CBC_SHA" },
  { 0x0037, "TLS_DH_RSA_WITH_AES_256_CBC_SHA" },
  { 0x0038, "TLS_DHE_DSS_WITH_AES_256_CBC_SHA" },
  { 0x0039, "TLS_DHE_RSA_WITH_AES_256_CBC_SHA" },
  { 0x003A, "TLS_DH_anon_WITH_AES_256_CBC_SHA" },
  { 0x003B, "TLS_RSA_WITH_NULL_SHA256" },
  { 0x003C, "TLS_RSA_WITH_AES_128_CBC_SHA256" },
  { 0x003D, "TLS_RSA_WITH_AES_256_CBC_SHA256" },
  { 0x003E, "TLS_DH_DSS_WITH_AES_128_CBC_SHA256" },
  { 0x003F, "TLS_DH_RSA_WITH_AES_128_CBC_SHA256" },
  { 0x0040, "TLS_DHE_DSS_WITH_AES_128_CBC_SHA256" },
  { 0x0041, "TLS_RSA_WITH_CAMELLIA_128_CBC_SHA" },
  { 0x0042, "TLS_DH_DSS_WITH_CAMELLIA_128_CBC_SHA" },
  { 0x0043, "TLS_DH_RSA_WITH_CAMELLIA_128_CBC_SHA" },
  { 0x0044, "TLS_DHE_DSS_WITH_CAMELLIA_128_CBC_SHA" },
  { 0x0045, "TLS_DHE_RSA_WITH_CAMELLIA_128_CBC_SHA" },
  { 0x0046, "TLS_DH_anon_WITH_CAMELLIA_128_CBC_SHA" },
  { 0x0067, "TLS_DHE_RSA_WITH_AES_128_CBC_SHA256" },
  { 0x0068, "TLS_DH_DSS_WITH_AES_256_CBC_SHA256" },
  { 0x0069, "TLS_DH_RSA_WITH_AES_256_CBC_SHA256" },
  { 0x006A, "TLS_DHE_DSS_WITH_AES_256_CBC_SHA256" },
  { 0x006B, "TLS_DHE_RSA_WITH_AES_256_CBC_SHA256" },
  { 0x006C, "TLS_DH_anon_WITH_AES_128_CBC_SHA256" },
  { 0x006D, "TLS_DH_anon_WITH_AES_256_CBC_SHA256" },
  { 0x0084, "TLS_RSA_WITH_CAMELLIA_256_CBC_SHA" },
  { 0x0085, "TLS_DH_DSS_WITH_CAMELLIA_256_CBC_SHA" },
  { 0x0086, "TLS_DH_RSA_WITH_CAMELLIA_256_CBC_SHA" },
  { 0x0087, "TLS_DHE_DSS_WITH_CAMELLIA_256_CBC_SHA" },
  { 0x0088, "TLS_DHE_RSA_WITH_CAMELLIA_256_CBC_SHA" },
  { 0x0089, "TLS_DH_anon_WITH_CAMELLIA_256_CBC_SHA" },
  { 0x008A, "TLS_PSK_WITH_RC4_128_SHA" },
  { 0x008B, "TLS_PSK_WITH_3DES_EDE_CBC_SHA" },
  { 0x008C, "TLS_PSK_WITH_AES_128_CBC_SHA" },
  { 0x008D, "TLS_PSK_WITH_AES_256_CBC_SHA" },
  { 0x008E, "TLS_DHE_PSK_WITH_RC4_128_SHA" },
  { 0x008F, "TLS_DHE_PSK_WITH_3DES_EDE_CBC_SHA" },
  { 0x0090, "TLS_DHE_PSK_WITH_AES_128_CBC_SHA" },
  { 0x0091, "TLS_DHE_PSK_WITH_AES_256_CBC_SHA" },
  { 0x0092, "TLS_RSA_PSK_WITH_RC4_128_SHA" },
  { 0x0093, "TLS_RSA_PSK_WITH_3DES_EDE_CBC_SHA" },
  { 0x0094, "TLS_RSA_PSK_WITH_AES_128_CBC_SHA" },
  { 0x0095, "TLS_RSA_PSK_WITH_AES_256_CBC_SHA" },
  { 0x0096, "TLS_RSA_WITH_SEED_CBC_SHA" },
  { 0x0097, "TLS_DH_DSS_WITH_SEED_CBC_SHA" },
  { 0x0098, "TLS_DH_RSA_WITH_SEED_CBC_SHA" },
  { 0x0099, "TLS_DHE_DSS_WITH_SEED_CBC_SHA" },
  { 0x009A, "TLS_DHE_RSA_WITH_SEED_CBC_SHA" },
  { 0x009B, "TLS_DH_anon_WITH_SEED_CBC_SHA" },
  { 0x009C, "TLS_RSA_WITH_AES_128_GCM_SHA256" },
  { 0x009D, "TLS_RSA_WITH_AES_256_GCM_SHA384" },
  { 0x009E, "TLS_DHE_RSA_WITH_AES_128_GCM_SHA256" },
  { 0x009F, "TLS_DHE_RSA_WITH_AES_256_GCM_SHA384" },
  { 0x00A0, "TLS_DH_RSA_WITH_AES_128_GCM_SHA256" },
  { 0x00A1, "TLS_DH_RSA_WITH_AES_256_GCM_SHA384" },
  { 0x00A2, "TLS_DHE_DSS_WITH_AES_128_GCM_SHA256" },
  { 0x00A3, "TLS_DHE_DSS_WITH_AES_256_GCM_SHA384" },
  { 0x00A4, "TLS_DH_DSS_WITH_AES_128_GCM_SHA256" },
  { 0x00A5, "TLS_DH_DSS_WITH_AES_256_GCM_SHA384" },
  { 0x00A6, "TLS_DH_anon_WITH_AES_128_GCM_SHA256" },
  { 0x00A7, "TLS_DH_anon_WITH_AES_256_GCM_SHA384" },
  { 0x00A8, "TLS_PSK_WITH_AES_128_GCM_SHA256" },
  { 0x00A9, "TLS_PSK_WITH_AES_256_GCM_SHA384" },
  { 0x00AA, "TLS_DHE_PSK_WITH_AES_128_GCM_SHA256" },
  { 0x00AB, "TLS_DHE_PSK_WITH_AES_256_GCM_SHA384" },
  { 0x00AC, "TLS_RSA_PSK_WITH_AES_128_GCM_SHA256" },
  { 0x00AD, "TLS_RSA_PSK_WITH_AES_256_GCM_SHA384" },
  { 0x00AE, "TLS_PSK_WITH_AES_128_CBC_SHA256" },
  { 0x00AF, "TLS_PSK_WITH_AES_256_CBC_SHA384" },
  { 0x00B0, "TLS_PSK_WITH_NULL_SHA256" },
  { 0x00B1, "TLS_PSK_WITH_NULL_SHA384" },
  { 0x00B2, "TLS_DHE_PSK_WITH_AES_128_CBC_SHA256" },
  { 0x00B3, "TLS_DHE_PSK_WITH_AES_256_CBC_SHA384"},
  { 0x00B4, "TLS_DHE_PSK_WITH_NULL_SHA256" },
  { 0x00B5, "TLS_DHE_PSK_WITH_NULL_SHA384" },
  { 0x00B6, "TLS_RSA_PSK_WITH_AES_128_CBC_SHA256" },
  { 0x00B7, "TLS_RSA_PSK_WITH_AES_256_CBC_SHA384" },
  { 0x00B8, "TLS_RSA_PSK_WITH_NULL_SHA256" },
  { 0x00B9, "TLS_RSA_PSK_WITH_NULL_SHA384" },
  { 0x00BA, "TLS_RSA_WITH_CAMELLIA_128_CBC_SHA256" },
  { 0x00BB, "TLS_DH_DSS_WITH_CAMELLIA_128_CBC_SHA256" },
  { 0x00BC, "TLS_DH_RSA_WITH_CAMELLIA_128_CBC_SHA256" },
  { 0x00BD, "TLS_DHE_DSS_WITH_CAMELLIA_128_CBC_SHA256" },
  { 0x00BE, "TLS_DHE_RSA_WITH_CAMELLIA_128_CBC_SHA256" },
  { 0x00BF, "TLS_DH_anon_WITH_CAMELLIA_128_CBC_SHA256" },
  { 0x00C0, "TLS_RSA_WITH_CAMELLIA_256_CBC_SHA256" },
  { 0x00C1, "TLS_DH_DSS_WITH_CAMELLIA_256_CBC_SHA256" },
  { 0x00C2, "TLS_DH_RSA_WITH_CAMELLIA_256_CBC_SHA256" },
  { 0x00C3, "TLS_DHE_DSS_WITH_CAMELLIA_256_CBC_SHA256" },
  { 0x00C4, "TLS_DHE_RSA_WITH_CAMELLIA_256_CBC_SHA256" },
  { 0x00C5, "TLS_DH_anon_WITH_CAMELLIA_256_CBC_SHA256" },
  { 0x00FF, "TLS_EMPTY_RENEGOTIATION_INFO_SCSV" },
  { 0x1301, "TLS_AES_128_GCM_SHA256" },
  { 0x1302, "TLS_AES_256_GCM_SHA384" },
  { 0x1303, "TLS_CHACHA20_POLY1305_SHA256" },
  { 0xC001, "TLS_ECDH_ECDSA_WITH_NULL_SHA" },
  { 0xC002, "TLS_ECDH_ECDSA_WITH_RC4_128_SHA" },
  { 0xC003, "TLS_ECDH_ECDSA_WITH_3DES_EDE_CBC_SHA" },
  { 0xC004, "TLS_ECDH_ECDSA_WITH_AES_128_CBC_SHA" },
  { 0xC005, "TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA" },
  { 0xC006, "TLS_ECDHE_ECDSA_WITH_NULL_SHA" },
  { 0xC007, "TLS_ECDHE_ECDSA_WITH_RC4_128_SHA" },
  { 0xC008, "TLS_ECDHE_ECDSA_WITH_3DES_EDE_CBC_SHA" },
  { 0xC009, "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA" },
  { 0xC00A, "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA" },
  { 0xC00B, "TLS_ECDH_RSA_WITH_NULL_SHA" },
  { 0xC00C, "TLS_ECDH_RSA_WITH_RC4_128_SHA" },
  { 0xC00D, "TLS_ECDH_RSA_WITH_3DES_EDE_CBC_SHA" },
  { 0xC00E, "TLS_ECDH_RSA_WITH_AES_128_CBC_SHA" },
  { 0xC00F, "TLS_ECDH_RSA_WITH_AES_256_CBC_SHA" },
  { 0xC010, "TLS_ECDHE_RSA_WITH_NULL_SHA" },
  { 0xC011, "TLS_ECDHE_RSA_WITH_RC4_128_SHA" },
  { 0xC012, "TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA" },
  { 0xC013, "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA" },
  { 0xC014, "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA" },
  { 0xC015, "TLS_ECDH_anon_WITH_NULL_SHA" },
  { 0xC016, "TLS_ECDH_anon_WITH_RC4_128_SHA" },
  { 0xC017, "TLS_ECDH_anon_WITH_3DES_EDE_CBC_SHA" },
  { 0xC018, "TLS_ECDH_anon_WITH_AES_128_CBC_SHA" },
  { 0xC019, "TLS_ECDH_anon_WITH_AES_256_CBC_SHA" },
  { 0xC01A, "TLS_SRP_SHA_WITH_3DES_EDE_CBC_SHA" },
  { 0xC01B, "TLS_SRP_SHA_RSA_WITH_3DES_EDE_CBC_SHA" },
  { 0xC01C, "TLS_SRP_SHA_DSS_WITH_3DES_EDE_CBC_SHA" },
  { 0xC01D, "TLS_SRP_SHA_WITH_AES_128_CBC_SHA" },
  { 0xC01E, "TLS_SRP_SHA_RSA_WITH_AES_128_CBC_SHA" },
  { 0xC01F, "TLS_SRP_SHA_DSS_WITH_AES_128_CBC_SHA" },
  { 0xC020, "TLS_SRP_SHA_WITH_AES_256_CBC_SHA" },
  { 0xC021, "TLS_SRP_SHA_RSA_WITH_AES_256_CBC_SHA" },
  { 0xC022, "TLS_SRP_SHA_DSS_WITH_AES_256_CBC_SHA" },
  { 0xC023, "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256" },
  { 0xC024, "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384" },
  { 0xC025, "TLS_ECDH_ECDSA_WITH_AES_128_CBC_SHA256" },
  { 0xC026, "TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA384" },
  { 0xC027, "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256" },
  { 0xC028, "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384" },
  { 0xC029, "TLS_ECDH_RSA_WITH_AES_128_CBC_SHA256" },
  { 0xC02A, "TLS_ECDH_RSA_WITH_AES_256_CBC_SHA384" },
  { 0xC02B, "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256" },
  { 0xC02C, "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384" },
  { 0xC02D, "TLS_ECDH_ECDSA_WITH_AES_128_GCM_SHA256" },
  { 0xC02E, "TLS_ECDH_ECDSA_WITH_AES_256_GCM_SHA384" },
  { 0xC02F, "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256" },
  { 0xC030, "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384" },
  { 0xC031, "TLS_ECDH_RSA_WITH_AES_128_GCM_SHA256" },
  { 0xC032, "TLS_ECDH_RSA_WITH_AES_256_GCM_SHA384" },
  { 0xC072, "TLS_ECDHE_ECDSA_WITH_CAMELLIA_128_CBC_SHA256" },
  { 0xC073, "TLS_ECDHE_ECDSA_WITH_CAMELLIA_256_CBC_SHA384" },
  { 0xC076, "TLS_ECDHE_RSA_WITH_CAMELLIA_128_CBC_SHA256" },
  { 0xC077, "TLS_ECDHE_RSA_WITH_CAMELLIA_256_CBC_SHA384" },
  { 0xC07A, "TLS_RSA_WITH_CAMELLIA_128_GCM_SHA256" },
  { 0xC07B, "TLS_RSA_WITH_CAMELLIA_256_GCM_SHA384" },
  { 0xC07C, "TLS_DHE_RSA_WITH_CAMELLIA_128_GCM_SHA256" },
  { 0xC07D, "TLS_DHE_RSA_WITH_CAMELLIA_256_GCM_SHA384" },
  { 0xC07E, "TLS_DH_RSA_WITH_CAMELLIA_128_GCM_SHA256" },
  { 0xC07F, "TLS_DH_RSA_WITH_CAMELLIA_256_GCM_SHA384" },
  { 0xC086, "TLS_ECDHE_ECDSA_WITH_CAMELLIA_128_GCM_SHA256" },
  { 0xC087, "TLS_ECDHE_ECDSA_WITH_CAMELLIA_256_GCM_SHA384" },
  { 0xC08A, "TLS_ECDHE_RSA_WITH_CAMELLIA_128_GCM_SHA256" },
  { 0xC08B, "TLS_ECDHE_RSA_WITH_CAMELLIA_256_GCM_SHA384" },
  { 0xC09C, "TLS_RSA_WITH_AES_128_CCM" },
  { 0xC09D, "TLS_RSA_WITH_AES_256_CCM" },
  { 0xC09E, "TLS_DHE_RSA_WITH_AES_128_CCM" },
  { 0xC09F, "TLS_DHE_RSA_WITH_AES_256_CCM" },
  { 0xC0A0, "TLS_RSA_WITH_AES_128_CCM_8" },
  { 0xC0A1, "TLS_RSA_WITH_AES_256_CCM_8" },
  { 0xC0A2, "TLS_DHE_RSA_WITH_AES_128_CCM_8" },
  { 0xC0A3, "TLS_DHE_RSA_WITH_AES_256_CCM_8" },
  { 0xC0AC, "TLS_ECDHE_ECDSA_WITH_AES_128_CCM" },
  { 0xC0AD, "TLS_ECDHE_ECDSA_WITH_AES_256_CCM" },
  { 0xC0AE, "TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8" },
  { 0xC0AF, "TLS_ECDHE_ECDSA_WITH_AES_256_CCM_8" },
  { 0xCCA8, "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256" },
  { 0xCCA9, "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256" },
  { 0xCCAA, "TLS_DHE_RSA_WITH_CHACHA20_POLY1305_SHA256" },
  { 0xCCAB, "TLS_PSK_WITH_CHACHA20_POLY1305_SHA256" },
  { 0xCCAC, "TLS_ECDHE_PSK_WITH_CHACHA20_POLY1305_SHA256" },
  { 0xCCAD, "TLS_DHE_PSK_WITH_CHACHA20_POLY1305_SHA256" },
  { 0xCCAE, "TLS_RSA_PSK_WITH_CHACHA20_POLY1305_SHA256" },
  { 0xFEFE, "SSL_RSA_FIPS_WITH_DES_CBC_SHA" },
  { 0xFEFF, "SSL_RSA_FIPS_WITH_3DES_EDE_CBC_SHA" },

  { 0, NULL }
};

/* Compressions */
static struct tls_label tls_compression_labels[] = {
  { 0x0000, "None" },
  { 0x0001, "Zlib" },

  { 0, NULL }
};

/* Extensions */
static struct tls_label tls_extension_labels[] = {
  { 0, "server_name" },
  { 1, "max_fragment_length" },
  { 2, "client_certificate_url" },
  { 3, "trusted_ca_keys" },
  { 4, "truncated_hmac" },
  { 5, "status_request" },
  { 6, "user_mapping" },
  { 7, "client_authz" },
  { 8, "server_authz" },
  { 9, "cert_type" },
  { 10, "elliptic_curves" },
  { 11, "ec_point_formats" },
  { 12, "srp" },
  { 13, "signature_algorithms" },
  { 14, "use_srtp" },
  { 15, "heartbeat" },
  { 16, "application_layer_protocol_negotiation" },
  { 18, "signed_certificate_timestamp" },
  { 21, "padding" },
  { 22, "encrypt_then_mac" },
  { 23, "extended_master_secret" },
  { 35, "session_ticket" },
  { 41, "psk" },
  { 42, "early_data" },
  { 43, "supported_versions" },
  { 44, "cookie" },
  { 45, "psk_kex_modes" },
  { 47, "certificate_authorities" },
  { 49, "post_handshake_auth" },
  { 50, "signature_algorithms_cert" },
  { 51, "key_share" },
  { 0xFF01, "renegotiate" },
  { 13172, "next_proto_neg" },

  { 0, NULL }
};

#if defined(TLSEXT_TYPE_signature_algorithms)
/* Signature Algorithms.  These values come from:
 *   https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#tls-parameters-16
 */
static struct tls_label tls_sigalgo_labels[] = {
  { 0x0201, "rsa_pkcs1_sha1" },
  { 0x0202, "dsa_sha1" },
  { 0x0203, "ecdsa_sha1" },
  { 0x0301, "rsa_pkcs1_sha224" },
  { 0x0302, "dsa_sha224" },
  { 0x0303, "ecdsa_sha224" },
  { 0x0401, "rsa_pkcs1_sha256" },
  { 0x0402, "dsa_sha256" },
  { 0x0403, "ecdsa_secp256r1_sha256" },
  { 0x0501, "rsa_pkcs1_sha384" },
  { 0x0502, "dsa_sha384" },
  { 0x0503, "ecdsa_secp384r1_sha384" },
  { 0x0601, "rsa_pkcs1_sha512" },
  { 0x0602, "dsa_sha512" },
  { 0x0603, "ecdsa_secp521r1_sha512" },
  { 0x0804, "rsa_pss_rsae_sha256" },
  { 0x0805, "rsa_pss_rsae_sha384" },
  { 0x0806, "rsa_pss_rsae_sha512" },
  { 0x0807, "ed25519" },
  { 0x0808, "ed448" },
  { 0x0809, "rsa_pss_pss_sha256" },
  { 0x080A, "rsa_pss_pss_sha384" },
  { 0x080B, "rsa_pss_pss_sha512" },

  { 0, NULL }
};
#endif /* TLSEXT_TYPE_signature_algorithms */

#if defined(TLSEXT_TYPE_psk_kex_modes)
/* PSK KEX modes.  These values come from:
 *   https://tools.ietf.org/html/rfc8446#section-4.2.9
 */
static struct tls_label tls_psk_kex_labels[] = {
  { 0, "psk_ke" },
  { 1, "psk_dhe_key" },

  { 0, NULL }
};
#endif /* TLSEXT_TYPE_psk_kex_modes */

static const char *tls_get_label(int labelno, struct tls_label *labels) {
  register unsigned int i;

  for (i = 0; labels[i].label_name != NULL; i++) {
    if (labels[i].labelno == labelno) {
      return labels[i].label_name;
    }
  }

  return "[unknown/unsupported]";
}

static void tls_print_ssl_version(BIO *bio, const char *name,
    const unsigned char **msg, size_t *msglen, int *pversion) {
  int version;

  if (*msglen < 2) {
    return;
  }

  version = ((*msg)[0] << 8) | (*msg)[1];
  BIO_printf(bio, "  %s = %s\n", name,
    tls_get_label(version, tls_version_labels));
  *msg += 2;
  *msglen -= 2;

  if (pversion != NULL) {
    *pversion = version;
  }
}

static void tls_print_hex(BIO *bio, const char *indent, const char *name,
    const unsigned char *msg, size_t msglen) {

  BIO_printf(bio, "%s (%lu %s)\n", name, (unsigned long) msglen,
    msglen != 1 ? "bytes" : "byte");

  if (msglen > 0) {
    register unsigned int i;

    BIO_puts(bio, indent);
    for (i = 0; i < msglen; i++) {
      BIO_printf(bio, "%02x", msg[i]);
    }
    BIO_puts(bio, "\n");
  }
}

static void tls_print_hexbuf(BIO *bio, const char *indent, const char *name,
    size_t namelen, const unsigned char **msg, size_t *msglen) {
  size_t buflen;
  const unsigned char *ptr;

  if (*msglen < namelen) {
    return;
  }

  ptr = *msg;
  buflen = ptr[0];

  if (namelen > 1) {
    buflen = (buflen << 8) | ptr[1];
  }

  if (*msglen < namelen + buflen) {
    return;
  }

  ptr += namelen;
  tls_print_hex(bio, indent, name, ptr, buflen);
  *msg += (buflen + namelen);
  *msglen -= (buflen + namelen);
}

static void tls_print_random(BIO *bio, const unsigned char **msg,
    size_t *msglen) {
  pool *tmp_pool;
  time_t ts;
  const unsigned char *ptr;

  if (*msglen < 32) {
    return;
  }

  ptr = *msg;

  ts = ((ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3]);
  ptr += 4;

  tmp_pool = make_sub_pool(session.pool);
  pr_pool_tag(tmp_pool, "TLS Diags pool");

  BIO_puts(bio, "  random:\n");
  BIO_printf(bio, "    gmt_unix_time = %s (not guaranteed to be accurate)\n",
    pr_strtime3(tmp_pool, ts, TRUE));
  tls_print_hex(bio, "      ", "    random_bytes", ptr, 28);
  *msg += 32;
  *msglen -= 32;
  destroy_pool(tmp_pool);
}

static void tls_print_session_id(BIO *bio, const unsigned char **msg,
    size_t *msglen) {
  tls_print_hexbuf(bio, "    ", "  session_id", 1, msg, msglen);
}

static void tls_print_ciphersuites(BIO *bio, const char *name,
    const unsigned char **msg, size_t *msglen) {
  size_t len;

  len = ((*msg[0]) << 8) | (*msg)[1];
  *msg += 2;
  *msglen -= 2;
  BIO_printf(bio, "  %s (%lu %s)\n", name, (unsigned long) len,
    len != 1 ? "bytes" : "byte");
  if (*msglen < len ||
      (len & 1)) {
    return;
  }

  while (len > 0) {
    unsigned int suiteno;

    pr_signals_handle();

    suiteno = ((*msg[0]) << 8) | (*msg)[1];
    BIO_printf(bio, "    %s (0x%x)\n",
      tls_get_label(suiteno, tls_ciphersuite_labels), suiteno);

    *msg += 2;
    *msglen -= 2;
    len -= 2;
  }
}

static void tls_print_compressions(BIO *bio, const char *name,
    const unsigned char **msg, size_t *msglen) {
  size_t len;

  len = (*msg)[0];
  *msg += 1;
  *msglen -= 1;

  if (*msglen < len) {
    return;
  }

  BIO_printf(bio, "  %s (%lu %s)\n", name, (unsigned long) len,
    len != 1 ? "bytes" : "byte");
  while (len > 0) {
    int comp_type;

    pr_signals_handle();

    comp_type = (*msg)[0];
    BIO_printf(bio, "    %s\n",
      tls_get_label(comp_type, tls_compression_labels));

    *msg += 1;
    *msglen -= 1;
    len -= 1;
  }
}

static void tls_print_extension(BIO *bio, const char *indent, int server,
    int ext_type, const unsigned char *ext, size_t extlen) {

  BIO_printf(bio, "%sextension_type = %s (%lu %s)\n", indent,
    tls_get_label(ext_type, tls_extension_labels), (unsigned long) extlen,
    extlen != 1 ? "bytes" : "byte");
}

static void tls_print_extensions(BIO *bio, const char *name, int server,
    const unsigned char **msg, size_t *msglen) {
  size_t len;

  if (*msglen == 0) {
    BIO_printf(bio, "%s: None\n", name);
    return;
  }

  len = ((*msg)[0] << 8) | (*msg)[1];
  if (len != (*msglen - 2)) {
    return;
  }

  *msg += 2;
  *msglen -= 2;

  BIO_printf(bio, "  %s (%lu %s)\n", name, (unsigned long) len,
    len != 1 ? "bytes" : "byte");
  while (len > 0) {
    int ext_type;
    size_t ext_len;

    pr_signals_handle();

    if (*msglen < 4) {
      break;
    }

    ext_type = ((*msg)[0] << 8) | (*msg)[1];
    ext_len = ((*msg)[2] << 8) | (*msg)[3];

    if (*msglen < (ext_len + 4)) {
      break;
    }

    *msg += 4;
    tls_print_extension(bio, "    ", server, ext_type, *msg, ext_len);
    *msg += ext_len;
    *msglen -= (ext_len + 4);
  }
}

static void tls_print_client_hello(int io_flag, int version, int content_type,
    const unsigned char *buf, size_t buflen, SSL *ssl, void *arg) {
  BIO *bio;
  char *data = NULL;
  long datalen;

  bio = BIO_new(BIO_s_mem());

  BIO_puts(bio, "\nClientHello:\n");
  tls_print_ssl_version(bio, "client_version", &buf, &buflen, NULL);
  tls_print_random(bio, &buf, &buflen);
  tls_print_session_id(bio, &buf, &buflen);
  if (buflen < 2) {
    BIO_free(bio);
    return;
  }
  tls_print_ciphersuites(bio, "cipher_suites", &buf, &buflen);
  if (buflen < 1) {
    BIO_free(bio);
    return;
  }
  tls_print_compressions(bio, "compression_methods", &buf, &buflen);
  tls_print_extensions(bio, "extensions", FALSE, &buf, &buflen);

  datalen = BIO_get_mem_data(bio, &data);
  if (data != NULL) {
    data[datalen] = '\0';
    tls_log("[msg] %.*s", (int) datalen, data);
  }

  BIO_free(bio);
}

static void tls_print_server_hello(int io_flag, int version, int content_type,
    const unsigned char *buf, size_t buflen, SSL *ssl, void *arg) {
  BIO *bio;
  char *data = NULL;
  long datalen;
  int print_session_id = TRUE, print_compressions = TRUE, server_version = 0;
  unsigned int suiteno;

  bio = BIO_new(BIO_s_mem());

  BIO_puts(bio, "\nServerHello:\n");
  tls_print_ssl_version(bio, "server_version", &buf, &buflen, &server_version);

#ifdef TLS1_3_VERSION
  if (server_version == TLS1_3_VERSION) {
    print_session_id = FALSE;
    print_compressions = FALSE;
  }
#endif /* TLS1_3_VERSION */

  tls_print_random(bio, &buf, &buflen);
  if (print_session_id == TRUE) {
    tls_print_session_id(bio, &buf, &buflen);
  }
  if (buflen < 2) {
    BIO_free(bio);
    return;
  }

  /* Print the selected ciphersuite. */
  BIO_printf(bio, "  cipher_suites (2 bytes)\n");
  suiteno = (buf[0] << 8) | buf[1];
  BIO_printf(bio, "    %s (0x%x)\n",
    tls_get_label(suiteno, tls_ciphersuite_labels), suiteno);
  buf += 2;
  buflen -= 2;

  if (print_compressions == TRUE) {
    int comp_type;

    if (buflen < 1) {
      BIO_free(bio);
      return;
    }

    /* Print the selected compression. */
    BIO_printf(bio, "  compression_methods (1 byte)\n");
    comp_type = *buf;
    BIO_printf(bio, "    %s\n",
      tls_get_label(comp_type, tls_compression_labels));
    buf += 1;
    buflen -= 1;
  }
  tls_print_extensions(bio, "extensions", TRUE, &buf, &buflen);

  datalen = BIO_get_mem_data(bio, &data);
  if (data != NULL) {
    data[datalen] = '\0';
    tls_log("[msg] %.*s", (int) datalen, data);
  }

  BIO_free(bio);
}

#ifdef SSL3_MT_NEWSESSION_TICKET
static void tls_print_ticket(int io_flag, int version, int content_type,
    const unsigned char *buf, size_t buflen, SSL *ssl, void *arg) {
  BIO *bio;
  char *data = NULL;
  long datalen;

  bio = BIO_new(BIO_s_mem());

  BIO_puts(bio, "\nNewSessionTicket:\n");
  if (buflen != 0) {
    unsigned int ticket_lifetime;
    int print_ticket_age = FALSE, print_extensions = FALSE;

    ticket_lifetime = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    buf += 4;
    buflen -= 4;
    BIO_printf(bio, "  ticket_lifetime_hint\n    %u (sec)\n", ticket_lifetime);

# if defined(TLS1_3_VERSION)
    if (SSL_version(ssl) == TLS1_3_VERSION) {
      print_ticket_age = TRUE;
      print_extensions = TRUE;
    }
# endif /* TLS1_3_VERSION */

    if (print_ticket_age == TRUE) {
      unsigned int ticket_age_add;

      ticket_age_add = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
      buf += 4;
      buflen -= 4;
      BIO_printf(bio, "  ticket_age_add\n    %u (sec)\n", ticket_age_add);
      tls_print_hexbuf(bio, "    ", "  ticket_nonce", 1, &buf, &buflen);
    }

    tls_print_hexbuf(bio, "    ", "  ticket", 2, &buf, &buflen);

    if (print_extensions == TRUE) {
      tls_print_extensions(bio, "extensions", TRUE, &buf, &buflen);
    }

  } else {
    BIO_puts(bio, "  <no ticket>\n");
  }

  datalen = BIO_get_mem_data(bio, &data);
  if (data != NULL) {
    data[datalen] = '\0';
    tls_log("[msg] %.*s", (int) datalen, data);
  }

  BIO_free(bio);
}
#endif /* SSL3_MT_NEWSESSION_TICKET */

static void tls_msg_cb(int io_flag, int version, int content_type,
    const void *buf, size_t buflen, SSL *ssl, void *arg) {
  char *action_str = NULL, *version_str = NULL;
  char *bytes_str = buflen != 1 ? "bytes" : "byte";

  if (io_flag == 0) {
    action_str = "received";

  } else if (io_flag == 1) {
    action_str = "sent";
  }

  switch (version) {
    case SSL2_VERSION:
      version_str = "SSLv2";
      break;

    case SSL3_VERSION:
      version_str = "SSLv3";
      break;

    case TLS1_VERSION:
      version_str = "TLSv1";
      break;

# if defined(TLS1_1_VERSION)
    case TLS1_1_VERSION:
      version_str = "TLSv1.1";
      break;
# endif /* TLS1_1_VERSION */

# if defined(TLS1_2_VERSION)
    case TLS1_2_VERSION:
      version_str = "TLSv1.2";
      break;
# endif /* TLS1_2_VERSION */

# if OPENSSL_VERSION_NUMBER >= 0x10101000L && \
     defined(TLS1_3_VERSION)
    case TLS1_3_VERSION:
      version_str = "TLSv1.3";
      break;
# endif /* TLS1_3_VERSION */

    default:
#ifdef SSL3_RT_HEADER
      /* OpenSSL calls this callback for SSL records received; filter those
       * from true "unknowns".
       */
      if (version == 0 &&
          (content_type != SSL3_RT_HEADER ||
           buflen != SSL3_RT_HEADER_LENGTH)) {
        tls_log("[msg] unknown/unsupported version: %d", version);
      }
#else
      tls_log("[msg] unknown/unsupported version: %d", version);
#endif
      break;
  }

  if (version == SSL3_VERSION ||
#if OPENSSL_VERSION_NUMBER >= 0x10001000L
      version == TLS1_1_VERSION ||
      version == TLS1_2_VERSION ||
# if OPENSSL_VERSION_NUMBER >= 0x10101000L && \
     defined(TLS1_3_VERSION)
      version == TLS1_3_VERSION ||
# endif
#endif
      version == TLS1_VERSION) {

    switch (content_type) {
      case SSL3_RT_CHANGE_CIPHER_SPEC:
        /* ChangeCipherSpec message */
        pr_trace_msg(trace_channel, 27,
          "%s %s ChangeCipherSpec (%u %s)", action_str, version_str,
          (unsigned int) buflen, bytes_str);
        tls_log("[msg] %s %s ChangeCipherSpec message (%u %s)",
          action_str, version_str, (unsigned int) buflen, bytes_str);
        break;

      case SSL3_RT_ALERT: {
        /* Alert messages */
        pr_trace_msg(trace_channel, 27,
          "%s %s Alert (%u %s)", action_str, version_str,
          (unsigned int) buflen, bytes_str);
        if (buflen == 2) {
          char *severity_str = NULL;

          /* Peek naughtily into the buffer. */
          switch (((const unsigned char *) buf)[0]) {
            case SSL3_AL_WARNING:
              severity_str = "warning";
              break;

            case SSL3_AL_FATAL:
              severity_str = "fatal";
              break;
          }

          switch (((const unsigned char *) buf)[1]) {
            case SSL3_AD_CLOSE_NOTIFY:
              tls_log("[msg] %s %s %s 'close_notify' Alert message (%u %s)",
                action_str, version_str, severity_str, (unsigned int) buflen,
                bytes_str);
              break;

            case SSL3_AD_UNEXPECTED_MESSAGE:
              tls_log("[msg] %s %s %s 'unexpected_message' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;

            case SSL3_AD_BAD_RECORD_MAC:
              tls_log("[msg] %s %s %s 'bad_record_mac' Alert message (%u %s)",
                action_str, version_str, severity_str, (unsigned int) buflen,
                bytes_str);
              break;

            case 21:
              tls_log("[msg] %s %s %s 'decryption_failed' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;

            case 22:
              tls_log("[msg] %s %s %s 'record_overflow' Alert message (%u %s)",
                action_str, version_str, severity_str, (unsigned int) buflen,
                bytes_str);
              break;

            case SSL3_AD_DECOMPRESSION_FAILURE:
              tls_log("[msg] %s %s %s 'decompression_failure' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;

            case SSL3_AD_HANDSHAKE_FAILURE:
              tls_log("[msg] %s %s %s 'handshake_failure' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;

#ifdef SSL3_AD_NO_CERTIFICATE
            case SSL3_AD_NO_CERTIFICATE:
              tls_log("[msg] %s %s %s 'no_certificate' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;
#endif /* SSL3_AD_NO_CERTIFICATE */

#ifdef SSL3_AD_BAD_CERTIFICATE
            case SSL3_AD_BAD_CERTIFICATE:
              tls_log("[msg] %s %s %s 'bad_certificate' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;
#endif /* SSL3_AD_BAD_CERTIFICATE */

#ifdef SSL3_AD_UNSUPPORTED_CERTIFICATE
            case SSL3_AD_UNSUPPORTED_CERTIFICATE:
              tls_log("[msg] %s %s %s 'unsupported_certificate' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;
#endif /* SSL3_AD_UNSUPPORTED_CERTIFICATE */

#ifdef SSL3_AD_CERTIFICATE_REVOKED
            case SSL3_AD_CERTIFICATE_REVOKED:
              tls_log("[msg] %s %s %s 'certificate_revoked' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;
#endif /* SSL3_AD_CERTIFICATE_REVOKED */

#ifdef SSL3_AD_CERTIFICATE_EXPIRED
            case SSL3_AD_CERTIFICATE_EXPIRED:
              tls_log("[msg] %s %s %s 'certificate_expired' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;
#endif /* SSL3_AD_CERTIFICATE_EXPIRED */

#ifdef SSL3_AD_CERTIFICATE_UNKNOWN
            case SSL3_AD_CERTIFICATE_UNKNOWN:
              tls_log("[msg] %s %s %s 'certificate_unknown' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;
#endif /* SSL3_AD_CERTIFICATE_UNKNOWN */

#ifdef SSL3_AD_ILLEGAL_PARAMETER
            case SSL3_AD_ILLEGAL_PARAMETER:
              tls_log("[msg] %s %s %s 'illegal_parameter' Alert message "
                "(%u %s)", action_str, version_str, severity_str,
                (unsigned int) buflen, bytes_str);
              break;
#endif /* SSL3_AD_ILLEGAL_PARAMETER */
          }

        } else {
          tls_log("[msg] %s %s Alert message, unknown type (%u %s)", action_str,
            version_str, (unsigned int) buflen, bytes_str);
        }

        break;
      }

      case SSL3_RT_HANDSHAKE: {
        /* Handshake messages */
        pr_trace_msg(trace_channel, 27,
          "%s %s Handshake (%u %s)", action_str, version_str,
          (unsigned int) buflen, bytes_str);
        if (buflen > 0) {
          /* Peek naughtily into the buffer. */
          switch (((const unsigned char *) buf)[0]) {
            case SSL3_MT_HELLO_REQUEST:
              tls_log("[msg] %s %s 'HelloRequest' Handshake message (%u %s)",
                action_str, version_str, (unsigned int) buflen, bytes_str);
              break;

            case SSL3_MT_CLIENT_HELLO: {
              const unsigned char *msg;
              size_t msglen;

              msg = buf;
              msglen = buflen;

              tls_log("[msg] %s %s 'ClientHello' Handshake message (%u %s)",
                action_str, version_str, (unsigned int) msglen, bytes_str);

              tls_print_client_hello(io_flag, version, content_type, msg + 4,
                msglen - 4, ssl, arg);
              break;
            }

            case SSL3_MT_SERVER_HELLO: {
              const unsigned char *msg;
              size_t msglen;

              msg = buf;
              msglen = buflen;

              tls_log("[msg] %s %s 'ServerHello' Handshake message (%u %s)",
                action_str, version_str, (unsigned int) buflen, bytes_str);

              tls_print_server_hello(io_flag, version, content_type, msg + 4,
                msglen - 4, ssl, arg);
              break;
            }

#ifdef SSL3_MT_NEWSESSION_TICKET
            case SSL3_MT_NEWSESSION_TICKET: {
              const unsigned char *msg;
              size_t msglen;

              msg = buf;
              msglen = buflen;

              tls_log("[msg] %s %s 'NewSessionTicket' Handshake message "
                "(%u %s)", action_str, version_str, (unsigned int) buflen,
                bytes_str);

              tls_print_ticket(io_flag, version, content_type, msg + 4,
                msglen - 4, ssl, arg);
              break;
            }
#endif /* SSL3_MT_NEWSESSION_TICKET */

            case SSL3_MT_CERTIFICATE:
              tls_log("[msg] %s %s 'Certificate' Handshake message (%u %s)",
                action_str, version_str, (unsigned int) buflen, bytes_str);
              break;

            case SSL3_MT_SERVER_KEY_EXCHANGE:
              tls_log("[msg] %s %s 'ServerKeyExchange' Handshake message "
                "(%u %s)", action_str, version_str, (unsigned int) buflen,
                bytes_str);
              break;

            case SSL3_MT_CERTIFICATE_REQUEST:
              tls_log("[msg] %s %s 'CertificateRequest' Handshake message "
                "(%u %s)", action_str, version_str, (unsigned int) buflen,
                bytes_str);
              break;

            case SSL3_MT_SERVER_DONE:
              tls_log("[msg] %s %s 'ServerHelloDone' Handshake message (%u %s)",
                action_str, version_str, (unsigned int) buflen, bytes_str);
              break;

            case SSL3_MT_CERTIFICATE_VERIFY:
              tls_log("[msg] %s %s 'CertificateVerify' Handshake message "
                "(%u %s)", action_str, version_str, (unsigned int) buflen,
                bytes_str);
              break;

            case SSL3_MT_CLIENT_KEY_EXCHANGE:
              tls_log("[msg] %s %s 'ClientKeyExchange' Handshake message "
                "(%u %s)", action_str, version_str, (unsigned int) buflen,
                bytes_str);
              break;

            case SSL3_MT_FINISHED:
              tls_log("[msg] %s %s 'Finished' Handshake message (%u %s)",
                action_str, version_str, (unsigned int) buflen, bytes_str);
              break;

#ifdef SSL3_MT_CERTIFICATE_STATUS
            case SSL3_MT_CERTIFICATE_STATUS:
              tls_log("[msg] %s %s 'CertificateStatus' Handshake message "
                "(%u %s)", action_str, version_str, (unsigned int) buflen,
                bytes_str);
              break;
#endif /* SSL3_MT_CERTIFICATE_STATUS */

#ifdef SSL3_MT_ENCRYPTED_EXTENSIONS
            case SSL3_MT_ENCRYPTED_EXTENSIONS: {
              const unsigned char *msg;
              size_t msglen;
              BIO *bio;
              char *data = NULL;
              long datalen;

              msg = buf;
              msglen = buflen;

              tls_log("[msg] %s %s 'EncryptedExtensions' Handshake message "
                "(%u %s)", action_str, version_str, (unsigned int) buflen,
                bytes_str);

              bio = BIO_new(BIO_s_mem());

              msg += 4;
              msglen -= 4;

              BIO_puts(bio, "\nEncryptedExtensions:\n");
              tls_print_extensions(bio, "extensions", TRUE, &msg, &msglen);
              datalen = BIO_get_mem_data(bio, &data);
              if (data != NULL) {
                data[datalen] = '\0';
                tls_log("[msg] %.*s", (int) datalen, data);
              }

              BIO_free(bio);
              break;
            }
#endif /* SSL3_MT_ENCRYPTED_EXTENSIONS */
          }

        } else {
          tls_log("[msg] %s %s Handshake message, unknown type %d (%u %s)",
            action_str, version_str, content_type, (unsigned int) buflen,
            bytes_str);
        }

        break;
      }
    }

  } else if (version == SSL2_VERSION) {
    /* SSLv2 message.  Ideally we wouldn't get these, but sometimes badly
     * behaving FTPS clients send them.
     */

    if (buflen > 0) {
      /* Peek naughtily into the buffer. */

      switch (((const unsigned char *) buf)[0]) {
        case 0: {
          /* Error */
          if (buflen > 3) {
            unsigned err_code = (((const unsigned char *) buf)[1] << 8) +
              ((const unsigned char *) buf)[2];

            switch (err_code) {
              case 0x0001:
                tls_log("[msg] %s %s 'NO-CIPHER-ERROR' Error message (%u %s)",
                  action_str, version_str, (unsigned int) buflen, bytes_str);
                break;

              case 0x0002:
                tls_log("[msg] %s %s 'NO-CERTIFICATE-ERROR' Error message "
                  "(%u %s)", action_str, version_str, (unsigned int) buflen,
                  bytes_str);
                break;

              case 0x0004:
                tls_log("[msg] %s %s 'BAD-CERTIFICATE-ERROR' Error message "
                  "(%u %s)", action_str, version_str, (unsigned int) buflen,
                  bytes_str);
                break;

              case 0x0006:
                tls_log("[msg] %s %s 'UNSUPPORTED-CERTIFICATE-TYPE-ERROR' "
                  "Error message (%u %s)", action_str, version_str,
                  (unsigned int) buflen, bytes_str);
                break;
            }

          } else {
            tls_log("[msg] %s %s Error message, unknown type %d (%u %s)",
              action_str, version_str, content_type, (unsigned int) buflen,
              bytes_str);
          }
          break;
        }

        case 1:
          tls_log("[msg] %s %s 'CLIENT-HELLO' message (%u %s)", action_str,
            version_str, (unsigned int) buflen, bytes_str);
          break;

        case 2:
          tls_log("[msg] %s %s 'CLIENT-MASTER-KEY' message (%u %s)", action_str,
            version_str, (unsigned int) buflen, bytes_str);
          break;

        case 3:
          tls_log("[msg] %s %s 'CLIENT-FINISHED' message (%u %s)", action_str,
            version_str, (unsigned int) buflen, bytes_str);
          break;

        case 4:
          tls_log("[msg] %s %s 'SERVER-HELLO' message (%u %s)", action_str,
            version_str, (unsigned int) buflen, bytes_str);
          break;

        case 5:
          tls_log("[msg] %s %s 'SERVER-VERIFY' message (%u %s)", action_str,
            version_str, (unsigned int) buflen, bytes_str);
          break;

        case 6:
          tls_log("[msg] %s %s 'SERVER-FINISHED' message (%u %s)", action_str,
            version_str, (unsigned int) buflen, bytes_str);
          break;

        case 7:
          tls_log("[msg] %s %s 'REQUEST-CERTIFICATE' message (%u %s)",
            action_str, version_str, (unsigned int) buflen, bytes_str);
          break;

        case 8:
          tls_log("[msg] %s %s 'CLIENT-CERTIFICATE' message (%u %s)",
            action_str, version_str, (unsigned int) buflen, bytes_str);
          break;
      }

    } else {
      tls_log("[msg] %s %s message (%u %s)", action_str, version_str,
        (unsigned int) buflen, bytes_str);
    }

#ifdef SSL3_RT_HEADER
  } else if (version == 0 &&
             content_type == SSL3_RT_HEADER &&
             buflen == SSL3_RT_HEADER_LENGTH) {
    const unsigned char *msg;
    const char *record_type;
    int msg_len;

    msg = buf;
    record_type = tls_get_label(msg[0], tls_record_type_labels);
    msg_len = msg[buflen - 2] << 8 | msg[buflen - 1];

    tls_log("[msg] %s protocol record message (content type = %s, len = %d)",
      action_str, record_type, msg_len);
#endif

  } else {
    /* This case might indicate an issue with OpenSSL itself; the version
     * given to the msg_callback function was not initialized, or not set to
     * one of the recognized SSL/TLS versions.  Weird.
     */

    tls_log("[msg] %s message of unknown version %d, type %d (%u %s)",
      action_str, version, content_type, (unsigned int) buflen, bytes_str);
  }

}
#endif

static const char *get_printable_subjaltname(pool *p, const char *data,
    size_t datalen) {
  register unsigned int i;
  char *ptr, *res;
  size_t reslen = 0;

  /* First, calculate the length of the resulting printable string we'll
   * be generating.
   */

  for (i = 0; i < datalen; i++) {
    if (PR_ISPRINT(data[i])) {
      reslen++;

    } else {
      reslen += 4;
    }
  }

  /* Leave one space in the allocated string for the terminating NUL. */
  ptr = res = pcalloc(p, reslen + 1);

  for (i = 0; i < datalen; i++) {
    if (PR_ISPRINT(data[i])) {
      *(ptr++) = data[i];

    } else {
      pr_snprintf(ptr, reslen - (ptr - res), "\\x%02x", data[i]);
      ptr += 4;
    }
  }

  return res;
}

static int tls_cert_match_dns_san(pool *p, X509 *cert, const char *dns_name) {
  int matched = 0;
#if OPENSSL_VERSION_NUMBER > 0x000907000L
  STACK_OF(GENERAL_NAME) *sans;

  sans = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
  if (sans != NULL) {
    register int i;
    int nsans = sk_GENERAL_NAME_num(sans);

    for (i = 0; i < nsans; i++) {
      GENERAL_NAME *alt_name;

      pr_signals_handle();

      alt_name = sk_GENERAL_NAME_value(sans, i);
      if (alt_name->type == GEN_DNS) {
        char *dns_san;
        size_t dns_sanlen;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
        dns_san = (char *) ASN1_STRING_get0_data(alt_name->d.ia5);
#else
        dns_san = (char *) ASN1_STRING_data(alt_name->d.ia5);
#endif /* OpenSSL 1.1.x and later */
        dns_sanlen = strlen(dns_san);

        /* Check for subjectAltName values which contain embedded NULs.
         * This can cause verification problems (spoofing), e.g. if the
         * string is "www.goodguy.com\0www.badguy.com"; the use of strcmp()
         * only checks "www.goodguy.com".
         */

        if ((size_t) ASN1_STRING_length(alt_name->d.ia5) != dns_sanlen) {
          tls_log("%s", "cert dNSName SAN contains embedded NULs, "
            "rejecting as possible spoof attempt");
          tls_log("suspicious dNSName SAN value: '%s'",
            get_printable_subjaltname(p, dns_san,
              ASN1_STRING_length(alt_name->d.dNSName)));

          GENERAL_NAME_free(alt_name);
          sk_GENERAL_NAME_free(sans);
          return 0;
        }

        if (strncasecmp(dns_name, dns_san, dns_sanlen + 1) == 0) {
          pr_trace_msg(trace_channel, 8,
            "found cert dNSName SAN matching '%s'", dns_name);
          matched = 1;

        } else {
          pr_trace_msg(trace_channel, 9,
            "cert dNSName SAN '%s' did not match '%s'", dns_san, dns_name);
        }
      }

      GENERAL_NAME_free(alt_name);

      if (matched == 1) {
        break;
      }
    }

    sk_GENERAL_NAME_free(sans);
  }
#endif /* OpenSSL-0.9.7 or later */

  return matched;
}

static int tls_cert_match_ip_san(pool *p, X509 *cert, const char *ipstr) {
  int matched = 0;
  STACK_OF(GENERAL_NAME) *sans;

  sans = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
  if (sans != NULL) {
    register int i;
    int nsans = sk_GENERAL_NAME_num(sans);

    for (i = 0; i < nsans; i++) {
      GENERAL_NAME *alt_name;

      pr_signals_handle();

      alt_name = sk_GENERAL_NAME_value(sans, i);
      if (alt_name->type == GEN_IPADD) {
        const unsigned char *san_data = NULL;
        int have_ipstr = FALSE, san_datalen;
#ifdef PR_USE_IPV6
        char san_ipstr[INET6_ADDRSTRLEN + 1] = {'\0'};
#else
        char san_ipstr[INET_ADDRSTRLEN + 1] = {'\0'};
#endif /* PR_USE_IPV6 */

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
        san_data = ASN1_STRING_get0_data(alt_name->d.ip);
#else
        san_data = ASN1_STRING_data(alt_name->d.ip);
#endif /* OpenSSL 1.1.x and later */

        memset(san_ipstr, '\0', sizeof(san_ipstr));

        san_datalen = ASN1_STRING_length(alt_name->d.ip);
        if (san_datalen == 4) {
          /* IPv4 address */
          pr_snprintf(san_ipstr, sizeof(san_ipstr)-1, "%u.%u.%u.%u",
            san_data[0], san_data[1], san_data[2], san_data[3]);
          have_ipstr = TRUE;

#ifdef PR_USE_IPV6
        } else if (san_datalen == 16) {
          /* IPv6 address */

          if (pr_inet_ntop(AF_INET6, san_data, san_ipstr,
              sizeof(san_ipstr)-1) == NULL) {
            tls_log("unable to convert cert iPAddress SAN value (length %d) "
              "to IPv6 representation: %s", san_datalen, strerror(errno));

          } else {
            have_ipstr = TRUE;
          }

#endif /* PR_USE_IPV6 */
        } else {
          pr_trace_msg(trace_channel, 3,
            "unsupported cert SAN ipAddress length (%d), ignoring",
            san_datalen);
          continue;
        }

        if (have_ipstr) {
          size_t san_ipstrlen;

          san_ipstrlen = strlen(san_ipstr);

          if (strncmp(ipstr, san_ipstr, san_ipstrlen + 1) == 0) {
            pr_trace_msg(trace_channel, 8,
              "found cert iPAddress SAN matching '%s'", ipstr);
            matched = 1;

          } else {
            if (san_datalen == 16) {
              /* We need to handle the case where the iPAddress SAN might
               * have contained an IPv4-mapped IPv6 address, and we're
               * comparing against an IPv4 address.
               */
              if (san_ipstrlen > 7 &&
                  strncasecmp(san_ipstr, "::ffff:", 7) == 0) {
                if (strncmp(ipstr, san_ipstr + 7, san_ipstrlen - 6) == 0) {
                  pr_trace_msg(trace_channel, 8,
                    "found cert iPAddress SAN '%s' matching '%s'",
                    san_ipstr, ipstr);
                    matched = 1;
                }
              }

            } else {
              pr_trace_msg(trace_channel, 9,
                "cert iPAddress SAN '%s' did not match '%s'", san_ipstr, ipstr);
            }
          }
        }
      }

      GENERAL_NAME_free(alt_name);

      if (matched == 1) {
        break;
      }
    }

    sk_GENERAL_NAME_free(sans);
  }

  return matched;
}

static char *tls_get_cert_cn(pool *p, X509 *cert) {
  int idx = -1;
  X509_NAME *subj_name = NULL;
  X509_NAME_ENTRY *cn_entry = NULL;
  ASN1_STRING *cn_asn1 = NULL;
  char *cn_str = NULL;
  size_t cn_len = 0;

  /* Find the position of the CommonName (CN) field within the Subject of
   * the cert.
   */
  subj_name = X509_get_subject_name(cert);
  if (subj_name == NULL) {
    errno = ENOENT;
    return NULL;
  }

  idx = X509_NAME_get_index_by_NID(subj_name, NID_commonName, -1);
  if (idx < 0) {
    errno = ENOENT;
    return NULL;
  }

  cn_entry = X509_NAME_get_entry(subj_name, idx);
  if (cn_entry == NULL) {
    errno = ENOENT;
    return NULL;
  }

  /* Convert the CN field to a string, by way of an ASN1 object. */
  cn_asn1 = X509_NAME_ENTRY_get_data(cn_entry);
  if (cn_asn1 == NULL) {
    pr_trace_msg(trace_channel, 12,
      "error converting CommoName attribute to ASN.1: %s", tls_get_errors());
    errno = EPERM;
    return NULL;
  }

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
  cn_str = (char *) ASN1_STRING_get0_data(cn_asn1);
#else
  cn_str = (char *) ASN1_STRING_data(cn_asn1);
#endif /* OpenSSL 1.1.x and later */

  /* Check for CommonName values which contain embedded NULs.  This can cause
   * verification problems (spoofing), e.g. if the string is
   * "www.goodguy.com\0www.badguy.com"; the use of strcmp() only checks
   * "www.goodguy.com".
   */

  cn_len = strlen(cn_str);

  if ((size_t) ASN1_STRING_length(cn_asn1) != cn_len) {
    tls_log("%s", "cert CommonName contains embedded NULs, rejecting as "
      "possible spoof attempt");
    tls_log("suspicious CommonName value: '%s'",
      get_printable_subjaltname(p, (const char *) cn_str,
        ASN1_STRING_length(cn_asn1)));
    errno = EPERM;
    return NULL;
  }

  return pstrdup(p, cn_str);
}

static int tls_cert_match_cn(pool *p, X509 *cert, const char *name,
    int allow_wildcards) {
  int matched = 0;
  char *cert_cn = NULL;

  cert_cn = tls_get_cert_cn(p, cert);
  if (cert_cn == NULL) {
    return 0;
  }

  /* Yes, this is deliberately a case-insensitive comparison.  Most CNs
   * contain a hostname (case-insensitive); if they contain an IP address,
   * the case-insensitivity won't hurt anything.  In fact, it's needed for
   * e.g. IPv6 addresses.
   */
  if (strcasecmp(name, cert_cn) == 0) {
    matched = 1;
  }

  if (matched == 0 &&
      allow_wildcards == TRUE) {

    /* XXX Implement wildcard checking. */
  }

  return matched;
}

static int tls_check_client_cert(SSL *ssl, conn_t *conn) {
  X509 *cert = NULL;
  unsigned char have_cn = FALSE, have_dns_ext = FALSE, have_ipaddr_ext = FALSE;
  int ok = -1;

  /* Only perform these more stringent checks if asked to verify clients. */
  if (!(tls_flags & TLS_SESS_VERIFY_CLIENT_REQUIRED)) {
    return 0;
  }

  /* Only perform these checks is configured to do so. */
  if (!(tls_opts & TLS_OPT_VERIFY_CERT_FQDN) &&
      !(tls_opts & TLS_OPT_VERIFY_CERT_IP_ADDR) &&
      !(tls_opts & TLS_OPT_VERIFY_CERT_CN)) {
    return 0;
  }

  cert = SSL_get_peer_certificate(ssl);
  if (cert == NULL) {
    /* This can be null in the case where some anonymous (insecure)
     * cipher suite was used.
     */
    tls_log("unable to verify '%s': client did not provide certificate",
      conn->remote_name);
    return -1;
  }

  /* Check the DNS SANs if configured. */
  if (tls_opts & TLS_OPT_VERIFY_CERT_FQDN) {
    int matched;

    matched = tls_cert_match_dns_san(conn->pool, cert, conn->remote_name);
    if (matched == 0) {
      tls_log("client cert dNSName SANs do not match remote name '%s'",
        conn->remote_name);
      return -1;

    } else {
      tls_log("client cert dNSName SAN matches remote name '%s'",
        conn->remote_name);
      have_dns_ext = TRUE;
      ok = 1;
    }
  }

  /* Check the IP SANs, if configured. */
  if (tls_opts & TLS_OPT_VERIFY_CERT_IP_ADDR) {
    int matched;

    matched = tls_cert_match_ip_san(conn->pool, cert,
      pr_netaddr_get_ipstr(conn->remote_addr));
    if (matched == 0) {
      tls_log("client cert iPAddress SANs do not match client IP '%s'",
        pr_netaddr_get_ipstr(conn->remote_addr));
      return -1;

    } else {
      tls_log("client cert iPAddress SAN matches client IP '%s'",
        pr_netaddr_get_ipstr(conn->remote_addr));
      have_ipaddr_ext = TRUE;
      ok = 1;
    }
  }

  /* Check the CN (Common Name) if configured. */
  if (tls_opts & TLS_OPT_VERIFY_CERT_CN) {
    int matched;

    matched = tls_cert_match_cn(conn->pool, cert, conn->remote_name, TRUE);
    if (matched == 0) {
      tls_log("client cert CommonName does not match client FQDN '%s'",
        conn->remote_name);
      return -1;

    } else {
      tls_log("client cert CommonName matches client FQDN '%s'",
        conn->remote_name);
      have_cn = TRUE;
      ok = 1;
    }
  }

  if ((tls_opts & TLS_OPT_VERIFY_CERT_CN) &&
      !have_cn) {
    tls_log("%s", "client cert missing required X509v3 CommonName");
  }

  if ((tls_opts & TLS_OPT_VERIFY_CERT_FQDN) &&
      !have_dns_ext) {
    tls_log("%s", "client cert missing required X509v3 SubjectAltName dNSName");
  }

  if ((tls_opts & TLS_OPT_VERIFY_CERT_IP_ADDR) &&
      !have_ipaddr_ext) {
    tls_log("%s", "client cert missing required X509v3 SubjectAltName iPAddress");
  }

  X509_free(cert);
  return ok;
}

static int tls_check_server_cert(SSL *ssl, conn_t *conn) {
  X509 *cert = NULL;
  int ok = -1;
  long verify_result;

  /* Only perform these more stringent checks if asked to verify servers. */
  if (!(tls_flags & TLS_SESS_VERIFY_SERVER) &&
      !(tls_flags & TLS_SESS_VERIFY_SERVER_NO_DNS)) {
    return 0;
  }

  /* Check SSL_get_verify_result */
  verify_result = SSL_get_verify_result(ssl);
  if (verify_result != X509_V_OK) {
    tls_log("unable to verify '%s' server certificate: %s",
      conn->remote_name, X509_verify_cert_error_string(verify_result));
    return -1;
  }

  cert = SSL_get_peer_certificate(ssl);
  if (cert == NULL) {
    /* This can be null in the case where some anonymous (insecure)
     * cipher suite was used.
     */
    tls_log("unable to verify '%s': server did not provide certificate",
      conn->remote_name);
    return -1;
  }

  /* XXX If using OpenSSL-1.0.2/1.1.0, we might be able to use:
   * X509_match_host() and X509_match_ip()/X509_match_ip_asc().
   */

  ok = tls_cert_match_ip_san(conn->pool, cert,
    pr_netaddr_get_ipstr(conn->remote_addr));
  if (ok == 0) {
    ok = tls_cert_match_cn(conn->pool, cert,
      pr_netaddr_get_ipstr(conn->remote_addr), FALSE);
  }

  if (ok == 0 &&
      !(tls_flags & TLS_SESS_VERIFY_SERVER_NO_DNS)) {
    int reverse_dns;
    const char *remote_name;
    pr_netaddr_t *remote_addr;

    reverse_dns = pr_netaddr_set_reverse_dns(TRUE);

    pr_netaddr_clear_ipcache(pr_netaddr_get_ipstr(conn->remote_addr));

    remote_addr = (pr_netaddr_t *) conn->remote_addr;
    remote_addr->na_have_dnsstr = FALSE;

    remote_name = pr_netaddr_get_dnsstr(conn->remote_addr);
    pr_netaddr_set_reverse_dns(reverse_dns);

    ok = tls_cert_match_dns_san(conn->pool, cert, remote_name);
    if (ok == 0) {
      ok = tls_cert_match_cn(conn->pool, cert, remote_name, TRUE);
    }
  }

  X509_free(cert);
  return ok;
}

struct tls_pkey_data {
  server_rec *s;
  int flags;
  char *buf;
  size_t buflen, bufsz;
  const char *prompt;
};

static void tls_prepare_provider_fds(int stdout_fd, int stderr_fd) {
  unsigned long nfiles = 0;
  register unsigned int i = 0;
  struct rlimit rlim;

  if (stdout_fd != STDOUT_FILENO) {
    if (dup2(stdout_fd, STDOUT_FILENO) < 0)
      tls_log("error duping fd %d to stdout: %s", stdout_fd, strerror(errno));

    close(stdout_fd);
  }

  if (stderr_fd != STDERR_FILENO) {
    if (dup2(stderr_fd, STDERR_FILENO) < 0)
      tls_log("error duping fd %d to stderr: %s", stderr_fd, strerror(errno));

    close(stderr_fd);
  }

  /* Make sure not to pass on open file descriptors. For stdout and stderr,
   * we dup some pipes, so that we can capture what the command may write
   * to stdout or stderr.  The stderr output will be logged to the TLSLog.
   *
   * First, use getrlimit() to obtain the maximum number of open files
   * for this process -- then close that number.
   */
#if defined(RLIMIT_NOFILE) || defined(RLIMIT_OFILE)
# if defined(RLIMIT_NOFILE)
  if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
# elif defined(RLIMIT_OFILE)
  if (getrlimit(RLIMIT_OFILE, &rlim) < 0) {
# endif
    /* Ignore ENOSYS (and EPERM, since some libc's use this as ENOSYS). */
    if (errno != ENOSYS &&
        errno != EPERM) {
      tls_log("getrlimit error: %s", strerror(errno));
    }

    /* Pick some arbitrary high number. */
    nfiles = 255;

  } else
    nfiles = (unsigned long) rlim.rlim_max;
#else /* no RLIMIT_NOFILE or RLIMIT_OFILE */
   nfiles = 255;
#endif

  if (nfiles > 255) {
    nfiles = 255;
  }

  /* Close the "non-standard" file descriptors. */
  for (i = 3; i < nfiles; i++) {
    (void) close(i);
  }

  return;
}

static void tls_prepare_provider_pipes(int *stdout_pipe, int *stderr_pipe) {
  if (pipe(stdout_pipe) < 0) {
    pr_trace_msg(trace_channel, 2, "error opening stdout pipe: %s",
      strerror(errno));
    stdout_pipe[0] = -1;
    stdout_pipe[1] = STDOUT_FILENO;

  } else {
    if (fcntl(stdout_pipe[0], F_SETFD, FD_CLOEXEC) < 0) {
      pr_trace_msg(trace_channel, 2,
        "error setting close-on-exec flag on stdout pipe read fd: %s",
        strerror(errno));
    }

    if (fcntl(stdout_pipe[1], F_SETFD, 0) < 0) {
      pr_trace_msg(trace_channel, 2,
        "error setting close-on-exec flag on stdout pipe write fd: %s",
        strerror(errno));
    }
  }

  if (pipe(stderr_pipe) < 0) {
    pr_trace_msg(trace_channel, 2, "error opening stderr pipe: %s",
      strerror(errno));
    stderr_pipe[0] = -1;
    stderr_pipe[1] = STDERR_FILENO;

  } else {
    if (fcntl(stderr_pipe[0], F_SETFD, FD_CLOEXEC) < 0) {
      pr_trace_msg(trace_channel, 2,
        "error setting close-on-exec flag on stderr pipe read fd: %s",
        strerror(errno));
    }

    if (fcntl(stderr_pipe[1], F_SETFD, 0) < 0) {
      pr_trace_msg(trace_channel, 2,
        "error setting close-on-exec flag on stderr pipe write fd: %s",
        strerror(errno));
    }
  }
}

static int tls_exec_passphrase_provider(server_rec *s, char *buf, int buflen,
    int flags) {
  pid_t pid;
  int status;
  int stdout_pipe[2], stderr_pipe[2];

  struct sigaction sa_ignore, sa_intr, sa_quit;
  sigset_t set_chldmask, set_save;

  /* Prepare signal dispositions. */
  sa_ignore.sa_handler = SIG_IGN;
  sigemptyset(&sa_ignore.sa_mask);
  sa_ignore.sa_flags = 0;

  if (sigaction(SIGINT, &sa_ignore, &sa_intr) < 0) {
    return -1;
  }

  if (sigaction(SIGQUIT, &sa_ignore, &sa_quit) < 0) {
    return -1;
  }

  sigemptyset(&set_chldmask);
  sigaddset(&set_chldmask, SIGCHLD);

  if (sigprocmask(SIG_BLOCK, &set_chldmask, &set_save) < 0) {
    return -1;
  }

  tls_prepare_provider_pipes(stdout_pipe, stderr_pipe);

  pid = fork();
  if (pid < 0) {
    int xerrno = errno;

    pr_log_pri(PR_LOG_ALERT,
      MOD_TLS_VERSION ": error: unable to fork: %s", strerror(xerrno));

    errno = xerrno;
    status = -1;

  } else if (pid == 0) {
    char nbuf[32];
    pool *tmp_pool;
    char *stdin_argv[4];

    /* Child process */
    session.pid = getpid();

    /* Note: there is no need to clean up this temporary pool, as we've
     * forked.  If the exec call succeeds, this child process will exit
     * normally, and its process space recovered by the OS.  If the exec
     * call fails, we still exit, and the process space is recovered by
     * the OS.  Either way, the memory will be cleaned up without need for
     * us to do it explicitly (unless one wanted to be pedantic about it,
     * of course).
     */
    tmp_pool = make_sub_pool(s->pool);

    /* Restore previous signal actions. */
    sigaction(SIGINT, &sa_intr, NULL);
    sigaction(SIGQUIT, &sa_quit, NULL);
    sigprocmask(SIG_SETMASK, &set_save, NULL);

    stdin_argv[0] = pstrdup(tmp_pool, tls_passphrase_provider);

    memset(nbuf, '\0', sizeof(nbuf));
    pr_snprintf(nbuf, sizeof(nbuf)-1, "%u", (unsigned int) s->ServerPort);
    nbuf[sizeof(nbuf)-1] = '\0';
    stdin_argv[1] = pstrcat(tmp_pool, s->ServerName, ":", nbuf, NULL);

    if (flags & TLS_PASSPHRASE_FL_RSA_KEY) {
      stdin_argv[2] = pstrdup(tmp_pool, "RSA");

    } else if (flags & TLS_PASSPHRASE_FL_DSA_KEY) {
      stdin_argv[2] = pstrdup(tmp_pool, "DSA");

    } else if (flags & TLS_PASSPHRASE_FL_EC_KEY) {
      stdin_argv[2] = pstrdup(tmp_pool, "EC");

    } else if (flags & TLS_PASSPHRASE_FL_PKCS12_PASSWD) {
      stdin_argv[2] = pstrdup(tmp_pool, "PKCS12");
    }

    stdin_argv[3] = NULL;

    PRIVS_ROOT

    pr_trace_msg(trace_channel, 17,
      "executing '%s' with uid %lu (euid %lu), gid %lu (egid %lu)",
      tls_passphrase_provider,
      (unsigned long) getuid(), (unsigned long) geteuid(),
      (unsigned long) getgid(), (unsigned long) getegid());

    pr_log_debug(DEBUG6, MOD_TLS_VERSION
      ": executing '%s' with uid %lu (euid %lu), gid %lu (egid %lu)",
      tls_passphrase_provider,
      (unsigned long) getuid(), (unsigned long) geteuid(),
      (unsigned long) getgid(), (unsigned long) getegid());

    /* Prepare the file descriptors that the process will inherit. */
    tls_prepare_provider_fds(stdout_pipe[1], stderr_pipe[1]);

    errno = 0;
    execv(tls_passphrase_provider, stdin_argv);

    /* Since all previous file descriptors (including those for log files)
     * have been closed, and root privs have been revoked, there's little
     * chance of directing a message of execv() failure to proftpd's log
     * files.  execv() only returns if there's an error; the only way we
     * can signal this to the waiting parent process is to exit with a
     * non-zero value (the value of errno will do nicely).
     */

    exit(errno);

  } else {
    int res;
    int maxfd, fds, send_sigterm = 1;
    fd_set readfds;
    time_t start_time = time(NULL);
    struct timeval tv;

    /* Parent process */

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    maxfd = (stderr_pipe[0] > stdout_pipe[0]) ?
      stderr_pipe[0] : stdout_pipe[0];

    res = waitpid(pid, &status, WNOHANG);
    while (res <= 0) {
      if (res < 0) {
        if (errno != EINTR) {
          pr_trace_msg(trace_channel, 2,
            "passphrase provider error: unable to wait for pid %u: %s",
            (unsigned int) pid, strerror(errno));
          status = -1;
          break;

        } else {
          pr_signals_handle();
        }
      }

      /* Check the time elapsed since we started. */
      if ((time(NULL) - start_time) > TLS_PASSPHRASE_TIMEOUT) {

        /* Send TERM, the first time, to be polite. */
        if (send_sigterm) {
          send_sigterm = 0;
          pr_log_debug(DEBUG6, MOD_TLS_VERSION
            ": '%s' has exceeded the timeout (%lu seconds), sending "
            "SIGTERM (signal %d)", tls_passphrase_provider,
            (unsigned long) TLS_PASSPHRASE_TIMEOUT, SIGTERM);
          kill(pid, SIGTERM);

        } else {
          /* The child is still around?  Terminate with extreme prejudice. */
          pr_log_debug(DEBUG6, MOD_TLS_VERSION
            ": '%s' has exceeded the timeout (%lu seconds), sending "
            "SIGKILL (signal %d)", tls_passphrase_provider,
            (unsigned long) TLS_PASSPHRASE_TIMEOUT, SIGKILL);
          kill(pid, SIGKILL);
        }
      }

      /* Select on the pipe read fds, to see if the child has anything
       * to tell us.
       */
      FD_ZERO(&readfds);

      FD_SET(stdout_pipe[0], &readfds);
      FD_SET(stderr_pipe[0], &readfds);

      /* Note: this delay should be configurable somehow. */
      tv.tv_sec = 2L;
      tv.tv_usec = 0L;

      fds = select(maxfd + 1, &readfds, NULL, NULL, &tv);

      if (fds == -1 &&
          errno == EINTR) {
        pr_signals_handle();
      }

      if (fds > 0) {
        /* The child sent us something.  How thoughtful. */

        if (FD_ISSET(stdout_pipe[0], &readfds)) {
          res = read(stdout_pipe[0], buf, buflen);
          if (res > 0) {
            buf[buflen-1] = '\0';

            while (res &&
                   (buf[res-1] == '\r' ||
                    buf[res-1] == '\n')) {
              pr_signals_handle();
              res--;
            }
            buf[res] = '\0';

            pr_trace_msg(trace_channel, 18, "read passphrase from '%s'",
              tls_passphrase_provider);

          } else if (res < 0) {
            int xerrno = errno;

            pr_trace_msg(trace_channel, 3,
              "error reading stdout from '%s': %s",
              tls_passphrase_provider, strerror(xerrno));

            pr_log_debug(DEBUG2, MOD_TLS_VERSION
              ": error reading stdout from '%s': %s",
              tls_passphrase_provider, strerror(xerrno));
          }
        }

        if (FD_ISSET(stderr_pipe[0], &readfds)) {
          long stderrlen, stderrsz;
          char *stderrbuf;
          pool *tmp_pool = make_sub_pool(s->pool);

          stderrbuf = pr_fsio_getpipebuf(tmp_pool, stderr_pipe[0], &stderrsz);
          memset(stderrbuf, '\0', stderrsz);

          stderrlen = read(stderr_pipe[0], stderrbuf, stderrsz-1);
          if (stderrlen > 0) {
            while (stderrlen &&
                   (stderrbuf[stderrlen-1] == '\r' ||
                    stderrbuf[stderrlen-1] == '\n')) {
              stderrlen--;
            }
            stderrbuf[stderrlen] = '\0';

            pr_trace_msg(trace_channel, 5,
              "stderr from '%s': %s", tls_passphrase_provider,
              stderrbuf);

            pr_log_debug(DEBUG5, MOD_TLS_VERSION
              ": stderr from '%s': %s", tls_passphrase_provider,
              stderrbuf);

          } else if (res < 0) {
            int xerrno = errno;

            pr_trace_msg(trace_channel, 2,
              "error reading stderr from '%s': %s",
              tls_passphrase_provider, strerror(xerrno));

            pr_log_debug(DEBUG2, MOD_TLS_VERSION
              ": error reading stderr from '%s': %s",
              tls_passphrase_provider, strerror(xerrno));
          }

          destroy_pool(tmp_pool);
          tmp_pool = NULL;
        }
      }

      res = waitpid(pid, &status, WNOHANG);
    }
  }

  /* Restore the previous signal actions. */
  if (sigaction(SIGINT, &sa_intr, NULL) < 0) {
    return -1;
  }

  if (sigaction(SIGQUIT, &sa_quit, NULL) < 0) {
    return -1;
  }

  if (sigprocmask(SIG_SETMASK, &set_save, NULL) < 0) {
    return -1;
  }

  if (WIFSIGNALED(status)) {
    pr_log_debug(DEBUG2, MOD_TLS_VERSION
      ": '%s' died from signal %d", tls_passphrase_provider,
      WTERMSIG(status));
    errno = EPERM;
    return -1;
  }

  return 0;
}

static int tls_passphrase_cb(char *buf, int buflen, int rwflag, void *d) {
  static int need_banner = TRUE;
  struct tls_pkey_data *pdata = d;

  if (tls_passphrase_provider == NULL) {
    register unsigned int attempt;
    int pwlen = 0;

    tls_log("requesting passphrase from admin");

    /* Similar to Apache's mod_ssl, we want to be nice, and display an
     * informative message to the proftpd admin, telling them for what
     * server they are being requested to provide a passphrase.
     */

    if (need_banner) {
      fprintf(stderr, "\nPlease provide passphrases for these encrypted certificate keys:\n");
      need_banner = FALSE;
    }

    /* You get three attempts at entering the passphrase correctly. */
    for (attempt = 0; attempt < 3; attempt++) {
      int res;

      /* Always handle signals in a loop. */
      pr_signals_handle();

      res = EVP_read_pw_string(buf, buflen, pdata->prompt, TRUE);

      /* A return value of zero from EVP_read_pw_string() means success; -1
       * means a system error occurred, and 1 means user interaction problems.
       */
      if (res != 0) {
        fprintf(stderr, "\nPassphrases do not match.  Please try again.\n");
        continue;
      }

      /* Ensure that the buffer is NUL-terminated. */
      buf[buflen-1] = '\0';
      pwlen = strlen(buf);
      if (pwlen < 1) {
        fprintf(stderr, "Error: passphrase must be at least one character\n");

      } else {
        sstrncpy(pdata->buf, buf, pdata->bufsz);
        pdata->buflen = pwlen;

        return pwlen;
      }
    }

  } else {
    tls_log("requesting passphrase from '%s'", tls_passphrase_provider);

    if (tls_exec_passphrase_provider(pdata->s, buf, buflen, pdata->flags) < 0) {
      tls_log("error obtaining passphrase from '%s': %s",
        tls_passphrase_provider, strerror(errno));

    } else {
      /* Ensure that the buffer is NUL-terminated. */
      buf[buflen-1] = '\0';

      sstrncpy(pdata->buf, buf, pdata->bufsz);
      pdata->buflen = strlen(buf);

      return pdata->buflen;
    }
  }

#if OPENSSL_VERSION_NUMBER < 0x00908001
  PEMerr(PEM_F_DEF_CALLBACK, PEM_R_PROBLEMS_GETTING_PASSWORD);
#else
  PEMerr(PEM_F_PEM_DEF_CALLBACK, PEM_R_PROBLEMS_GETTING_PASSWORD);
#endif

  pr_memscrub(buf, buflen);
  return -1;
}

static int prompt_fd = -1;

static void set_prompt_fds(void) {

  /* Reconnect stderr to the term because proftpd connects stderr, earlier,
   * to the general stderr logfile.
   */
  prompt_fd = open("/dev/null", O_WRONLY);
  if (prompt_fd == -1) {
    /* This is an arbitrary, meaningless placeholder number. */
    prompt_fd = 76;
  }

  dup2(STDERR_FILENO, prompt_fd);
  dup2(STDOUT_FILENO, STDERR_FILENO);
}

static void restore_prompt_fds(void) {
  dup2(prompt_fd, STDERR_FILENO);
  close(prompt_fd);
  prompt_fd = -1;
}

static int tls_get_pkcs12_passwd(server_rec *s, FILE *fp, const char *prompt,
    char *buf, size_t bufsz, int flags, struct tls_pkey_data *pdata) {
  EVP_PKEY *pkey = NULL;
  X509 *cert = NULL;
  PKCS12 *p12 = NULL;
  char *passwd = NULL;
  int res, ok = FALSE;

  p12 = d2i_PKCS12_fp(fp, NULL);
  if (p12 != NULL) {

    /* Check if a password is needed. */
    res = PKCS12_verify_mac(p12, NULL, 0);
    if (res == 1) {
      passwd = NULL;

    } else if (res == 0) {
      res = PKCS12_verify_mac(p12, "", 0);
      if (res == 1) {
        passwd = "";
      }
    }

    if (res == 0) {
      register unsigned int attempt;

      /* This PKCS12 file is password-protected; need to get the password
       * from the admin.
       */
      for (attempt = 0; attempt < 3; attempt++) {
        int len = -1;

        /* Always handle signals in a loop. */
        pr_signals_handle();

        /* Clear the error queue at the start of the loop. */
        ERR_clear_error();

        len = tls_passphrase_cb(buf, bufsz, 0, pdata);
        if (len > 0) {
          res = PKCS12_verify_mac(p12, buf, -1);
          if (res == 1) {
#if OPENSSL_VERSION_NUMBER >= 0x000905000L
            /* Use the obtained password as additional entropy, ostensibly
             * unknown to attackers who may be watching the network, for
             * OpenSSL's PRNG.
             *
             * Human language gives about 2-3 bits of entropy per byte
             * (as per RFC1750).
             */
            RAND_add(buf, pdata->buflen, pdata->buflen * 0.25);
#endif

            res = PKCS12_parse(p12, buf, &pkey, &cert, NULL);
            if (res != 1) {
              PKCS12_free(p12);
              return -1;
            }

            ok = TRUE;
            break;
          }
        }

        fprintf(stderr, "\nWrong password for this PKCS12 file.  Please try again.\n");
      }
    } else {
      res = PKCS12_parse(p12, passwd, &pkey, &cert, NULL);
      if (res != 1) {
        PKCS12_free(p12);
        return -1;
      }

      ok = TRUE;
    }

  } else {
    fprintf(stderr, "\nUnable to read PKCS12 file.\n");
    return -1;
  }

  /* Now we should have an EVP_PKEY (which may or may not need a passphrase),
   * and a cert.  We don't really care about the cert right now.  But we
   * DO need to get the passphrase for the private key.  Do this by writing
   * the key to a BIO, then calling tls_get_passphrase() for that BIO.
   *
   * It looks like OpenSSL's pkcs12 command-line tool does not allow
   * passphrase-protected keys to be written into a PKCS12 structure;
   * the key is decrypted first (hence, probably, the password protection
   * for the entire PKCS12 structure).  Can the same be assumed to be true
   * for PKCS12 files created via other applications?
   *
   * For now, assume yes, that all PKCS12 files will have private keys which
   * are not encrypted.  If this is found to NOT be the case, then we
   * will need to write the obtained private key out to a BIO somehow,
   * then call tls_get_passphrase() on that BIO, rather than on a path.
   */

  if (cert)
    X509_free(cert);

  if (pkey)
    EVP_PKEY_free(pkey);

  if (p12)
    PKCS12_free(p12);

  if (!ok) {
#if OPENSSL_VERSION_NUMBER < 0x00908001
    PEMerr(PEM_F_DEF_CALLBACK, PEM_R_PROBLEMS_GETTING_PASSWORD);
#else
    PEMerr(PEM_F_PEM_DEF_CALLBACK, PEM_R_PROBLEMS_GETTING_PASSWORD);
#endif
    return -1;
  }

  ERR_clear_error();
  return res;
}

static int tls_get_passphrase(server_rec *s, const char *path,
    const char *prompt, char *buf, size_t bufsz, int flags) {
  FILE *keyf = NULL;
  EVP_PKEY *pkey = NULL;
  struct tls_pkey_data pdata;
  register unsigned int attempt;

  if (path) {
    int fd, res, xerrno;

    /* Open an fp on the cert file. */
    PRIVS_ROOT
    fd = open(path, O_RDONLY);
    xerrno = errno;
    PRIVS_RELINQUISH

    if (fd < 0) {
      SYSerr(SYS_F_FOPEN, xerrno);
      return -1;
    }

    /* Make sure the fd isn't one of the big three. */
    if (fd <= STDERR_FILENO) {
      res = pr_fs_get_usable_fd(fd);
      if (res >= 0) {
        close(fd);
        fd = res;
      }
    }

    keyf = fdopen(fd, "r");
    if (keyf == NULL) {
      xerrno = errno;

      (void) close(fd);
      SYSerr(SYS_F_FOPEN, xerrno);
      return -1;
    }

    /* As the file contains sensitive data, we do not want it lingering
     * around in stdio buffers.
     */
    (void) setvbuf(keyf, NULL, _IONBF, 0);
  }

  pdata.s = s;
  pdata.flags = flags;
  pdata.buf = buf;
  pdata.buflen = 0;
  pdata.bufsz = bufsz;
  pdata.prompt = prompt;

  set_prompt_fds();

  if (flags & TLS_PASSPHRASE_FL_PKCS12_PASSWD) {
    int res;

    res = tls_get_pkcs12_passwd(s, keyf, prompt, buf, bufsz, flags, &pdata);

    if (keyf != NULL) {
      fclose(keyf);
      keyf = NULL;
    }

    /* Restore the normal stderr logging. */
    restore_prompt_fds();

    return res;
  }

  /* The user gets three tries to enter the correct passphrase. */
  for (attempt = 0; attempt < 3; attempt++) {

    /* Always handle signals in a loop. */
    pr_signals_handle();

    /* Clear the error queue at the start of the loop. */
    ERR_clear_error();

    pkey = PEM_read_PrivateKey(keyf, NULL, tls_passphrase_cb, &pdata);
    if (pkey != NULL) {
      break;
    }

    if (keyf != NULL) {
      fseek(keyf, 0, SEEK_SET);
    }

    fprintf(stderr, "\nWrong passphrase for this key.  Please try again.\n");
  }

  if (keyf != NULL) {
    fclose(keyf);
    keyf = NULL;
  }

  /* Restore the normal stderr logging. */
  restore_prompt_fds();

  if (pkey == NULL) {
    return -1;
  }

  EVP_PKEY_free(pkey);

  if (pdata.buflen > 0) {
#if OPENSSL_VERSION_NUMBER >= 0x000905000L
    /* Use the obtained passphrase as additional entropy, ostensibly
     * unknown to attackers who may be watching the network, for
     * OpenSSL's PRNG.
     *
     * Human language gives about 2-3 bits of entropy per byte (RFC1750).
     */
    RAND_add(buf, pdata.buflen, pdata.buflen * 0.25);
#endif

#ifdef HAVE_MLOCK
    PRIVS_ROOT
    if (mlock(buf, bufsz) < 0) {
      pr_log_debug(DEBUG1, MOD_TLS_VERSION
        ": error locking passphrase into memory: %s", strerror(errno));

    } else {
      pr_log_debug(DEBUG1, MOD_TLS_VERSION ": passphrase locked into memory");
    }
    PRIVS_RELINQUISH
#endif
  }

  return pdata.buflen;
}

static int tls_handshake_timeout_cb(CALLBACK_FRAME) {
  tls_handshake_timed_out = TRUE;
  return 0;
}

static void tls_scrub_pkey(tls_pkey_t *k) {
  if (k->rsa_pkey != NULL) {
    pr_memscrub(k->rsa_pkey, k->pkeysz);
    free(k->rsa_pkey_ptr);
    k->rsa_pkey = k->rsa_pkey_ptr = NULL;
    k->rsa_passlen = 0;
  }

  if (k->dsa_pkey != NULL) {
    pr_memscrub(k->dsa_pkey, k->pkeysz);
    free(k->dsa_pkey_ptr);
    k->dsa_pkey = k->dsa_pkey_ptr = NULL;
    k->dsa_passlen = 0;
  }

#ifdef PR_USE_OPENSSL_ECC
  if (k->ec_pkey != NULL) {
    pr_memscrub(k->ec_pkey, k->pkeysz);
    free(k->ec_pkey_ptr);
    k->ec_pkey = k->ec_pkey_ptr = NULL;
    k->ec_passlen = 0;
  }
#endif /* PR_USE_OPENSSL_ECC */

  if (k->pkcs12_passwd != NULL) {
    pr_memscrub(k->pkcs12_passwd, k->pkeysz);
    free(k->pkcs12_passwd_ptr);
    k->pkcs12_passwd = k->pkcs12_passwd_ptr = NULL;
    k->pkcs12_passlen = 0;
  }

  if (k->path != NULL) {
    free((void *) k->path);
    k->path = NULL;
  }

  k->next = NULL;
  k->sid = 0;
}

static tls_pkey_t *tls_lookup_pkey(server_rec *s, int lock_if_found,
    int scrub_unless_match) {
  tls_pkey_t *k, *knext, *pkey = NULL;

  for (k = tls_pkey_list; k; k = knext) {
    pr_signals_handle();

    knext = k->next;

    if (k->sid != s->sid) {
      if (scrub_unless_match) {
        /* Scrub the passphrase's memory areas. */
        tls_scrub_pkey(k);
      }

      continue;
    }

    pkey = k;

    if (lock_if_found) {
#ifdef HAVE_MLOCK
      /* mlock() the passphrase memory areas again; page locks are not
       * inherited across forks.
       */
      PRIVS_ROOT
      if (k->rsa_pkey != NULL &&
          k->rsa_passlen > 0) {
        if (mlock(k->rsa_pkey, k->pkeysz) < 0) {
          tls_log("error locking passphrase into memory: %s", strerror(errno));
        }
      }

      if (k->dsa_pkey != NULL &&
          k->dsa_passlen > 0) {
        if (mlock(k->dsa_pkey, k->pkeysz) < 0) {
          tls_log("error locking passphrase into memory: %s", strerror(errno));
        }
      }

# ifdef PR_USE_OPENSSL_ECC
      if (k->ec_pkey != NULL &&
          k->ec_passlen > 0) {
        if (mlock(k->ec_pkey, k->pkeysz) < 0) {
          tls_log("error locking passphrase into memory: %s", strerror(errno));
        }
      }
# endif /* PR_USE_OPENSSL_ECC */

      if (k->pkcs12_passwd != NULL &&
          k->pkcs12_passlen > 0) {
        if (mlock(k->pkcs12_passwd, k->pkeysz) < 0) {
          tls_log("error locking password into memory: %s", strerror(errno));
        }
      }
      PRIVS_RELINQUISH
#endif /* HAVE_MLOCK */
    }

    break;
  }

  return pkey;
}

static int tls_pkey_cb(char *buf, int buflen, int rwflag, void *data) {
  tls_pkey_t *k;

  if (data == NULL) {
    return 0;
  }

  k = (tls_pkey_t *) data;

  if ((k->flags & TLS_PKEY_USE_RSA) && k->rsa_pkey) {
    sstrncpy(buf, k->rsa_pkey, buflen);
    buf[buflen - 1] = '\0';
    return strlen(buf);
  }

  if ((k->flags & TLS_PKEY_USE_DSA) && k->dsa_pkey) {
    sstrncpy(buf, k->dsa_pkey, buflen);
    buf[buflen - 1] = '\0';
    return strlen(buf);
  }

#ifdef PR_USE_OPENSSL_ECC
  if ((k->flags & TLS_PKEY_USE_EC) && k->ec_pkey) {
    sstrncpy(buf, k->ec_pkey, buflen);
    buf[buflen - 1] = '\0';
    return strlen(buf);
  }
#endif /* PR_USE_OPENSSL_ECC */

  return 0;
}

static void tls_remove_pkey(tls_pkey_t *k) {
  if (tls_pkey_list != k) {
    tls_pkey_t *ki, *prev;

    prev = tls_pkey_list;
    for (ki = tls_pkey_list->next; ki; ki = ki->next) {
      if (ki == k) {
        prev->next = k->next;
        break;
      }

      prev = ki;
    }

  } else {
    /* We are the head of the list. */
    tls_pkey_list = k->next;
  }

  if (tls_npkeys > 0) {
    tls_npkeys--;
  }
}

static void tls_scrub_pkeys(void) {
  tls_pkey_t *k, *knext;
  unsigned int passphrase_count = 0;

  if (tls_pkey_list == NULL) {
    return;
  }

  /* Scrub and free all passphrases in memory. */
  for (k = tls_pkey_list; k; k = k->next) {
    if (k->rsa_pkey != NULL &&
        k->rsa_passlen > 0) {
      passphrase_count++;
    }

    if (k->dsa_pkey != NULL &&
        k->dsa_passlen > 0) {
      passphrase_count++;
    }

#ifdef PR_USE_OPENSSL_ECC
    if (k->ec_pkey != NULL &&
        k->ec_passlen > 0) {
      passphrase_count++;
    }
#endif /* PR_USE_OPENSSL_ECC */

    if (k->pkcs12_passwd != NULL &&
        k->pkcs12_passlen > 0) {
      passphrase_count++;
    }
  }

  if (passphrase_count == 0) {
    tls_pkey_list = NULL;
    tls_npkeys = 0;
    return;
  }

  pr_log_debug(DEBUG5, MOD_TLS_VERSION
    ": scrubbing %u %s from memory", passphrase_count,
    passphrase_count != 1 ? "passphrases" : "passphrase");

  for (k = tls_pkey_list; k; k = knext) {
    knext = k->next;

    pr_signals_handle();
    tls_scrub_pkey(k);
  }

  tls_pkey_list = NULL;
  tls_npkeys = 0;
}

static void tls_clean_pkeys(void) {
  register unsigned int i;
  tls_pkey_t *k;
  pool *tmp_pool;
  array_header *dead_keys, *valid_sids;
  server_rec *s;

  /* We scan the tls_pkey_list for any keys belonging to vhosts (by SID) which
   * no longer appear in our configuration.
   */

  if (tls_pkey_list == NULL) {
    return;
  }

  tmp_pool = make_sub_pool(tls_pool);
  pr_pool_tag(tmp_pool, "TLS Passphrase Cleaning");

  dead_keys = make_array(tmp_pool, 0, sizeof(tls_pkey_t *));
  valid_sids = make_array(tmp_pool, 0, sizeof(unsigned int));

  for (s = (server_rec *) server_list->xas_list; s; s = s->next) {
    *((unsigned int *) push_array(valid_sids)) = s->sid;
  }

  for (k = tls_pkey_list; k; k = k->next) {
    int dead_key = TRUE;

    for (i = 0; i < valid_sids->nelts; i++) {
      unsigned int sid;

      sid = ((unsigned int *) valid_sids->elts)[i];
      if (k->sid == sid) {
        dead_key = FALSE;
        break;
      }
    }

    if (dead_key) {
      *((tls_pkey_t **) push_array(dead_keys)) = k;
    }
  }

  for (i = 0; i < dead_keys->nelts; i++) {
    pr_signals_handle();

    k = ((tls_pkey_t **) dead_keys->elts)[i];
    tls_remove_pkey(k);
    tls_scrub_pkey(k);
    destroy_pool(k->pool);
  }

  destroy_pool(tmp_pool);
  return;
}

#if OPENSSL_VERSION_NUMBER > 0x000907000L
static int tls_renegotiate_timeout_cb(CALLBACK_FRAME) {
  if ((tls_flags & TLS_SESS_ON_CTRL) &&
      (tls_flags & TLS_SESS_CTRL_RENEGOTIATING)) {
    int ctrl_renegotiated = FALSE;

    switch (SSL_version(ctrl_ssl)) {
# if defined(TLS1_3_VERSION) && !defined(HAVE_LIBRESSL)
      case TLS1_3_VERSION:
        if (SSL_get_key_update_type(ctrl_ssl) == SSL_KEY_UPDATE_NONE) {
          ctrl_renegotiated = TRUE;
        }
        break;
# endif /* TLS1_3_VERSION and no LibreSSL */

      default:
        if (SSL_renegotiate_pending(ctrl_ssl) == 0) {
          ctrl_renegotiated = TRUE;
        }
        break;
    }

    if (ctrl_renegotiated == TRUE) {
      tls_log("%s", "control channel TLS session renegotiated");
      tls_flags &= ~TLS_SESS_CTRL_RENEGOTIATING;

    } else if (tls_renegotiate_required == TRUE) {
      tls_log("%s", "requested TLS renegotiation timed out on control channel");
      tls_log("%s", "shutting down control channel TLS session");
      tls_end_sess(ctrl_ssl, session.c, 0);
      pr_table_remove(tls_ctrl_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
      pr_table_remove(tls_ctrl_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
      ctrl_ssl = NULL;
    }
  }

  if ((tls_flags & TLS_SESS_ON_DATA) &&
      (tls_flags & TLS_SESS_DATA_RENEGOTIATING)) {
    int data_renegotiated = FALSE;
    SSL *ssl;

    ssl = (SSL *) pr_table_get(tls_data_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
    switch (SSL_version(ssl)) {
# if defined(TLS1_3_VERSION) && !defined(HAVE_LIBRESSL)
      case TLS1_3_VERSION:
        if (SSL_get_key_update_type(ssl) == SSL_KEY_UPDATE_NONE) {
          data_renegotiated = TRUE;
        }
        break;
# endif /* TLS1_3_VERSION and no LibreSSL */

      default:
        if (SSL_renegotiate_pending(ssl) == 0) {
          data_renegotiated = TRUE;
        }
        break;
    }

    if (data_renegotiated == TRUE) {
      tls_log("%s", "data channel TLS session renegotiated");
      tls_flags &= ~TLS_SESS_DATA_RENEGOTIATING;
      tls_data_renegotiate_current = 0;

    } else if (tls_renegotiate_required == TRUE) {
      tls_log("%s", "requested TLS renegotiation timed out on data channel");
      tls_log("%s", "shutting down data channel TLS session");
      tls_end_sess(ssl, session.d, 0);
      pr_table_remove(tls_data_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
      pr_table_remove(tls_data_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
    }
  }

  return 0;
}

static int tls_ctrl_renegotiate_cb(CALLBACK_FRAME) {
  /* Guard against a timer firing as the SSL session is being torn down. */
  if (ctrl_ssl == NULL) {
    return 0;
  }

  if (tls_flags & TLS_SESS_ON_CTRL) {
    switch (SSL_version(ctrl_ssl)) {
#if defined(TLS1_3_VERSION) && !defined(HAVE_LIBRESSL)
      /* If we're a TLSv1.3 session, use SSL_key_update() to request new
       * session keys; TLSv1.3 does not support renegotiations.
       */
      case TLS1_3_VERSION: {
        if (SSL_get_key_update_type(ctrl_ssl) == SSL_KEY_UPDATE_NONE) {
          tls_flags |= TLS_SESS_CTRL_RENEGOTIATING;

          tls_log("requesting TLS key updates on control channel "
            "(%lu sec renegotiation interval)", p1);

          if (SSL_key_update(ctrl_ssl, SSL_KEY_UPDATE_REQUESTED) != 1) {
            tls_log("error requesting TLS key update on control channel: %s",
              tls_get_errors());
          }

        } else {
          pr_trace_msg(trace_channel, 7,
            "TLS key update on control channel already in progress");
        }
      }
      break;
#endif /* TLS1_3_VERSION and no LibreSSL */

      default: {
#if OPENSSL_VERSION_NUMBER >= 0x009080cfL
         /* In OpenSSL-0.9.8l and later, SSL session renegotiations
          * (both client- and server-initiated) are automatically disabled.
          * Unless the admin explicitly configured support for
          * client-initiated renegotiations via the AllowClientRenegotiations
          * TLSOption, we can't request renegotiations ourselves.
          */
        if (tls_opts & TLS_OPT_ALLOW_CLIENT_RENEGOTIATIONS) {
#else
        if (TRUE) {
#endif
          tls_flags |= TLS_SESS_CTRL_RENEGOTIATING;

          tls_log("requesting TLS renegotiation on control channel "
            "(%lu sec renegotiation interval)", p1);

          if (SSL_renegotiate(ctrl_ssl) != 1) {
            tls_log("error requesting TLS renegotiation on control channel: %s",
              tls_get_errors());
          }
        }
      }
    }

    /* Restart the timer. */
    return 1;
  }

  return 0;
}
#endif

static DH *tls_dh_cb(SSL *ssl, int is_export, int keylen) {
  DH *dh = NULL;
  EVP_PKEY *pkey;
  int pkeylen = 0, use_pkeylen = FALSE;

  /* OpenSSL will only ever call us (currently) with a keylen of 512 or 1024;
   * see the SSL_EXPORT_PKEYLENGTH macro in ssl_locl.h.  Sigh.
   *
   * Thus we adjust the DH parameter length according to the size of the
   * RSA/DSA private key used for the current connection.
   *
   * NOTE: This MAY cause interoperability issues with some clients, notably
   * Java 7 (and earlier) clients, since Java 7 and earlier supports
   * Diffie-Hellman only up to 1024 bits.  More sighs.  To deal with these
   * clients, then, you need to configure a certificate/key of 1024 bits.
   */
  pkey = SSL_get_privatekey(ssl);
  if (pkey != NULL) {
    if (get_pkey_type(pkey) == EVP_PKEY_RSA ||
        get_pkey_type(pkey) == EVP_PKEY_DSA) {
      pkeylen = EVP_PKEY_bits(pkey);

      if (pkeylen < TLS_DH_MIN_LEN) {
        if (!(tls_opts & TLS_OPT_ALLOW_WEAK_DH)) {
          pr_trace_msg(trace_channel, 11,
            "certificate private key length %d less than %d bits, using %d "
            "(see AllowWeakDH TLSOption)", pkeylen, TLS_DH_MIN_LEN,
            TLS_DH_MIN_LEN);
          pkeylen = TLS_DH_MIN_LEN;
        }
      }

      if (pkeylen != keylen) {
        pr_trace_msg(trace_channel, 13,
          "adjusted DH parameter length from %d to %d bits", keylen, pkeylen);
        use_pkeylen = TRUE;
      }
    }
  }

  if (tls_tmp_dhs != NULL &&
      tls_tmp_dhs->nelts > 0) {
    register unsigned int i;
    DH *best_dh = NULL, **dhs;
    int best_dhlen = 0;

    dhs = tls_tmp_dhs->elts;

    /* Search the configured list of DH parameters twice: once for any sizes
     * matching the actual requested size (usually 1024), and once for any
     * matching the certificate private key size (pkeylen).
     *
     * This behavior allows site admins to configure a TLSDHParamFile that
     * contains 1024-bit parameters, for e.g. Java 7 (and earlier) clients.
     */

    /* Note: the keylen argument is in BITS, but DH_size() returns the number
     * of BYTES.
     */
    for (i = 0; i < tls_tmp_dhs->nelts; i++) {
      int dhlen;

      dhlen = DH_size(dhs[i]) * 8;
      if (dhlen == keylen) {
        pr_trace_msg(trace_channel, 11,
          "found matching DH parameter for key length %d", keylen);
        return dhs[i];
      }

      /* Try to find the next "best" DH to use, where "best" means
       * the smallest DH that is larger than the necessary keylen.
       */
      if (dhlen > keylen) {
        if (best_dh != NULL) {
          if (dhlen < best_dhlen) {
            best_dh = dhs[i];
            best_dhlen = dhlen;
          }

        } else {
          best_dh = dhs[i];
          best_dhlen = dhlen;
        }
      }
    }

    for (i = 0; i < tls_tmp_dhs->nelts; i++) {
      int dhlen;

      dhlen = DH_size(dhs[i]) * 8;
      if (dhlen == pkeylen) {
        pr_trace_msg(trace_channel, 11,
          "found matching DH parameter for certificate private key length %d",
          pkeylen);
        return dhs[i];
      }

      if (dhlen > pkeylen) {
        if (best_dh != NULL) {
          if (dhlen < best_dhlen) {
            best_dh = dhs[i];
            best_dhlen = dhlen;
          }

        } else {
          best_dh = dhs[i];
          best_dhlen = dhlen;
        }
      }
    }

    if (best_dh != NULL) {
      pr_trace_msg(trace_channel, 11,
        "using best DH parameter for key length %d (length %d)", keylen,
        best_dhlen);
      return best_dh;
    }
  }

  /* Still no DH parameters found?  Use the built-in ones. */

  if (keylen < TLS_DH_MIN_LEN) {
    if (!(tls_opts & TLS_OPT_ALLOW_WEAK_DH)) {
      pr_trace_msg(trace_channel, 11,
        "requested key length %d less than %d bits, using %d "
        "(see AllowWeakDH TLSOption)", keylen, TLS_DH_MIN_LEN, TLS_DH_MIN_LEN);
      keylen = TLS_DH_MIN_LEN;
    }
  }

  if (use_pkeylen) {
    keylen = pkeylen;
  }

  switch (keylen) {
    case 512:
      dh = get_dh512();
      break;

    case 768:
      dh = get_dh768();
      break;

    case 1024:
      dh = get_dh1024();
      break;

    case 1536:
      dh = get_dh1536();
      break;

    case 2048:
      dh = get_dh2048();
      break;

    case 3072:
      dh = get_dh3072();
      break;

    case 4096:
      dh = get_dh4096();
      break;

    default:
      tls_log("unsupported DH key length %d requested, returning 1024 bits",
        keylen);
      dh = get_dh1024();
      break;
  }

  pr_trace_msg(trace_channel, 11, "using builtin DH for %d bits", keylen);

  /* Add this DH to the list, so that it can be freed properly later. */
  if (tls_tmp_dhs == NULL) {
    tls_tmp_dhs = make_array(session.pool, 1, sizeof(DH *));
  }

  *((DH **) push_array(tls_tmp_dhs)) = dh;
  return dh;
}

#if !defined(OPENSSL_NO_TLSEXT)
# if defined(TLSEXT_MAXLEN_host_name)
static int tls_sni_cb(SSL *ssl, int *alert_desc, void *user_data) {
  const char *server_name = NULL;

  server_name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (server_name != NULL) {
    const char *host = NULL, *sni;
    server_rec *named_server = NULL;

    pr_trace_msg(trace_channel, 5, "received SNI '%s'", server_name);

    /* If we have already received a HOST command, then we need to
     * check that the SNI value matches that of the HOST.  Otherwise,
     * we stash the SNI, so that if/when a HOST command is received,
     * we can check the HOST name against the SNI.
     *
     * RFC 7151, Section 3.2.2, does not mandate whether HOST must be
     * sent before e.g. AUTH TLS or not; the only example the RFC provides
     * shows AUTH TLS being used before HOST.  Section 4 of that RFC goes
     * on to recommend using HOST before AUTH, in general, unless the SNI
     * extension will be used, in which case, clients should use AUTH TLS
     * before HOST.  We need to be ready for either case.
     *
     * Note that this SNI/HOST check can only really happen for control
     * connections, not data connections.  FTPS clients do not receive/use
     * DNS hostnames for data connections, only IP addresses.
     */

    host = pr_table_get(session.notes, "mod_core.host", NULL);

    /* If we have already stashed an SNI, it means this is probably a data
     * connection.
     *
     * For data connections where an SNI is provided, we MIGHT be able to
     * validate that SNI (assuming it is an IP address) against our IP address,
     * at least.
     */
    sni = pr_table_get(session.notes, "mod_tls.sni", NULL);

    if (host != NULL &&
        sni == NULL) {
      /* If the requested HOST does not match the SNI, it's a fatal error.
       *
       * Bear in mind, however, that the HOST command might have used an
       * IPv4/IPv6 address, NOT a name.  If that is the case, we do NOT want
       * compare that with SNI.  Do we?
       */

      if (pr_netaddr_is_v4(host) != TRUE &&
          pr_netaddr_is_v6(host) != TRUE) {
        if (strcasecmp(host, server_name) != 0) {
          tls_log("warning: SNI '%s' does not match HOST '%s', rejecting "
            "SSL/TLS connection", server_name, host);
          pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
            ": SNI '%s' does not match HOST '%s', rejecting SSL/TLS connection",
            server_name, host);
          *alert_desc = SSL_AD_ACCESS_DENIED;
          return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
      }
    }

    if (tls_opts & TLS_OPT_IGNORE_SNI) {
      pr_trace_msg(trace_channel, 5,
        "client sent SNI '%s', ignoring due to IgnoreSNI TLSOption",
        server_name);
      return SSL_TLSEXT_ERR_OK;
    }

    /* Per RFC 6066, literal IPv4/IPv6 addresses are NOT permitted in the
     * SNI.  However, some clients will still send such IP addresses.
     * We can safely ignore these, since any IP-based virtual host selection
     * happens at TCP connect time.
     */
    if (pr_netaddr_is_v4(server_name) == TRUE ||
        pr_netaddr_is_v6(server_name) == TRUE) {
      pr_trace_msg(trace_channel, 5,
        "client sent IP address SNI '%s', ignoring", server_name);
      return SSL_TLSEXT_ERR_OK;
    }

    if (pr_table_add_dup(session.notes, "mod_tls.sni",
        (char *) server_name, 0) < 0) {

      /* The session.notes may already have the SNI from the ctrl channel;
       * no need to overwrite that.
       */
      if (errno != EEXIST) {
        pr_trace_msg(trace_channel, 3,
          "error stashing 'mod_tls.sni' in session.notes: %s", strerror(errno));
      }
    }

    /* Notify any listeners (e.g. mod_autohost) of the requested name, to
     * given them a chance to update/modify the configuration.
     */
    pr_event_generate("mod_tls.sni", server_name);

    /* If we have no name-based virtual servers configured (no ServerAlias),
     * then just ignore the SNI.  Otherwise, configurations which have been
     * working will "break" unexpectedly, due to SNI support.
     */
    if (pr_namebind_count(main_server) == 0) {
      pr_trace_msg(trace_channel, 5,
        "no name-based <VirtualHost> configured, ignoring SNI '%s'",
        server_name);
      return SSL_TLSEXT_ERR_OK;
    }

    /* See if we have a name-based vhost matching the SNI; if so, we want
     * to switch to that vhost, in case it is configured with different
     * server certificates, CAs.
     */

    named_server = pr_namebind_get_server(server_name, main_server->addr,
      session.c->local_port);
    if (named_server == NULL) {
      tls_log("no matching server found for client-sent SNI '%s', rejecting "
        "SSL/TLS connection", server_name);
      pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
        ": no matching server found for client-sent SNI '%s', "
        "rejecting SSL/TLS connection", server_name);
#if defined(SSL_AD_UNRECOGNIZED_NAME)
      *alert_desc = SSL_AD_UNRECOGNIZED_NAME;
#else
      *alert_desc = SSL_AD_ACCESS_DENIED;
#endif /* SSL_AD_UNRECOGNIZED_NAME */
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    if (named_server != main_server) {
      unsigned char *engine;
      SSL_SESSION *sess;

      /* First, check to see whether mod_tls is even enabled for the named
       * vhost.
       */
      engine = get_param_ptr(named_server->conf, "TLSEngine", FALSE);
      if (engine == NULL ||
          *engine == FALSE) {
        tls_log("TLSEngine not enabled for SNI '%s', rejecting client",
          server_name);
        pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
          ": TLSEngine not enabled for SNI '%s', rejecting client",
          server_name);

#if defined(SSL_AD_UNRECOGNIZED_NAME)
        *alert_desc = SSL_AD_UNRECOGNIZED_NAME;
#else
        *alert_desc = SSL_AD_HANDSHAKE_FAILURE;
#endif /* SSL_AD_UNRECOGNIZED_NAME */
        return SSL_TLSEXT_ERR_ALERT_FATAL;
      }

      tls_lookup_all(named_server);

      /* Check to see if a passphrase has been entered for this server. */
      tls_pkey = tls_lookup_pkey(named_server, TRUE, TRUE);

      if (tls_ssl_set_all(named_server, ssl) < 0) {
        tls_log("error initializing OpenSSL session for SNI '%s'", server_name);
        pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
          ": error initializing OpenSSL session for SNI '%s'", server_name);
        *alert_desc = SSL_AD_ACCESS_DENIED;
        return SSL_TLSEXT_ERR_ALERT_FATAL;
      }

      /* Our new SSL_CTX, from the SNI, might have different protocol
       * versions enabled.  However, due to the _timing_ of when this SNI
       * callback is invoked during the TLS handshake processing, the
       * "acceptable" server protocol version has already been determined
       * based on the original SSL_CTX.  This means that we need to check
       * for incompatible protocol versions ourselves.  Fun!
       *
       * We implement this in a naive manner: if the new SSL_CTX has an
       * SSL_OP_NO_ option enabled for the protocol version currently
       * selected by our SSL_SESSION, fail the callback with
       * SSL_AD_PROTOCOL_VERSION.
       */
      sess = SSL_get_session(ssl);
      if (sess != NULL) {
        SSL_CTX *ctx;
        long ctx_options;
        int protocol_ok = FALSE, sess_version;

        ctx = SSL_get_SSL_CTX(ssl);
        ctx_options = SSL_CTX_get_options(ctx);

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
        sess_version = SSL_SESSION_get_protocol_version(sess);
#else
        sess_version = sess->ssl_version;
#endif /* OpenSSL 1.1.x/LibreSSL-3.5.x and later */

        switch (sess_version) {
          case SSL3_VERSION:
            if (!(ctx_options & SSL_OP_NO_SSLv3)) {
              protocol_ok = TRUE;
            }
            break;

          case TLS1_VERSION:
            if (!(ctx_options & SSL_OP_NO_TLSv1)) {
              protocol_ok = TRUE;
            }
            break;

#if defined(TLS1_1_VERSION)
          case TLS1_1_VERSION:
# if defined(SSL_OP_NO_TLSv1_1)
            if (!(ctx_options & SSL_OP_NO_TLSv1_1)) {
              protocol_ok = TRUE;
            }
# endif /* SSL_OP_NO_TLSv1_1 */
            break;
#endif /* TLS1_1_VERSION */

#if defined(TLS1_2_VERSION)
          case TLS1_2_VERSION:
# if defined(SSL_OP_NO_TLSv1_2)
            if (!(ctx_options & SSL_OP_NO_TLSv1_2)) {
              protocol_ok = TRUE;
            }
# endif /* SSL_OP_NO_TLSv1_2 */
            break;
#endif /* TLS1_2_VERSION */

#if defined(TLS1_3_VERSION)
          case TLS1_3_VERSION:
            if (!(ctx_options & SSL_OP_NO_TLSv1_3)) {
              protocol_ok = TRUE;
            }
            break;
#endif /* TLS1_3_VERSION */

          default:
            pr_trace_msg(trace_channel, 3,
              "unknown/unsupported protocol version '%s' (%d) requested by "
              "client", SSL_get_version(ssl), sess_version);
            break;
        }

        if (protocol_ok == FALSE) {
          tls_log("client-requested protocol version %s not supported by "
            "SNI '%s' host", SSL_get_version(ssl), server_name);
          pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
            ": client-requested protocol version %s not supported by "
            "SNI '%s' host", SSL_get_version(ssl), server_name);
          *alert_desc = SSL_AD_PROTOCOL_VERSION;
          return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
      }
    }
  }

  return SSL_TLSEXT_ERR_OK;
}
# endif /* !TLSEXT_MAXLEN_host_name */

static void tls_tlsext_cb(SSL *ssl, int server, int type,
    unsigned char *tlsext_data, int tlsext_datalen, void *data) {
  char *extension_name = "(unknown)";
  int print_basic_info = TRUE;

  /* Note: OpenSSL does not implement all possible extensions.  For the
   * "(unknown)" extensions, see:
   *
   *  http://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xhtml#tls-extensiontype-values-1
   */
  switch (type) {
# ifdef TLSEXT_TYPE_server_name
    case TLSEXT_TYPE_server_name: {
      BIO *bio = NULL;
      char *ext_info = "";
      long ext_infolen = 0;

      extension_name = "server name";

      if (pr_trace_get_level(trace_channel) >= 19 &&
          tlsext_datalen >= 2) {

        /* We read 2 bytes for the extension length, 1 byte for the
         * name type, and 2 bytes for the name length.  For the SNI
         * format specification, see:
         *   https://tools.ietf.org/html/rfc6066#section-3
         */
        int ext_len;

        ext_len = (tlsext_data[0] << 8) | tlsext_data[1];
        if (tlsext_datalen == ext_len + 2 &&
            (ext_len & 1) == 0) {
          int name_type;
          tlsext_data += 2;

          name_type = tlsext_data[0];
          tlsext_data += 1;

          if (name_type == TLSEXT_NAMETYPE_host_name) {
            size_t name_len;

            name_len = (tlsext_data[0] << 8) | tlsext_data[1];
            tlsext_data += 2;

            bio = BIO_new(BIO_s_mem());
            BIO_printf(bio, "\n  %.*s (%lu)", (int) name_len, tlsext_data,
              (unsigned long) name_len);

            ext_infolen = BIO_get_mem_data(bio, &ext_info);
            if (ext_info != NULL) {
              ext_info[ext_infolen] = '\0';
            }
          }
        }
      }

      pr_trace_msg(trace_channel, 6,
        "[tls.tlsext] TLS %s extension \"%s\" (ID %d, %d %s)%.*s",
        server ? "server" : "client", extension_name, type,
        tlsext_datalen, tlsext_datalen != 1 ? "bytes" : "byte",
        (int) ext_infolen, ext_info != NULL ? ext_info : "");

      if (bio != NULL) {
        BIO_free(bio);
      }

      print_basic_info = FALSE;
      break;
    }
# endif

# ifdef TLSEXT_TYPE_max_fragment_length
    case TLSEXT_TYPE_max_fragment_length:
      extension_name = "max fragment length";
      break;
# endif

# ifdef TLSEXT_TYPE_client_certificate_url
    case TLSEXT_TYPE_client_certificate_url:
      extension_name = "client certificate URL";
      break;
# endif

# ifdef TLSEXT_TYPE_trusted_ca_keys
    case TLSEXT_TYPE_trusted_ca_keys:
      extension_name = "trusted CA keys";
      break;
# endif

# ifdef TLSEXT_TYPE_truncated_hmac
    case TLSEXT_TYPE_truncated_hmac:
      extension_name = "truncated HMAC";
      break;
# endif

# ifdef TLSEXT_TYPE_status_request
    case TLSEXT_TYPE_status_request:
      extension_name = "status request";
      break;
# endif

# ifdef TLSEXT_TYPE_user_mapping
    case TLSEXT_TYPE_user_mapping:
      extension_name = "user mapping";
      break;
# endif

# ifdef TLSEXT_TYPE_client_authz
    case TLSEXT_TYPE_client_authz:
      extension_name = "client authz";
      break;
# endif

# ifdef TLSEXT_TYPE_server_authz
    case TLSEXT_TYPE_server_authz:
      extension_name = "server authz";
      break;
# endif

# ifdef TLSEXT_TYPE_cert_type
    case TLSEXT_TYPE_cert_type:
      extension_name = "cert type";
      break;
# endif

# ifdef TLSEXT_TYPE_elliptic_curves
    case TLSEXT_TYPE_elliptic_curves:
      extension_name = "elliptic curves";
      break;
# endif

# ifdef TLSEXT_TYPE_ec_point_formats
    case TLSEXT_TYPE_ec_point_formats:
      extension_name = "EC point formats";
      break;
# endif

# ifdef TLSEXT_TYPE_srp
    case TLSEXT_TYPE_srp:
      extension_name = "SRP";
      break;
# endif

# ifdef TLSEXT_TYPE_signature_algorithms
    case TLSEXT_TYPE_signature_algorithms: {
      BIO *bio = NULL;
      char *ext_info = "";
      long ext_infolen = 0;

      extension_name = "signature algorithms";

      if (pr_trace_get_level(trace_channel) >= 19 &&
          tlsext_datalen >= 2) {
        int len;

        len = (tlsext_data[0] << 8) | tlsext_data[1];
        if (tlsext_datalen == len + 2 &&
            (len & 1) == 0) {
          tlsext_data += 2;

          bio = BIO_new(BIO_s_mem());
          BIO_puts(bio, "\n");

          while (len > 0) {
            unsigned int sig_algo;

            pr_signals_handle();

            sig_algo = (tlsext_data[0] << 8) | tlsext_data[1];
            BIO_printf(bio, "  %s (0x%x)\n",
              tls_get_label(sig_algo, tls_sigalgo_labels), sig_algo);
            len -= 2;
            tlsext_data += 2;
          }

          ext_infolen = BIO_get_mem_data(bio, &ext_info);
          if (ext_info != NULL) {
            ext_info[ext_infolen] = '\0';
          }
        }
      }

      pr_trace_msg(trace_channel, 6,
        "[tls.tlsext] TLS %s extension \"%s\" (ID %d, %d %s)%.*s",
        server ? "server" : "client", extension_name, type,
        tlsext_datalen, tlsext_datalen != 1 ? "bytes" : "byte",
        (int) ext_infolen, ext_info != NULL ? ext_info : "");

      if (bio != NULL) {
        BIO_free(bio);
      }

      print_basic_info = FALSE;
      break;
    }
# endif

# ifdef TLSEXT_TYPE_use_srtp
    case TLSEXT_TYPE_use_srtp:
      extension_name = "use SRTP";
      break;
# endif

# ifdef TLSEXT_TYPE_heartbeat
    case TLSEXT_TYPE_heartbeat:
      extension_name = "heartbeat";
      break;
# endif

# ifdef TLSEXT_TYPE_signed_certificate_timestamp
    case TLSEXT_TYPE_signed_certificate_timestamp:
      extension_name = "signed certificate timestamp";
      break;
# endif

# ifdef TLSEXT_TYPE_encrypt_then_mac
    case TLSEXT_TYPE_encrypt_then_mac:
      extension_name = "encrypt then mac";
      break;
# endif

# ifdef TLSEXT_TYPE_extended_master_secret
    case TLSEXT_TYPE_extended_master_secret:
      extension_name = "extended master secret";
      break;
# endif

# ifdef TLSEXT_TYPE_session_ticket
    case TLSEXT_TYPE_session_ticket:
      extension_name = "session ticket";
      break;
# endif

# ifdef TLSEXT_TYPE_psk
    case TLSEXT_TYPE_psk:
      extension_name = "PSK";
      break;
# endif

# ifdef TLSEXT_TYPE_supported_versions
    case TLSEXT_TYPE_supported_versions: {
      BIO *bio = NULL;
      char *ext_info = NULL;
      long ext_infolen = 0;

      /* If we are the server responding, we only indicate the selected
       * protocol version.  Otherwise, we are a client indicating the range
       * of versions supported.
       */

      extension_name = "supported versions";

      if (pr_trace_get_level(trace_channel) >= 19) {
        bio = BIO_new(BIO_s_mem());

        if (server) {
          if (tlsext_datalen == 2) {
            int version;

            version = (tlsext_data[0] << 8) | tlsext_data[1];
            BIO_printf(bio, "\n  %s (0x%x)\n",
              tls_get_label(version, tls_version_labels), version);
          }

        } else {
          if (tlsext_datalen >= 1) {
            int len;

            len = tlsext_data[0];
            if (tlsext_datalen == len + 1) {
              tlsext_data += 1;

              BIO_puts(bio, "\n");

              while (len > 0) {
                int version;

                pr_signals_handle();

                version = (tlsext_data[0] << 8) | tlsext_data[1];
                BIO_printf(bio, "  %s (0x%x)\n",
                  tls_get_label(version, tls_version_labels), version);
                len -= 2;
                tlsext_data += 2;
              }
            }
          }
        }

        ext_infolen = BIO_get_mem_data(bio, &ext_info);
        if (ext_info != NULL) {
          ext_info[ext_infolen] = '\0';
        }
      }

      pr_trace_msg(trace_channel, 6,
        "[tls.tlsext] TLS %s extension \"%s\" (ID %d, %d %s)%.*s",
        server ? "server" : "client", extension_name, type,
        tlsext_datalen, tlsext_datalen != 1 ? "bytes" : "byte",
        (int) ext_infolen, ext_info != NULL ? ext_info : "");

      if (bio != NULL) {
        BIO_free(bio);
      }

      print_basic_info = FALSE;
      break;
    }
# endif

# ifdef TLSEXT_TYPE_psk_kex_modes
    case TLSEXT_TYPE_psk_kex_modes: {
      BIO *bio = NULL;
      char *ext_info = NULL;
      long ext_infolen = 0;

      extension_name = "PSK KEX modes";

      if (pr_trace_get_level(trace_channel) >= 19) {
        if (tlsext_datalen >= 1) {
          int len;

          bio = BIO_new(BIO_s_mem());
          len = tlsext_data[0];

          if (tlsext_datalen == len + 1) {
            tlsext_data += 1;

            BIO_puts(bio, "\n");

            while (len > 0) {
              int kex_mode;

              pr_signals_handle();

              kex_mode = tlsext_data[0];
              BIO_printf(bio, "  %s (%d)\n",
                tls_get_label(kex_mode, tls_psk_kex_labels), kex_mode);
              len -= 1;
              tlsext_data += 1;
            }
          }
        }

        ext_infolen = BIO_get_mem_data(bio, &ext_info);
        if (ext_info != NULL) {
          ext_info[ext_infolen] = '\0';
        }
      }

      pr_trace_msg(trace_channel, 6,
        "[tls.tlsext] TLS %s extension \"%s\" (ID %d, %d %s)%.*s",
        server ? "server" : "client", extension_name, type,
        tlsext_datalen, tlsext_datalen != 1 ? "bytes" : "byte",
        (int) ext_infolen, ext_info != NULL ? ext_info : "");

      if (bio != NULL) {
        BIO_free(bio);
      }

      print_basic_info = FALSE;
      break;
    }
# endif

# ifdef TLSEXT_TYPE_key_share
    case TLSEXT_TYPE_key_share:
      extension_name = "key share";
      break;
# endif

# ifdef TLSEXT_TYPE_renegotiate
    case TLSEXT_TYPE_renegotiate:
      extension_name = "renegotiation info";
      break;
# endif

# ifdef TLSEXT_TYPE_opaque_prf_input
    case TLSEXT_TYPE_opaque_prf_input:
      extension_name = "opaque PRF input";
      break;
# endif

# ifdef TLSEXT_TYPE_next_proto_neg
    case TLSEXT_TYPE_next_proto_neg:
      extension_name = "next protocol";
      break;
# endif

# ifdef TLSEXT_TYPE_application_layer_protocol_negotiation
    case TLSEXT_TYPE_application_layer_protocol_negotiation:
      extension_name = "application layer protocol";
      break;
# endif

# ifdef TLSEXT_TYPE_padding
    case TLSEXT_TYPE_padding:
      extension_name = "TLS padding";
      break;
# endif

# ifdef TLSEXT_TYPE_early_data
    case TLSEXT_TYPE_early_data:
      extension_name = "early data";
      break;
# endif

# ifdef TLSEXT_TYPE_post_handshake_auth
    case TLSEXT_TYPE_post_handshake_auth:
      extension_name = "post handshake auth";
      break;
# endif

    default:
      print_basic_info = TRUE;
      break;
  }

  if (print_basic_info == TRUE) {
    pr_trace_msg(trace_channel, 6,
      "[tls.tlsext] TLS %s extension \"%s\" (ID %d, %d %s)",
      server ? "server" : "client", extension_name, type, tlsext_datalen,
      tlsext_datalen != 1 ? "bytes" : "byte");
  }
}
#endif /* !OPENSSL_NO_TLSEXT */

#if defined(PR_USE_OPENSSL_OCSP)
static OCSP_RESPONSE *ocsp_send_request(pool *p, BIO *bio, const char *host,
    const char *uri, OCSP_REQUEST *req, unsigned int request_timeout) {
  int fd, res;
  OCSP_RESPONSE *resp = NULL;
  OCSP_REQ_CTX *ctx = NULL;
  const char *header_name, *header_value;

  res = BIO_get_fd(bio, &fd);
  if (res <= 0) {
    pr_trace_msg(trace_channel, 3,
      "error obtaining OCSP responder socket fd: %s", tls_get_errors());
    return NULL;
  }

  ctx = OCSP_sendreq_new(bio, (char *) uri, NULL, -1);
  if (ctx == NULL) {
    pr_trace_msg(trace_channel, 4,
      "error allocating OCSP request context: %s", tls_get_errors());
    return NULL;
  }

# if OPENSSL_VERSION_NUMBER >= 0x10000001L
  header_name = "Host";
  header_value = host;
  res = OCSP_REQ_CTX_add1_header(ctx, header_name, header_value);
  if (res != 1) {
    pr_trace_msg(trace_channel, 4,
      "error adding '%s: %s' header to OCSP request context: %s", header_name,
      header_value, tls_get_errors());
    OCSP_REQ_CTX_free(ctx);
    return NULL;
  }

  header_name = "Accept";
  header_value = "application/ocsp-response";
  res = OCSP_REQ_CTX_add1_header(ctx, header_name, header_value);
  if (res != 1) {
    pr_trace_msg(trace_channel, 4,
      "error adding '%s: %s' header to OCSP request context: %s", header_name,
      header_value, tls_get_errors());
    OCSP_REQ_CTX_free(ctx);
    return NULL;
  }

  header_name = "User-Agent";
  header_value = "proftpd+" MOD_TLS_VERSION;
  res = OCSP_REQ_CTX_add1_header(ctx, header_name, header_value);
  if (res != 1) {
    pr_trace_msg(trace_channel, 4,
      "error adding '%s: %s' header to OCSP request context: %s", header_name,
      header_value, tls_get_errors());
    OCSP_REQ_CTX_free(ctx);
    return NULL;
  }

  /* If we are using nonces, then we need to explicitly request that no
   * caches along the way interfere.
   */
  if (!(tls_stapling_opts & TLS_STAPLING_OPT_NO_NONCE)) {
    header_name = "Pragma";
    header_value = "no-cache";
    res = OCSP_REQ_CTX_add1_header(ctx, header_name, header_value);
    if (res != 1) {
      pr_trace_msg(trace_channel, 4,
        "error adding '%s: %s' header to OCSP request context: %s", header_name,
        header_value, tls_get_errors());
      OCSP_REQ_CTX_free(ctx);
      return NULL;
    }

    header_name = "Cache-Control";
    header_value = "no-cache, no-store";
    res = OCSP_REQ_CTX_add1_header(ctx, header_name, header_value);
    if (res != 1) {
      pr_trace_msg(trace_channel, 4,
        "error adding '%s: %s' header to OCSP request context: %s", header_name,
        header_value, tls_get_errors());
      OCSP_REQ_CTX_free(ctx);
      return NULL;
    }
  }

  /* Only add the request after we've added our headers. */
  res = OCSP_REQ_CTX_set1_req(ctx, req);
  if (res != 1) {
    pr_trace_msg(trace_channel, 4,
      "error adding OCSP request to context: %s", tls_get_errors());
    OCSP_REQ_CTX_free(ctx);
    return NULL;
  }
# endif /* OpenSSL-1.0.0 and later */

  while (TRUE) {
    fd_set fds;
    struct timeval tv;

    res = OCSP_sendreq_nbio(&resp, ctx);
    if (res != -1) {
      break;
    }

    if (request_timeout == 0) {
      break;
    }

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_usec = 0;
    tv.tv_sec = request_timeout;

    if (BIO_should_read(bio)) {
      res = select(fd + 1, (void *) &fds, NULL, NULL, &tv);

    } else if (BIO_should_write(bio)) {
      res = select(fd + 1, NULL, (void *) &fds, NULL, &tv);

    } else {
      pr_trace_msg(trace_channel, 3,
        "unexpected retry condition when talking to OCSP responder '%s%s'",
        host, uri);
      res = -1;
      break;
    }

    if (res == 0) {
      pr_trace_msg(trace_channel, 3,
         "timed out talking to OCSP responder '%s%s'", host, uri);
      errno = ETIMEDOUT;
      res = -1;
      break;
    }
  }

  OCSP_REQ_CTX_free(ctx);

  if (res) {
    if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
      BIO *diags_bio;

      diags_bio = BIO_new(BIO_s_mem());
      if (diags_bio != NULL) {
        if (OCSP_RESPONSE_print(diags_bio, resp, 0) == 1) {
          char *data = NULL;
          long datalen = 0;

          datalen = BIO_get_mem_data(diags_bio, &data);
          if (data != NULL) {
            data[datalen] = '\0';
            tls_log("received OCSP response (%ld bytes):\n%s", datalen, data);
          }
        }
      }

      BIO_free(diags_bio);
    }

    return resp;
  }

  pr_trace_msg(trace_channel, 4,
    "error obtaining OCSP response from responder: %s", tls_get_errors());
  return NULL;
}

static X509 *ocsp_get_issuing_cert(pool *p, X509 *cert, SSL *ssl) {
  int res;
  X509 *issuer = NULL;
  SSL_CTX *ctx;
  X509_STORE *store;
  X509_STORE_CTX *store_ctx;
  STACK_OF(X509) *extra_certs = NULL;

  if (ssl == NULL) {
    pr_trace_msg(trace_channel, 4, "%s",
      "unable to get issuing cert: no TLS session provided");
    errno = EINVAL;
    return NULL;
  }

  ctx = SSL_get_SSL_CTX(ssl);
  if (ctx == NULL) {
    pr_trace_msg(trace_channel, 4,
      "no SSL_CTX found for TLS session: %s", tls_get_errors());
    errno = EINVAL;
    return NULL;
  }

  /* First look for the issuer in the CertificateChainFile certs, if any. */
# if OPENSSL_VERSION_NUMBER >= 0x10001000L
  (void) SSL_CTX_get_extra_chain_certs(ctx, &extra_certs);
# else
  extra_certs = ctx->extra_certs;
# endif

  if (extra_certs != NULL &&
      sk_X509_num(extra_certs) > 0) {
    register int i;

    for (i = 0; i < sk_X509_num(extra_certs); i++) {
      X509 *extra_cert;

      extra_cert = sk_X509_value(extra_certs, i);
      if (X509_check_issued(extra_cert, cert) == X509_V_OK) {
        issuer = X509_dup(extra_cert);
        pr_trace_msg(trace_channel, 14, "found issuer %p for certificate",
          issuer);

        return issuer;
      }
    }
  }

  /* If not found, look in the trusted certs (CACertificateFile/Path). */
  store = SSL_CTX_get_cert_store(ctx);
  if (store == NULL) {
    pr_trace_msg(trace_channel, 4,
      "no certificate store found for SSL_CTX: %s", tls_get_errors());

    errno = EINVAL;
    return NULL;
  }

  store_ctx = X509_STORE_CTX_new();
  if (store_ctx == NULL) {
    pr_trace_msg(trace_channel, 4,
      "error allocating certificate store context: %s", tls_get_errors());

    errno = ENOMEM;
    return NULL;
  }

  res = X509_STORE_CTX_init(store_ctx, store, NULL, NULL);
  if (res != 1) {
    pr_trace_msg(trace_channel, 4,
      "error initializing certificate store context: %s", tls_get_errors());
    X509_STORE_CTX_free(store_ctx);

    errno = ENOMEM;
    return NULL;
  }

  res = X509_STORE_CTX_get1_issuer(&issuer, store_ctx, cert);
  if (res == -1) {
    pr_trace_msg(trace_channel, 4,
      "error finding issuing certificate: %s", tls_get_errors());
    X509_STORE_CTX_free(store_ctx);

    errno = EINVAL;
    return NULL;
  }

  if (res == 0) {
    pr_trace_msg(trace_channel, 4,
      "no issuing certificate found: %s", tls_get_errors());
    X509_STORE_CTX_free(store_ctx);

    errno = ENOENT;
    return NULL;
  }

  X509_STORE_CTX_free(store_ctx);

  pr_trace_msg(trace_channel, 14, "found issuer %p for certificate", issuer);
  return issuer;
}

static OCSP_REQUEST *ocsp_get_request(pool *p, X509 *cert, X509 *issuer) {
  OCSP_REQUEST *req = NULL;
  OCSP_CERTID *cert_id = NULL;

  req = OCSP_REQUEST_new();
  if (req == NULL) {
    pr_trace_msg(trace_channel, 4, "error allocating OCSP request: %s",
      tls_get_errors());
    return NULL;
  }

  cert_id = OCSP_cert_to_id(NULL, cert, issuer);
  if (cert_id == NULL) {
    pr_trace_msg(trace_channel, 4, "error obtaining ID for cert: %s",
      tls_get_errors());
    OCSP_REQUEST_free(req);
    return NULL;
  }

  if (OCSP_request_add0_id(req, cert_id) == NULL) {
    pr_trace_msg(trace_channel, 4, "error adding ID to OCSP request: %s",
      tls_get_errors());
    OCSP_CERTID_free(cert_id);
    OCSP_REQUEST_free(req);
    return NULL;
  }

  if (!(tls_stapling_opts & TLS_STAPLING_OPT_NO_NONCE)) {
    OCSP_request_add1_nonce(req, NULL, -1);
  }

  if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
    BIO *diags_bio;

    diags_bio = BIO_new(BIO_s_mem());
    if (diags_bio != NULL) {
      if (OCSP_REQUEST_print(diags_bio, req, 0) == 1) {
        char *data = NULL;
        long datalen = 0;

        datalen = BIO_get_mem_data(diags_bio, &data);
        if (data != NULL) {
          data[datalen] = '\0';
          tls_log("sending OCSP request (%ld bytes):\n%s", datalen, data);
        }
      }

      BIO_free(diags_bio);
    }
  }

  return req;
}

static int ocsp_check_cert_status(pool *p, X509 *cert, X509 *issuer,
    OCSP_BASICRESP *basic_resp, int *ocsp_status, int *ocsp_reason) {
  int res, status, reason;
  OCSP_CERTID *cert_id = NULL;
  ASN1_GENERALIZEDTIME *this_update = NULL, *next_update = NULL,
    *revoked_at = NULL;

  cert_id = OCSP_cert_to_id(NULL, cert, issuer);
  if (cert_id == NULL) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 3,
      "error obtaining cert ID from basic OCSP response: %s", tls_get_errors());

    errno = xerrno;
    return -1;
  }

  res = OCSP_resp_find_status(basic_resp, cert_id, &status, &reason,
    &revoked_at, &this_update, &next_update);
  if (res != 1) {
    pr_trace_msg(trace_channel, 3,
      "error locating certificate status in OCSP response: %s",
      tls_get_errors());

    OCSP_CERTID_free(cert_id);
    errno = ENOENT;
    return -1;
  }

  OCSP_CERTID_free(cert_id);

  res = OCSP_check_validity(this_update, next_update,
    TLS_OCSP_RESP_MAX_AGE_SECS, -1);
  if (res != 1) {
    pr_trace_msg(trace_channel, 3,
      "failed time-based validity check of OCSP response: %s",
      tls_get_errors());
    errno = EINVAL;
    return -1;
  }

  /* Valid or not, we still want to cache this response, AND communicate
   * the certificate status, as is, backed to the client via the stapled
   * response.
   */

  pr_trace_msg(trace_channel, 8,
    "found certificate status '%s' in OCSP response",
    OCSP_cert_status_str(status));
  if (status == V_OCSP_CERTSTATUS_REVOKED) {
    if (reason != -1) {
      pr_trace_msg(trace_channel, 8, "revocation reason: %s",
        OCSP_crl_reason_str(reason));
    }
  }

  if (ocsp_status != NULL) {
    *ocsp_status = status;
  }

  if (ocsp_reason != NULL) {
    *ocsp_reason = reason;
  }

  return 0;
}

static int ocsp_check_response(pool *p, X509 *cert, X509 *issuer, SSL *ssl,
    OCSP_REQUEST *req, OCSP_RESPONSE *resp) {
  int flags = 0, res = 0, resp_status;
  OCSP_BASICRESP *basic_resp = NULL;
  SSL_CTX *ctx = NULL;
  X509_STORE *store = NULL;
  STACK_OF(X509) *chain = NULL;

  ctx = SSL_get_SSL_CTX(ssl);
  if (ctx == NULL) {
    pr_trace_msg(trace_channel, 4,
      "no SSL_CTX found for TLS session: %s", tls_get_errors());

    errno = EINVAL;
    return -1;
  }

  store = SSL_CTX_get_cert_store(ctx);
  if (store == NULL) {
    pr_trace_msg(trace_channel, 4,
      "no certificate store found for SSL_CTX: %s", tls_get_errors());

    errno = EINVAL;
    return -1;
  }

  basic_resp = OCSP_response_get1_basic(resp);
  if (basic_resp == NULL) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 3,
      "error getting basic OCSP response: %s", tls_get_errors());

    errno = xerrno;
    return -1;
  }

  if (!(tls_stapling_opts & TLS_STAPLING_OPT_NO_NONCE)) {
    res = OCSP_check_nonce(req, basic_resp);
    if (res < 0) {
      pr_trace_msg(trace_channel, 1,
        "WARNING: OCSP response is missing request nonce");

    } else if (res == 0) {
      pr_trace_msg(trace_channel, 3,
        "error verifying OCSP response nonce: %s", tls_get_errors());

      OCSP_BASICRESP_free(basic_resp);
      errno = EINVAL;
      return -1;
    }
  }

  chain = sk_X509_new_null();
  if (chain != NULL) {
    STACK_OF(X509) *extra_certs = NULL;

    sk_X509_push(chain, issuer);

# if OPENSSL_VERSION_NUMBER >= 0x10001000L
    SSL_CTX_get_extra_chain_certs(ctx, &extra_certs);
# else
    extra_certs = ctx->extra_certs;
# endif

    if (extra_certs != NULL) {
      register int i;

      for (i = 0; i < sk_X509_num(extra_certs); i++) {
        sk_X509_push(chain, sk_X509_value(extra_certs, i));
      }
    }
  }

  flags = OCSP_TRUSTOTHER;
  if (tls_stapling_opts & TLS_STAPLING_OPT_NO_VERIFY) {
    flags = OCSP_NOVERIFY;
  }

  res = OCSP_basic_verify(basic_resp, chain, store, flags);
  if (res != 1) {
    pr_trace_msg(trace_channel, 3,
      "error verifying basic OCSP response data: %s", tls_get_errors());

    OCSP_BASICRESP_free(basic_resp);

    if (chain != NULL) {
      sk_X509_free(chain);
    }

    errno = EINVAL;
    return -1;
  }

  if (chain != NULL) {
    sk_X509_free(chain);
  }

  /* Now that we have verified the response, we can check the response status.
   * If we only looked at the status first, then a malicious responder
   * could be tricking us, e.g.:
   *
   *  http://www.thoughtcrime.org/papers/ocsp-attack.pdf
   */

  resp_status = OCSP_response_status(resp);
  if (resp_status != OCSP_RESPONSE_STATUS_SUCCESSFUL) {
    pr_trace_msg(trace_channel, 3,
      "OCSP response not successful: %s (%d)",
      OCSP_response_status_str(resp_status), resp_status);

    OCSP_BASICRESP_free(basic_resp);
    errno = EINVAL;
    return -1;
  }

  res = ocsp_check_cert_status(p, cert, issuer, basic_resp, NULL, NULL);
  OCSP_BASICRESP_free(basic_resp);

  return res;
}

static int ocsp_connect(pool *p, BIO *bio, unsigned int request_timeout) {
  int fd, res;

  if (request_timeout > 0) {
    BIO_set_nbio(bio, 1);
  }

  res = BIO_do_connect(bio);
  if (res <= 0 &&
      (request_timeout == 0 || !BIO_should_retry(bio))) {
    pr_trace_msg(trace_channel, 4,
      "error connecting to OCSP responder: %s", tls_get_errors());
    errno = EPERM;
    return -1;
  }

  if (BIO_get_fd(bio, &fd) < 0) {
    pr_trace_msg(trace_channel, 3,
      "error obtaining OCSP responder socket fd: %s", tls_get_errors());
    errno = EINVAL;
    return -1;
  }

  if (request_timeout > 0 &&
      res <= 0) {
    struct timeval tv;
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_usec = 0;
    tv.tv_sec = request_timeout;
    res = select(fd + 1, NULL, (void *) &fds, NULL, &tv);
    if (res == 0) {
      errno = ETIMEDOUT;
      return -1;
    }
  }

  return 0;
}

static OCSP_RESPONSE *ocsp_request_response(pool *p, X509 *cert, SSL *ssl,
    const char *url, unsigned int request_timeout) {
  BIO *bio;
  SSL_CTX *ctx = NULL;
  X509 *issuer = NULL;
  char *host = NULL, *port = NULL, *uri = NULL;
  int res, use_ssl = FALSE;
  OCSP_REQUEST *req = NULL;
  OCSP_RESPONSE *resp = NULL;

  issuer = ocsp_get_issuing_cert(p, cert, ssl);
  if (issuer == NULL) {
    return NULL;
  }

  /* Current OpenSSL implementation of OCSP_parse_url() guarantees that
   * host, port, and uri will never be NULL.  Nice.
   */
  res = OCSP_parse_url((char *) url, &host, &port, &uri, &use_ssl);
  if (res != 1) {
    pr_trace_msg(trace_channel, 4, "error parsing OCSP URL '%s': %s", url,
      tls_get_errors());
    X509_free(issuer);
    return NULL;
  }

  req = ocsp_get_request(p, cert, issuer);
  if (req == NULL) {
    X509_free(issuer);
    OCSP_REQUEST_free(req);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);
    return NULL;
  }

  pr_trace_msg(trace_channel, 9,
    "parsed OCSP URL '%s' to get host '%s', port '%s', URI '%s'%s",
    url, host, port, uri, use_ssl ? ", using TLS" : "");

  bio = BIO_new_connect(host);
  if (bio == NULL) {
    pr_trace_msg(trace_channel, 4, "error allocating connect BIO: %s",
      tls_get_errors());

    X509_free(issuer);
    OCSP_REQUEST_free(req);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);
    return NULL;
  }

  BIO_set_conn_port(bio, port);

  if (use_ssl) {
    BIO *ssl_bio;

    ctx = SSL_CTX_new(SSLv23_client_method());
    if (ctx == NULL) {
      pr_trace_msg(trace_channel, 4, "error allocating SSL context: %s",
        tls_get_errors());

      X509_free(issuer);
      OCSP_REQUEST_free(req);
      BIO_free_all(bio);
      OPENSSL_free(host);
      OPENSSL_free(port);
      OPENSSL_free(uri);
      return NULL;
    }

    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    ssl_bio = BIO_new_ssl(ctx, 1);
    bio = BIO_push(ssl_bio, bio);
  }

  res = ocsp_connect(p, bio, request_timeout);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 3,
      "error connecting to OCSP responder %s:%s: %s", host, port,
      strerror(xerrno));

    X509_free(issuer);
    OCSP_REQUEST_free(req);
    BIO_free_all(bio);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    errno = xerrno;
    return NULL;
  }

  resp = ocsp_send_request(p, bio, host, uri, req, request_timeout);

  OPENSSL_free(host);
  OPENSSL_free(port);
  OPENSSL_free(uri);

  if (ctx != NULL) {
    SSL_CTX_free(ctx);
  }

  if (bio != NULL) {
    BIO_free_all(bio);
  }

  if (resp == NULL) {
    X509_free(issuer);
    OCSP_REQUEST_free(req);
    return NULL;
  }

  if (ocsp_check_response(p, cert, issuer, ssl, req, resp) < 0) {
    if (errno != ENOSYS) {
      X509_free(issuer);
      OCSP_REQUEST_free(req);
      OCSP_RESPONSE_free(resp);
      errno = EINVAL;
      return NULL;
    }
  }

  X509_free(issuer);
  OCSP_REQUEST_free(req);
  return resp;
}

#if OPENSSL_VERSION_NUMBER < 0x10002000L || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER < 0x3050000L)
/* We need to provide our own backport of the ASN1_TIME_diff() function. */
static time_t ASN1_TIME_seconds(const ASN1_TIME *a) {
  static const int min[9] = { 0, 0, 1, 1, 0, 0, 0, 0, 0 };
  static const int max[9] = { 99, 99, 12, 31, 23, 59, 59, 12, 59 };
  time_t t = 0;
  char *text;
  int text_len;
  int i, j, n;
  unsigned int nyears, nmons, nhours, nmins, nsecs;

  if (a->type != V_ASN1_GENERALIZEDTIME) {
    return 0;
  }

  text_len = a->length;
  text = (char *) a->data;

  /* GENERALIZEDTIME is similar to UTCTIME except the year is represented
   * as YYYY. This stuff treats everything as a two digit field so make
   * first two fields 00 to 99
   */

  if (text_len < 13) {
    return 0;
  }

  nyears = nmons = nhours = nmins = nsecs = 0;

  for (i = 0, j = 0; i < 7; i++) {
    if (i == 6 &&
        (text[j] == 'Z' ||
         text[j] == '+' ||
         text[j] == '-')) {
      i++;
      break;
    }

    if (text[j] < '0' ||
        text[j] > '9') {
      return 0;
    }

    n = text[j] - '0';
    if (++j > text_len) {
      return 0;
    }

    if (text[j] < '0' ||
        text[j] > '9') {
      return 0;
    }

    n = (n * 10) + (text[j] - '0');
    if (++j > text_len) {
      return 0;
    }

    if (n < min[i] ||
        n > max[i]) {
      return 0;
    }

    switch (i) {
      case 0:
        /* Years */
        nyears = (n * 100);
        break;

      case 1:
        /* Years */
        nyears += n;
        break;

      case 2:
        /* Month */
        nmons = n - 1;
        break;

      case 3:
        /* Day of month; ignored */
        break;

      case 4:
        /* Hours */
        nhours = n;
        break;

      case 5:
        /* Minutes */
        nmins = n;
        break;

      case 6:
        /* Seconds */
        nsecs = n;
        break;
    }
  }

  /* Yes, this is not calendrical accurate.  It only needs to be a good
   * enough estimation, as it is used (currently) only for determining the
   * validity window of an OCSP request (in seconds).
   */
  t = (nyears * 365 * 86400) + (nmons * 30 * 86400) * (nhours * 3600) + nsecs;

  /* Optional fractional seconds: decimal point followed by one or more
   * digits.
   */
  if (text[j] == '.') {
    if (++j > text_len) {
      return 0;
    }

    i = j;

    while (text[j] >= '0' &&
           text[j] <= '9' &&
           j <= text_len) {
      j++;
    }

    /* Must have at least one digit after decimal point */
    if (i == j) {
      return 0;
    }
  }

  if (text[j] == 'Z') {
    j++;

  } else if (text[j] == '+' ||
             text[j] == '-') {
    int offsign, offset = 0;

    offsign = text[j] == '-' ? -1 : 1;
    j++;

    if (j + 4 > text_len) {
      return 0;
    }

    for (i = 7; i < 9; i++) {
      if (text[j] < '0' ||
          text[j] > '9') {
        return 0;
      }

      n = text[j] - '0';
      j++;

      if (text[j] < '0' ||
          text[j] > '9') {
        return 0;
      }

      n = (n * 10) + text[j] - '0';

      if (n < min[i] ||
          n > max[i]) {
        return 0;
      }

      if (i == 7) {
        offset = n * 3600;

      } else if (i == 8) {
        offset += n * 60;
      }

      j++;
    }

    if (offset > 0) {
      t += (offset * offsign);
    }

  } else if (text[j]) {
    /* Missing time zone information. */
    return 0;
  }

  return t;
}

static int ASN1_TIME_diff(int *pday, int *psec, const ASN1_TIME *from,
    const ASN1_TIME *to) {
  time_t from_secs, to_secs, diff_secs;
  long diff_days;

  from_secs = ASN1_TIME_seconds(from);
  if (from_secs == 0) {
    return 0;
  }

  to_secs = ASN1_TIME_seconds(to);
  if (to_secs == 0) {
    return 0;
  }

  if (to_secs > from_secs) {
    diff_secs = to_secs - from_secs;

  } else {
    diff_secs = from_secs - to_secs;
  }

  /* The ASN1_TIME_diff() API in OpenSSL-1.0.2+ offers days and seconds,
   * possibly to handle LARGE time differences without overflowing the data
   * type for seconds.  So we do the same.
   */

  diff_days = diff_secs % 86400;
  diff_secs -= (diff_days * 86400);

  if (pday) {
    *pday = (int) diff_days;
  }

  if (psec) {
    *psec = diff_secs;
  }

  return 1;
}
#endif /* Before OpenSSL-1.0.2, or libressl */

static int ocsp_stale_response(pool *p, OCSP_RESPONSE *resp, X509 *cert,
    SSL *ssl, time_t age, time_t *expired) {
  int res = -1, ocsp_status, stale = FALSE;

  ocsp_status = OCSP_response_status(resp);
  *expired = 0;

  /* If we received a SUCCESSFUL response from the OCSP responder, then
   * we consider the response to be stale starting at halfway through its
   * validity period (and expired if after the validity period).  Otherwise,
   * we expire the cached entry after 5 minutes (hardcoded).
   */

  if (ocsp_status == OCSP_RESPONSE_STATUS_SUCCESSFUL) {
    OCSP_BASICRESP *basic_resp = NULL;

    basic_resp = OCSP_response_get1_basic(resp);
    if (basic_resp != NULL) {
      X509 *issuer;

      issuer = ocsp_get_issuing_cert(p, cert, ssl);
      if (issuer != NULL) {
        OCSP_CERTID *cert_id = NULL;

        cert_id = OCSP_cert_to_id(NULL, cert, issuer);
        if (cert_id != NULL) {
          ASN1_GENERALIZEDTIME *this_update = NULL, *next_update = NULL;

          res = OCSP_resp_find_status(basic_resp, cert_id, NULL, NULL, NULL,
            &this_update, &next_update);
          if (res == 1) {
            time_t now;

            now = time(NULL);

            /* If we have passed the nextUpdate time, we have expired. */
            if (next_update != NULL) {
              res = X509_cmp_time(next_update, &now);
              if (res < 0) {
                pr_trace_msg(trace_channel, 17,
                  "cached OCSP response has EXPIRED");
                *expired = now;
                stale = TRUE;

              } else {
                int ndays = 0, nsecs = 0;

                /* Start requesting fresh responses halfway through the validity
                 * period:
                 *
                 *  now > (thisUpdate + ((nextUpdate - thisUpdate) / 2))
                 *
                 * or, rephrased slightly differently:
                 *
                 *  now - ((nextUpdate - thisUpdate) / 2) > thisUpdate
                 */

                res = ASN1_TIME_diff(&ndays, &nsecs, this_update, next_update);
                if (res == 1) {
                  int validity_secs;
                  time_t refresh_ts;

                  validity_secs = (ndays * 86400) + nsecs;
                  refresh_ts = now - (validity_secs / 2);

                  res = X509_cmp_time(this_update, &refresh_ts);
                  if (res < 0) {
                    pr_trace_msg(trace_channel, 17,
                      "cached OCSP response is stale");
                    stale = TRUE;
                  }

                } else {
                  pr_trace_msg(trace_channel, 3, "error computing difference "
                    "in OCSP response timestamps: %s", tls_get_errors());
                }
              }

            } else {
              /* If the OCSP response has no nextUpdate time, then we assume
               * it to be stale after one hour (hardcoded).
               */
              if (age > 3600) {
                stale = TRUE;
              }
            }
          }

          OCSP_CERTID_free(cert_id);
        }

        X509_free(issuer);
      }

      OCSP_BASICRESP_free(basic_resp);

    } else {
      if (age > 300) {
        stale = TRUE;
      }
    }

  } else {
    if (age > 300) {
      stale = TRUE;
    }
  }

  if (stale == TRUE) {
    pr_trace_msg(trace_channel, 8,
      "cached %s OCSP response is %s", OCSP_response_status_str(ocsp_status),
      *expired > 0 ? "EXPIRED" : "stale");
    return 0;
  }

  return -1;
}

static OCSP_RESPONSE *ocsp_get_cached_response(pool *p,
    const char *fingerprint, X509 *cert, SSL *ssl, int *stale) {
  OCSP_RESPONSE *resp = NULL;
  time_t cache_age = 0, resp_age = 0;
  int res;

  if (tls_ocsp_cache == NULL) {
    errno = ENOSYS;
    return NULL;
  }

  resp = (tls_ocsp_cache->get)(tls_ocsp_cache, fingerprint, &resp_age);
  if (resp != NULL) {
    time_t now = 0;

    time(&now);
    cache_age = now - resp_age;
    pr_trace_msg(trace_channel, 9,
      "found cached OCSP response for fingerprint '%s': %lu %s old",
      fingerprint, (unsigned long) cache_age, cache_age != 1 ? "secs" : "sec");
  }

  if (resp != NULL) {
    time_t expired = 0;

    res = ocsp_stale_response(p, resp, cert, ssl, cache_age, &expired);
    if (expired > 0) {
      /* If the response has expired, we need to delete it. */
      pr_trace_msg(trace_channel, 5,
        "cached OCSP response for fingerprint '%s' expired at %s",
        fingerprint, pr_strtime3(p, expired, TRUE));
      res = (tls_ocsp_cache->delete)(tls_ocsp_cache, fingerprint);
      if (res < 0) {
        pr_trace_msg(trace_channel, 3,
          "error deleting expired OCSP response from '%s' cache for "
          "fingerprint '%s': %s", tls_ocsp_cache->cache_name, fingerprint,
          strerror(errno));
      }

      OCSP_RESPONSE_free(resp);
      resp = NULL;
      errno = ENOENT;

    } else {
      if (res == 0) {
        *stale = TRUE;

      } else {
        *stale = FALSE;
      }
    }

  } else {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 3,
      "error retrieving OCSP response from '%s' cache for "
      "fingerprint '%s': %s", tls_ocsp_cache->cache_name, fingerprint,
      strerror(xerrno));

    errno = xerrno;
  }

  return resp;
}

static int ocsp_add_cached_response(pool *p, const char *fingerprint,
    OCSP_RESPONSE *resp) {
  int res;
  time_t resp_age = 0;

  if (fingerprint == NULL ||
      tls_ocsp_cache == NULL) {
    errno = ENOSYS;
    return -1;
  }

  /* Cache this fake response, so that we don't have to keep redoing this
   * for a short amount of time (e.g. 5 minutes).
   */
  time(&resp_age);
  res = (tls_ocsp_cache->add)(tls_ocsp_cache, fingerprint, resp, resp_age);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 3,
      "error adding OCSP response to '%s' cache for fingerprint '%s': %s",
      tls_ocsp_cache->cache_name, fingerprint, strerror(xerrno));

    errno = xerrno;

  } else {
    pr_trace_msg(trace_channel, 15,
      "added OCSP response to '%s' cache for fingerprint '%s'",
      tls_ocsp_cache->cache_name, fingerprint);
  }

  return res;
}

static int tls_feature_cmp(ASN1_STRING *str, void *feat_data,
    size_t feat_datasz) {
  int is_feat = FALSE, res;
  ASN1_STRING *feat;

  feat = ASN1_STRING_type_new(V_ASN1_OCTET_STRING);
  ASN1_STRING_set(feat, feat_data, feat_datasz);

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  res = ASN1_STRING_cmp(str, feat);
#else
  res = M_ASN1_OCTET_STRING_cmp(str, feat);
#endif /* Before OpenSSL-1.1.0, or libressl */

  if (res == 0) {
    is_feat = TRUE;
  }
  ASN1_STRING_free(feat);

  return is_feat;
}

static int tls_cert_must_staple(X509 *cert, int *v2) {
  register int i;
  int ext_count = 0, must_staple = FALSE;

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
  ext_count = X509_get_ext_count(cert);
#else
  X509_CINF *ci;
  STACK_OF(X509_EXTENSION) *exts;

  ci = cert->cert_info;
  if (ci == NULL) {
    return FALSE;
  }

  exts = ci->extensions;
  ext_count = sk_X509_EXTENSION_num(exts);
#endif /* OpenSSL-1.1.x/LibreSSL-3.5.x or later */

  for (i = 0; i < ext_count; i++) {
    char buf[1024];
    X509_EXTENSION *ext;
    ASN1_OBJECT *obj;

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
    ext = X509_get_ext(cert, i);
#else
    ext = sk_X509_EXTENSION_value(exts, i);
#endif /* OpenSSL-1.1.x/LibreSSL-3.5.x or later */

    obj = X509_EXTENSION_get_object(ext);
    memset(buf, '\0', sizeof(buf));
    OBJ_obj2txt(buf, sizeof(buf)-1, obj, 1);

    /* Double-check that the OID is that of the "TLS Feature" extension. */
    if (strcmp(buf, TLS_X509V3_TLS_FEAT_OID_TEXT) == 0) {
      char status_request[] = TLS_X509V3_TLS_FEAT_STATUS_REQUEST;
      ASN1_OCTET_STRING *value;

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
      value = X509_EXTENSION_get_data(ext);
#else
      value = ext->value;
#endif /* OpenSSL-1.1.x/LibreSSL-3.5.x or later */

      /* Is the value of this extension the "status_request" value? */
      must_staple = tls_feature_cmp(value, status_request, 5);
      if (must_staple != TRUE) {
        char status_request_v2[] = TLS_X509V3_TLS_FEAT_STATUS_REQUEST_V2;

        /* Is the value of this extension the "status_request_v2" value? */
        must_staple = tls_feature_cmp(value, status_request_v2, 5);
        if (must_staple == TRUE) {
          *v2 = TRUE;
        }
      }
    }
  }

  return must_staple;
}

static OCSP_RESPONSE *ocsp_get_response(pool *p, SSL *ssl) {
  X509 *cert;
  const char *fingerprint = NULL;
  OCSP_RESPONSE *resp = NULL, *cached_resp = NULL;
  int stale_cache = FALSE, use_fake_trylater = FALSE;

  /* We need to find a cached OCSP response for the server cert in question,
   * thus we need to find out which server cert is used for this session.
   */
  cert = SSL_get_certificate(ssl);
  if (cert != NULL) {
    fingerprint = tls_get_fingerprint(p, cert);
    if (fingerprint != NULL) {
      pr_trace_msg(trace_channel, 3,
        "using fingerprint '%s' for server cert", fingerprint);
      if (tls_ocsp_cache != NULL) {
        cached_resp = ocsp_get_cached_response(p, fingerprint, cert, ssl,
          &stale_cache);
        if (cached_resp != NULL) {
          if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
            BIO *diags_bio;

            diags_bio = BIO_new(BIO_s_mem());
            if (diags_bio != NULL) {
              if (OCSP_RESPONSE_print(diags_bio, cached_resp, 0) == 1) {
                char *data = NULL;
                long datalen = 0;

                datalen = BIO_get_mem_data(diags_bio, &data);
                if (data != NULL) {
                  data[datalen] = '\0';
                  tls_log("cached OCSP response (%ld bytes):\n%s", datalen,
                    data);
                }
              }
            }

            BIO_free(diags_bio);
          }

          resp = cached_resp;

        } else {
          int xerrno = errno;

          pr_trace_msg(trace_channel, 17,
            "no cached OCSP response found in '%s' cache for "
            "fingerprint '%s': %s", tls_ocsp_cache->cache_name, fingerprint,
            strerror(errno));

          errno = xerrno;
        }

      } else {
        /* No TLSStaplingCache configured. */
        pr_trace_msg(trace_channel, 17,
          "no cached OCSP response found (TLSStaplingCache not configured)");
        errno = ENOENT;
      }

      if (cached_resp == NULL ||
          stale_cache == TRUE) {
        int xerrno = errno;
        OCSP_RESPONSE *fresh_resp = NULL;

        if (xerrno == ENOENT ||
            stale_cache == TRUE) {
          const char *ocsp_url;

          if (tls_stapling_responder == NULL) {
            ocsp_url = ocsp_get_responder_url(p, cert);
            if (ocsp_url != NULL) {
              pr_trace_msg(trace_channel, 8,
                "found OCSP responder URL '%s' in certificate "
                "(fingerprint '%s')", ocsp_url, fingerprint);

            } else {
              pr_trace_msg(trace_channel, 8,
                "no OCSP responder URL found in certificate "
                "(fingerprint '%s')", fingerprint);
            }

          } else {
            ocsp_url = tls_stapling_responder;
            pr_trace_msg(trace_channel, 8,
              "using configured OCSP responder URL '%s'", ocsp_url);
          }

          if (ocsp_url != NULL) {
            fresh_resp = ocsp_request_response(p, cert, ssl, ocsp_url,
              tls_stapling_timeout);
            if (fresh_resp != NULL) {
              resp = fresh_resp;

              /* If our previously cached response was stale, delete it so
               * that we can cache our new one.
               */
              if (stale_cache == TRUE) {
                int res;

                res = (tls_ocsp_cache->delete)(tls_ocsp_cache, fingerprint);
                if (res < 0) {
                  pr_trace_msg(trace_channel, 3,
                    "error deleting OCSP response from '%s' cache for "
                    "fingerprint '%s': %s", tls_ocsp_cache->cache_name,
                    fingerprint, strerror(errno));
                }

                OCSP_RESPONSE_free(cached_resp);
                cached_resp = NULL;
              }
            }

          } else {
            pr_trace_msg(trace_channel, 5,
              "no OCSP responder URL found in certificate (fingerprint '%s')",
              fingerprint);
          }

        } else {
          pr_trace_msg(trace_channel, 5,
            "no cached OCSP response found: %s", strerror(xerrno));
        }
      }
    }

  } else {
    pr_trace_msg(trace_channel, 8, "%s",
      "no server certificate found for TLS session");
  }

  if (resp == NULL) {
    use_fake_trylater = TRUE;

    /* No proper OCSP response found for stapling; provide a fake tryLater
     * response as a fallback.  However, if the NoFakeTryLater
     * TLSStaplingOption is used, we omit this fake response; some
     * implementation e.g. GnuTLS reject/choke on these fake responses.
     */
    if (tls_stapling_opts & TLS_STAPLING_OPT_NO_FAKE_TRY_LATER) {
      use_fake_trylater = FALSE;
    }

    /* On the other hand, if the server certificate uses the "must staple"
     * X509v3 feature, then we MUST provide an OCSP response, even a fake one.
     */
    if (cert != NULL) {
      int must_staple, is_v2 = FALSE;

      must_staple = tls_cert_must_staple(cert, &is_v2);
      if (must_staple == TRUE) {
        pr_trace_msg(trace_channel, 8,
          "found status_request%s 'must staple' TLS feature in certificate "
          "(fingerprint '%s')", is_v2 ? "_v2" : "", fingerprint);
        use_fake_trylater = TRUE;
      }
    }
  }

  if (use_fake_trylater) {
    pr_trace_msg(trace_channel, 5, "returning fake tryLater OCSP response");

    /* If we have not found an OCSP response, then fall back to using
     * a fake "tryLater" response.
     */
    resp = OCSP_response_create(OCSP_RESPONSE_STATUS_TRYLATER, NULL);
    if (resp == NULL) {
      pr_trace_msg(trace_channel, 1,
        "error allocating fake 'tryLater' OCSP response: %s", tls_get_errors());
      return NULL;
    }
  }

  /* If this response is not the one we just pulled from the cache, then
   * add it.
   */
  if (resp != cached_resp) {
    if (ocsp_add_cached_response(p, fingerprint, resp) < 0) {
      if (errno != ENOSYS) {
        pr_trace_msg(trace_channel, 3,
          "error caching OCSP response: %s", strerror(errno));
      }
    }
  }

  return resp;
}

static int tls_ocsp_cb(SSL *ssl, void *user_data) {
  OCSP_RESPONSE *resp;
  int resp_derlen, reused;
  unsigned char *resp_der = NULL;
  pool *ocsp_pool;

  if (tls_stapling == FALSE) {
    /* OCSP stapling disabled; do nothing. */
    return SSL_TLSEXT_ERR_NOACK;
  }

  reused = SSL_session_reused(ssl);
  if (reused > 0) {
    /* Per RFC 6066, if we are a resumed TLS session, then we should NOT be
     * stapling an OCSP response; the original handshake still applies
     * (Issue #528).
     */
    pr_trace_msg(trace_channel, 9,
      "OCSP stapling requested but ignored for resumed session, per RFC 6066");
    return SSL_TLSEXT_ERR_NOACK;
  }

  ocsp_pool = make_sub_pool(session.pool);
  pr_pool_tag(ocsp_pool, "Session OCSP response pool");

  resp = ocsp_get_response(ocsp_pool, ssl);
  resp_derlen = i2d_OCSP_RESPONSE(resp, &resp_der);
  if (resp_derlen <= 0) {
    tls_log("error determining OCSP response length: %s", tls_get_errors());
  }
  destroy_pool(ocsp_pool);

  /* Success or failure, we're done with the OCSP response. */
  OCSP_RESPONSE_free(resp);

  if (resp_derlen <= 0) {
    return SSL_TLSEXT_ERR_NOACK;
  }

  SSL_set_tlsext_status_ocsp_resp(ssl, resp_der, resp_derlen);
  return SSL_TLSEXT_ERR_OK;
}
#endif /* PR_USE_OPENSSL_OCSP */

#if defined(TLS_USE_SESSION_TICKETS)
static int tls_ticket_key_cmp(xasetmember_t *a, xasetmember_t *b) {
  struct tls_ticket_key *k1, *k2;

  k1 = (struct tls_ticket_key *) a;
  k2 = (struct tls_ticket_key *) b;

  if (k1->created == k2->created) {
    return 0;
  }

  if (k1->created < k2->created) {
    return -1;
  }

  return 1;
}

static struct tls_ticket_key *create_ticket_key(void) {
  struct tls_ticket_key *k;
  void *page_ptr = NULL;
  size_t pagesz;
  char *ptr;
# ifdef HAVE_MLOCK
  int res, xerrno = 0;
# endif /* HAVE_MLOCK */

  pagesz = sizeof(struct tls_ticket_key);
  ptr = tls_get_page(pagesz, &page_ptr);
  if (ptr == NULL) {
    if (page_ptr != NULL) {
      free(page_ptr);
    }
    return NULL;
  }

  k = (void *) ptr;
  time(&(k->created));

  if (RAND_bytes(k->key_name, 16) != 1) {
    pr_log_debug(DEBUG1, MOD_TLS_VERSION
      ": error generating random bytes: %s", tls_get_errors());
    free(page_ptr);
    errno = EPERM;
    return NULL;
  }

  if (RAND_bytes(k->cipher_key, 32) != 1) {
    pr_log_debug(DEBUG1, MOD_TLS_VERSION
      ": error generating random bytes: %s", tls_get_errors());
    free(page_ptr);
    errno = EPERM;
    return NULL;
  }

  if (RAND_bytes(k->hmac_key, 32) != 1) {
    pr_log_debug(DEBUG1, MOD_TLS_VERSION
      ": error generating random bytes: %s", tls_get_errors());
    free(page_ptr);
    errno = EPERM;
    return NULL;
  }

# ifdef HAVE_MLOCK
  PRIVS_ROOT
  res = mlock(page_ptr, pagesz);
  xerrno = errno;
  PRIVS_RELINQUISH

  if (res < 0) {
    /* Ideally, we would not proceed unless we can ensure this key remains
     * only in memory.  However, on some systems there may be a limit to the
     * amount of locked memory.  Erasing the session ticket key here would lead
     * to hard-to-debug TLS handshake issues later, for TLSv1.3 sessions which
     * use session tickets (and thus need these keys); see Issue #1014.
     *
     * Thus we now only _try_ to lock the key in memory, but proceed if we
     * cannot.
     */
    pr_log_debug(DEBUG1, MOD_TLS_VERSION
      ": error locking session ticket key into memory: %s", strerror(xerrno));
  }
# endif /* HAVE_MLOCK */

  k->page_ptr = page_ptr;
  k->pagesz = pagesz;
  return k;
}

static void destroy_ticket_key(struct tls_ticket_key *k) {
  void *page_ptr;
  size_t pagesz;
# ifdef HAVE_MLOCK
  int res, xerrno = 0;
# endif /* HAVE_MLOCK */

  if (k == NULL) {
    return;
  }

  page_ptr = k->page_ptr;
  pagesz = k->pagesz;

  pr_memscrub(k->page_ptr, k->pagesz);

# ifdef HAVE_MLOCK
  PRIVS_ROOT
  res = munlock(page_ptr, pagesz);
  xerrno = errno;
  PRIVS_RELINQUISH

  if (res < 0) {
    pr_log_debug(DEBUG1, MOD_TLS_VERSION
      ": error unlocking session ticket key memory: %s", strerror(xerrno));
  }
# endif /* HAVE_MLOCK */

  free(page_ptr);
}

static int remove_expired_ticket_keys(void) {
  struct tls_ticket_key *k = NULL;
  int expired_count = 0;
  time_t now;

  if (tls_ticket_key_curr_count < 2) {
    /* Always keep at least one key. */
    return 0;
  }

  time(&now);

  for (k = (struct tls_ticket_key *) tls_ticket_keys->xas_list;
       k;
       k = k->next) {
    time_t key_age;

    key_age = now - k->created;
    if (key_age > tls_ticket_key_max_age) {
      if (xaset_remove(tls_ticket_keys, (xasetmember_t *) k) == 0) {
        expired_count++;
        tls_ticket_key_curr_count--;
      }
    }
  }

  return expired_count;
}

static int remove_oldest_ticket_key(void) {
  struct tls_ticket_key *k = NULL;
  int res;

  if (tls_ticket_key_curr_count < 2) {
    /* Always keep at least one key. */
    return 0;
  }

  /* Remove the last ticket key in the set. */
  for (k = (struct tls_ticket_key *) tls_ticket_keys->xas_list;
       k && k->next != NULL;
       k = k->next);

  res = xaset_remove(tls_ticket_keys, (xasetmember_t *) k);
  if (res == 0) {
    tls_ticket_key_curr_count--;
  }

  return res;
}

static int add_ticket_key(struct tls_ticket_key *k) {
  int res;

  res = remove_expired_ticket_keys();
  if (res > 0) {
    pr_trace_msg(trace_channel, 9, "removed %d expired %s", res,
      res != 1 ? "keys" : "key");
  }

  if (tls_ticket_key_curr_count == tls_ticket_key_max_count) {
    res = remove_oldest_ticket_key();
    if (res < 0) {
      return -1;
    }
  }

  res = xaset_insert_sort(tls_ticket_keys, (xasetmember_t *) k, FALSE);
  if (res == 0) {
    tls_ticket_key_curr_count++;
  }

  return res;
}

/* Note: This lookup routine is where we might look in external storage,
 * e.g. Redis/memcache, for clustered/shared pool of ticket keys generated by
 * other servers.
 */
static struct tls_ticket_key *get_ticket_key(unsigned char *key_name,
    size_t key_namelen) {
  struct tls_ticket_key *k = NULL;

  if (tls_ticket_keys == NULL) {
    return NULL;
  }

  for (k = (struct tls_ticket_key *) tls_ticket_keys->xas_list;
       k;
       k = k->next) {
    if (memcmp(key_name, k->key_name, key_namelen) == 0) {
      break;
    }
  }

  return k;
}

static int new_ticket_key_timer_cb(CALLBACK_FRAME) {
  struct tls_ticket_key *k;

  pr_log_debug(DEBUG9, MOD_TLS_VERSION
    ": generating new TLS session ticket key");

  k = create_ticket_key();
  if (k == NULL) {
    pr_log_debug(DEBUG0, MOD_TLS_VERSION
      ": unable to generate new session ticket key: %s", strerror(errno));

  } else {
    add_ticket_key(k);
  }

  /* Always restart this timer. */
  return 1;
}

/* Remember that mlock(2) locks are not inherited across forks, thus
 * we want to renew those locks for session processes.
 */
static void lock_ticket_keys(void) {
# ifdef HAVE_MLOCK
  struct tls_ticket_key *k;

  if (tls_ticket_keys == NULL) {
    return;
  }

  for (k = (struct tls_ticket_key *) tls_ticket_keys->xas_list;
       k;
       k = k->next) {
    if (k->locked == FALSE) {
      int res, xerrno = 0;

      PRIVS_ROOT
      res = mlock(k->page_ptr, k->pagesz);
      xerrno = errno;
      PRIVS_RELINQUISH

      if (res < 0) {
        pr_log_debug(DEBUG1, MOD_TLS_VERSION
          ": error locking session ticket key into memory: %s",
          strerror(xerrno));

      } else {
        k->locked = TRUE;
      }
    }
  }
# endif /* HAVE_MLOCK */
}

static void scrub_ticket_keys(void) {
  struct tls_ticket_key *k, *next_k;

  if (tls_ticket_keys == NULL) {
    return;
  }

  for (k = (struct tls_ticket_key *) tls_ticket_keys->xas_list; k; k = next_k) {
    next_k = k->next;
    destroy_ticket_key(k);
  }

  tls_ticket_keys = NULL;
}

/* TLS session ticket _key_ callback; not to be confused with the TLSv1.3
 * session ticket callbacks.
 */
static int tls_ticket_key_cb(SSL *ssl, unsigned char *key_name,
    unsigned char *iv, EVP_CIPHER_CTX *cipher_ctx, HMAC_CTX *hmac_ctx,
    int mode) {
  struct tls_ticket_key *k;
  char *key_name_str;

  /* Note: should we have a list of ciphers from which we randomly choose,
   * when creating a key?  I.e. should the keys themselves hold references
   * to their ciphers, digests?
   */
  const EVP_CIPHER *cipher = EVP_aes_256_cbc();
# if defined(OPENSSL_NO_SHA256)
  const EVP_MD *md = EVP_sha1();
# else
  const EVP_MD *md = EVP_sha256();
# endif

  pr_trace_msg(trace_channel, 19,
    "handling session ticket key request on %s session (%s mode)",
    SSL_get_version(ssl), mode ? "encrypt" : "decrypt");

  if (mode == 1) {
    int ticket_key_len, sess_key_len;

    if (tls_ticket_keys == NULL) {
      return -1;
    }

    /* Creating a new session ticket.  Always use the first key in the set. */
    k = (struct tls_ticket_key *) tls_ticket_keys->xas_list;

    key_name_str = pr_str_bin2hex(session.pool, k->key_name, 16,
      PR_STR_FL_HEX_USE_LC);

    pr_trace_msg(trace_channel, 3,
      "TLS session ticket: encrypting using key name '%s' for %s session",
      key_name_str, SSL_session_reused(ssl) ? "reused" : "new");

    /* Warn loudly if the ticket key we are using is not as strong (based on
     * cipher key length) as the one negotiated for the session.
     */
    ticket_key_len = EVP_CIPHER_key_length(cipher) * 8;
    sess_key_len = SSL_get_cipher_bits(ssl, NULL);
    if (ticket_key_len < sess_key_len) {
      pr_log_pri(PR_LOG_INFO, MOD_TLS_VERSION
        ": WARNING: TLS session tickets encrypted with weaker key than "
        "session: ticket key = %s (%d bytes), session key = %s (%d bytes)",
        OBJ_nid2sn(EVP_CIPHER_type(cipher)), ticket_key_len,
        SSL_get_cipher_name(ssl), sess_key_len);
    }

    if (RAND_bytes(iv, EVP_CIPHER_iv_length(cipher)) != 1) {
      pr_trace_msg(trace_channel, 3,
        "unable to initialize session ticket key IV: %s", tls_get_errors());
      return -1;
    }

    if (EVP_EncryptInit_ex(cipher_ctx, cipher, NULL, k->cipher_key, iv) != 1) {
      pr_trace_msg(trace_channel, 3,
        "unable to initialize session ticket key cipher: %s", tls_get_errors());
      return -1;
    }

# if OPENSSL_VERSION_NUMBER >= 0x10000001L
    if (HMAC_Init_ex(hmac_ctx, k->hmac_key, 32, md, NULL) != 1) {
      pr_trace_msg(trace_channel, 3,
        "unable to initialize session ticket key HMAC: %s", tls_get_errors());
      return -1;
    }
# else
    HMAC_Init_ex(hmac_ctx, k->hmac_key, 32, md, NULL);
# endif /* OpenSSL-1.0.0 and later */

    memcpy(key_name, k->key_name, 16);
    return 1;
  }

  if (mode == 0) {
    struct tls_ticket_key *newest_key;
    time_t key_age, now;

    key_name_str = pr_str_bin2hex(session.pool, key_name, 16,
      PR_STR_FL_HEX_USE_LC);

    k = get_ticket_key(key_name, 16);
    if (k == NULL) {
      /* No matching key found. */
      pr_trace_msg(trace_channel, 3,
        "TLS session ticket: decrypting ticket using key name '%s': "
        "key not found", key_name_str);
      return 0;
    }

    pr_trace_msg(trace_channel, 3,
      "TLS session ticket: decrypting ticket using key name '%s'",
      key_name_str);

# if OPENSSL_VERSION_NUMBER >= 0x10000001L
    if (HMAC_Init_ex(hmac_ctx, k->hmac_key, 32, md, NULL) != 1) {
      pr_trace_msg(trace_channel, 3,
        "unable to initialize session ticket key HMAC: %s", tls_get_errors());
      return 0;
    }
# else
    HMAC_Init_ex(hmac_ctx, k->hmac_key, 32, md, NULL);
# endif /* OpenSSL-1.0.0 and later */

    if (EVP_DecryptInit_ex(cipher_ctx, cipher, NULL, k->cipher_key, iv) != 1) {
      pr_trace_msg(trace_channel, 3,
        "unable to initialize session ticket key cipher: %s", tls_get_errors());
      return 0;
    }

    /* If the key we found is older than the newest key, tell the client to
     * get a new ticket.  This helps to reduce the window of time a given
     * ticket key is used.
     */
    time(&now);
    key_age = now - k->created;

    newest_key = (struct tls_ticket_key *) tls_ticket_keys->xas_list;
    if (k != newest_key) {
      time_t newest_age;

      newest_age = now - newest_key->created;

      pr_trace_msg(trace_channel, 3,
        "key '%s' age (%lu %s) older than newest key (%lu %s), requesting "
        "ticket renewal", key_name_str, (unsigned long) key_age,
        key_age != 1 ? "secs" : "sec", (unsigned long) newest_age,
        newest_age != 1 ? "secs" : "sec");
    }

# if defined(TLS1_3_VERSION)
    /* If we're a TLSv1.3 session, aim for single-use tickets, and indicate
     * that the client should get a new ticket -- but only for control
     * connection sessions, not data transfer (see Issue #1931).  Will the
     * FTPS client Do The Right Thing(tm), and use this new ticket for future
     * data transfers?
     */
    if (SSL_version(ssl) == TLS1_3_VERSION) {
      return 2;
    }
# endif /* TLS1_3_VERSION */

    return 1;
  }

  pr_trace_msg(trace_channel, 3, "TLS session ticket: unknown mode (%d)", mode);
  return -1;
}
#endif /* TLS_USE_SESSION_TICKETS */

#if defined(PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK)
static int tls_generate_session_ticket_cb(SSL *ssl, void *user_data) {
  SSL_SESSION *ssl_session;

  ssl_session = SSL_get_session(ssl);

  if (SSL_SESSION_set1_ticket_appdata(ssl_session,
      tls_ctrl_ticket_appdata, tls_ctrl_ticket_appdata_len) != 1) {
    tls_log("error setting ticket appdata for ticket: %s",
      tls_get_errors());

  } else {
    if (pr_trace_get_level(trace_channel) >= 19) {
      register unsigned int i;
      const unsigned char *ticket_appdata;
      BIO *bio;
      char *text = NULL;
      long text_len = 0;

      bio = BIO_new(BIO_s_mem());
      BIO_printf(bio, "set %lu bytes of ticket appdata (",
        (unsigned long) tls_ctrl_ticket_appdata_len);
      ticket_appdata = tls_ctrl_ticket_appdata;
      for (i = 0; i < tls_ctrl_ticket_appdata_len; i++) {
        BIO_printf(bio, "%02x", ticket_appdata[i]);
      }
      BIO_printf(bio, ") for %s session ticket", SSL_get_version(ssl));

      text_len = BIO_get_mem_data(bio, &text);
      if (text != NULL) {
        text[text_len] = '\0';
        pr_trace_msg(trace_channel, 19, "%.*s", (int) text_len, text);
      }

      BIO_free(bio);

    } else {
      pr_trace_msg(trace_channel, 9,
        "set %lu bytes of ticket appdata for %s session ticket",
        (unsigned long) tls_ctrl_ticket_appdata_len, SSL_get_version(ssl));
    }
  }

  return 1;
}

static void get_session_ticket_appdata(SSL *ssl, SSL_SESSION *ssl_session) {
  void *appdata = NULL;
  size_t appdata_len = 0;

  if (SSL_SESSION_get0_ticket_appdata(ssl_session, &appdata,
      &appdata_len) != 1) {
    tls_log("error obtaining ticket appdata from data transfer ticket: %s",
      tls_get_errors());
    tls_data_ticket_appdata_len = 0;

    return;
  }

  /* Make sure the ticket appdata length is what we expect; we might have
   * received a ticket with different appdata that what we want.
   */
  if (appdata_len != tls_data_ticket_appdatasz) {
    tls_log("received %s session ticket with unexpected appdata "
      "(expected %lu bytes, got %lu), ignoring", SSL_get_version(ssl),
      (unsigned long) tls_data_ticket_appdatasz, (unsigned long) appdata_len);
    tls_data_ticket_appdata_len = 0;

    return;
  }

  /* Note that we need to copy the appdata into our own buffers for later
   * comparisons; the OpenSSL docs do not do a good job of describing the
   * lifetime of the pointer/buffer returned by
   * SSL_SESSION_get0_ticket_appdata.
   */
  tls_data_ticket_appdata_len = appdata_len;
  memcpy((void *) tls_data_ticket_appdata, appdata, appdata_len);

  if (pr_trace_get_level(trace_channel) >= 19) {
    register unsigned int i;
    const unsigned char *ticket_appdata;
    BIO *bio;
    char *text = NULL;
    long text_len = 0;

    bio = BIO_new(BIO_s_mem());
    BIO_printf(bio, "obtained %lu bytes of ticket appdata (",
      (unsigned long) tls_data_ticket_appdata_len);
    ticket_appdata = tls_data_ticket_appdata;
    for (i = 0; i < tls_data_ticket_appdata_len; i++) {
      BIO_printf(bio, "%02x", ticket_appdata[i]);
    }
    BIO_printf(bio, ") from %s session ticket", SSL_get_version(ssl));

    text_len = BIO_get_mem_data(bio, &text);
    if (text != NULL) {
      text[text_len] = '\0';
      pr_trace_msg(trace_channel, 19, "%.*s", (int) text_len, text);
    }

    BIO_free(bio);

  } else {
    pr_trace_msg(trace_channel, 9,
      "obtained %lu bytes of ticket appdata from %s session ticket",
      (unsigned long) tls_data_ticket_appdata_len, SSL_get_version(ssl));
  }
}

static SSL_TICKET_RETURN tls_decrypt_ctrl_session_ticket_cb(SSL *ssl,
    SSL_SESSION *ssl_session, const unsigned char *key_name, size_t key_namelen,
    SSL_TICKET_STATUS status, void *user_data) {
  SSL_TICKET_RETURN res;
  const char *ssl_proto;

  ssl_proto = SSL_get_version(ssl);

  switch (status) {
    case SSL_TICKET_EMPTY:
    case SSL_TICKET_NO_DECRYPT:
      tls_data_ticket_appdata_len = 0;
      pr_trace_msg(trace_channel, 21,
        "requesting new %s session ticket for control session", ssl_proto);
      res = SSL_TICKET_RETURN_IGNORE_RENEW;
      break;

    case SSL_TICKET_SUCCESS:
      get_session_ticket_appdata(ssl, ssl_session);
      pr_trace_msg(trace_channel, 21,
        "using existing %s session ticket for control session", ssl_proto);
      res = SSL_TICKET_RETURN_USE;
      break;

    case SSL_TICKET_SUCCESS_RENEW:
      get_session_ticket_appdata(ssl, ssl_session);
      pr_trace_msg(trace_channel, 21, "using existing %s session ticket "
        "but requesting renewal for control session", ssl_proto);
      res = SSL_TICKET_RETURN_USE_RENEW;
      break;

    default:
      res = SSL_TICKET_RETURN_IGNORE;
  }

  return res;
}

/* Note that we want to _use_ any provided TLSv1.3 session tickets, so that
 * data transfers can reuse the TLS session from the control connection.  BUT
 * we do not want to _renew_ (or issue) new tickets, as that may tickle the
 * ECONNRESET bug (see Issue #959) for some data transfers.
 */
static SSL_TICKET_RETURN tls_decrypt_data_session_ticket_cb(SSL *ssl,
    SSL_SESSION *ssl_session, const unsigned char *key_name, size_t key_namelen,
    SSL_TICKET_STATUS status, void *user_data) {
  SSL_TICKET_RETURN res;
  const char *ssl_proto;

  ssl_proto = SSL_get_version(ssl);

  switch (status) {
    case SSL_TICKET_SUCCESS:
    case SSL_TICKET_SUCCESS_RENEW:
      get_session_ticket_appdata(ssl, ssl_session);
      pr_trace_msg(trace_channel, 21,
        "using existing %s session ticket for data transfer", ssl_proto);
      res = SSL_TICKET_RETURN_USE;
      break;

    case SSL_TICKET_EMPTY:
    case SSL_TICKET_NO_DECRYPT:
    default:
      tls_data_ticket_appdata_len = 0;
      res = SSL_TICKET_RETURN_IGNORE;
      break;
  }

  return res;
}
#endif /* PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK */

#if defined(PR_USE_OPENSSL_ECC) && \
    OPENSSL_VERSION_NUMBER < 0x10100000L
static EC_KEY *tls_ecdh_cb(SSL *ssl, int is_export, int keylen) {
  static EC_KEY *ecdh = NULL;
  static int init = 0;

  if (init == 0) {
    ecdh = EC_KEY_new();

    if (ecdh != NULL) {
      EC_KEY_set_group(ecdh,
        EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
    }

    init = 1;
  }

  return ecdh;
}
#endif /* PR_USE_OPENSSL_ECC and before OpenSSL-1.1.x */

#if defined(PR_USE_OPENSSL_ALPN)
static int tls_alpn_select_cb(SSL *ssl,
    const unsigned char **selected_proto, unsigned char *selected_protolen,
    const unsigned char *advertised_proto, unsigned int advertised_protolen,
    void *user_data) {
  register unsigned int i;
  struct tls_next_proto *next_proto;
  char *selected_alpn;

  pr_trace_msg(trace_channel, 9, "%s",
    "ALPN protocols advertised by client:");
  for (i = 0; i < advertised_protolen; i++) {
    pr_trace_msg(trace_channel, 9,
      " %.*s", advertised_proto[i], &(advertised_proto[i+1]));
    i += advertised_proto[i] + 1;
  }

  next_proto = user_data;

  if (SSL_select_next_proto(
      (unsigned char **) selected_proto, selected_protolen,
      next_proto->encoded_proto, next_proto->encoded_protolen,
      advertised_proto, advertised_protolen) != OPENSSL_NPN_NEGOTIATED) {
    pr_trace_msg(trace_channel, 9,
      "no common ALPN protocols found (no '%s' in ALPN protocols)",
      next_proto->proto);
    return SSL_TLSEXT_ERR_NOACK;
  }

  selected_alpn = pstrndup(session.pool, (char *) *selected_proto,
    *selected_protolen);
  pr_trace_msg(trace_channel, 9,
    "selected ALPN protocol '%s'", selected_alpn);
  return SSL_TLSEXT_ERR_OK;
}
#endif /* ALPN */

#if defined(PR_USE_OPENSSL_NPN)
static int tls_npn_advertised_cb(SSL *ssl,
    const unsigned char **advertise_proto, unsigned int *advertise_protolen,
    void *user_data) {
  struct tls_next_proto *next_proto;

  next_proto = user_data;

  pr_trace_msg(trace_channel, 9,
    "advertising NPN protocol '%s'", next_proto->proto);
  *advertise_proto = next_proto->encoded_proto;
  *advertise_protolen = next_proto->encoded_protolen;

  return SSL_TLSEXT_ERR_OK;
}
#endif /* NPN */

/* Post 0.9.7a, RSA blinding is turned on by default, so there is no need to
 * do this manually.
 */
#if OPENSSL_VERSION_NUMBER < 0x0090702fL
static void tls_blinding_on(SSL *ssl) {
  EVP_PKEY *pkey = NULL;
  RSA *rsa = NULL;

  /* RSA keys are subject to timing attacks.  To attempt to make such
   * attacks harder, use RSA blinding.
   */

  pkey = SSL_get_privatekey(ssl);

  if (pkey)
    rsa = EVP_PKEY_get1_RSA(pkey);

  if (rsa) {
    if (RSA_blinding_on(rsa, NULL) != 1) {
      tls_log("error setting RSA blinding: %s",
        ERR_error_string(ERR_get_error(), NULL));

    } else {
      tls_log("set RSA blinding on");
    }

    /* Now, "free" the RSA pointer, to properly decrement the reference
     * counter.
     */
    RSA_free(rsa);

  } else {

    /* The administrator may have configured DSA keys rather than RSA keys.
     * In this case, there is nothing to do.
     */
  }

  return;
}
#endif

static int tls_set_fips(void) {
  if (pr_define_exists("TLS_USE_FIPS") != TRUE) {
    return 0;
  }

#ifdef OPENSSL_FIPS
  if (!FIPS_mode()) {
    /* Make sure OpenSSL is set to use the default RNG, as per an email
     * discussion on the OpenSSL developer list:
     *
     *  "The internal FIPS logic uses the default RNG to see the FIPS RNG
     *   as part of the self test process..."
     */
    RAND_set_rand_method(NULL);

    if (!FIPS_mode_set(1)) {
      const char *errstr;

      errstr = tls_get_errors();
      tls_log("unable to use FIPS mode: %s", errstr);
      pr_log_pri(PR_LOG_ERR, MOD_TLS_VERSION
        ": unable to use FIPS mode: %s", errstr);

      errno = EPERM;
      return -1;
    }

    pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION ": FIPS mode enabled");

  } else {
    pr_log_pri(PR_LOG_DEBUG, MOD_TLS_VERSION ": FIPS mode already enabled");
  }
#else
  pr_log_pri(PR_LOG_WARNING, MOD_TLS_VERSION ": FIPS mode requested, but " OPENSSL_VERSION_TEXT " not built with FIPS support");
#endif /* OPENSSL_FIPS */

  return 0;
}

static SSL_CTX *tls_init_ctx(server_rec *s) {
  int ssl_opts = tls_ssl_opts;
  long ssl_mode = 0;
  SSL_CTX *ctx;
#if defined(TLS_USE_SESSION_TICKETS)
  config_rec *c;
#endif /* TLS_USE_SESSION_TICKETS */

  ctx = SSL_CTX_new(SSLv23_server_method());
  if (ctx == NULL) {
    pr_log_debug(DEBUG0, MOD_TLS_VERSION ": error: SSL_CTX_new(): %s",
      tls_get_errors());
    return NULL;
  }

#if OPENSSL_VERSION_NUMBER > 0x000906000L
  /* The SSL_MODE_AUTO_RETRY mode was added in 0.9.6. */
  ssl_mode |= SSL_MODE_AUTO_RETRY;
#endif

#if OPENSSL_VERSION_NUMBER >= 0x1000001fL
  /* The SSL_MODE_RELEASE_BUFFERS mode was added in 1.0.0a. */
  ssl_mode |= SSL_MODE_RELEASE_BUFFERS;
#endif

  if (ssl_mode != 0) {
    SSL_CTX_set_mode(ctx, ssl_mode);
  }

  /* If using OpenSSL-0.9.7 or greater, prevent session resumptions on
   * renegotiations (more secure).
   */
#if OPENSSL_VERSION_NUMBER > 0x000907000L
  ssl_opts |= SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION;
#endif

  /* Disable SSL compression. */
#ifdef SSL_OP_NO_COMPRESSION
  ssl_opts |= SSL_OP_NO_COMPRESSION;
#endif /* SSL_OP_NO_COMPRESSION */

#if defined(PR_USE_OPENSSL_ECC)
# if defined(SSL_OP_SINGLE_ECDH_USE)
  ssl_opts |= SSL_OP_SINGLE_ECDH_USE;
# endif
# if defined(SSL_OP_SAFARI_ECDHE_ECDSA_BUG)
  ssl_opts |= SSL_OP_SAFARI_ECDHE_ECDSA_BUG;
# endif
#endif /* ECC support */

#if defined(SSL_OP_CIPHER_SERVER_PREFERENCE)
  /* Use the server cipher preferences by default. */
  if (tls_use_server_cipher_preference == TRUE) {
    ssl_opts |= SSL_OP_CIPHER_SERVER_PREFERENCE;
  }
#endif /* SSL_OP_CIPHER_SERVER_PREFERENCE */

  SSL_CTX_set_options(ctx, ssl_opts);

#if defined(TLS_USE_SESSION_TICKETS)
  c = find_config(s->conf, CONF_PARAM, "TLSSessionTicketKeys", FALSE);
  if (c != NULL) {
    tls_ticket_key_max_age = *((unsigned int *) c->argv[0]);
    tls_ticket_key_max_count = *((unsigned int *) c->argv[1]);
  }

  /* Generate a random session ticket key, if necessary.  Maybe this list
   * of keys could be stored as ex/app data in the SSL_CTX?
   */
  if (tls_ticket_keys == NULL) {
    struct tls_ticket_key *k;
    unsigned int new_ticket_key_intvl;

    pr_log_debug(DEBUG9, MOD_TLS_VERSION
      ": generating initial TLS session ticket key");

    k = create_ticket_key();
    if (k == NULL) {
      pr_log_debug(DEBUG0, MOD_TLS_VERSION
        ": unable to generate initial session ticket key: %s",
        strerror(errno));

    } else {
      tls_ticket_keys = xaset_create(permanent_pool, tls_ticket_key_cmp);
      add_ticket_key(k);
    }

    /* Also register a timer, to generate new keys every hour (or just under
     * the max age of a key, whichever is smaller).
     */

    new_ticket_key_intvl = 3600;
    if (tls_ticket_key_max_age < new_ticket_key_intvl) {
      /* Try to get a new ticket a little before one expires. */
      new_ticket_key_intvl = tls_ticket_key_max_age - 1;
    }

    pr_log_debug(DEBUG9, MOD_TLS_VERSION
      ": scheduling new TLS session ticket key every %d %s",
      new_ticket_key_intvl, new_ticket_key_intvl != 1 ? "secs" : "sec");

    pr_timer_add(new_ticket_key_intvl, -1, &tls_module, new_ticket_key_timer_cb,
      "New TLS Session Ticket Key");

  } else {
    struct tls_ticket_key *k;

    /* Generate a new key on restart, as part of a good cryptographic
     * hygiene.
     */
    pr_log_debug(DEBUG9, MOD_TLS_VERSION ": generating TLS session ticket key");

    k = create_ticket_key();
    if (k == NULL) {
      pr_log_debug(DEBUG0, MOD_TLS_VERSION
        ": unable to generate new session ticket key: %s", strerror(errno));

    } else {
      add_ticket_key(k);
    }
  }
#endif /* TLS_USE_SESSION_TICKETS */

#if defined(PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK)
  if (SSL_CTX_set_session_ticket_cb(ctx, tls_generate_session_ticket_cb,
      tls_decrypt_ctrl_session_ticket_cb, NULL) != 1) {
    pr_trace_msg(trace_channel, 3,
      "error setting TLSv1.3 session ticket callback: %s", tls_get_errors());
  }
#endif /* PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK */

  SSL_CTX_set_tmp_dh_callback(ctx, tls_dh_cb);

#if defined(PR_USE_OPENSSL_ECC)
  /* If using OpenSSL 1.0.2 or later, let it automatically choose the
   * correct/best curve, rather than having to hardcode a fallback.
   */
# if defined(SSL_CTX_set_ecdh_auto)
  SSL_CTX_set_ecdh_auto(ctx, 1);
# endif
#endif /* PR_USE_OPENSSL_ECC */

  /* We always install an info callback, in order to watch for
   * client-initiated session renegotiations (Bug#3324).  If EnableDiags
   * is enabled, that info callback will also log the OpenSSL diagnostic
   * information.
   */
  SSL_CTX_set_info_callback(ctx, tls_info_cb);

  return ctx;
}

static const char *tls_get_proto_str(pool *p, unsigned int protos,
    unsigned int *count) {
  char *proto_str = "";
  unsigned int nproto = 0;

  if (protos & TLS_PROTO_SSL_V3) {
    proto_str = pstrcat(p, proto_str, *proto_str ? ", " : "",
      "SSLv3", NULL);
    nproto++;
  }

  if (protos & TLS_PROTO_TLS_V1) {
    proto_str = pstrcat(p, proto_str, *proto_str ? ", " : "",
      "TLSv1", NULL);
    nproto++;
  }

  if (protos & TLS_PROTO_TLS_V1_1) {
    proto_str = pstrcat(p, proto_str, *proto_str ? ", " : "",
      "TLSv1.1", NULL);
    nproto++;
  }

  if (protos & TLS_PROTO_TLS_V1_2) {
    proto_str = pstrcat(p, proto_str, *proto_str ? ", " : "",
      "TLSv1.2", NULL);
    nproto++;
  }

  if (protos & TLS_PROTO_TLS_V1_3) {
    proto_str = pstrcat(p, proto_str, *proto_str ? ", " : "",
      "TLSv1.3", NULL);
    nproto++;
  }

  *count = nproto;
  return proto_str;
}

/* Construct the options value that disables all unsupported protocols. */
static int get_disabled_protocols(unsigned int supported_protocols) {
  int disabled_protocols;

  /* First, create an options value where ALL protocols are disabled. */
  disabled_protocols = (SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3|SSL_OP_NO_TLSv1);

#ifdef SSL_OP_NO_TLSv1_1
  disabled_protocols |= SSL_OP_NO_TLSv1_1;
#endif
#ifdef SSL_OP_NO_TLSv1_2
  disabled_protocols |= SSL_OP_NO_TLSv1_2;
#endif
#ifdef SSL_OP_NO_TLSv1_3
  disabled_protocols |= SSL_OP_NO_TLSv1_3;
#endif

  /* Now, based on the given bitset of supported protocols, clear the
   * necessary bits.
   */

  if (supported_protocols & TLS_PROTO_SSL_V3) {
    disabled_protocols &= ~SSL_OP_NO_SSLv3;
  }

  if (supported_protocols & TLS_PROTO_TLS_V1) {
    disabled_protocols &= ~SSL_OP_NO_TLSv1;
  }

#if OPENSSL_VERSION_NUMBER >= 0x10001000L
  if (supported_protocols & TLS_PROTO_TLS_V1_1) {
    disabled_protocols &= ~SSL_OP_NO_TLSv1_1;
  }

  if (supported_protocols & TLS_PROTO_TLS_V1_2) {
    disabled_protocols &= ~SSL_OP_NO_TLSv1_2;
  }
#endif /* OpenSSL-1.0.1 or later */

#ifdef SSL_OP_NO_TLSv1_3
  if (supported_protocols & TLS_PROTO_TLS_V1_3) {
    disabled_protocols &= ~SSL_OP_NO_TLSv1_3;
  }
#endif /* OpenSSL 1.1.1 or later */

  return disabled_protocols;
}

static int tls_get_block(conn_t *conn) {
  int flags;

  flags = fcntl(conn->rfd, F_GETFL);
  if (flags & O_NONBLOCK) {
    return FALSE;
  }

  return TRUE;
}

static int tls_compare_session_ids(SSL_SESSION *ctrl_sess,
    SSL_SESSION *data_sess) {
  int res = -1;

#if OPENSSL_VERSION_NUMBER < 0x000907000L
  /* In the OpenSSL source code, SSL_SESSION_cmp() ultimately uses memcmp(3)
   * to check, and thus returns memcmp(3)'s return value.
   */
  res = SSL_SESSION_cmp(ctrl_sess, data_sess);
#else
  const unsigned char *ctrl_sess_id, *data_sess_id;
  unsigned int ctrl_sess_id_len, data_sess_id_len;

# if OPENSSL_VERSION_NUMBER > 0x000908000L
  ctrl_sess_id = (const unsigned char *) SSL_SESSION_get_id(ctrl_sess,
    &ctrl_sess_id_len);
  data_sess_id = (const unsigned char *) SSL_SESSION_get_id(data_sess,
    &data_sess_id_len);
# else
  /* XXX Directly accessing these fields cannot be a Good Thing. */
  ctrl_sess_id = ctrl_sess->session_id;
  ctrl_sess_id_len = ctrl_sess->session_id_length;
  data_sess_id = data_sess->session_id;
  data_sess_id_len = data_sess->session_id_length;
# endif

  /* We might use SSL_has_matching_session_id, except that it does not
   * appear to be reliable using older versions of OpenSSL (e.g. 1.0.1t in
   * my local testing).  So just compare the raw session IDs ourselves.
   */
  if (ctrl_sess_id_len == data_sess_id_len) {
    res = memcmp(ctrl_sess_id, data_sess_id, ctrl_sess_id_len);
    if (res != 0) {
      res = -1;
    }

  } else {
    res = -1;
  }
#endif

  return res;
}

static unsigned long tls_sess_remaining(SSL_SESSION *ssl_sess) {
  long sess_created, sess_expires;
  time_t now;
  unsigned long sess_remaining = 0;

  sess_created = SSL_SESSION_get_time(ssl_sess);
  sess_expires = SSL_SESSION_get_timeout(ssl_sess);
  now = time(NULL);

  if ((sess_created + sess_expires) >= now) {
    sess_remaining = (unsigned long) ((sess_created + sess_expires) - now);
  }

  return sess_remaining;
}

static int tls_accept(conn_t *conn, unsigned char on_data) {
  static unsigned char logged_data = FALSE;
  int blocking, res = 0, xerrno = 0;
  long cache_mode = 0;
  char *subj = NULL;
  SSL *ssl = NULL;
  BIO *rbio = NULL, *wbio = NULL;

  if (ssl_ctx == NULL) {
    tls_log("%s", "unable to start session: null SSL_CTX");
    return -1;
  }

  ssl = SSL_new(ssl_ctx);
  if (ssl == NULL) {
    tls_log("error: unable to start session: %s",
      ERR_error_string(ERR_get_error(), NULL));
    return -2;
  }

  /* This works with either rfd or wfd (I hope). */
  rbio = BIO_new_socket(conn->rfd, FALSE);
  wbio = BIO_new_socket(conn->wfd, FALSE);

  SSL_set_bio(ssl, rbio, wbio);

#if !defined(OPENSSL_NO_TLSEXT)
  if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
    /* Note that older OpenSSL versions, e.g. 0.9.8, do not implement this
     * callback.  Newer versions which DO implement it do as via a macro.
     */
# ifdef SSL_set_tlsext_debug_callback
    SSL_set_tlsext_debug_callback(ssl, tls_tlsext_cb);
# endif /* SSL_set_tlsext_debug_callback */
  }
#endif /* !OPENSSL_NO_TLSEXT */

  /* If configured, set a timer for the handshake. */
  if (tls_handshake_timeout) {
    tls_handshake_timed_out = FALSE;
    tls_handshake_timer_id = pr_timer_add(tls_handshake_timeout, -1,
      &tls_module, tls_handshake_timeout_cb, "SSL/TLS handshake");
  }

  if (on_data == TRUE) {
    /* Make sure that TCP_NODELAY is enabled for the handshake. */
    if (pr_inet_set_proto_nodelay(conn->pool, conn, 1) < 0) {
      pr_trace_msg(trace_channel, 9,
        "error enabling TCP_NODELAY on data conn: %s", strerror(errno));
    }

    /* Make sure that TCP_CORK (aka TCP_NOPUSH) is DISABLED for the handshake.
     * This socket option is set via the pr_inet_set_proto_opts() call made
     * in mod_core, upon handling the PASV/EPSV command.
     */
    if (pr_inet_set_proto_cork(conn->wfd, 0) < 0) {
      pr_trace_msg(trace_channel, 9,
        "error disabling TCP_CORK on data conn: %s", strerror(errno));
    }

    cache_mode = SSL_CTX_get_session_cache_mode(ssl_ctx);
    if (cache_mode != SSL_SESS_CACHE_OFF) {
      /* Disable STORING of any new session IDs in the session cache. We DO
       * want to allow LOOKUP of session IDs in the session cache, however.
       */
      long data_cache_mode;
      data_cache_mode = SSL_SESS_CACHE_SERVER|SSL_SESS_CACHE_NO_INTERNAL_STORE;
      SSL_CTX_set_session_cache_mode(ssl_ctx, data_cache_mode);
    }

#if !defined(OPENSSL_NO_TLSEXT) && defined(TLSEXT_MAXLEN_host_name)
    /* Disable any SNI handling for data TLS sessions. */
    pr_trace_msg(trace_channel, 5, "ignoring SNI for data connections");
    SSL_CTX_set_tlsext_servername_callback(ssl_ctx, NULL);
    SSL_CTX_set_tlsext_servername_arg(ssl_ctx, NULL);
#endif /* !OPENSSL_NO_TLSEXT */

#if defined(PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK)
    tls_data_ticket_appdata_len = 0;

    if (session.curr_cmd_id == PR_CMD_APPE_ID ||
        session.curr_cmd_id == PR_CMD_LIST_ID ||
        session.curr_cmd_id == PR_CMD_MLSD_ID ||
        session.curr_cmd_id == PR_CMD_NLST_ID ||
        session.curr_cmd_id == PR_CMD_RETR_ID ||
        session.curr_cmd_id == PR_CMD_STOR_ID ||
        session.curr_cmd_id == PR_CMD_STOU_ID) {

      /* Some versions of SSL_do_handshake have a bug in how they handle the
       * TLS 1.3 handshake on the server side: after the handshake finishes,
       * they automatically send session tickets, even though the client may
       * not be expecting data to arrive at this point and sending it could
       * cause a deadlock or lost data. This applies at least to OpenSSL 1.1.1c
       * and earlier, and the OpenSSL devs currently have no plans to fix it:
       *
       *  https://github.com/openssl/openssl/issues/7948
       *  https://github.com/openssl/openssl/issues/7967
       *
       * The correct behavior is to wait to send session tickets on the
       * first call to SSL_write. (This is what BoringSSL does.) So, we
       * programmatically disable sending/renewing TLSv1.3 session tickets
       * for the TLS session on data transfers, specifically uploads
       * (Issue #959).
       *
       * Why is this needed for FTPS uploads only, and not downloads or
       * directory listings?  An FTPS upload is the only case where we
       * are a) the server, and b) in a read-only role.  Thus we are not
       * expected to _send_ any TCP data, including session tickets.
       */
      if (SSL_CTX_set_session_ticket_cb(ssl_ctx, tls_generate_session_ticket_cb,
          tls_decrypt_data_session_ticket_cb, NULL) != 1) {
        pr_trace_msg(trace_channel, 3, "error setting TLSv1.3 session ticket "
          "callback for '%s' data transfer: %s", session.curr_cmd,
          tls_get_errors());
      }

    } else {
      /* Restore default session ticket callbacks for any other transfer. */
      if (SSL_CTX_set_session_ticket_cb(ssl_ctx, tls_generate_session_ticket_cb,
          tls_decrypt_ctrl_session_ticket_cb, NULL) != 1) {
        pr_trace_msg(trace_channel, 3, "error setting TLSv1.3 session ticket "
          "callback for '%s': %s", session.curr_cmd,
          tls_get_errors());
      }
    }
#endif /* PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK */
  }

  retry:

  blocking = tls_get_block(conn);
  if (blocking) {
    /* Put the connection in non-blocking mode for the duration of the
     * TLS handshake.  This lets us handle EAGAIN/retries better (i.e.
     * without spinning in a tight loop and consuming the CPU).
     */
    if (pr_inet_set_nonblock(conn->pool, conn) < 0) {
      pr_trace_msg(trace_channel, 3,
        "error making %s connection nonblocking: %s",
        on_data ? "data" : "ctrl", strerror(errno));
    }
  }

  pr_signals_handle();

  pr_trace_msg(trace_channel, 17, "calling SSL_accept() on %s conn fd %d",
    on_data ? "data" : "ctrl", conn->rfd);
  res = SSL_accept(ssl);
  xerrno = errno;

  pr_trace_msg(trace_channel, 17, "SSL_accept() returned %d for %s conn fd %d",
    res, on_data ? "data" : "ctrl", conn->rfd);

  if (blocking) {
    /* Return the connection to blocking mode. */
    if (pr_inet_set_block(conn->pool, conn) < 0) {
      pr_trace_msg(trace_channel, 3,
        "error making %s connection blocking: %s",
        on_data ? "data" : "ctrl", strerror(errno));
    }
  }

  if (res < 1) {
    const char *msg = "unable to accept TLS connection";
    int errcode;

    errcode = SSL_get_error(ssl, res);
    pr_signals_handle();

    if (tls_handshake_timed_out) {
      tls_log("TLS negotiation timed out (%u seconds)", tls_handshake_timeout);
      tls_end_sess(ssl, on_data ? session.d : session.c, 0);
      return -4;
    }

    switch (errcode) {
      case SSL_ERROR_WANT_READ:
        pr_trace_msg(trace_channel, 17,
          "WANT_READ encountered while accepting %s conn on fd %d, "
          "waiting to read data", on_data ? "data" : "ctrl", conn->rfd);
        tls_readmore(conn->rfd);
        goto retry;

      case SSL_ERROR_WANT_WRITE:
        pr_trace_msg(trace_channel, 17,
          "WANT_WRITE encountered while accepting %s conn on fd %d, "
          "waiting to send data", on_data ? "data" : "ctrl", conn->rfd);
        tls_writemore(conn->rfd);
        goto retry;

      case SSL_ERROR_ZERO_RETURN:
        tls_log("%s: TLS connection closed", msg);
        break;

      case SSL_ERROR_WANT_X509_LOOKUP:
        tls_log("%s: needs X509 lookup", msg);
        break;

      case SSL_ERROR_SYSCALL: {
        int xerrcode;

        /* Check to see if the OpenSSL error queue has info about this. */
        xerrcode = ERR_peek_error();

        if (xerrcode == 0) {
          /* The OpenSSL error queue doesn't have any more info, so we'll
           * examine the SSL_accept() return value itself.
           */

          if (res == 0) {
            /* EOF */
            tls_log("%s: received EOF that violates protocol", msg);
            tls_log("%s: usually this indicates an FTP-aware router, NAT, or "
              "firewall interfering with the TLS handshake", msg);

          } else if (res == -1) {
            /* Check errno */
            tls_log("%s: system call error: [%d] %s", msg, xerrno,
              strerror(xerrno));
          }

        } else {
          tls_log("%s: system call error: %s", msg, tls_get_errors());
        }

        break;
      }

      case SSL_ERROR_SSL: {
        pool *tmp_pool;
        unsigned long ssl_errcode = ERR_peek_error();

        tls_log("%s: protocol error: %s", msg, tls_get_errors());

        tmp_pool = make_sub_pool(conn->pool);

        /* The error codes in the OpenSSL error queue are "packed"; we need
         * to unpack them to get the reason value.
         *
         * Try to provide more context for the most commonly ocurring/reported
         * handshake errors here.
         */

        switch (ERR_GET_REASON(ssl_errcode)) {
          case SSL_R_UNKNOWN_PROTOCOL: {
            long ssl_opts;
            char *proto_str = "";

            ssl_opts = SSL_get_options(ssl);

#if SSL_OP_NO_SSLv2
            if (ssl_opts & SSL_OP_NO_SSLv2) {
              proto_str = pstrcat(tmp_pool, proto_str, *proto_str ? ", " : "",
                "SSLv2", NULL);
            }
#endif /* SSLv2 */

            if (ssl_opts & SSL_OP_NO_SSLv3) {
              proto_str = pstrcat(tmp_pool, proto_str, *proto_str ? ", " : "",
                "SSLv3", NULL);
            }

            if (ssl_opts & SSL_OP_NO_TLSv1) {
              proto_str = pstrcat(tmp_pool, proto_str, *proto_str ? ", " : "",
                "TLSv1", NULL);
            }

#ifdef SSL_OP_NO_TLSv1_1
            if (ssl_opts & SSL_OP_NO_TLSv1_1) {
              proto_str = pstrcat(tmp_pool, proto_str, *proto_str ? ", " : "",
                "TLSv1.1", NULL);
            }
#endif /* TLSv1.1 */

#ifdef SSL_OP_NO_TLSv1_2
            if (ssl_opts & SSL_OP_NO_TLSv1_2) {
              proto_str = pstrcat(tmp_pool, proto_str, *proto_str ? ", " : "",
                "TLSv1.2", NULL);
            }
#endif /* TLSv1.2 */

#ifdef SSL_OP_NO_TLSv1_3
            if (ssl_opts & SSL_OP_NO_TLSv1_3) {
              proto_str = pstrcat(tmp_pool, proto_str, *proto_str ? ", " : "",
                "TLSv1.3", NULL);
            }
#endif /* TLSv1.3 */

            tls_log("%s: perhaps client requested disabled TLS protocol "
              "version: %s", msg, proto_str);
            break;
          }

          case SSL_R_NO_SHARED_CIPHER: {
# if OPENSSL_VERSION_NUMBER >= 0x10101000L && \
     defined(TLS1_3_VERSION)
            if (tlsv13_cipher_suite != NULL) {
              tls_log("%s: client does not support any cipher from "
                "'TLSCipherSuite %s' or 'TLSCipherSuite TLSv1.3 %s' "
                "(see `openssl ciphers` for full list)",
                msg, tls_cipher_suite, tlsv13_cipher_suite);

            } else {
              tls_log("%s: client does not support any cipher from "
                "'TLSCipherSuite %s' (see `openssl ciphers %s` for full list)",
                msg, tls_cipher_suite, tls_cipher_suite);
            }
# else
            tls_log("%s: client does not support any cipher from "
              "'TLSCipherSuite %s' (see `openssl ciphers %s` for full list)",
              msg, tls_cipher_suite, tls_cipher_suite);
# endif /* TLS1_3_VERSION */

            break;
          }

          case SSL_R_PEER_DID_NOT_RETURN_A_CERTIFICATE: {
            if (tls_flags & TLS_SESS_VERIFY_CLIENT_REQUIRED) {
              tls_log("%s: client did not provide certificate, but one is "
                "required via 'TLSVerifyClient on'", msg);
            }
            break;
          }

#if defined(SSL_R_VERSION_TOO_LOW) && !defined(HAVE_LIBRESSL)
          case SSL_R_VERSION_TOO_LOW: {
            int client_version;

            client_version = SSL_client_version(ssl);
            switch (client_version) {
# if defined(SSL3_VERSION) && defined(OPENSSL_NO_SSL3)
              case SSL3_VERSION:
                tls_log("%s: %s lacks support for client requested TLS "
                  "protocol version: %s", msg, OPENSSL_VERSION_TEXT,
                  SSL_get_version(ssl));
                break;
# endif /* SSLv3 and OPENSSL_NO_SSL3 */

# if defined(TLS1_VERSION) && defined(OPENSSL_NO_TLS1)
              case TLS1_VERSION:
                tls_log("%s: %s lacks support for client requested TLS "
                  "protocol version: %s", msg, OPENSSL_VERSION_TEXT,
                  SSL_get_version(ssl));
                break;
# endif /* TLSv1 and OPENSSL_NO_TLS1 */

# if defined(TLS1_1_VERSION) && defined(OPENSSL_NO_TLS1_1)
              case TLS1_1_VERSION:
                tls_log("%s: %s lacks support for client requested TLS "
                  "protocol version: %s", msg, OPENSSL_VERSION_TEXT,
                  SSL_get_version(ssl));
                break;
# endif /* TLSv1.1 and OPENSSL_NO_TLS1_1 */

# if defined(TLS1_2_VERSION) && defined(OPENSSL_NO_TLS1_2)
              case TLS1_2_VERSION:
                tls_log("%s: %s lacks support for client requested TLS "
                  "protocol version: %s", msg, OPENSSL_VERSION_TEXT,
                  SSL_get_version(ssl));
                break;
# endif /* TLSv1.2 and OPENSSL_NO_TLS1_2 */

# if defined(TLS1_3_VERSION) && defined(OPENSSL_NO_TLS1_3)
              case TLS1_3_VERSION:
                tls_log("%s: %s lacks support for client requested TLS "
                  "protocol version: %s", msg, OPENSSL_VERSION_TEXT,
                  SSL_get_version(ssl));
                break;
# endif /* TLSv1.3 and OPENSSL_NO_TLS1_3 */

              default:
                tls_log("%s: perhaps client requested unsupported TLS protocol "
                  "version: %s", msg, SSL_get_version(ssl));
            }
            break;
          }
#endif /* SSL_R_VERSION_TOO_LOW */

          default:
            break;
        }

        destroy_pool(tmp_pool);
        break;
      }
    }

    if (on_data == TRUE) {
      pr_event_generate("mod_tls.data-handshake-failed", &errcode);

    } else {
      pr_event_generate("mod_tls.ctrl-handshake-failed", &errcode);
    }

    tls_end_sess(ssl, on_data ? session.d : session.c, 0);
    return -3;
  }

  pr_trace_msg(trace_channel, 17,
    "TLS handshake on %s conn fd %d COMPLETED", on_data ? "data" : "ctrl",
    conn->rfd);

  if (on_data == TRUE) {
    if (conn->use_nodelay == FALSE) {
      /* Disable TCP_NODELAY, now that the handshake is done. */
      if (pr_inet_set_proto_nodelay(conn->pool, conn, 0) < 0) {
        pr_trace_msg(trace_channel, 9,
          "error disabling TCP_NODELAY on data conn: %s", strerror(errno));
      }
    }

    /* Re-enable TCP_CORK (aka TCP_NOPUSH), now that the handshake is done. */
    if (pr_inet_set_proto_cork(conn->wfd, 1) < 0) {
      pr_trace_msg(trace_channel, 9,
        "error re-enabling TCP_CORK on data conn: %s", strerror(errno));
    }

    if (cache_mode != SSL_SESS_CACHE_OFF) {
      /* Restore the previous session cache mode. */
      SSL_CTX_set_session_cache_mode(ssl_ctx, cache_mode);
    }
  }

  /* Disable the handshake timer. */
  pr_timer_remove(tls_handshake_timer_id, &tls_module);

#if defined(PR_USE_OPENSSL_NPN)
  /* Which NPN protocol was selected, if any? */
  {
    const unsigned char *npn = NULL;
    unsigned int npn_len = 0;

    SSL_get0_next_proto_negotiated(ssl, &npn, &npn_len);
    if (npn != NULL &&
        npn_len > 0) {
      pr_trace_msg(trace_channel, 9,
        "negotiated NPN '%.*s'", npn_len, npn);

    } else {
      pr_trace_msg(trace_channel, 9, "%s", "no NPN negotiated");
    }
  }
#endif /* NPN */

#if defined(PR_USE_OPENSSL_ALPN)
  /* Which ALPN protocol was selected, if any? */
  {
    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;

    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn != NULL &&
        alpn_len > 0) {
      pr_trace_msg(trace_channel, 9,
        "selected ALPN '%.*s'", alpn_len, alpn);
    } else {
      pr_trace_msg(trace_channel, 9, "%s", "no ALPN selected");
    }
  }
#endif /* ALPN */

  /* Manually update the raw bytes counters with the network IO from the
   * TLS handshake.
   */
  session.total_raw_in += (BIO_number_read(rbio) +
    BIO_number_read(wbio));
  session.total_raw_out += (BIO_number_written(rbio) +
    BIO_number_written(wbio));

  /* Stash the SSL object in the pointers of the correct NetIO streams. */
  if (conn == session.c) {
    pr_buffer_t *strm_buf;

    ctrl_ssl = ssl;

    if (pr_table_add(tls_ctrl_rd_nstrm->notes,
        pstrdup(tls_ctrl_rd_nstrm->strm_pool, TLS_NETIO_NOTE),
        ssl, sizeof(SSL *)) < 0) {
      if (errno != EEXIST) {
        tls_log("error stashing '%s' note on ctrl read stream: %s",
          TLS_NETIO_NOTE, strerror(errno));
      }
    }

    if (pr_table_add(tls_ctrl_wr_nstrm->notes,
        pstrdup(tls_ctrl_wr_nstrm->strm_pool, TLS_NETIO_NOTE),
        ssl, sizeof(SSL *)) < 0) {
      if (errno != EEXIST) {
        tls_log("error stashing '%s' note on ctrl write stream: %s",
          TLS_NETIO_NOTE, strerror(errno));
      }
    }

#if OPENSSL_VERSION_NUMBER >= 0x009080dfL
    if (SSL_get_secure_renegotiation_support(ssl) == 1) {
      /* If the peer indicates that it can support secure renegotiations, log
       * this fact for reference.
       *
       * Note that while we could use this fact to automatically enable the
       * AllowClientRenegotiations TLSOption, in light of CVE-2011-1473
       * (where malicious SSL/TLS clients can abuse the fact that session
       * renegotiations are more computationally intensive for servers than
       * for clients and repeatedly request renegotiations to create a
       * denial of service attack), we won't enable AllowClientRenegotiations
       * programmatically.  The admin will still need to explicitly configure
       * that.
       */
      tls_log("%s", "client supports secure renegotiations");
    }
#endif /* OpenSSL 0.9.8m and later */

    /* Clear any data from the NetIO stream buffers which may have been read
     * in before the SSL/TLS handshake occurred (Bug#3624).
     */
    strm_buf = tls_ctrl_rd_nstrm->strm_buf;
    if (strm_buf != NULL) {
      strm_buf->current = NULL;
      strm_buf->remaining = strm_buf->buflen;
    }

  } else if (conn == session.d) {
    pr_buffer_t *strm_buf;

    if (pr_table_add(tls_data_rd_nstrm->notes,
        pstrdup(tls_data_rd_nstrm->strm_pool, TLS_NETIO_NOTE),
        ssl, sizeof(SSL *)) < 0) {
      if (errno != EEXIST) {
        tls_log("error stashing '%s' note on data read stream: %s",
          TLS_NETIO_NOTE, strerror(errno));
      }
    }

    if (pr_table_add(tls_data_wr_nstrm->notes,
        pstrdup(tls_data_wr_nstrm->strm_pool, TLS_NETIO_NOTE),
        ssl, sizeof(SSL *)) < 0) {
      if (errno != EEXIST) {
        tls_log("error stashing '%s' note on data write stream: %s",
          TLS_NETIO_NOTE, strerror(errno));
      }
    }

    /* Clear any data from the NetIO stream buffers which may have been read
     * in before the SSL/TLS handshake occurred (Bug#3624).
     */
    strm_buf = tls_data_rd_nstrm->strm_buf;
    if (strm_buf != NULL) {
      strm_buf->current = NULL;
      strm_buf->remaining = strm_buf->buflen;
    }
  }

#if OPENSSL_VERSION_NUMBER == 0x009080cfL
  /* In OpenSSL-0.9.8l, SSL session renegotiations are automatically
   * disabled.  Thus if the admin explicitly configured support for
   * client-initiated renegotiations via the AllowClientRenegotiations
   * TLSOption, then we need to do some hackery to enable renegotiations.
   */
  if (tls_opts & TLS_OPT_ALLOW_CLIENT_RENEGOTIATIONS) {
    ssl->s3->flags |= SSL3_FLAGS_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
  }
#endif

  /* TLS handshake on the control channel... */
  if (on_data == FALSE) {
    int reused;

    subj = tls_get_subj_name(ctrl_ssl);
    if (subj != NULL) {
      tls_log("Client: %s", subj);
    }

    if (tls_flags & TLS_SESS_VERIFY_CLIENT_REQUIRED) {
      /* Now we can go on with our post-handshake, application level
       * requirement checks.
       */
      if (tls_check_client_cert(ssl, conn) < 0) {
        tls_end_sess(ssl, session.c, 0);
        ctrl_ssl = NULL;
        return -1;
      }
    }

    reused = SSL_session_reused(ssl);

    tls_log("%s connection accepted, using cipher %s (%d bits%s)",
      SSL_get_version(ssl), SSL_get_cipher_name(ssl),
      SSL_get_cipher_bits(ssl, NULL),
      reused > 0 ? ", resumed session" : "");

    /* Setup the TLS environment variables and notes. */
    tls_setup_environ(session.pool, ssl);
    tls_setup_notes(session.pool, ssl);

    if (reused > 0) {
      pr_log_writefile(tls_logfd, MOD_TLS_VERSION, "%s",
        "client reused previous TLS session for control connection");
    }

  /* TLS handshake on the data channel... */
  } else {

    /* We won't check for session reuse for data connections when either
     * a) the NoSessionReuseRequired TLSOption has been configured, or
     * b) the CCC command has been used (Bug#3465).
     */
    if (!(tls_opts & TLS_OPT_NO_SESSION_REUSE_REQUIRED) &&
        !(tls_flags & TLS_SESS_HAVE_CCC)) {
      int reused;
      SSL_SESSION *ctrl_sess;

      /* Ensure that the following conditions are met:
       *
       *   1. The client reused an existing TLS session
       *   2. The reused TLS session matches the TLS session from the control
       *      connection.
       *
       * Make sure we handle session tickets, too, especially for TLSv1.3.
       *
       * Shutdown the TLS session unless the conditions are met.  By
       * requiring these conditions, we make sure that the client which is
       * talking to us on the control connection is indeed the same client
       * that is using this data connection.  Without these checks, a
       * malicious client might be able to hijack/steal the data transfer.
       */

      reused = SSL_session_reused(ssl);
      if (reused != 1) {
        tls_log("%s", "client did not reuse TLS session, rejecting data "
          "connection (see the NoSessionReuseRequired TLSOptions parameter)");
        tls_end_sess(ssl, session.d, 0);
        pr_table_remove(tls_data_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
        pr_table_remove(tls_data_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
        return -1;
      }

      tls_log("%s", "client reused TLS session for data connection");

      ctrl_sess = SSL_get_session(ctrl_ssl);
      if (ctrl_sess != NULL) {
        SSL_SESSION *data_sess;

        data_sess = SSL_get_session(ssl);
        if (data_sess != NULL) {
          int matching_sess = -1;
          unsigned long sess_remaining;

          matching_sess = tls_compare_session_ids(ctrl_sess, data_sess);

#ifdef TLS1_3_VERSION
          /* TLSv1.3 sessions will show session reuse, but NOT matching session
           * IDs.  Why?  TLSv1.3 completely redid session resumption, and now
           * favors session tickets (and stateless servers) rather than
           * session IDs (and stateful server session caches).
           *
           * To prove session reuse via TLSv1.3 session tickets, then, we use
           * ticket appdata to generate a random "FTP session ID" that we stick
           * in the control session tickets; the data session tickets presented
           * should have the exact same appdata, if they are coming from our
           * expected control session.
           *
           * However, we cannot just assume that session tickets will be used
           * only for TLSv1.3 sessions.  It is possible that session tickets,
           * rather than session IDs, will be used for TLSv1.2 and earlier
           * sessions as well.
           */
          if (matching_sess != 0) {

# if defined(PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK)
            if (tls_ctrl_ticket_appdata_len > 0 &&
                tls_data_ticket_appdata_len > 0 &&
                tls_ctrl_ticket_appdata_len == tls_data_ticket_appdata_len) {
              if (pr_trace_get_level(trace_channel) >= 19) {
                register unsigned int i;
                const unsigned char *ticket_appdata;
                BIO *bio;
                char *text = NULL;
                long text_len = 0;

                bio = BIO_new(BIO_s_mem());
                BIO_puts(bio, "comparing control ticket appdata (");
                ticket_appdata = tls_ctrl_ticket_appdata;
                for (i = 0; i < tls_ctrl_ticket_appdata_len; i++) {
                  BIO_printf(bio, "%02x", ticket_appdata[i]);
                }

                BIO_puts(bio, ") and data ticket appdata (");
                ticket_appdata = tls_data_ticket_appdata;
                for (i = 0; i < tls_data_ticket_appdata_len; i++) {
                  BIO_printf(bio, "%02x", ticket_appdata[i]);
                }
                BIO_puts(bio, ")");

                text_len = BIO_get_mem_data(bio, &text);
                if (text != NULL) {
                  text[text_len] = '\0';
                  pr_trace_msg(trace_channel, 19, "%.*s", (int) text_len, text);
                }

                BIO_free(bio);
              }

              matching_sess = memcmp(tls_ctrl_ticket_appdata,
                tls_data_ticket_appdata, tls_ctrl_ticket_appdata_len);
              if (matching_sess != 0) {
                pr_trace_msg(trace_channel, 9,
                  "mismatched control/data ticket appdata for %s sessions",
                  SSL_get_version(ssl));
              }

            } else {
              pr_trace_msg(trace_channel, 9,
                "mismatched control/data ticket appdata (ctrl = %lu bytes, "
                "data = %lu bytes) for %s sessions",
                (unsigned long) tls_ctrl_ticket_appdata_len,
                (unsigned long) tls_data_ticket_appdata_len,
                SSL_get_version(ssl));
            }
# else
            pr_trace_msg(trace_channel, 9,
             "ignoring mismatched control/data session IDs for %s sessions, "
             "for now", SSL_get_version(ssl));
            matching_sess = 0;
# endif /* PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK */
          }
#endif /* TLS1_3_VERSION */

          if (matching_sess != 0) {
            tls_log("client did not reuse TLS session from control channel, "
              "rejecting data connection (see the NoSessionReuseRequired "
              "TLSOptions parameter)");
            tls_end_sess(ssl, session.d, 0);
            pr_table_remove(tls_data_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
            pr_table_remove(tls_data_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
            return -1;
          }

          /* The TLS session ID for data and control channels matches.
           *
           * Many sites are using mod_tls such that OpenSSL's internal
           * session caching is being used.  And by default, that
           * cache expires sessions after 300 secs (5 min).  It's possible
           * that the control channel TLS session will expire soon;
           * unless the client renegotiates that session, the internal
           * cache will expire the cached session, and the next data
           * transfer could fail (since mod_tls won't allow that session ID
           * to be reused again, as it will no longer be in the session
           * cache).
           *
           * Try to warn if this is about to happen.
           */

          sess_remaining = tls_sess_remaining(ctrl_sess);
          if (sess_remaining <= 60) {
            long sess_expires;

            sess_expires = SSL_SESSION_get_timeout(ctrl_sess);

            tls_log("control channel TLS session expires in %lu secs "
              "(%lu session cache expiration)", sess_remaining, sess_expires);
            tls_log("%s", "Consider using 'TLSSessionCache internal:' to "
              "increase the session cache expiration if necessary, or "
              "renegotiate the control channel TLS session");
          }

        } else {
          /* This should never happen, so log if it does. */
          tls_log("%s", "BUG: unable to determine whether client reused TLS "
            "session: SSL_get_session() for data connection returned NULL");
          tls_log("%s", "rejecting data connection (see TLSOption NoSessionReuseRequired)");
          tls_end_sess(ssl, session.d, 0);
          pr_table_remove(tls_data_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
          pr_table_remove(tls_data_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
          return -1;
        }

      } else {
        /* This should never happen, so log if it does. */
        tls_log("%s", "BUG: unable to determine whether client reused TLS "
          "session: SSL_get_session() for control connection returned NULL!");
        tls_log("%s", "rejecting data connection (see TLSOption NoSessionReuseRequired)");
        tls_end_sess(ssl, session.d, 0);
        pr_table_remove(tls_data_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
        pr_table_remove(tls_data_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
        return -1;
      }
    }

    /* Only be verbose with the first TLS data connection, otherwise there
     * might be too much noise.
     */
    if (!logged_data) {
      int reused;

      reused = SSL_session_reused(ssl);

      tls_log("%s data connection accepted, using cipher %s (%d bits%s)",
        SSL_get_version(ssl), SSL_get_cipher_name(ssl),
        SSL_get_cipher_bits(ssl, NULL),
        reused > 0 ? ", resumed session" : "");
      logged_data = TRUE;
    }
  }

  return 0;
}

static int tls_connect(conn_t *conn) {
  int blocking, res = 0, xerrno = 0;
  char *subj = NULL;
  SSL *ssl = NULL;
  BIO *rbio = NULL, *wbio = NULL;

  if (ssl_ctx == NULL) {
    tls_log("%s", "unable to start session: null SSL_CTX");
    return -1;
  }

  ssl = SSL_new(ssl_ctx);
  if (ssl == NULL) {
    tls_log("error: unable to start session: %s",
      ERR_error_string(ERR_get_error(), NULL));
    return -2;
  }

  /* Make sure our SSL object uses client methods (Issue#618). */
  if (SSL_set_ssl_method(ssl, SSLv23_client_method()) != 1) {
    tls_log("error: unable to set client methods: %s",
      ERR_error_string(ERR_get_error(), NULL));
    SSL_free(ssl);
    return -2;
  }

  /* We deliberately set SSL_VERIFY_NONE here, so that we get to
   * determine how to handle the server cert verification result ourselves.
   */
  SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);

  /* This works with either rfd or wfd (I hope). */
  rbio = BIO_new_socket(conn->rfd, FALSE);
  wbio = BIO_new_socket(conn->rfd, FALSE);
  SSL_set_bio(ssl, rbio, wbio);

  /* If configured, set a timer for the handshake. */
  if (tls_handshake_timeout) {
    tls_handshake_timed_out = FALSE;
    tls_handshake_timer_id = pr_timer_add(tls_handshake_timeout, -1,
      &tls_module, tls_handshake_timeout_cb, "SSL/TLS handshake");
  }

  /* Make sure that TCP_NODELAY is enabled for the handshake. */
  (void) pr_inet_set_proto_nodelay(conn->pool, conn, 1);

  retry:

  blocking = tls_get_block(conn);
  if (blocking) {
    /* Put the connection in non-blocking mode for the duration of the
     * TLS handshake.  This lets us handle EAGAIN/retries better (i.e.
     * without spinning in a tight loop and consuming the CPU).
     */
    if (pr_inet_set_nonblock(conn->pool, conn) < 0) {
      pr_trace_msg(trace_channel, 3,
        "error making connection nonblocking: %s", strerror(errno));
    }
  }

  pr_signals_handle();
  res = SSL_connect(ssl);
  xerrno = errno;

  if (blocking) {
    /* Return the connection to blocking mode. */
    if (pr_inet_set_block(conn->pool, conn) < 0) {
      pr_trace_msg(trace_channel, 3,
        "error making connection blocking: %s", strerror(errno));
    }
  }

  if (res < 1) {
    const char *msg = "unable to connect using TLS connection";
    int errcode = SSL_get_error(ssl, res);

    pr_signals_handle();

    if (tls_handshake_timed_out) {
      tls_log("TLS negotiation timed out (%u seconds)", tls_handshake_timeout);
      tls_end_sess(ssl, conn, 0);
      return -4;
    }

    switch (errcode) {
      case SSL_ERROR_WANT_READ:
        pr_trace_msg(trace_channel, 17,
          "WANT_READ encountered while connecting on fd %d, "
          "waiting to read data", conn->rfd);
        tls_readmore(conn->rfd);
        goto retry;

      case SSL_ERROR_WANT_WRITE:
        pr_trace_msg(trace_channel, 17,
          "WANT_WRITE encountered while connecting on fd %d, "
          "waiting to read data", conn->rfd);
        tls_writemore(conn->rfd);
        goto retry;

      case SSL_ERROR_ZERO_RETURN:
        tls_log("%s: TLS connection closed", msg);
        break;

      case SSL_ERROR_WANT_X509_LOOKUP:
        tls_log("%s: needs X509 lookup", msg);
        break;

      case SSL_ERROR_SYSCALL: {
        /* Check to see if the OpenSSL error queue has info about this. */
        int xerrcode = ERR_get_error();

        if (xerrcode == 0) {
          /* The OpenSSL error queue doesn't have any more info, so we'll
           * examine the SSL_connect() return value itself.
           */

          if (res == 0) {
            /* EOF */
            tls_log("%s: received EOF that violates protocol", msg);

          } else if (res == -1) {
            /* Check errno */
            tls_log("%s: system call error: [%d] %s", msg, xerrno,
              strerror(xerrno));
          }

        } else {
          tls_log("%s: system call error: %s", msg, tls_get_errors());
        }

        break;
      }

      case SSL_ERROR_SSL:
        tls_log("%s: protocol error: %s", msg, tls_get_errors());
        break;
    }

    pr_event_generate("mod_tls.data-handshake-failed", &errcode);

    tls_end_sess(ssl, conn, 0);
    return -3;
  }

  if (conn->use_nodelay == FALSE) {
    /* Disable TCP_NODELAY, now that the handshake is done. */
    (void) pr_inet_set_proto_nodelay(conn->pool, conn, 0);
  }

  /* Disable the handshake timer. */
  pr_timer_remove(tls_handshake_timer_id, &tls_module);

  /* Manually update the raw bytes counters with the network IO from the
   * TLS handshake.
   */
  session.total_raw_in += (BIO_number_read(rbio) +
    BIO_number_read(wbio));
  session.total_raw_out += (BIO_number_written(rbio) +
    BIO_number_written(wbio));

  /* Stash the SSL object in the pointers of the correct NetIO streams. */
  if (conn == session.d) {
    pr_buffer_t *strm_buf;

    if (pr_table_add(tls_data_rd_nstrm->notes,
        pstrdup(tls_data_rd_nstrm->strm_pool, TLS_NETIO_NOTE),
        ssl, sizeof(SSL *)) < 0) {
      if (errno != EEXIST) {
        tls_log("error stashing '%s' note on data read stream: %s",
          TLS_NETIO_NOTE, strerror(errno));
      }
    }

    if (pr_table_add(tls_data_wr_nstrm->notes,
        pstrdup(tls_data_wr_nstrm->strm_pool, TLS_NETIO_NOTE),
        ssl, sizeof(SSL *)) < 0) {
      if (errno != EEXIST) {
        tls_log("error stashing '%s' note on data write stream: %s",
          TLS_NETIO_NOTE, strerror(errno));
      }
    }

    /* Clear any data from the NetIO stream buffers which may have been read
     * in before the SSL/TLS handshake occurred (Bug#3624).
     */
    strm_buf = tls_data_rd_nstrm->strm_buf;
    if (strm_buf != NULL) {
      strm_buf->current = NULL;
      strm_buf->remaining = strm_buf->buflen;
    }
  }

#if OPENSSL_VERSION_NUMBER == 0x009080cfL
  /* In OpenSSL-0.9.8l, SSL session renegotiations are automatically
   * disabled.  Thus if the admin explicitly configured support for
   * client-initiated renegotiations via the AllowClientRenegotiations
   * TLSOption, then we need to do some hackery to enable renegotiations.
   */
  if (tls_opts & TLS_OPT_ALLOW_CLIENT_RENEGOTIATIONS) {
    ssl->s3->flags |= SSL3_FLAGS_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
  }
#endif

  subj = tls_get_subj_name(ssl);
  if (subj) {
    tls_log("Server: %s", subj);
  }

  if (tls_check_server_cert(ssl, conn) < 0) {
    tls_end_sess(ssl, conn, 0);
    return -1;
  }

  tls_log("%s connection created, using cipher %s (%d bits)",
    SSL_get_version(ssl), SSL_get_cipher_name(ssl),
    SSL_get_cipher_bits(ssl, NULL));

  return 0;
}

static void tls_cleanup(int flags) {

#if OPENSSL_VERSION_NUMBER > 0x000907000L && \
    OPENSSL_VERSION_NUMBER < 0x10100000L
# if defined(PR_USE_OPENSSL_ENGINE)
  if (tls_crypto_device != NULL) {
    ENGINE_cleanup();
    tls_crypto_device = NULL;
  }
# endif /* PR_USE_OPENSSL_ENGINE */
#endif

  if (ssl_ctx != NULL) {
    SSL_CTX_free(ssl_ctx);
    ssl_ctx = NULL;
  }

  /* Close the session cache only AFTER the SSL context has been
   * freed/deleted (Issue #795).
   */
  tls_sess_cache_close();
  tls_ocsp_cache_close();

  if (tls_tmp_dhs != NULL) {
    register unsigned int i;
    DH **dhs;

    dhs = tls_tmp_dhs->elts;
    for (i = 0; i < tls_tmp_dhs->nelts; i++) {
      DH_free(dhs[i]);
    }

    tls_tmp_dhs = NULL;
  }

  if (tls_tmp_rsa != NULL) {
    RSA_free(tls_tmp_rsa);
    tls_tmp_rsa = NULL;
  }

  if (!(flags & TLS_CLEANUP_FL_SESS_INIT)) {
#if OPENSSL_VERSION_NUMBER >= 0x10000001L
    /* The ERR_remove_state(0) usage is deprecated due to thread ID
     * differences among platforms; see the OpenSSL-1.0.0 CHANGES file
     * for details.  So for new enough OpenSSL installations, use the
     * proper way to clear the error queue state.
     */
# if OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_remove_thread_state(NULL);
# endif /* prior to OpenSSL-1.1.x */
#else
    ERR_remove_state(0);
#endif /* OpenSSL prior to 1.0.0-beta1 */

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_free_strings();
    EVP_cleanup();
#endif /* prior to OpenSSL-1.1.x */

  } else {
    /* Only call EVP_cleanup() et al if other OpenSSL-using modules are not
     * present.  If we called EVP_cleanup() here during session
     * initialization, and other modules want to use OpenSSL, we may
     * be depriving those modules of OpenSSL functionality.
     *
     * At the moment, the modules known to use OpenSSL are:
     *   mod_auth_otp
     *   mod_digest
     *   mod_ldap
     *   mod_proxy
     *   mod_sftp
     *   mod_sql
     *   mod_sql_passwd
     */
    if (pr_module_get("mod_auth_otp.c") == NULL &&
        pr_module_get("mod_digest.c") == NULL &&
        pr_module_get("mod_ldap.c") == NULL &&
        pr_module_get("mod_proxy.c") == NULL &&
        pr_module_get("mod_sftp.c") == NULL &&
        pr_module_get("mod_sql.c") == NULL &&
        pr_module_get("mod_sql_passwd.c") == NULL) {

#if OPENSSL_VERSION_NUMBER >= 0x10000001L
      /* The ERR_remove_state(0) usage is deprecated due to thread ID
       * differences among platforms; see the OpenSSL-1.0.0c CHANGES file
       * for details.  So for new enough OpenSSL installations, use the
       * proper way to clear the error queue state.
       */
# if OPENSSL_VERSION_NUMBER < 0x10100000L
      ERR_remove_thread_state(NULL);
# endif /* prior to OpenSSL-1.1.x */
#else
      ERR_remove_state(0);
#endif /* OpenSSL prior to 1.0.0-beta1 */

#if OPENSSL_VERSION_NUMBER < 0x10100000L
      ERR_free_strings();
      EVP_cleanup();
#endif /* prior to OpenSSL-1.1.x */
    }
  }
}

/* Returns TRUE for SSL data, FALSE for FTP data, and -1 on error.
 *
 * First, we want for readable data on the socket via select(2).  Then
 * we use recv(2) to peek at the data; we only need three bytes.  Why only
 * three bytes?  According to the TLS RFCs on SSL record layering, each
 * well-formed SSL record starts with:
 *
 *    struct {
 *        uint8 major;
 *        uint8 minor;
 *    } ProtocolVersion;
 *
 *    enum {
 *        change_cipher_spec(20), alert(21), handshake(22),
 *        application_data(23), (255)
 *    } ContentType;
 *
 *    struct {
 *        ContentType type;
 *        ProtocolVersion version;
 *
 * It helps that the first byte, if an SSL record, will be 20-23, or 255.
 * The printable ASCII range, which would be used for an FTP command, starts
 * at 32 (sp).  We read three bytes to include the protocol version, to
 * help increase confidence in our guess at whether these data are a well-
 * formed SSL record, or an FTP command.
 */
static int peek_is_ssl_data(int fd) {
  register unsigned int i;
  int res;
  fd_set rfds;
  struct timeval tv;
  ssize_t len;
  unsigned char buf[3];

  /* We'll wait up to 5 secs, or until interrupted, for the next data. */
  tv.tv_sec = 5;
  tv.tv_usec = 0;

  pr_trace_msg(trace_channel, 20, "peeking at next data for fd %d, for %d secs",
    fd, (int) tv.tv_sec);

  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);

  res = select(fd + 1, &rfds, NULL, NULL, &tv);
  while (res < 0) {
    int xerrno = errno;

    if (xerrno == EINTR) {
      pr_signals_handle();
      res = select(fd + 1, &rfds, NULL, NULL, &tv);
      continue;
    }

    pr_trace_msg(trace_channel, 20,
      "error waiting for next data on fd %d: %s", fd, strerror(xerrno));
    errno = xerrno;
    return -1;
  }

  if (res == 0) {
    /* Timed out.  Error on the side of optimism, and assume the next data
     * will be an SSL record.  This is what mod_tls would do, if we were not
     * peeking.
     */
    pr_trace_msg(trace_channel, 20,
      "timed out after %d secs peeking at next data, assuming SSL data",
      (int) tv.tv_sec);
    return TRUE;
  }

  /* If we reach here, the peer must have sent something.  Let's see what it
   * might be.  Chances are that we received at least 3 bytes, but to be
   * defensive, we use MSG_WAITALL anyway.  TCP allows for sending one byte
   * at time, if need be.  Fortunately, either SSL record or FTP command will
   * require more than three bytes.
   */
  memset(&buf, 0, sizeof(buf));
  len = recv(fd, buf, sizeof(buf), MSG_PEEK|MSG_WAITALL);
  while (len < 0) {
    int xerrno = errno;

    if (xerrno == EINTR) {
      pr_signals_handle();
      len = recv(fd, &buf, sizeof(buf), MSG_PEEK|MSG_WAITALL);
      continue;
    }

    pr_trace_msg(trace_channel, 20,
      "error peeking at next data: %s", strerror(xerrno));
    errno = xerrno;
    return -1;
  }

  pr_trace_msg(trace_channel, 20, "peeking at %ld bytes of next data",
    (long) len);
  for (i = 0; i < len; i++) {
    if (PR_ISPRINT(buf[i]) == 0) {
      /* Not a printable character; assume it's SSL data. */
      pr_trace_msg(trace_channel, 20,
        "byte %u of peeked data is a non-printable ASCII character (%d), "
        "assuming SSL data", i, (int) buf[i]);
      return TRUE;
    }
  }

  pr_trace_msg(trace_channel, 20,
    "all %ld bytes of peeked data are printable ASCII characters, assuming "
    "FTP data", (long) len);
  return FALSE;
}

static void tls_end_sess(SSL *ssl, conn_t *conn, int flags) {
  int lineno, res = 0;
  int shutdown_state;
  BIO *rbio, *wbio;
  int bread, bwritten;
  unsigned long rbio_rbytes, rbio_wbytes, wbio_rbytes, wbio_wbytes;

  if (ssl == NULL) {
    return;
  }

  rbio = SSL_get_rbio(ssl);
  rbio_rbytes = BIO_number_read(rbio);
  rbio_wbytes = BIO_number_written(rbio);

  wbio = SSL_get_wbio(ssl);
  wbio_rbytes = BIO_number_read(wbio);
  wbio_wbytes = BIO_number_written(wbio);

  /* A 'close_notify' alert (SSL shutdown message) may have been previously
   * sent to the client via tls_netio_shutdown_cb().
   */

  shutdown_state = SSL_get_shutdown(ssl);
  if (!(shutdown_state & SSL_SENT_SHUTDOWN)) {
    errno = 0;

    if (conn != NULL) {
      /* Disable any socket buffering (Nagle, TCP_CORK), so that the alert
       * is sent in a timely manner (avoiding TLS shutdown latency).
       */
      if (pr_inet_set_proto_nodelay(conn->pool, conn, 1) < 0) {
        pr_trace_msg(trace_channel, 9,
          "error enabling TCP_NODELAY on conn: %s", strerror(errno));
      }

      if (pr_inet_set_proto_cork(conn->wfd, 0) < 0) {
        pr_trace_msg(trace_channel, 9,
          "error disabling TCP_CORK on fd %d: %s", conn->wfd, strerror(errno));
      }
    }

    /* 'close_notify' not already sent; send it now. */
    pr_trace_msg(trace_channel, 17,
      "shutting down %s TLS session, 'close_notify' not already sent; "
      "sending now", ssl == ctrl_ssl ? "control" : "data");
    lineno = __LINE__ + 1;
    res = SSL_shutdown(ssl);
  }

  if (res == 0) {
    /* Now call SSL_shutdown() again, but only if necessary. */
    if (flags & TLS_SHUTDOWN_FL_BIDIRECTIONAL) {
      shutdown_state = SSL_get_shutdown(ssl);

      res = 1;
      if (!(shutdown_state & SSL_RECEIVED_SHUTDOWN) &&
          conn != NULL) {
        int is_ssl_data = FALSE, xerrno;

        pr_trace_msg(trace_channel, 17,
          "shutting down %s TLS session, 'close_notify' not received; "
          "peeking at next data", ssl == ctrl_ssl ? "control" : "data");

        /* This where we need to peek at the next data, to see whether we
         * dealing with a well-behaved FTPS client, which will be sending
         * its `close_notify` to properly shutdown the TLS session, or with
         * a buggy/ill-behaved FTPS client which will not send a `close_notify`,
         * and instead will just send the next FTP command in plaintext.
         *
         * If it's the latter case, then we do NOT want to wait for a
         * `close_notify` that will never come.  Doing so will cause the
         * plaintext FTP command to be treated as an SSL record, which will
         * fail with a "wrong version number" SSL error.
         */

        is_ssl_data = peek_is_ssl_data(conn->rfd);
        if (is_ssl_data < 0) {
          SSL_free(ssl);
          pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_BY_APPLICATION,
            NULL);
          return;
        }

        if (is_ssl_data == FALSE) {
          /* We're dealing with a buggy/ill-behaved FTPS client.  Sigh. */
          pr_trace_msg(trace_channel, 17,
            "shut down TLS session uncleanly, next data is FTP command from "
            "buggy/ill-behaved FTPS client");
          SSL_free(ssl);
          return;
        }

        errno = 0;
        lineno = __LINE__ + 1;
        res = SSL_shutdown(ssl);
        xerrno = errno;

        pr_trace_msg(trace_channel, 17,
          "shutting down %s TLS session, 'close_notify' not received; "
          "SSL_shutdown() returned %d", ssl == ctrl_ssl ? "control" : "data",
          res);

        errno = xerrno;
      }
    }

    /* If SSL_shutdown() returned -1 here, an error occurred during the
     * shutdown.
     */
    if (res < 0) {
      long err_code;

      err_code = SSL_get_error(ssl, res);
      switch (err_code) {
        case SSL_ERROR_WANT_READ:
          tls_log("SSL_shutdown error: WANT_READ");
          break;

        case SSL_ERROR_WANT_WRITE:
          tls_log("SSL_shutdown error: WANT_WRITE");
          break;

        case SSL_ERROR_SSL: {
          unsigned long ssl_errcode = ERR_peek_error();

          /* The error codes in the OpenSSL error queue are "packed"; we need
           * to unpack them to get the reason value.
           */

#ifdef SSL_R_SHUTDOWN_WHILE_IN_INIT
          if (ERR_GET_REASON(ssl_errcode) != SSL_R_SHUTDOWN_WHILE_IN_INIT) {
            /* This SHUTDOWN_WHILE_IN_INIT can happen if the TLS handshake
             * failed before being completed.  As such, logging this shutdown
             * error on an incomplete session is spurious and not helpful.
             */
            tls_log("SSL_shutdown error: SSL: %s", tls_get_errors());
          }
#else
          (void) ssl_errcode;
          tls_log("SSL_shutdown error: SSL: %s", tls_get_errors());
#endif /* No SSL_R_SHUTDOWN_WHILE_IN_INIT */

          break;
        }

        case SSL_ERROR_ZERO_RETURN:
          /* Clean shutdown, nothing we need to do. */
          break;

        case SSL_ERROR_SYSCALL:
          if (errno != 0 &&
              errno != EOF &&
              errno != EBADF &&
              errno != EPIPE &&
              errno != EPERM &&
              errno != ENOSYS) {
            tls_log("SSL_shutdown syscall error: %s", strerror(errno));
          }
          break;

        default:
          tls_log("SSL_shutdown error [%ld], line %d: %s", err_code, lineno,
            tls_get_errors());
          pr_log_debug(DEBUG0, MOD_TLS_VERSION
            ": SSL_shutdown error [%ld], line %d: %s", err_code, lineno,
            tls_get_errors());
          break;
      }
    }

  } else if (res < 0) {
    long err_code;

    err_code = SSL_get_error(ssl, res);
    switch (err_code) {
      case SSL_ERROR_WANT_READ:
      case SSL_ERROR_WANT_WRITE:
      case SSL_ERROR_ZERO_RETURN:
        /* Clean shutdown, nothing we need to do.  The WANT_READ/WANT_WRITE
         * error codes crept into OpenSSL 0.9.8m, with changes to make
         * SSL_shutdown() work properly for non-blocking sockets.  And
         * handling these error codes for older OpenSSL versions won't break
         * things.
         */
        break;

      case SSL_ERROR_SSL: {
        unsigned long ssl_errcode = ERR_peek_error();

        /* The error codes in the OpenSSL error queue are "packed"; we need
         * to unpack them to get the reason value.
         */

#ifdef SSL_R_SHUTDOWN_WHILE_IN_INIT
        if (ERR_GET_REASON(ssl_errcode) != SSL_R_SHUTDOWN_WHILE_IN_INIT) {
          /* This SHUTDOWN_WHILE_IN_INIT can happen if the TLS handshake
           * failed before being completed.  As such, logging this shutdown
           * error on an incomplete session is spurious and not helpful.
           */
          tls_log("SSL_shutdown error: SSL: %s", tls_get_errors());
        }
#else
        (void) ssl_errcode;
        tls_log("SSL_shutdown error: SSL: %s", tls_get_errors());
#endif /* No SSL_R_SHUTDOWN_WHILE_IN_INIT */
        break;
      }

      case SSL_ERROR_SYSCALL:
        if (errno != 0 &&
            errno != EOF &&
            errno != EBADF &&
            errno != EPIPE &&
            errno != EPERM &&
            errno != ENOSYS) {
          tls_log("SSL_shutdown syscall error: %s", strerror(errno));
        }
        break;

      default:
        tls_fatal_error(err_code, lineno);
        break;
    }
  }

  bread = (BIO_number_read(rbio) - rbio_rbytes) +
    (BIO_number_read(wbio) - wbio_rbytes);
  bwritten = (BIO_number_written(rbio) - rbio_wbytes) +
    (BIO_number_written(wbio) - wbio_wbytes);

  /* Manually update session.total_raw_in/out, in order to have %I/%O be
   * accurately represented for the raw traffic.
   */
  if (bread > 0) {
    session.total_raw_in += bread;
  }

  if (bwritten > 0) {
    session.total_raw_out += bwritten;
  }

  if (ssl != ctrl_ssl &&
      SSL_get_session(ssl) == SSL_get_session(ctrl_ssl)) {
    /* Uh-oh; our two SSL objects are pointing at the same SSL_SESSION object.
     * This can happen when the SSL_SESSION is resumed, enabled by session
     * caching.  Which is normally a Good Thing.
     *
     * But Issue#1963 happens when that SSL_SESSION object is freed twice:
     * once via SSL_free() on this SSL, and again on SSL_free() for the
     * other SSL.  Yuck.
     *
     * So we manually handle the situation by clearing the duplicate
     * SSL_SESSION pointer from the data SSL.  Why the data SSL, and not the
     * control SSL?  We could be handling multiple concurrent data transfers,
     * and/or the client may reuse the session again on a subsequent data
     * transfer.
     *
     * Note that older OpenSSL versions used two different SSL_SESSION objects,
     * hence why we have not seen this occur frequently in the past.  (Or it
     * was not reported.)  And for TLSv1.3 resumed sessions, the SSL_SESSION
     * objects are different, hence no double-free.
     */
    pr_trace_msg(trace_channel, 29,
      "data SSL %p being ended has same SSL_SESSION %p as control SSL, "
      "clearing the data SSL pointer manually (Issue #1963)", ssl,
      SSL_get_session(ssl));
    SSL_set_session(ssl, NULL);
  }

  SSL_free(ssl);

  if (res >= 0) {
    pr_trace_msg(trace_channel, 17, "%s TLS session cleanly shut down",
      ssl == ctrl_ssl ? "control" : "data");
  }
}

static const char *tls_get_errors2(pool *p) {
  unsigned int count = 0;
  unsigned long error_code;
  BIO *bio = NULL;
  char *data = NULL;
  long datalen;
  const char *error_data = NULL, *str = "(unknown)";
  int error_flags = 0;

  /* Use ERR_print_errors() and a memory BIO to build up a string with
   * all of the error messages from the error queue.
   */

  error_code = ERR_get_error_line_data(NULL, NULL, &error_data, &error_flags);
  if (error_code) {
    bio = BIO_new(BIO_s_mem());
  }

  while (error_code) {
    pr_signals_handle();

    if (error_flags & ERR_TXT_STRING) {
      BIO_printf(bio, "\n  (%u) %s [%s]", ++count,
        ERR_error_string(error_code, NULL), error_data);

    } else {
      BIO_printf(bio, "\n  (%u) %s", ++count,
        ERR_error_string(error_code, NULL));
    }

    error_data = NULL;
    error_flags = 0;
    error_code = ERR_get_error_line_data(NULL, NULL, &error_data, &error_flags);
  }

  datalen = BIO_get_mem_data(bio, &data);
  if (data) {
    data[datalen] = '\0';
    str = pstrdup(p, data);
  }

  if (bio) {
    BIO_free(bio);
  }

  return str;
}

static const char *tls_get_errors(void) {
  return tls_get_errors2(session.pool);
}

/* Return a page-aligned pointer to memory of at least the given size. */
static char *tls_get_page(size_t sz, void **ptr) {
  void *d;
  long pagesz = tls_get_pagesz(), p;

  d = calloc(1, sz + (pagesz-1));
  if (d == NULL) {
    pr_log_pri(PR_LOG_ALERT, MOD_TLS_VERSION ": Out of memory!");
    exit(1);
  }

  *ptr = d;

  p = ((long) d + (pagesz-1)) &~ (pagesz-1);

  return ((char *) p);
}

/* Return the size of a page on this architecture. */
static size_t tls_get_pagesz(void) {
  long pagesz;

#if defined(_SC_PAGESIZE)
  pagesz = sysconf(_SC_PAGESIZE);
#elif defined(_SC_PAGE_SIZE)
  pagesz = sysconf(_SC_PAGE_SIZE);
#else
  /* Default to using OpenSSL's defined buffer size for PEM files. */
  pagesz = PEM_BUFSIZE;
#endif /* !_SC_PAGESIZE and !_SC_PAGE_SIZE */

  return pagesz;
}

static char *tls_get_subj_name(SSL *ssl) {
  X509 *cert;

  cert = SSL_get_peer_certificate(ssl);
  if (cert != NULL) {
    char *name = tls_x509_name_oneline(X509_get_subject_name(cert));
    X509_free(cert);
    return name;
  }

  return NULL;
}

static void tls_fatal_error(long error, int lineno) {

  switch (error) {
    case SSL_ERROR_NONE:
      return;

    case SSL_ERROR_SSL:
      tls_log("panic: SSL_ERROR_SSL, line %d: %s", lineno, tls_get_errors());
      break;

    case SSL_ERROR_WANT_READ:
      tls_log("panic: SSL_ERROR_WANT_READ, line %d", lineno);
      break;

    case SSL_ERROR_WANT_WRITE:
      tls_log("panic: SSL_ERROR_WANT_WRITE, line %d", lineno);
      break;

    case SSL_ERROR_WANT_X509_LOOKUP:
      tls_log("panic: SSL_ERROR_WANT_X509_LOOKUP, line %d", lineno);
      break;

    case SSL_ERROR_SYSCALL: {
      long xerrcode = ERR_get_error();

      if (errno == ECONNRESET) {
        pr_trace_msg(trace_channel, 17,
          "SSL_ERROR_SYSCALL error (errcode %ld) occurred on line %d; ignoring "
          "ECONNRESET (%s)", xerrcode, lineno, strerror(errno));
        return;
      }

      /* Check to see if the OpenSSL error queue has info about this. */
      if (xerrcode == 0) {
        /* The OpenSSL error queue doesn't have any more info, so we'll
         * examine the error value itself.
         */

        if (errno == EOF) {
          tls_log("panic: SSL_ERROR_SYSCALL, line %d: "
            "EOF that violates protocol", lineno);

        } else {
          /* Check errno */
          tls_log("panic: SSL_ERROR_SYSCALL, line %d: system error: %s", lineno,
            strerror(errno));
        }

      } else {
        tls_log("panic: SSL_ERROR_SYSCALL, line %d: %s", lineno,
          tls_get_errors());
      }

      break;
    }

    case SSL_ERROR_ZERO_RETURN:
      tls_log("panic: SSL_ERROR_ZERO_RETURN, line %d", lineno);
      break;

    case SSL_ERROR_WANT_CONNECT:
      tls_log("panic: SSL_ERROR_WANT_CONNECT, line %d", lineno);
      break;

    default:
      tls_log("panic: SSL_ERROR %ld, line %d", error, lineno);
      break;
  }

  tls_log("%s", "unexpected OpenSSL error, disconnecting");
  pr_log_pri(PR_LOG_WARNING, "%s", MOD_TLS_VERSION
    ": unexpected OpenSSL error, disconnecting");

  pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_BY_APPLICATION, NULL);
}

/* This function checks if the client's cert is in the ~/.tlslogin file
 * of the "user".
 */
static int tls_dotlogin_allow(const char *user) {
  char buf[512] = {'\0'}, *home = NULL;
  FILE *fp = NULL;
  X509 *client_cert = NULL, *file_cert = NULL;
  struct passwd *pwd = NULL;
  pool *tmp_pool = NULL;
  unsigned char allow_user = FALSE;
  int xerrno;

  if (!(tls_flags & TLS_SESS_ON_CTRL) ||
      ctrl_ssl == NULL ||
      user == NULL) {
    return FALSE;
  }

  /* If the client did not provide a cert, we cannot do the .tlslogin check. */
  client_cert = SSL_get_peer_certificate(ctrl_ssl);
  if (client_cert == NULL) {
    pr_trace_msg(trace_channel, 9, "%s",
      "client did not provide certificate, skipping AllowDotLogin check");
    return FALSE;
  }

  tmp_pool = make_sub_pool(permanent_pool);

  PRIVS_ROOT
  pwd = pr_auth_getpwnam(tmp_pool, user);
  PRIVS_RELINQUISH

  if (pwd == NULL) {
    X509_free(client_cert);
    destroy_pool(tmp_pool);
    return FALSE;
  }

  /* Handle the case where the user's home directory is a symlink. */
  PRIVS_USER
  home = dir_realpath(tmp_pool, pwd->pw_dir);
  PRIVS_RELINQUISH

  pr_snprintf(buf, sizeof(buf), "%s/.tlslogin", home ? home : pwd->pw_dir);
  buf[sizeof(buf)-1] = '\0';

  /* No need for the temporary pool any more. */
  destroy_pool(tmp_pool);
  tmp_pool = NULL;

  PRIVS_ROOT
  fp = fopen(buf, "r");
  xerrno = errno;
  PRIVS_RELINQUISH

  if (fp == NULL) {
    X509_free(client_cert);
    tls_log(".tlslogin check: unable to open '%s': %s", buf, strerror(xerrno));
    return FALSE;
  }

  /* As the file may contain sensitive data, we do not want it lingering
   * around in stdio buffers.
   */
  (void) setvbuf(fp, NULL, _IONBF, 0);

  while ((file_cert = PEM_read_X509(fp, NULL, NULL, NULL))) {
    const ASN1_BIT_STRING *client_sig = NULL, *file_sig = NULL;

    pr_signals_handle();

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
    X509_get0_signature(&client_sig, NULL, client_cert);
    X509_get0_signature(&file_sig, NULL, file_cert);
#else
    client_sig = client_cert->signature;
    file_sig = file_cert->signature;
#endif /* OpenSSL-1.1.x/LibreSSL-3.5.x and later */

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    if (!ASN1_STRING_cmp(client_sig, file_sig)) {
#else
    if (!M_ASN1_BIT_STRING_cmp(client_sig, file_sig)) {
#endif /* OpenSSL-1.1.x and later */
      allow_user = TRUE;
    }

    if (allow_user == FALSE) {
      BIO *bio;
      char *data;
      long datalen;
      unsigned long nameflags, skipflags;

      nameflags = XN_FLAG_ONELINE;
      skipflags = X509_FLAG_NO_PUBKEY|X509_FLAG_NO_EXTENSIONS|X509_FLAG_NO_SIGDUMP|X509_FLAG_NO_AUX;
#ifdef X509_FLAG_NO_ATTRIBUTES
      skipflags |= X509_FLAG_NO_ATTRIBUTES;
#endif
#ifdef X509_FLAG_NO_IDS
      skipflags |= X509_FLAG_NO_IDS;
#endif

      tls_log(".tlslogin local/remote certificate MISMATCH");
      bio = BIO_new(BIO_s_mem());
      X509_print_ex(bio, file_cert, nameflags, skipflags);
      datalen = BIO_get_mem_data(bio, &data);
      data[datalen] = '\0';
      tls_log(".tlslogin local file certificate:\n%.*s", (int) datalen, data);
      BIO_free(bio);

      bio = BIO_new(BIO_s_mem());
      X509_print_ex(bio, client_cert, nameflags, skipflags);
      datalen = BIO_get_mem_data(bio, &data);
      data[datalen] = '\0';
      tls_log(".tlslogin remote client certificate:\n%.*s", (int) datalen, data);
      BIO_free(bio);
    }

    X509_free(file_cert);
    if (allow_user == TRUE) {
      break;
    }
  }

  X509_free(client_cert);
  fclose(fp);

  return allow_user;
}

static int tls_cert_to_user(const char *user_name, const char *field_name) {
  X509 *client_cert = NULL;
  unsigned char allow_user = FALSE;
  const unsigned char *field_value = NULL;

  if (!(tls_flags & TLS_SESS_ON_CTRL) ||
      ctrl_ssl == NULL ||
      user_name == NULL ||
      field_name == NULL) {
    return FALSE;
  }

  /* If the client did not provide a cert, we cannot do the TLSUserName
   * check.
   */
  client_cert = SSL_get_peer_certificate(ctrl_ssl);
  if (client_cert == NULL) {
    return FALSE;
  }

  if (strcmp(field_name, "CommonName") == 0) {
    X509_NAME *name;
    int pos = -1;

    name = X509_get_subject_name(client_cert);

    while (TRUE) {
      X509_NAME_ENTRY *entry;
      ASN1_STRING *data;
      int data_len;
      const unsigned char *data_str = NULL;

      pr_signals_handle();

      pos = X509_NAME_get_index_by_NID(name, NID_commonName, pos);
      if (pos == -1) {
        break;
      }

      entry = X509_NAME_get_entry(name, pos);
      data = X509_NAME_ENTRY_get_data(entry);
      data_len = ASN1_STRING_length(data);
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
      data_str = ASN1_STRING_get0_data(data);
#else
      data_str = ASN1_STRING_data(data);
#endif /* OpenSSL 1.1.x and later */

      /* Watch for any embedded NULs, which can cause verification
       * problems via spoofing.
       */
      if ((size_t) data_len != strlen((char *) data_str)) {
        tls_log("%s", "client cert CommonName contains embedded NULs, "
          "ignoring as possible spoof attempt");
        tls_log("suspicious CommonName value: '%s'", data_str);

      } else {

        /* There can be multiple CommonNames... */
        if (strcmp((char *) data_str, user_name) == 0) {
          field_value = data_str;
          allow_user = TRUE;

          tls_log("matched client cert CommonName '%s' to user '%s'",
            field_value, user_name);
          break;
        }
      }
    }

  } else if (strcmp(field_name, "EmailSubjAltName") == 0) {
    STACK_OF(GENERAL_NAME) *sk_alt_names;

    sk_alt_names = X509_get_ext_d2i(client_cert, NID_subject_alt_name, NULL,
      NULL);
    if (sk_alt_names != NULL) {
      register int i;
      int nnames = sk_GENERAL_NAME_num(sk_alt_names);

      for (i = 0; i < nnames; i++) {
        GENERAL_NAME *name;

        pr_signals_handle();

        name = sk_GENERAL_NAME_value(sk_alt_names, i);

        /* We're only looking for the Email type. */
        if (name->type == GEN_EMAIL) {
          int data_len;
          const unsigned char *data_str = NULL;

          data_len = ASN1_STRING_length(name->d.ia5);

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
          data_str = ASN1_STRING_get0_data(name->d.ia5);
#else
          data_str = ASN1_STRING_data(name->d.ia5);
#endif /* OpenSSL 1.1.x and later */

          /* Watch for any embedded NULs, which can cause verification
           * problems via spoofing.
           */
          if ((size_t) data_len != strlen((char *) data_str)) {
            tls_log("%s", "client cert Email SAN contains embedded NULs, "
              "ignoring as possible spoof attempt");
            tls_log("suspicious Email SubjAltName value: '%s'", data_str);

          } else {

            /* There can be multiple Email SANs... */
            if (strcmp((char *) data_str, user_name) == 0) {
              field_value = data_str;
              allow_user = TRUE;

              tls_log("matched client cert Email SubjAltName '%s' to user '%s'",
                field_value, user_name);
              GENERAL_NAME_free(name);
              break;
            }
          }
        }

        GENERAL_NAME_free(name);
      }

      sk_GENERAL_NAME_free(sk_alt_names);
    }

  } else {
    /* Custom OID. */
    int nexts;

    nexts = X509_get_ext_count(client_cert);
    if (nexts > 0) {
      register int i;

      for (i = 0; i < nexts; i++) {
        X509_EXTENSION *ext = NULL;
        ASN1_OBJECT *asn_object = NULL;
        char oid[PR_TUNABLE_PATH_MAX];

        pr_signals_handle();

        ext = X509_get_ext(client_cert, i);
        asn_object = X509_EXTENSION_get_object(ext);

        /* Get the OID of this extension, as a string. */
        memset(oid, '\0', sizeof(oid));
        if (OBJ_obj2txt(oid, sizeof(oid)-1, asn_object, 1) > 0) {
          if (strcmp(oid, field_name) == 0) {
            ASN1_OCTET_STRING *asn_data = NULL;
            const unsigned char *asn_datastr = NULL;
            int asn_datalen;

            asn_data = X509_EXTENSION_get_data(ext);
            asn_datalen = ASN1_STRING_length(asn_data);
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
            asn_datastr = ASN1_STRING_get0_data(asn_data);
#else
            asn_datastr = ASN1_STRING_data(asn_data);
#endif /* OpenSSL 1.1.x and later */

            /* Watch for any embedded NULs, which can cause verification
             * problems via spoofing.
             */
            if ((size_t) asn_datalen != strlen((char *) asn_datastr)) {
              tls_log("client cert %s extension contains embedded NULs, "
                "ignoring as possible spoof attempt", field_name);
              tls_log("suspicious %s extension value: '%s'", field_name,
                asn_datastr);

            } else {

              /* There might be multiple matching extensions? */
              if (strcmp((char *) asn_datastr, user_name) == 0) {
                field_value = asn_datastr;
                allow_user = TRUE;

                tls_log("matched client cert %s extension '%s' to user '%s'",
                  field_name, field_value, user_name);
                break;
              }
            }
          }
        }
      }
    }
  }

  X509_free(client_cert);
  return allow_user;
}

static int tls_readmore(int rfd) {
  fd_set rfds;
  struct timeval tv;

  FD_ZERO(&rfds);
  FD_SET(rfd, &rfds);

  /* Use a timeout of 15 seconds */
  tv.tv_sec = 15;
  tv.tv_usec = 0;

  return select(rfd + 1, &rfds, NULL, NULL, &tv);
}

static int tls_writemore(int wfd) {
  fd_set wfds;
  struct timeval tv;

  FD_ZERO(&wfds);
  FD_SET(wfd, &wfds);

  /* Use a timeout of 15 seconds */
  tv.tv_sec = 15;
  tv.tv_usec = 0;

  return select(wfd + 1, NULL, &wfds, NULL, &tv);
}

static ssize_t tls_read(SSL *ssl, void *buf, size_t len) {
  ssize_t count;
  int lineno, xerrno = 0;

  retry:
  pr_signals_handle();

  /* Help ensure we have a clean/trustable errno value. */
  errno = 0;

  lineno = __LINE__ + 1;
  count = SSL_read(ssl, buf, len);
  xerrno = errno;

  if (count < 0) {
    long err;
    int fd;

    err = SSL_get_error(ssl, count);
    fd = SSL_get_fd(ssl);

    /* read(2) returns only the generic error number -1 */
    count = -1;

    switch (err) {
      case SSL_ERROR_WANT_READ:
        /* OpenSSL needs more data from the wire to finish the current block,
         * so we wait a little while for it.
         */
        pr_trace_msg(trace_channel, 17,
          "WANT_READ encountered while reading TLS data on fd %d, "
          "waiting to read data", fd);
        err = tls_readmore(fd);
        if (err > 0) {
          goto retry;

        } else if (err == 0) {
          /* Still missing data after timeout. Simulate an EINTR and return.
           */
          xerrno = EINTR;

          /* If err < 0, i.e. some error from the select(), everything is
           * already in place; errno is properly set and this function
           * returns -1.
           */
          break;
        }

      case SSL_ERROR_WANT_WRITE:
        /* OpenSSL needs to write more data to the wire to finish the current
         * block, so we wait a little while for it.
         */
        pr_trace_msg(trace_channel, 17,
          "WANT_WRITE encountered while writing TLS data on fd %d, "
          "waiting to send data", fd);
        err = tls_writemore(fd);
        if (err > 0) {
          goto retry;

        } else if (err == 0) {
          /* Still missing data after timeout. Simulate an EINTR and return.
           */
          xerrno = EINTR;

          /* If err < 0, i.e. some error from the select(), everything is
           * already in place; errno is properly set and this function
           * returns -1.
           */
          break;
        }

      case SSL_ERROR_ZERO_RETURN:
        tls_log("read EOF from client");
        break;

      case SSL_ERROR_SSL:
      case SSL_ERROR_SYSCALL:
      default:
        tls_fatal_error(err, lineno);
        break;
    }
  }

  errno = xerrno;
  return count;
}

static int tls_seed_prng(void) {
  char *heapdata, stackdata[1024];
  static char rand_file[300];
  FILE *fp = NULL;

#if OPENSSL_VERSION_NUMBER >= 0x00905100L
  if (RAND_status() == 1)

    /* PRNG already well-seeded. */
    return 0;
#endif

  tls_log("PRNG not seeded with enough data, looking for entropy sources");

  /* If the device '/dev/urandom' is present, OpenSSL uses it by default.
   * Check if it's present, else we have to make random data ourselves.
   */
  fp = fopen("/dev/urandom", "r");
  if (fp != NULL) {
    fclose(fp);

    tls_log("device /dev/urandom is present, assuming OpenSSL will use that "
      "for PRNG data");
    return 0;
  }

  /* Lookup any configured TLSRandomSeed. */
  tls_rand_file = get_param_ptr(main_server->conf, "TLSRandomSeed", FALSE);
  if (tls_rand_file == NULL) {
    /* The ftpd's random file is (openssl-dir)/.rnd */
    memset(rand_file, '\0', sizeof(rand_file));
    pr_snprintf(rand_file, sizeof(rand_file)-1, "%s/.rnd",
      X509_get_default_cert_area());
    tls_rand_file = rand_file;
  }

#if OPENSSL_VERSION_NUMBER >= 0x00905100L
  /* In OpenSSL 0.9.5 and later, specifying -1 here means "read the entire
   * file", which is exactly what we want.
   */
  if (RAND_load_file(tls_rand_file, -1) == 0) {
#else
  /* In versions of OpenSSL prior to 0.9.5, we have to specify the amount of
   * bytes to read in.  Since RAND_write_file(3) typically writes 1K of data
   * out, we will read 1K bytes in.
   */
  if (RAND_load_file(tls_rand_file, 1024) != 1024) {
#endif
    struct timeval tv;
    pid_t pid;

#if OPENSSL_VERSION_NUMBER >= 0x00905100L
    tls_log("unable to load PRNG seed data from '%s': %s", tls_rand_file,
      tls_get_errors());
#else
    tls_log("unable to load 1024 bytes of PRNG seed data from '%s': %s",
      tls_rand_file, tls_get_errors());
#endif

    /* No random file found, create new seed. */
    gettimeofday(&tv, NULL);
    RAND_seed(&(tv.tv_sec), sizeof(tv.tv_sec));
    RAND_seed(&(tv.tv_usec), sizeof(tv.tv_usec));

    pid = getpid();
    RAND_seed(&pid, sizeof(pid_t));
    RAND_seed(stackdata, sizeof(stackdata));

    heapdata = malloc(sizeof(stackdata));
    if (heapdata != NULL) {
      RAND_seed(heapdata, sizeof(stackdata));
      free(heapdata);
    }

  } else {
    tls_log("loaded PRNG seed data from '%s'", tls_rand_file);
  }

#if OPENSSL_VERSION_NUMBER >= 0x00905100L
  if (RAND_status() == 0) {
     /* PRNG still badly seeded. */
     return -1;
  }
#endif

  return 0;
}

/* Note: these mappings should probably be added to the mod_tls docs.
 */

static void tls_setup_cert_ext_environ(const char *env_prefix, X509 *cert) {

  /* NOTE: in the future, add ways of adding subjectAltName (and other
   * extensions?) to the environment.
   */

#if 0
  int nexts = 0;

  nexts = X509_get_ext_count(cert);
  if (nexts > 0) {
    register int i;

    for (i = 0; i < nexts; i++) {
      X509_EXTENSION *ext;
      const char *extstr;

      ext = X509_get_ext(cert, i);
      extstr = OBJ_nid2sn(OBJ_obj2nid(X509_EXTENSION_get_object(ext)));
    }
  }
#endif

  return;
}

/* Note: these mappings should probably be added to the mod_tls docs.
 *
 *   Name                    Short Name    NID
 *   ----                    ----------    ---
 *   countryName             C             NID_countryName
 *   commonName              CN            NID_commonName
 *   description             D             NID_description
 *   givenName               G             NID_givenName
 *   initials                I             NID_initials
 *   localityName            L             NID_localityName
 *   organizationName        O             NID_organizationName
 *   organizationalUnitName  OU            NID_organizationalUnitName
 *   stateOrProvinceName     ST            NID_stateOrProvinceName
 *   surname                 S             NID_surname
 *   title                   T             NID_title
 *   uniqueIdentifer         UID           NID_x500UniqueIdentifier
 *                                         (or NID_uniqueIdentifier, depending
 *                                         on OpenSSL version)
 *   email                   Email         NID_pkcs9_emailAddress
 */

static void tls_setup_cert_dn_environ(const char *env_prefix, X509_NAME *name) {
  register int i;
  int nentries;
  char *k, *v;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  nentries = X509_NAME_entry_count(name);
#else
  nentries = sk_X509_NAME_ENTRY_num(name->entries);
#endif /* OpenSSL-1.1.x and later */

  for (i = 0; i < nentries; i++) {
    X509_NAME_ENTRY *entry;
    const unsigned char *entry_data;
    int nid, entry_len;

    pr_signals_handle();

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    entry = X509_NAME_get_entry(name, i);
    nid = OBJ_obj2nid(X509_NAME_ENTRY_get_object(entry));
    entry_data = ASN1_STRING_get0_data(X509_NAME_ENTRY_get_data(entry));
    entry_len = ASN1_STRING_length(X509_NAME_ENTRY_get_data(entry));
#else
    entry = sk_X509_NAME_ENTRY_value(name->entries, i);
    nid = OBJ_obj2nid(entry->object);
    entry_data = entry->value->data;
    entry_len = entry->value->length;
#endif /* OpenSSL-1.1.x and later */

    switch (nid) {
      case NID_countryName:
        k = pstrcat(session.pool, env_prefix, "C", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(session.pool, k, v);
        break;

      case NID_commonName:
        k = pstrcat(session.pool, env_prefix, "CN", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(session.pool, k, v);
        break;

      case NID_description:
        k = pstrcat(main_server->pool, env_prefix, "D", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

      case NID_givenName:
        k = pstrcat(main_server->pool, env_prefix, "G", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

      case NID_initials:
        k = pstrcat(main_server->pool, env_prefix, "I", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

      case NID_localityName:
        k = pstrcat(main_server->pool, env_prefix, "L", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

      case NID_organizationName:
        k = pstrcat(main_server->pool, env_prefix, "O", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

      case NID_organizationalUnitName:
        k = pstrcat(main_server->pool, env_prefix, "OU", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

      case NID_stateOrProvinceName:
        k = pstrcat(main_server->pool, env_prefix, "ST", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

      case NID_surname:
        k = pstrcat(main_server->pool, env_prefix, "S", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

      case NID_title:
        k = pstrcat(main_server->pool, env_prefix, "T", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

#if OPENSSL_VERSION_NUMBER >= 0x00907000L
      case NID_x500UniqueIdentifier:
#else
      case NID_uniqueIdentifier:
#endif
        k = pstrcat(main_server->pool, env_prefix, "UID", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

      case NID_pkcs9_emailAddress:
        k = pstrcat(main_server->pool, env_prefix, "Email", NULL);
        v = pstrndup(session.pool, (const char *) entry_data, entry_len);
        pr_env_set(main_server->pool, k, v);
        break;

      default:
        break;
    }
  }
}

static void tls_setup_cert_environ(pool *p, const char *env_prefix,
    X509 *cert) {
  char *data = NULL, *k, *v;
  long datalen = 0;
  BIO *bio = NULL;

  if (tls_opts & TLS_OPT_STD_ENV_VARS) {
    char buf[80] = {'\0'};
    ASN1_INTEGER *serial = X509_get_serialNumber(cert);
    const X509_ALGOR *algo = NULL;
    X509_PUBKEY *pubkey = NULL;

    memset(buf, '\0', sizeof(buf));
    pr_snprintf(buf, sizeof(buf) - 1, "%lu", X509_get_version(cert) + 1);
    buf[sizeof(buf)-1] = '\0';

    k = pstrcat(p, env_prefix, "M_VERSION", NULL);
    v = pstrdup(p, buf);
    pr_env_set(p, k, v);

    if (serial->length < 4) {
      memset(buf, '\0', sizeof(buf));
      pr_snprintf(buf, sizeof(buf) - 1, "%lu", ASN1_INTEGER_get(serial));
      buf[sizeof(buf)-1] = '\0';

      k = pstrcat(p, env_prefix, "M_SERIAL", NULL);
      v = pstrdup(p, buf);
      pr_env_set(p, k, v);

    } else {

      /* NOTE: actually, the number is printable, I'm just being lazy. This
       * case is much harder to deal with, and not really worth the effort.
       */
      tls_log("%s", "certificate serial number not printable");
    }

    k = pstrcat(p, env_prefix, "S_DN", NULL);
    v = pstrdup(p, tls_x509_name_oneline(X509_get_subject_name(cert)));
    pr_env_set(p, k, v);

    tls_setup_cert_dn_environ(pstrcat(p, env_prefix, "S_DN_",
      NULL), X509_get_subject_name(cert));

    k = pstrcat(p, env_prefix, "I_DN", NULL);
    v = pstrdup(p, tls_x509_name_oneline(X509_get_issuer_name(cert)));
    pr_env_set(p, k, v);

    tls_setup_cert_dn_environ(pstrcat(p, env_prefix, "I_DN_", NULL),
      X509_get_issuer_name(cert));

    tls_setup_cert_ext_environ(pstrcat(p, env_prefix, "EXT_", NULL), cert);

    bio = BIO_new(BIO_s_mem());
#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    defined(HAVE_LIBRESSL)
    ASN1_TIME_print(bio, X509_get_notBefore(cert));
#else
    ASN1_TIME_print(bio, X509_get0_notBefore(cert));
#endif
    datalen = BIO_get_mem_data(bio, &data);
    data[datalen] = '\0';

    k = pstrcat(p, env_prefix, "V_START", NULL);
    v = pstrdup(p, data);
    pr_env_set(p, k, v);

    BIO_free(bio);

    bio = BIO_new(BIO_s_mem());
#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    defined(HAVE_LIBRESSL)
    ASN1_TIME_print(bio, X509_get_notAfter(cert));
#else
    ASN1_TIME_print(bio, X509_get0_notAfter(cert));
#endif
    datalen = BIO_get_mem_data(bio, &data);
    data[datalen] = '\0';

    k = pstrcat(p, env_prefix, "V_END", NULL);
    v = pstrdup(p, data);
    pr_env_set(p, k, v);

    BIO_free(bio);

    bio = BIO_new(BIO_s_mem());
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
    X509_get0_signature(NULL, &algo, cert);
#else
    algo = cert->cert_info->signature;
#endif /* OpenSSL-1.1.x/LibreSSL-3.5.x and later */
    i2a_ASN1_OBJECT(bio, algo->algorithm);
    datalen = BIO_get_mem_data(bio, &data);
    data[datalen] = '\0';

    k = pstrcat(p, env_prefix, "A_SIG", NULL);
    v = pstrdup(p, data);
    pr_env_set(p, k, v);

    BIO_free(bio);

    bio = BIO_new(BIO_s_mem());
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
    pubkey = X509_get_X509_PUBKEY(cert);
    X509_PUBKEY_get0_param(NULL, NULL, NULL, (X509_ALGOR **) &algo, pubkey);
#else
    pubkey = cert->cert_info->key;
    algo = pubkey->algor;
#endif /* OpenSSL-1.1.x/LibreSSL-3.5.x and later */
    i2a_ASN1_OBJECT(bio, algo->algorithm);
    datalen = BIO_get_mem_data(bio, &data);
    data[datalen] = '\0';

    k = pstrcat(p, env_prefix, "A_KEY", NULL);
    v = pstrdup(p, data);
    pr_env_set(p, k, v);

    BIO_free(bio);
  }

  bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio, cert);
  datalen = BIO_get_mem_data(bio, &data);
  data[datalen] = '\0';

  k = pstrcat(p, env_prefix, "CERT", NULL);
  v = pstrdup(p, data);
  pr_env_set(p, k, v);

  BIO_free(bio);
}

static void tls_setup_environ(pool *p, SSL *ssl) {
  X509 *cert = NULL;
  STACK_OF(X509) *sk_cert_chain = NULL;
  char *k, *v;

  if (!(tls_opts & TLS_OPT_EXPORT_CERT_DATA) &&
      !(tls_opts & TLS_OPT_STD_ENV_VARS)) {
    return;
  }

  if (tls_opts & TLS_OPT_STD_ENV_VARS) {
    SSL_CIPHER *cipher = NULL;
    SSL_SESSION *ssl_session = NULL;
    const char *sni = NULL;

    k = pstrdup(p, "FTPS");
    v = pstrdup(p, "1");
    pr_env_set(p, k, v);

    k = pstrdup(p, "TLS_PROTOCOL");
    v = pstrdup(p, SSL_get_version(ssl));
    pr_env_set(p, k, v);

    /* Process the TLS session-related environ variable. */
    ssl_session = SSL_get_session(ssl);
    if (ssl_session != NULL) {
      const unsigned char *sess_data;
      unsigned int sess_datalen;
      char *sess_id;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
      sess_data = SSL_SESSION_get_id(ssl_session, &sess_datalen);
#else
      sess_datalen = ssl_session->session_id_length;
      sess_data = ssl_session->session_id;
#endif /* OpenSSL-1.1.x and later */

      sess_id = pr_str_bin2hex(p, sess_data, sess_datalen,
        PR_STR_FL_HEX_USE_UC);

      k = pstrdup(p, "TLS_SESSION_ID");
      pr_env_set(p, k, sess_id);
    }

    /* Process the TLS cipher-related environ variables. */
    cipher = (SSL_CIPHER *) SSL_get_current_cipher(ssl);
    if (cipher != NULL) {
      char buf[10] = {'\0'};
      int cipher_bits_used = 0, cipher_bits_possible = 0;

      k = pstrdup(p, "TLS_CIPHER");
      v = pstrdup(p, SSL_CIPHER_get_name(cipher));
      pr_env_set(p, k, v);

      cipher_bits_used = SSL_CIPHER_get_bits(cipher, &cipher_bits_possible);

      if (cipher_bits_used < 56) {
        k = pstrdup(p, "TLS_CIPHER_EXPORT");
        v = pstrdup(p, "1");
        pr_env_set(p, k, v);
      }

      memset(buf, '\0', sizeof(buf));
      pr_snprintf(buf, sizeof(buf), "%d", cipher_bits_possible);
      buf[sizeof(buf)-1] = '\0';

      k = pstrdup(p, "TLS_CIPHER_KEYSIZE_POSSIBLE");
      v = pstrdup(p, buf);
      pr_env_set(p, k, v);

      memset(buf, '\0', sizeof(buf));
      pr_snprintf(buf, sizeof(buf), "%d", cipher_bits_used);
      buf[sizeof(buf)-1] = '\0';

      k = pstrdup(p, "TLS_CIPHER_KEYSIZE_USED");
      v = pstrdup(p, buf);
      pr_env_set(p, k, v);
    }

    sni = pr_table_get(session.notes, "mod_tls.sni", NULL);
    if (sni != NULL) {
      k = pstrdup(p, "TLS_SERVER_NAME");
      v = pstrdup(p, sni);
      pr_env_set(p, k, v);
    }

    k = pstrdup(p, "TLS_LIBRARY_VERSION");
    v = pstrdup(p, OPENSSL_VERSION_TEXT);
    pr_env_set(p, k, v);
  }

  sk_cert_chain = SSL_get_peer_cert_chain(ssl);
  if (sk_cert_chain != NULL) {
    register int i;
    char *data = NULL;
    long datalen = 0;
    BIO *bio = NULL;

    /* Adding TLS_CLIENT_CERT_CHAIN environ variables. */
    for (i = 0; i < sk_X509_num(sk_cert_chain); i++) {
      size_t klen = 256;

      pr_signals_handle();

      k = pcalloc(p, klen);
      pr_snprintf(k, klen - 1, "%s%u", "TLS_CLIENT_CERT_CHAIN", i + 1);

      bio = BIO_new(BIO_s_mem());
      PEM_write_bio_X509(bio, sk_X509_value(sk_cert_chain, i));
      datalen = BIO_get_mem_data(bio, &data);
      data[datalen] = '\0';

      v = pstrdup(p, data);
      pr_env_set(p, k, v);

      BIO_free(bio);
    }
  }

  /* Note: SSL_get_certificate() does NOT increment a reference counter,
   * so we do not call X509_free() on it.
   */
  cert = SSL_get_certificate(ssl);
  if (cert != NULL) {
    tls_setup_cert_environ(p, "TLS_SERVER_", cert);

  } else {
    tls_log("unable to set server certificate environ variables: "
      "Server certificate unavailable");
  }

  cert = SSL_get_peer_certificate(ssl);
  if (cert != NULL) {
    tls_setup_cert_environ(p, "TLS_CLIENT_", cert);
    X509_free(cert);

  } else {
    tls_log("unable to set client certificate environ variables: "
      "Client certificate unavailable");
  }
}

static void tls_setup_notes(pool *p, SSL *ssl) {
  X509 *client_cert = NULL;
  SSL_CIPHER *cipher = NULL;
  const char *sni = NULL;

  (void) pr_table_add_dup(session.notes, "FTPS", "1", 0);
  (void) pr_table_add_dup(session.notes, "TLS_PROTOCOL", SSL_get_version(ssl),
    0);

  /* Process the TLS cipher-related values. */
  cipher = (SSL_CIPHER *) SSL_get_current_cipher(ssl);
  if (cipher != NULL) {
    (void) pr_table_add_dup(session.notes, "TLS_CIPHER",
      SSL_CIPHER_get_name(cipher), 0);
  }

  sni = pr_table_get(session.notes, "mod_tls.sni", NULL);
  if (sni != NULL) {
    (void) pr_table_add_dup(session.notes, "TLS_SERVER_NAME", sni, 0);
  }

  client_cert = SSL_get_peer_certificate(ssl);
  if (client_cert != NULL) {
    const X509_ALGOR *algo = NULL;
    X509_PUBKEY *pubkey = NULL;
    BIO *bio = NULL;
    char *data = NULL;
    long datalen = 0;

    /* Client cert CN */
    data = tls_get_cert_cn(p, client_cert);
    if (data != NULL) {
      (void) pr_table_add_dup(session.notes, "TLS_CLIENT_S_DN_CN", data, 0);
    }

    /* Client cert key algo */
    bio = BIO_new(BIO_s_mem());
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    pubkey = X509_get_X509_PUBKEY(client_cert);
    X509_PUBKEY_get0_param(NULL, NULL, NULL, (X509_ALGOR **) &algo, pubkey);
#else
    pubkey = client_cert->cert_info->key;
    algo = pubkey->algor;
#endif /* OpenSSL-1.1.x and later */
    i2a_ASN1_OBJECT(bio, algo->algorithm);
    datalen = BIO_get_mem_data(bio, &data);
    data[datalen] = '\0';

    (void) pr_table_add_dup(session.notes, "TLS_CLIENT_A_KEY", data, 0);
    BIO_free(bio);

    /* Client cert signature algorithm. */
    bio = BIO_new(BIO_s_mem());
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
    X509_get0_signature(NULL, &algo, client_cert);
#else
    algo = client_cert->cert_info->signature;
#endif /* OpenSSL-1.1.x/Libre-3.5.x and later */
    i2a_ASN1_OBJECT(bio, algo->algorithm);
    datalen = BIO_get_mem_data(bio, &data);
    data[datalen] = '\0';

    (void) pr_table_add_dup(session.notes, "TLS_CLIENT_A_SIG", data, 0);
    BIO_free(bio);
  }

  (void) pr_table_add_dup(session.notes, "TLS_LIBRARY_VERSION",
    OPENSSL_VERSION_TEXT, 0);
}

static int tls_verify_cb(int ok, X509_STORE_CTX *ctx) {
  config_rec *c;
  int verify_error = 0;

  /* We can configure the server to skip the peer's cert verification */
  if (!(tls_flags & TLS_SESS_VERIFY_CLIENT_REQUIRED) &&
      !(tls_flags & TLS_SESS_VERIFY_CLIENT_OPTIONAL)) {
    return 1;
  }

  c = find_config(main_server->conf, CONF_PARAM, "TLSVerifyOrder", FALSE);
  if (c != NULL) {
    register unsigned int i;

    for (i = 0; i < c->argc; i++) {
      char *mech = c->argv[i];

      if (strcasecmp(mech, "crl") == 0) {
        ok = tls_verify_crl(ok, ctx);
        if (!ok) {
          break;
        }

      } else if (strcasecmp(mech, "ocsp") == 0) {
        ok = tls_verify_ocsp(ok, ctx);
        if (!ok) {
          break;
        }
      }
    }

  } else {
    /* If no TLSVerifyOrder was specified, default to the old behavior of
     * always checking CRLs, if configured, and not paying attention to
     * any AIA attributes (i.e. no use of OCSP).
     */
    ok = tls_verify_crl(ok, ctx);
  }

  if (!ok) {
    X509 *cert;
    int ctx_error, depth;

    cert = X509_STORE_CTX_get_current_cert(ctx);
    depth = X509_STORE_CTX_get_error_depth(ctx);

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    verify_error = X509_STORE_CTX_get_error(ctx);
#else
    verify_error = ctx->error;
#endif /* OpenSSL-1.1.x and later */

    tls_log("error: unable to verify certificate at depth %d: %s", depth,
      X509_verify_cert_error_string(verify_error));
    tls_log("error: cert subject: %s", tls_x509_name_oneline(
      X509_get_subject_name(cert)));
    tls_log("error: cert issuer: %s", tls_x509_name_oneline(
      X509_get_issuer_name(cert)));

    /* Catch a too long certificate chain here. */
    if (depth > tls_verify_depth) {
      /* Note that by setting this error, we are effectively overriding
       * the previous verification error; this could become the value of
       * the subsequent ctx_error.
       */
      X509_STORE_CTX_set_error(ctx, X509_V_ERR_CERT_CHAIN_TOO_LONG);
    }

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    ctx_error = X509_STORE_CTX_get_error(ctx);
#else
    ctx_error = ctx->error;
#endif /* OpenSSL-1.1.x and later */

    switch (ctx_error) {
      case X509_V_ERR_APPLICATION_VERIFICATION:
      case X509_V_ERR_CERT_CHAIN_TOO_LONG:
      case X509_V_ERR_CERT_HAS_EXPIRED:
      case X509_V_ERR_CERT_REVOKED:
      case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
      case X509_V_ERR_INVALID_PURPOSE:
      case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
      case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
      case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        tls_log("client certificate failed verification: %s",
          X509_verify_cert_error_string(ctx_error));
        ok = 0;
        break;

      default:
        tls_log("error verifying client certificate: [%d] %s",
          ctx_error, X509_verify_cert_error_string(ctx_error));
        ok = 0;
        break;
    }
  }

  if (ok) {
    pr_event_generate("mod_tls.verify-client", NULL);

  } else {
    pr_event_generate("mod_tls.verify-client-failed", &verify_error);
  }

  return ok;
}

static int tls_verify_crl(int ok, X509_STORE_CTX *ctx) {
  register int i = 0;
  X509_NAME *subject = NULL, *issuer = NULL;
  X509 *xs = NULL;
  STACK_OF(X509_CRL) *crls = NULL;
  int res, verify_error;

  verify_error = X509_STORE_CTX_get_error(ctx);
  xs = X509_STORE_CTX_get_current_cert(ctx);
  subject = X509_get_subject_name(xs);
  issuer = X509_get_issuer_name(xs);

  pr_trace_msg(trace_channel, 15,
    "verifying cert: subject = '%s', issuer = '%s', error = %s (ok = %d)",
    tls_x509_name_oneline(subject), tls_x509_name_oneline(issuer),
    X509_verify_cert_error_string(verify_error), ok);

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
  crls = X509_STORE_CTX_get1_crls(ctx, issuer);
#elif OPENSSL_VERSION_NUMBER >= 0x10000000L && \
      !defined(HAVE_LIBRESSL)
  crls = X509_STORE_get1_crls(ctx, issuer);
#else
  /* Your OpenSSL is before 1.0.0.  You really need to upgrade. */
  crls = NULL;
#endif /* OpenSSL-1.1.x and later */

  if (crls != NULL) {
    unsigned int crl_count;
    X509_OBJECT *obj = NULL;
    EVP_PKEY *pkey;

    obj = X509_STORE_CTX_get_obj_by_subject(ctx, X509_LU_X509, issuer);
    if (obj == NULL) {
      pr_trace_msg(trace_channel, 1,
        "error getting CRL issuer '%s' certificate: %s",
        tls_x509_name_oneline(issuer), tls_get_errors());
      X509_STORE_CTX_set_error(ctx, X509_V_ERR_UNABLE_TO_GET_CRL);
      sk_X509_CRL_free(crls);
      return FALSE;
    }

    pkey = X509_get_pubkey(X509_OBJECT_get0_X509(obj));
    X509_OBJECT_free(obj);

    if (pkey == NULL) {
      pr_trace_msg(trace_channel, 1,
        "error getting CRL issuer '%s' public key: %s",
        tls_x509_name_oneline(issuer), tls_get_errors());
      X509_STORE_CTX_set_error(ctx, X509_V_ERR_CRL_SIGNATURE_FAILURE);
      sk_X509_CRL_free(crls);
      return FALSE;
    }

    crl_count = sk_X509_CRL_num(crls);
    for (i = 0; i < crl_count; i++) {
      X509_CRL *crl = NULL;
      X509_NAME *crl_issuer = NULL;
      char buf[512];
      int len;
      BIO *b = BIO_new(BIO_s_mem());

      crl = sk_X509_CRL_value(crls, i);
      BIO_printf(b, "Issuer: ");
      crl_issuer = X509_CRL_get_issuer(crl);
      X509_NAME_print(b, crl_issuer, 0);

      BIO_printf(b, ", lastUpdate: ");
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
      ASN1_UTCTIME_print(b, X509_CRL_get0_lastUpdate(crl));
#else
      ASN1_UTCTIME_print(b, crl->crl->lastUpdate);
#endif /* OpenSSL-1.1.x and later */

      BIO_printf(b, ", nextUpdate: ");
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
      ASN1_UTCTIME_print(b, X509_CRL_get0_nextUpdate(crl));
#else
      ASN1_UTCTIME_print(b, crl->crl->nextUpdate);
#endif /* OpenSSL-1.1.x and later */

      len = BIO_read(b, buf, sizeof(buf) - 1);
      if ((size_t) len >= sizeof(buf)) {
        len = sizeof(buf)-1;
      }
      buf[len] = '\0';

      BIO_free(b);
      tls_log("CRL: %s", buf);

      /* Verify the signature on this CRL using the public key from the issuer
       * of the CRL.
       */
      res = X509_CRL_verify(crl, pkey);
      if (res <= 0) {
        tls_log("invalid signature on CRL: %s", tls_get_errors());

        X509_STORE_CTX_set_error(ctx, X509_V_ERR_CRL_SIGNATURE_FAILURE);
        sk_X509_CRL_free(crls);
        return FALSE;
      }

      /* Check the last update of the CRL to make sure it's valid. */
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
      res = X509_cmp_current_time(X509_CRL_get0_lastUpdate(crl));
#else
      res = X509_cmp_current_time(X509_CRL_get_lastUpdate(crl));
#endif /* OpenSSL 1.1.x and later */
      if (res == 0) {
        tls_log("CRL has invalid lastUpdate field: %s", tls_get_errors());

        X509_STORE_CTX_set_error(ctx,
          X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD);
        sk_X509_CRL_free(crls);
        return FALSE;
      }

      if (res > 0) {
        tls_log("%s", "CRL is not yet valid, ignoring all certificates until "
          "an updated CRL is obtained");

        X509_STORE_CTX_set_error(ctx, X509_V_ERR_CRL_NOT_YET_VALID);
        sk_X509_CRL_free(crls);
        return TRUE;
      }

      /* Check next update of CRL to make sure it's not expired. */
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
      res = X509_cmp_current_time(X509_CRL_get0_nextUpdate(crl));
#else
      res = X509_cmp_current_time(X509_CRL_get_nextUpdate(crl));
#endif /* OpenSSL 1.1.x and later */

      if (res == 0) {
        tls_log("CRL has invalid lastUpdate field: %s", tls_get_errors());

        X509_STORE_CTX_set_error(ctx,
          X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD);
        sk_X509_CRL_free(crls);
        return FALSE;
      }

      if (res == 0) {
        tls_log("CRL has invalid nextUpdate field: %s", tls_get_errors());

        X509_STORE_CTX_set_error(ctx,
          X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD);
        sk_X509_CRL_free(crls);
        return FALSE;
      }

      if (res < 0) {
        /* XXX This is a bit draconian, rejecting all certificates if the CRL
         * has expired.  See also Bug#3216, about automatically reloading
         * the CRL file when it has expired.
         */
        tls_log("%s", "CRL is expired, revoking all certificates until an "
          "updated CRL is obtained");

        X509_STORE_CTX_set_error(ctx, X509_V_ERR_CRL_HAS_EXPIRED);
        sk_X509_CRL_free(crls);
        return FALSE;
      }
    }

    sk_X509_CRL_free(crls);
    crls = NULL;
  }

  return ok;
}

#if OPENSSL_VERSION_NUMBER > 0x000907000L && defined(PR_USE_OPENSSL_OCSP)
static int tls_verify_ocsp_url(X509_STORE_CTX *ctx, X509 *cert,
    const char *url) {
  BIO *conn;
  X509 *issuing_cert = NULL;
  X509_NAME *subj = NULL;
  X509_STORE *store = NULL;
  const char *subj_name;
  char *host = NULL, *port = NULL, *uri = NULL;
  int ok = FALSE, res = 0 , use_ssl = 0;
  int ocsp_status, ocsp_cert_status, ocsp_reason;
  OCSP_REQUEST *req = NULL;
  OCSP_RESPONSE *resp = NULL;
  OCSP_BASICRESP *basic_resp = NULL;
  SSL_CTX *ocsp_ssl_ctx = NULL;

  if (cert == NULL ||
      url == NULL) {
    return FALSE;
  }

  subj = X509_get_subject_name(cert);
  subj_name = tls_x509_name_oneline(subj);

  tls_log("checking OCSP URL '%s' for client cert '%s'", url, subj_name);

  /* Current OpenSSL implementation of OCSP_parse_url() guarantees that
   * host, port, and uri will never be NULL.  Nice.
   */
  if (OCSP_parse_url((char *) url, &host, &port, &uri, &use_ssl) != 1) {
    tls_log("error parsing OCSP URL '%s': %s", url, tls_get_errors());
    return FALSE;
  }

  tls_log("connecting to OCSP responder at host '%s', port '%s', URI '%s'%s",
    host, port, uri, use_ssl ? ", using SSL/TLS" : "");

  /* Connect to the OCSP responder indicated */
  conn = BIO_new_connect(host);
  if (conn == NULL) {
    tls_log("error creating connection BIO: %s", tls_get_errors());

    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    return FALSE;
  }

  BIO_set_conn_port(conn, port);

  /* If use_ssl is true, we need to a) create an SSL_CTX object to use, and
   * push it onto the BIO chain so that SSL/TLS actually happens.
   * When doing so, what version of SSL/TLS should we use?
   */
  if (use_ssl == 1) {
    BIO *ocsp_ssl_bio = NULL;

    /* Note: this code used openssl/apps/ocsp.c as a model. */
    ocsp_ssl_ctx = SSL_CTX_new(SSLv23_client_method());
    if (ocsp_ssl_ctx != NULL) {
      SSL_CTX_set_mode(ocsp_ssl_ctx, SSL_MODE_AUTO_RETRY);

      ocsp_ssl_bio = BIO_new_ssl(ocsp_ssl_ctx, 1);
      BIO_push(ocsp_ssl_bio, conn);

    } else {
      tls_log("error allocating SSL_CTX object for OCSP verification: %s",
        tls_get_errors());
    }
  }

  res = ocsp_connect(session.pool, conn, 0);
  if (res < 0) {
    tls_log("error connecting to OCSP URL '%s': %s", url, tls_get_errors());

    if (ocsp_ssl_ctx != NULL) {
      SSL_CTX_free(ocsp_ssl_ctx);
    }

    BIO_free_all(conn);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    /* XXX Should we give the client the benefit of the doubt, and allow
     * it to connect even though we can't talk to its OCSP responder?  Or
     * do we fail-close, and penalize the client if the OCSP responder is
     * down (e.g. for maintenance)?
     */

    return FALSE;
  }

  /* XXX Why are we querying the OCSP responder about the client cert's
   * issuing CA, rather than querying about the client cert itself?
   */

  res = X509_STORE_CTX_get1_issuer(&issuing_cert, ctx, cert);
  if (res != 1) {
    tls_log("error retrieving issuing cert for client cert '%s': %s",
      subj_name, tls_get_errors());

    if (ocsp_ssl_ctx != NULL) {
      SSL_CTX_free(ocsp_ssl_ctx);
    }

    BIO_free_all(conn);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    return FALSE;
  }

  req = ocsp_get_request(session.pool, cert, issuing_cert);
  if (req == NULL) {
    if (ocsp_ssl_ctx != NULL) {
      SSL_CTX_free(ocsp_ssl_ctx);
    }

    X509_free(issuing_cert);
    BIO_free_all(conn);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    return FALSE;
  }

# if 0
  /* XXX ideally we would set the requestor name to the subject name of the
   * cert configured via TLS{DSA,RSA}CertificateFile here.
   */
  if (OCSP_request_set1_name(req, /* server cert X509_NAME subj name */) != 1) {
    tls_log("error adding requestor name '%s' to OCSP request: %s",
      requestor_name, tls_get_errors());

    if (ocsp_ssl_ctx != NULL) {
      SSL_CTX_free(ocsp_ssl_ctx);
    }

    OCSP_REQUEST_free(req);
    X509_free(issuing_cert);
    BIO_free_all(conn);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    return FALSE;
  }
# endif

  if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
    BIO *bio;

    bio = BIO_new(BIO_s_mem());
    if (bio != NULL) {
      if (OCSP_REQUEST_print(bio, req, 0) == 1) {
        char *data = NULL;
        long datalen;

        datalen = BIO_get_mem_data(bio, &data);
        if (data != NULL) {
          data[datalen] = '\0';
          tls_log("sending OCSP request (%ld bytes):\n%s", datalen, data);
        }
      }

      BIO_free(bio);
    }
  }

  resp = ocsp_send_request(session.pool, conn, host, uri, req, 0);
  if (resp == NULL) {
    tls_log("error receiving response from OCSP responder at '%s': %s", url,
      tls_get_errors());

    if (ocsp_ssl_ctx != NULL) {
      SSL_CTX_free(ocsp_ssl_ctx);
    }

    OCSP_REQUEST_free(req);
    X509_free(issuing_cert);
    BIO_free_all(conn);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    return FALSE;
  }

  if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
    BIO *bio;

    bio = BIO_new(BIO_s_mem());
    if (bio != NULL) {
      if (OCSP_RESPONSE_print(bio, resp, 0) == 1) {
        char *data = NULL;
        long datalen;

        datalen = BIO_get_mem_data(bio, &data);
        if (data != NULL) {
          data[datalen] = '\0';
          tls_log("received OCSP response (%ld bytes):\n%s", datalen, data);
        }
      }

      BIO_free(bio);
    }
  }

  tls_log("checking response from OCSP responder at URL '%s' for client cert "
    "'%s'", url, subj_name);

  basic_resp = OCSP_response_get1_basic(resp);
  if (basic_resp == NULL) {
    tls_log("error retrieving basic response from OCSP responder at '%s': %s",
      url, tls_get_errors());

    if (ocsp_ssl_ctx != NULL) {
      SSL_CTX_free(ocsp_ssl_ctx);
    }

    OCSP_RESPONSE_free(resp);
    OCSP_REQUEST_free(req);
    X509_free(issuing_cert);
    BIO_free_all(conn);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    return FALSE;
  }

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(HAVE_LIBRESSL)) || \
    (defined(HAVE_LIBRESSL) && LIBRESSL_VERSION_NUMBER >= 0x3050000L)
  store = X509_STORE_CTX_get0_store(ctx);
#else
  store = ctx->ctx;
#endif /* OpenSSL-1.1.x/LibreSSL-3.5.x and later */
  res = OCSP_basic_verify(basic_resp, NULL, store, 0);
  if (res != 1) {
    tls_log("error verifying basic response from OCSP responder at '%s': %s",
      url, tls_get_errors());

    if (ocsp_ssl_ctx != NULL) {
      SSL_CTX_free(ocsp_ssl_ctx);
    }

    OCSP_REQUEST_free(req);
    OCSP_BASICRESP_free(basic_resp);
    OCSP_RESPONSE_free(resp);
    X509_free(issuing_cert);
    BIO_free_all(conn);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    return FALSE;
  }

  /* Now that we have verified the response, we can check the response status.
   * If we only looked at the status first, then a malicious responder
   * could be tricking us, e.g.:
   *
   *  http://www.thoughtcrime.org/papers/ocsp-attack.pdf
   */

  ocsp_status = OCSP_response_status(resp);
  if (ocsp_status != OCSP_RESPONSE_STATUS_SUCCESSFUL) {
    tls_log("unable to verify client cert '%s' via OCSP responder at '%s': "
      "response status '%s' (%d)", subj_name, url,
      OCSP_response_status_str(ocsp_status), ocsp_status);

    if (ocsp_ssl_ctx != NULL) {
      SSL_CTX_free(ocsp_ssl_ctx);
    }

    OCSP_REQUEST_free(req);
    OCSP_BASICRESP_free(basic_resp);
    OCSP_RESPONSE_free(resp);
    X509_free(issuing_cert);
    BIO_free_all(conn);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    switch (ocsp_status) {
      case OCSP_RESPONSE_STATUS_MALFORMEDREQUEST:
      case OCSP_RESPONSE_STATUS_INTERNALERROR:
      case OCSP_RESPONSE_STATUS_SIGREQUIRED:
      case OCSP_RESPONSE_STATUS_UNAUTHORIZED:
      case OCSP_RESPONSE_STATUS_TRYLATER:
        /* XXX For now, for the above OCSP response reasons, give the client
         * the benefit of the doubt, since all of the above non-success
         * response codes indicate either a) an issue with the OCSP responder
         * outside of the client's control, or b) an issue with our OCSP
         * implementation (e.g. SIGREQUIRED, UNAUTHORIZED).
         */
        ok = TRUE;
        break;

      default:
        ok = FALSE;
    }

    return ok;
  }

  if (ocsp_check_cert_status(session.pool, cert, issuing_cert, basic_resp,
      &ocsp_cert_status, &ocsp_reason) < 0) {
    tls_log("unable to retrieve cert status from OCSP response: %s",
      tls_get_errors());

    if (ocsp_ssl_ctx != NULL) {
      SSL_CTX_free(ocsp_ssl_ctx);
    }

    OCSP_REQUEST_free(req);
    OCSP_BASICRESP_free(basic_resp);
    OCSP_RESPONSE_free(resp);
    X509_free(issuing_cert);
    BIO_free_all(conn);
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(uri);

    return FALSE;
  }

  tls_log("client cert '%s' has '%s' (%d) status according to OCSP responder "
    "at '%s'", subj_name, OCSP_cert_status_str(ocsp_cert_status),
    ocsp_cert_status, url);

  switch (ocsp_cert_status) {
    case V_OCSP_CERTSTATUS_GOOD:
      ok = TRUE;
      break;

    case V_OCSP_CERTSTATUS_REVOKED:
      tls_log("client cert '%s' has '%s' status due to: %s", subj_name,
        OCSP_cert_status_str(ocsp_status), OCSP_crl_reason_str(ocsp_reason));
      ok = FALSE;
      break;

    case V_OCSP_CERTSTATUS_UNKNOWN:
      /* If the client cert points to an OCSP responder which claims not to
       * know about the client cert, then we shouldn't trust that client
       * cert.  Otherwise, a client could present a cert pointing to an
       * OCSP responder which they KNOW won't know about the client cert,
       * and could then slip through the verification process.
       */
      ok = FALSE;
      break;

    default:
      ok = FALSE;
  }

  if (ocsp_ssl_ctx != NULL) {
    SSL_CTX_free(ocsp_ssl_ctx);
  }

  OCSP_REQUEST_free(req);
  OCSP_BASICRESP_free(basic_resp);
  OCSP_RESPONSE_free(resp);
  X509_free(issuing_cert);
  BIO_free_all(conn);
  OPENSSL_free(host);
  OPENSSL_free(port);
  OPENSSL_free(uri);

  return ok;
}
#endif

static int tls_verify_ocsp(int ok, X509_STORE_CTX *ctx) {
#if OPENSSL_VERSION_NUMBER > 0x000907000L && defined(PR_USE_OPENSSL_OCSP)
  register int i;
  X509 *cert;
  const char *subj;
  STACK_OF(ACCESS_DESCRIPTION) *descs;
  pool *tmp_pool = NULL;
  array_header *ocsp_urls = NULL;

  /* Set a default verification error here; it will be superseded as needed
   * later during the verification process.
   */
  X509_STORE_CTX_set_error(ctx, X509_V_ERR_APPLICATION_VERIFICATION);

  cert = X509_STORE_CTX_get_current_cert(ctx);
  if (cert == NULL) {
    return ok;
  }

  subj = tls_x509_name_oneline(X509_get_subject_name(cert));

  descs = X509_get_ext_d2i(cert, NID_info_access, NULL, NULL);
  if (descs == NULL) {
    tls_log("Client cert '%s' contained no AuthorityInfoAccess attribute, "
      "unable to verify via OCSP", subj);
    return ok;
  }

  for (i = 0; i < sk_ACCESS_DESCRIPTION_num(descs); i++) {
    ACCESS_DESCRIPTION *desc;

    desc = sk_ACCESS_DESCRIPTION_value(descs, i);
    if (OBJ_obj2nid(desc->method) == NID_ad_OCSP) {
      /* Found an OCSP AuthorityInfoAccess attribute */

      if (desc->location->type != GEN_URI) {
        /* Not a valid URI, ignore it. */
        continue;
      }

      /* Add this URL to the list of OCSP URLs to check. */
      if (ocsp_urls == NULL) {
        tmp_pool = make_sub_pool(session.pool);
        ocsp_urls = make_array(tmp_pool, 1, sizeof(char *));
      }

      *((char **) push_array(ocsp_urls)) = pstrdup(tmp_pool,
        (char *) desc->location->d.uniformResourceIdentifier->data);
    }
  }

  if (ocsp_urls == NULL) {
    tls_log("found no OCSP URLs in AuthorityInfoAccess attribute for client "
      "cert '%s', unable to verify via OCSP", subj);
    AUTHORITY_INFO_ACCESS_free(descs);
    return ok;
  }

  tls_log("found %u OCSP %s in AuthorityInfoAccess attribute for client cert "
    "'%s'", ocsp_urls->nelts, ocsp_urls->nelts != 1 ? "URLs" : "URL", subj);

  /* Check each of the URLs. */
  for (i = 0; i < (int) ocsp_urls->nelts; i++) {
    char *url = ((char **) ocsp_urls->elts)[i];

    ok = tls_verify_ocsp_url(ctx, cert, url);
    if (ok)
      break;
  }

  destroy_pool(tmp_pool);
  AUTHORITY_INFO_ACCESS_free(descs);

  return ok;
#else
  return ok;
#endif
}

static ssize_t tls_write(SSL *ssl, const void *buf, size_t len) {
  ssize_t count;
  int lineno, xerrno = 0;

  /* Help ensure we have a clean/trustable errno value. */
  errno = 0;

  lineno = __LINE__ + 1;
  count = SSL_write(ssl, buf, len);
  xerrno = errno;

  if (count < 0) {
    long err = SSL_get_error(ssl, count);

    /* write(2) returns only the generic error number -1 */
    count = -1;

    switch (err) {
      case SSL_ERROR_WANT_READ:
      case SSL_ERROR_WANT_WRITE:
        /* Simulate an EINTR in case OpenSSL wants to write more. */
        xerrno = EINTR;
        break;

      case SSL_ERROR_SSL:
      case SSL_ERROR_SYSCALL:
      default:
        tls_fatal_error(err, lineno);
        break;
    }
  }

  errno = xerrno;
  return count;
}

static char *tls_x509_name_oneline(X509_NAME *x509_name) {
  static char buf[1024] = {'\0'};

  /* If we are using OpenSSL 0.9.6 or newer, we want to use
   * X509_NAME_print_ex() instead of X509_NAME_oneline().
   */

#if OPENSSL_VERSION_NUMBER < 0x000906000L
  memset(&buf, '\0', sizeof(buf));
  return X509_NAME_oneline(x509_name, buf, sizeof(buf)-1);
#else

  /* Sigh...do it the hard way. */
  BIO *mem = BIO_new(BIO_s_mem());
  char *data = NULL;
  long datalen = 0;
  int ok;

  ok = X509_NAME_print_ex(mem, x509_name, 0, XN_FLAG_ONELINE);
  if (ok) {
    datalen = BIO_get_mem_data(mem, &data);

    if (data) {
      memset(&buf, '\0', sizeof(buf));

      if ((size_t) datalen >= sizeof(buf)) {
        datalen = sizeof(buf)-1;
      }

      memcpy(buf, data, datalen);

      buf[datalen] = '\0';
      buf[sizeof(buf)-1] = '\0';

      BIO_free(mem);
      return buf;
    }
  }

  BIO_free(mem);
  return NULL;
#endif /* OPENSSL_VERSION_NUMBER >= 0x000906000 */
}

/* Session cache API */

struct tls_scache {
  struct tls_scache *next, *prev;

  const char *name;
  tls_sess_cache_t *cache;
};

static pool *tls_sess_cache_pool = NULL;
static struct tls_scache *tls_sess_caches = NULL;
static unsigned int tls_sess_ncaches = 0;

int tls_sess_cache_register(const char *name, tls_sess_cache_t *cache) {
  struct tls_scache *sc;

  if (name == NULL ||
      cache == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (tls_sess_cache_pool == NULL) {
    tls_sess_cache_pool = make_sub_pool(permanent_pool);
    pr_pool_tag(tls_sess_cache_pool, "TLS Session Cache API Pool");
  }

  /* Make sure this cache has not already been registered. */
  if (tls_sess_cache_get_cache(name) != NULL) {
    errno = EEXIST;
    return -1;
  }

  sc = pcalloc(tls_sess_cache_pool, sizeof(struct tls_scache));

  /* XXX Should this name string be dup'd from the tls_sess_cache_pool? */
  sc->name = name;
  cache->cache_name = pstrdup(tls_sess_cache_pool, name);
  sc->cache = cache;

  if (tls_sess_caches) {
    sc->next = tls_sess_caches;

  } else {
    sc->next = NULL;
  }

  tls_sess_caches = sc;
  tls_sess_ncaches++;

  return 0;
}

int tls_sess_cache_unregister(const char *name) {
  struct tls_scache *sc;

  if (name == NULL) {
    errno = EINVAL;
    return -1;
  }

  for (sc = tls_sess_caches; sc; sc = sc->next) {
    if (strcmp(sc->name, name) == 0) {

      if (sc->prev) {
        sc->prev->next = sc->next;

      } else {
        /* If prev is NULL, this is the head of the list. */
        tls_sess_caches = sc->next;
      }

      if (sc->next) {
        sc->next->prev = sc->prev;
      }

      sc->next = sc->prev = NULL;
      tls_sess_ncaches--;

      /* If the session cache being unregistered is in use, update the
       * session-cache-in-use pointer.
       */
      if (sc->cache == tls_sess_cache) {
        tls_sess_cache_close();
        tls_sess_cache = NULL;
      }

      /* NOTE: a counter should be kept of the number of unregistrations,
       * as the memory for a registration is not freed on unregistration.
       */

      return 0;
    }
  }

  errno = ENOENT;
  return -1;
}

static tls_sess_cache_t *tls_sess_cache_get_cache(const char *name) {
  struct tls_scache *sc;

  if (name == NULL) {
    errno = EINVAL;
    return NULL;
  }

  for (sc = tls_sess_caches; sc; sc = sc->next) {
    if (strcmp(sc->name, name) == 0) {
      return sc->cache;
    }
  }

  errno = ENOENT;
  return NULL;
}

static long tls_sess_cache_get_cache_mode(void) {
  if (tls_sess_cache == NULL) {
    return 0;
  }

  return tls_sess_cache->cache_mode;
}

static int tls_sess_cache_open(char *info, long timeout) {
  int res;

  if (tls_sess_cache == NULL) {
    errno = ENOSYS;
    return -1;
  }

  res = (tls_sess_cache->open)(tls_sess_cache, info, timeout);
  return res;
}

static int tls_sess_cache_close(void) {
  int res;

  if (tls_sess_cache == NULL) {
    errno = ENOSYS;
    return -1;
  }

  res = (tls_sess_cache->close)(tls_sess_cache);
  return res;
}

#ifdef PR_USE_CTRLS
static int tls_sess_cache_clear(void) {
  int res;

  if (tls_sess_cache == NULL) {
    errno = ENOSYS;
    return -1;
  }

  res = (tls_sess_cache->clear)(tls_sess_cache);
  return res;
}

static int tls_sess_cache_remove(void) {
  int res;

  if (tls_sess_cache == NULL) {
    errno = ENOSYS;
    return -1;
  }

  res = (tls_sess_cache->remove)(tls_sess_cache);
  return res;
}

static void sess_cache_printf(void *ctrl, const char *fmt, ...) {
  char buf[PR_TUNABLE_BUFFER_SIZE];
  va_list msg;

  memset(buf, '\0', sizeof(buf));

  va_start(msg, fmt);
  pr_vsnprintf(buf, sizeof(buf), fmt, msg);
  va_end(msg);

  buf[sizeof(buf)-1] = '\0';
  pr_ctrls_add_response(ctrl, "%s", buf);
}

static int tls_sess_cache_status(pr_ctrls_t *ctrl, int flags) {
  int res = 0;

  if (tls_sess_cache != NULL) {
    res = (tls_sess_cache->status)(tls_sess_cache, sess_cache_printf, ctrl,
      flags);
    return res;
  }

  pr_ctrls_add_response(ctrl, "No TLSSessionCache configured");
  return res;
}
#endif /* PR_USE_CTRLS */

/* OCSP response cache API */

struct tls_ocache {
  struct tls_ocache *next, *prev;

  const char *name;
  tls_ocsp_cache_t *cache;
};

#if defined(PR_USE_OPENSSL_OCSP)
static pool *tls_ocsp_cache_pool = NULL;
static struct tls_ocache *tls_ocsp_caches = NULL;
static unsigned int tls_ocsp_ncaches = 0;
#endif /* PR_USE_OPENSSL_OCSP */

int tls_ocsp_cache_register(const char *name, tls_ocsp_cache_t *cache) {
#if defined(PR_USE_OPENSSL_OCSP)
  struct tls_ocache *oc;

  if (name == NULL ||
      cache == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (tls_ocsp_cache_pool == NULL) {
    tls_ocsp_cache_pool = make_sub_pool(permanent_pool);
    pr_pool_tag(tls_ocsp_cache_pool, "TLS OCSP Response Cache API Pool");
  }

  /* Make sure this cache has not already been registered. */
  if (tls_ocsp_cache_get_cache(name) != NULL) {
    errno = EEXIST;
    return -1;
  }

  oc = pcalloc(tls_ocsp_cache_pool, sizeof(struct tls_ocache));

  /* XXX Should this name string be dup'd from the tls_ocsp_cache_pool? */
  oc->name = name;
  cache->cache_name = pstrdup(tls_ocsp_cache_pool, name);
  oc->cache = cache;

  if (tls_ocsp_caches != NULL) {
    oc->next = tls_ocsp_caches;

  } else {
    oc->next = NULL;
  }

  tls_ocsp_caches = oc;
  tls_ocsp_ncaches++;

  return 0;
#else
  errno = ENOSYS;
  return -1;
#endif /* PR_USE_OPENSSL_OCSP */
}

int tls_ocsp_cache_unregister(const char *name) {
#if defined(PR_USE_OPENSSL_OCSP)
  struct tls_ocache *oc;

  if (name == NULL) {
    errno = EINVAL;
    return -1;
  }

  for (oc = tls_ocsp_caches; oc; oc = oc->next) {
    if (strcmp(oc->name, name) == 0) {

      if (oc->prev) {
        oc->prev->next = oc->next;

      } else {
        /* If prev is NULL, this is the head of the list. */
        tls_ocsp_caches = oc->next;
      }

      if (oc->next) {
        oc->next->prev = oc->prev;
      }

      oc->next = oc->prev = NULL;
      tls_ocsp_ncaches--;

      /* If the OCSP response cache being unregistered is in use, update the
       * ocsp-cache-in-use pointer.
       */
      if (oc->cache == tls_ocsp_cache) {
        tls_ocsp_cache_close();
        tls_ocsp_cache = NULL;
      }

      /* NOTE: a counter should be kept of the number of unregistrations,
       * as the memory for a registration is not freed on unregistration.
       */

      return 0;
    }
  }

  errno = ENOENT;
  return -1;
#else
  errno = ENOSYS;
  return -1;
#endif /* PR_USE_OPENSSL_OCSP */
}

static tls_ocsp_cache_t *tls_ocsp_cache_get_cache(const char *name) {
#if defined(PR_USE_OPENSSL_OCSP)
  struct tls_ocache *oc;

  if (name == NULL) {
    errno = EINVAL;
    return NULL;
  }

  for (oc = tls_ocsp_caches; oc; oc = oc->next) {
    if (strcmp(oc->name, name) == 0) {
      return oc->cache;
    }
  }

  errno = ENOENT;
  return NULL;
#else
  errno = ENOSYS;
  return NULL;
#endif /* PR_USE_OPENSSL_OCSP */
}

static int tls_ocsp_cache_open(char *info) {
#if defined(PR_USE_OPENSSL_OCSP)
  int res;

  if (tls_ocsp_cache == NULL) {
    errno = ENOSYS;
    return -1;
  }

  res = (tls_ocsp_cache->open)(tls_ocsp_cache, info);
  return res;
#else
  errno = ENOSYS;
  return -1;
#endif /* PR_USE_OPENSSL_OCSP */
}

static int tls_ocsp_cache_close(void) {
#if defined(PR_USE_OPENSSL_OCSP)
  int res;

  if (tls_ocsp_cache == NULL) {
    errno = ENOSYS;
    return -1;
  }

  res = (tls_ocsp_cache->close)(tls_ocsp_cache);
  return res;
#else
  errno = ENOSYS;
  return -1;
#endif /* PR_USE_OPENSSL_OCSP */
}

#ifdef PR_USE_CTRLS
static int tls_ocsp_cache_clear(void) {
# if defined(PR_USE_OPENSSL_OCSP)
  int res;

  if (tls_ocsp_cache == NULL) {
    errno = ENOSYS;
    return -1;
  }

  res = (tls_ocsp_cache->clear)(tls_ocsp_cache);
  return res;
# else
  errno = ENOSYS;
  return -1;
# endif /* PR_USE_OPENSSL_OCSP */
}

static int tls_ocsp_cache_remove(void) {
# if defined(PR_USE_OPENSSL_OCSP)
  int res;

  if (tls_ocsp_cache == NULL) {
    errno = ENOSYS;
    return -1;
  }

  res = (tls_ocsp_cache->remove)(tls_ocsp_cache);
  return res;
# else
  errno = ENOSYS;
  return -1;
# endif /* PR_USE_OPENSSL_OCSP */
}

# if defined(PR_USE_OPENSSL_OCSP)
static void ocsp_cache_printf(void *ctrl, const char *fmt, ...) {
  char buf[PR_TUNABLE_BUFFER_SIZE];
  va_list msg;

  memset(buf, '\0', sizeof(buf));

  va_start(msg, fmt);
  pr_vsnprintf(buf, sizeof(buf), fmt, msg);
  va_end(msg);

  buf[sizeof(buf)-1] = '\0';
  pr_ctrls_add_response(ctrl, "%s", buf);
}
# endif /* PR_USE_OPENSSL_OCSP */

static int tls_ocsp_cache_status(pr_ctrls_t *ctrl, int flags) {
# if defined(PR_USE_OPENSSL_OCSP)
  int res = 0;

  if (tls_ocsp_cache != NULL) {
    res = (tls_ocsp_cache->status)(tls_ocsp_cache, ocsp_cache_printf, ctrl,
      flags);
    return res;
  }

  pr_ctrls_add_response(ctrl, "No TLSStaplingCache configured");
  return res;
# else
  errno = ENOSYS;
  return -1;
# endif /* PR_USE_OPENSSL_OCSP */
}
#endif /* PR_USE_CTRLS */

/* Controls
 */

#if defined(PR_USE_CTRLS)
static int tls_handle_sesscache_clear(pr_ctrls_t *ctrl, int reqargc,
    char **reqargv) {
  int res;

  res = tls_sess_cache_clear();
  if (res < 0) {
    pr_ctrls_add_response(ctrl,
      "tls sesscache: error clearing session cache: %s", strerror(errno));
    return PR_CTRLS_STATUS_INTERNAL_ERROR;
  }

  pr_ctrls_add_response(ctrl, "tls sesscache: cleared %d %s from '%s' "
    "session cache", res, res != 1 ? "sessions" : "session",
    tls_sess_cache->cache_name);
  return PR_CTRLS_STATUS_OK;
}

static int tls_handle_sesscache_info(pr_ctrls_t *ctrl, int reqargc,
    char **reqargv) {
  int flags = 0, optc, res;
  const char *opts = "v";

  pr_getopt_reset();

  while ((optc = getopt(reqargc, reqargv, opts)) != -1) {
    switch (optc) {
      case 'v':
        flags = TLS_SESS_CACHE_STATUS_FL_SHOW_SESSIONS;
        break;

      case '?':
        pr_ctrls_add_response(ctrl,
          "tls sesscache: unsupported parameter: '%s'", reqargv[1]);
        return PR_CTRLS_STATUS_WRONG_PARAMETERS;
    }
  }

  res = tls_sess_cache_status(ctrl, flags);
  if (res < 0) {
    pr_ctrls_add_response(ctrl,
      "tls sesscache: error obtaining session cache status: %s",
      strerror(errno));
    return PR_CTRLS_STATUS_INTERNAL_ERROR;
  }

  return PR_CTRLS_STATUS_OK;
}

static int tls_handle_sesscache_remove(pr_ctrls_t *ctrl, int reqargc,
    char **reqargv) {
  int res;

  res = tls_sess_cache_remove();
  if (res < 0) {
    pr_ctrls_add_response(ctrl,
      "tls sesscache: error removing session cache: %s", strerror(errno));
    return PR_CTRLS_STATUS_INTERNAL_ERROR;
  }

  pr_ctrls_add_response(ctrl, "tls sesscache: removed '%s' session cache",
    tls_sess_cache->cache_name);
  return PR_CTRLS_STATUS_OK;
}

static int tls_handle_sesscache(pr_ctrls_t *ctrl, int reqargc, char **reqargv) {

  /* Sanity check */
  if (reqargc == 0 ||
      reqargv == NULL) {
    pr_ctrls_add_response(ctrl, "tls sesscache: missing required parameters");
    return PR_CTRLS_STATUS_WRONG_PARAMETERS;
  }

  if (strcmp(reqargv[0], "info") == 0) {
    /* Check the ACLs. */
    if (pr_ctrls_check_acl(ctrl, tls_acttab, "info") != TRUE) {
      pr_ctrls_add_response(ctrl, "access denied");
      return PR_CTRLS_STATUS_ACCESS_DENIED;
    }

    return tls_handle_sesscache_info(ctrl, reqargc, reqargv);

  } else if (strcmp(reqargv[0], "clear") == 0) {
    /* Check the ACLs. */
    if (pr_ctrls_check_acl(ctrl, tls_acttab, "clear") != TRUE) {
      pr_ctrls_add_response(ctrl, "access denied");
      return PR_CTRLS_STATUS_ACCESS_DENIED;
    }

    return tls_handle_sesscache_clear(ctrl, reqargc, reqargv);

  } else if (strcmp(reqargv[0], "remove") == 0) {
    /* Check the ACLs. */
    if (pr_ctrls_check_acl(ctrl, tls_acttab, "remove") != TRUE) {
      pr_ctrls_add_response(ctrl, "access denied");
      return PR_CTRLS_STATUS_ACCESS_DENIED;
    }

    return tls_handle_sesscache_remove(ctrl, reqargc, reqargv);
  }

  pr_ctrls_add_response(ctrl, "tls sesscache: unknown sesscache action: '%s'",
    reqargv[0]);
  return PR_CTRLS_STATUS_UNSUPPORTED_OPERATION;
}

static int tls_handle_ocspcache_info(pr_ctrls_t *ctrl, int reqargc,
    char **reqargv) {
  int flags = 0, optc, res;
  const char *opts = "v";

  pr_getopt_reset();

  while ((optc = getopt(reqargc, reqargv, opts)) != -1) {
    switch (optc) {
      case '?':
        pr_ctrls_add_response(ctrl,
          "tls ocspcache: unsupported parameter: '%s'", reqargv[1]);
        return PR_CTRLS_STATUS_WRONG_PARAMETERS;
    }
  }

  res = tls_ocsp_cache_status(ctrl, flags);
  if (res < 0) {
    pr_ctrls_add_response(ctrl,
      "tls ocspcache: error obtaining OCSP cache status: %s",
      strerror(errno));
    return PR_CTRLS_STATUS_INTERNAL_ERROR;
  }

  return PR_CTRLS_STATUS_OK;
}

static int tls_handle_ocspcache_clear(pr_ctrls_t *ctrl, int reqargc,
    char **reqargv) {
  int res;

  res = tls_ocsp_cache_clear();
  if (res < 0) {
    pr_ctrls_add_response(ctrl,
      "tls ocspcache: error clearing OCSP cache: %s", strerror(errno));
    return PR_CTRLS_STATUS_INTERNAL_ERROR;
  }

  pr_ctrls_add_response(ctrl, "tls ocspcache: cleared %d %s from '%s' "
    "OCSP cache", res, res != 1 ? "responses" : "response",
    tls_ocsp_cache->cache_name);
  return PR_CTRLS_STATUS_OK;
}

static int tls_handle_ocspcache_remove(pr_ctrls_t *ctrl, int reqargc,
    char **reqargv) {
  int res;

  res = tls_ocsp_cache_remove();
  if (res < 0) {
    pr_ctrls_add_response(ctrl,
      "tls ocspcache: error removing OCSP cache: %s", strerror(errno));
    return PR_CTRLS_STATUS_INTERNAL_ERROR;
  }

  pr_ctrls_add_response(ctrl, "tls sesscache: removed '%s' OCSP cache",
    tls_ocsp_cache->cache_name);
  return PR_CTRLS_STATUS_OK;
}

static int tls_handle_ocspcache(pr_ctrls_t *ctrl, int reqargc, char **reqargv) {
  /* Sanity check */
  if (reqargc == 0 ||
      reqargv == NULL) {
    pr_ctrls_add_response(ctrl, "tls ocspcache: missing required parameters");
    return PR_CTRLS_STATUS_WRONG_PARAMETERS;
  }

  if (strcmp(reqargv[0], "info") == 0) {
    /* Check the ACLs. */
    if (pr_ctrls_check_acl(ctrl, tls_acttab, "info") != TRUE) {
      pr_ctrls_add_response(ctrl, "access denied");
      return PR_CTRLS_STATUS_ACCESS_DENIED;
    }

    return tls_handle_ocspcache_info(ctrl, reqargc, reqargv);

  } else if (strcmp(reqargv[0], "clear") == 0) {
    /* Check the ACLs. */
    if (pr_ctrls_check_acl(ctrl, tls_acttab, "clear") != TRUE) {
      pr_ctrls_add_response(ctrl, "access denied");
      return PR_CTRLS_STATUS_ACCESS_DENIED;
    }

    return tls_handle_ocspcache_clear(ctrl, reqargc, reqargv);

  } else if (strcmp(reqargv[0], "remove") == 0) {
    /* Check the ACLs. */
    if (pr_ctrls_check_acl(ctrl, tls_acttab, "remove") != TRUE) {
      pr_ctrls_add_response(ctrl, "access denied");
      return PR_CTRLS_STATUS_ACCESS_DENIED;
    }

    return tls_handle_ocspcache_remove(ctrl, reqargc, reqargv);
  }

  pr_ctrls_add_response(ctrl, "tls ocspcache: unknown ocspcache action: '%s'",
    reqargv[0]);
  return PR_CTRLS_STATUS_UNSUPPORTED_OPERATION;
}

/* Our main ftpdctl action handler */
static int tls_handle_tls(pr_ctrls_t *ctrl, int reqargc, char **reqargv) {

  /* Sanity check */
  if (reqargc == 0 ||
      reqargv == NULL) {
    pr_ctrls_add_response(ctrl, "missing required parameters");
    return PR_CTRLS_STATUS_WRONG_PARAMETERS;
  }

  if (strcmp(reqargv[0], "sesscache") == 0) {
    /* Check the ACLs. */
    if (pr_ctrls_check_acl(ctrl, tls_acttab, "sesscache") != TRUE) {
      pr_ctrls_add_response(ctrl, "access denied");
      return PR_CTRLS_STATUS_ACCESS_DENIED;
    }

    return tls_handle_sesscache(ctrl, --reqargc, ++reqargv);
  }

  if (strcmp(reqargv[0], "ocspcache") == 0) {
    /* Check the ACLs. */
    if (pr_ctrls_check_acl(ctrl, tls_acttab, "ocspcache") != TRUE) {
      pr_ctrls_add_response(ctrl, "access denied");
      return PR_CTRLS_STATUS_ACCESS_DENIED;
    }

    return tls_handle_ocspcache(ctrl, --reqargc, ++reqargv);
  }

  pr_ctrls_add_response(ctrl, "unknown tls action: '%s'", reqargv[0]);
  return PR_CTRLS_STATUS_UNSUPPORTED_OPERATION;
}
#endif /* PR_USE_CTRLS */

/* TLSSessionCache callbacks
 */

static int tls_sess_cache_add_sess_cb(SSL *ssl, SSL_SESSION *sess) {
  unsigned char *sess_id;
  unsigned int sess_id_len;
  int res;
  long expires;

  if (tls_sess_cache == NULL) {
    tls_log("unable to add session to session cache: %s", strerror(ENOSYS));

    SSL_SESSION_free(sess);
    return 1;
  }

  pr_trace_msg(trace_channel, 9, "adding new SSL session to '%s' cache",
    tls_sess_cache->cache_name);

  SSL_set_timeout(sess, tls_sess_cache->cache_timeout);

#if OPENSSL_VERSION_NUMBER > 0x000908000L
  sess_id = (unsigned char *) SSL_SESSION_get_id(sess, &sess_id_len);
#else
  /* XXX Directly accessing these fields cannot be a Good Thing. */
  sess_id = sess->session_id;
  sess_id_len = sess->session_id_length;
#endif

  /* The expiration timestamp stored in the session cache is the
   * Unix epoch time, not an interval.
   */
  expires = SSL_SESSION_get_time(sess) + tls_sess_cache->cache_timeout;

  res = (tls_sess_cache->add)(tls_sess_cache, sess_id, sess_id_len, expires,
    sess);
  if (res < 0) {
    long cache_mode;

    tls_log("error adding session to '%s' cache: %s",
      tls_sess_cache->cache_name, strerror(errno));

    cache_mode = tls_sess_cache_get_cache_mode();
#ifdef SSL_SESS_CACHE_NO_INTERNAL
    if (cache_mode & SSL_SESS_CACHE_NO_INTERNAL) {
      /* Call SSL_SESSION_free() here, and return 1.  We told OpenSSL that we
       * are the only cache, so failing to call SSL_SESSION_free() could
       * result in a memory leak.
       */
      SSL_SESSION_free(sess);
      return 1;
    }
#endif /* !SSL_SESS_CACHE_NO_INTERNAL */
  }

  /* Return zero to indicate to OpenSSL that we have not called
   * SSL_SESSION_free().
   */
  return 0;
}

static SSL_SESSION *tls_sess_cache_get_sess_cb(SSL *ssl,
    const unsigned char *id, int sess_id_len, int *do_copy) {
  SSL_SESSION *sess;
  const unsigned char *sess_id;

  pr_trace_msg(trace_channel, 9, "getting SSL session from '%s' cache",
    tls_sess_cache->cache_name);

  sess_id = id;

  /* Indicate to OpenSSL that the ref count should not be incremented
   * by setting the do_copy pointer to zero.
   */
  *do_copy = 0;

  /* The actual session_id_length field in the OpenSSL SSL_SESSION struct
   * is unsigned, not signed.  But for some reason, the expected callback
   * signature uses 'int', not 'unsigned int'.  Hopefully the implicit
   * cast below (our callback uses 'unsigned int') won't cause problems.
   * Just to be sure, check if OpenSSL is giving us a negative ID length.
   */
  if (sess_id_len <= 0) {
    tls_log("OpenSSL invoked TLS session cache 'get' callback with session "
      "ID length %d, returning null", sess_id_len);
    return NULL;
  }

  if (tls_sess_cache == NULL) {
    tls_log("unable to get session from session cache: %s", strerror(ENOSYS));
    return NULL;
  }

  sess = (tls_sess_cache->get)(tls_sess_cache, sess_id, sess_id_len);
  if (sess == NULL) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 5,
      "error retrieving session from '%s' cache: %s",
      tls_sess_cache->cache_name, strerror(xerrno));
    if (xerrno != ENOENT) {
      tls_log("error retrieving session from '%s' cache: %s",
        tls_sess_cache->cache_name, strerror(xerrno));
    }
  }

  return sess;
}

static void tls_sess_cache_delete_sess_cb(SSL_CTX *ctx, SSL_SESSION *sess) {
  unsigned char *sess_id;
  unsigned int sess_id_len;
  int res;

  if (tls_sess_cache == NULL) {
    tls_log("unable to remove session from session cache: %s",
      strerror(ENOSYS));
    return;
  }

  pr_trace_msg(trace_channel, 9, "removing SSL session from '%s' cache",
    tls_sess_cache->cache_name);

#if OPENSSL_VERSION_NUMBER > 0x000908000L
  sess_id = (unsigned char *) SSL_SESSION_get_id(sess, &sess_id_len);
#else
  /* XXX Directly accessing these fields cannot be a Good Thing. */
  sess_id = sess->session_id;
  sess_id_len = sess->session_id_length;
#endif

  res = (tls_sess_cache->delete)(tls_sess_cache, sess_id, sess_id_len);
  if (res < 0) {
    tls_log("error removing session from '%s' cache: %s",
      tls_sess_cache->cache_name, strerror(errno));
  }

  return;
}

/* Ideally we would use the OPENSSL_NO_PSK macro.  However, to use this, we
 * would need to say "if !defined(OPENSSL_NO_PSK)".  And that does not work
 * as well for older OpenSSL installations, where that macro would not be
 * defined anyway.  So instead, we use the presence of another PSK-related
 * macro as a more reliable sentinel.
 */

#if defined(PSK_MAX_PSK_LEN)
/* PSK callbacks */

static int set_random_bn(unsigned char *psk, unsigned int max_psklen) {
  BIGNUM *bn = NULL;
  int res = 0;

  bn = BN_new();
  if (BN_pseudo_rand(bn, max_psklen, 0, 0) != 1) {
    tls_log("error generating pseudo-random number: %s",
      ERR_error_string(ERR_get_error(), NULL));
  }

  res = BN_bn2bin(bn, psk);
  BN_free(bn);

  return res;
}

static unsigned int tls_lookup_psk(SSL *ssl, const char *identity,
    unsigned char *psk, unsigned int max_psklen) {
  const void *v = NULL;
  BIGNUM *bn = NULL;
  int bn_len = -1, res;

  if (identity == NULL) {
    tls_log("%s", "error: client did not provide PSK identity name, providing "
      "random fake PSK");

    res = set_random_bn(psk, max_psklen);
    return res;
  }

  pr_trace_msg(trace_channel, 5,
    "PSK lookup: identity '%s' requested", identity);

  if (tls_psks == NULL) {
    tls_log("warning: no pre-shared keys configured, providing random fake "
      "PSK for identity '%s'", identity);

    res = set_random_bn(psk, max_psklen);
    return res;
  }

  v = pr_table_get(tls_psks, identity, NULL);
  if (v == NULL) {
    tls_log("warning: requested PSK identity '%s' not configured, providing "
      "random fake PSK", identity);

    res = set_random_bn(psk, max_psklen);
    return res;
  }

  bn = (BIGNUM *) v;
  bn_len = BN_num_bytes(bn);

  if (bn_len > (int) max_psklen) {
    tls_log("warning: unable to use '%s' PSK: max buffer size (%u bytes) "
      "too small for key (%d bytes), providing random fake PSK", identity,
      max_psklen, bn_len);

    res = set_random_bn(psk, max_psklen);
    return res;
  }

  res = BN_bn2bin(bn, psk);
  if (res == 0) {
    tls_log("error converting PSK for identity '%s' to binary: %s",
      identity, tls_get_errors());
    return 0;
  }

  pr_trace_msg(trace_channel, 5,
    "found PSK (%d bytes) for identity '%s'", res, identity);
  return res;
}

#endif /* PSK_MAX_PSK_LEN */

/* NetIO callbacks
 */

static void tls_netio_abort_cb(pr_netio_stream_t *nstrm) {
  nstrm->strm_flags |= PR_NETIO_SESS_ABORT;
}

static int tls_netio_close_cb(pr_netio_stream_t *nstrm) {
  int res = 0;
  SSL *ssl = NULL;

  ssl = (SSL *) pr_table_get(nstrm->notes, TLS_NETIO_NOTE, NULL);
  if (ssl != NULL) {
    if (nstrm->strm_type == PR_NETIO_STRM_CTRL) {
      if (nstrm->strm_mode == PR_NETIO_IO_RD) {
        tls_ctrl_rd_nstrm = NULL;

      } else if (nstrm->strm_mode == PR_NETIO_IO_WR) {
        tls_ctrl_wr_nstrm = NULL;

        tls_end_sess(ssl, session.c, 0);
        tls_ctrl_netio = NULL;
        tls_flags &= ~TLS_SESS_ON_CTRL;
      }
    }

    if (nstrm->strm_type == PR_NETIO_STRM_DATA) {
      if (nstrm->strm_mode == PR_NETIO_IO_RD) {
        tls_data_rd_nstrm = NULL;

      } else if (nstrm->strm_mode == PR_NETIO_IO_WR) {
        tls_data_wr_nstrm = NULL;

        tls_end_sess(ssl, session.d, 0);
        tls_data_netio = NULL;
        tls_flags &= ~TLS_SESS_ON_DATA;
        tls_data_renegotiate_current = 0;
      }
    }
  }

  res = close(nstrm->strm_fd);
  nstrm->strm_fd = -1;

  return res;
}

static pr_netio_stream_t *tls_netio_open_cb(pr_netio_stream_t *nstrm, int fd,
    int mode) {
  nstrm->strm_fd = fd;
  nstrm->strm_mode = mode;

  /* Cache a pointer to this stream. */
  if (nstrm->strm_type == PR_NETIO_STRM_CTRL) {
    /* Note: We need to make this more generalized, so that mod_tls can be
     * used to open multiple different read/write control streams.  To do
     * so, we need a small (e.g. 4) array of pr_netio_stream_t pointers
     * for both read and write streams, and to iterate through them.  Need
     * iterate through and set strm_data appropriately in tls_accept(), too.
     *
     * This will needed to support FTPS connections to backend servers from
     * mod_proxy, for example.
     */
    if (nstrm->strm_mode == PR_NETIO_IO_RD) {
      if (tls_ctrl_rd_nstrm == NULL) {
        tls_ctrl_rd_nstrm = nstrm;
      }
    }

    if (nstrm->strm_mode == PR_NETIO_IO_WR) {
      if (tls_ctrl_wr_nstrm == NULL) {
        tls_ctrl_wr_nstrm = nstrm;
      }
    }

  } else if (nstrm->strm_type == PR_NETIO_STRM_DATA) {
    tls_data_renegotiate_current = 0;

    if (nstrm->strm_mode == PR_NETIO_IO_RD) {
      tls_data_rd_nstrm = nstrm;
    }

    if (nstrm->strm_mode == PR_NETIO_IO_WR) {
      tls_data_wr_nstrm = nstrm;
    }

    /* Note: from the FTP-TLS Draft 9.2:
     *
     *  It is quite reasonable for the server to insist that the data
     *  connection uses a TLS cached session.  This might be a cache of a
     *  previous data connection or of the control connection.  If this is
     *  the reason for the refusal to allow the data transfer then the
     *  '522' reply should indicate this.
     *
     * and, from 10.4:
     *
     *   If a server needs to have the connection protected then it will
     *   reply to the STOR/RETR/NLST/... command with a '522' indicating
     *   that the current state of the data connection protection level is
     *   not sufficient for that data transfer at that time.
     *
     * This points out the need for a module to be able to influence
     * command response codes in a more flexible manner...
     */
  }

  return nstrm;
}

static int tls_netio_poll_cb(pr_netio_stream_t *nstrm) {
  fd_set rfds, wfds;
  struct timeval tval;

  FD_ZERO(&rfds);
  FD_ZERO(&wfds);

  if (nstrm->strm_mode == PR_NETIO_IO_RD) {
    FD_SET(nstrm->strm_fd, &rfds);

  } else {
    FD_SET(nstrm->strm_fd, &wfds);
  }

  tval.tv_sec = (nstrm->strm_flags & PR_NETIO_SESS_INTR) ?
    nstrm->strm_interval : 10;
  tval.tv_usec = 0;

  return select(nstrm->strm_fd + 1, &rfds, &wfds, NULL, &tval);
}

static int tls_netio_postopen_cb(pr_netio_stream_t *nstrm) {

  /* If this is a data stream, and it's for writing, and TLS is required,
   * then do a TLS handshake.
   */

  if (nstrm->strm_type == PR_NETIO_STRM_DATA &&
      nstrm->strm_mode == PR_NETIO_IO_WR) {

    /* Enforce the "data" part of TLSRequired, if configured. */
    if (tls_required_on_data == 1 ||
        (tls_flags & TLS_SESS_NEED_DATA_PROT)) {
      SSL *ssl = NULL;

      /* XXX How to force 421 response code for failed secure FXP/SSCN? */

      /* Directory listings (LIST, MLSD, NLST) are ALWAYS handled in server
       * mode, regardless of SSCN mode.
       */
      if (session.curr_cmd_id == PR_CMD_LIST_ID ||
          session.curr_cmd_id == PR_CMD_MLSD_ID ||
          session.curr_cmd_id == PR_CMD_NLST_ID ||
          tls_sscn_mode == TLS_SSCN_MODE_SERVER) {
        X509 *ctrl_cert = NULL, *data_cert = NULL;
        uint64_t start_ms = 0;

        pr_gettimeofday_millis(&start_ms);

        tls_data_need_init_handshake = TRUE;
        if (tls_accept(session.d, TRUE) < 0) {
          tls_log("%s",
            "unable to open data connection: TLS negotiation failed");
          session.d->xerrno = errno = EPERM;
          return -1;
        }

        if (pr_trace_get_level(timing_channel) >= 4) {
          unsigned long elapsed_ms;
          uint64_t finish_ms;

          pr_gettimeofday_millis(&finish_ms);
          elapsed_ms = (unsigned long) (finish_ms - start_ms);

          pr_trace_msg(timing_channel, 4,
            "TLS data handshake duration: %lu ms", elapsed_ms);
        }

        ssl = (SSL *) pr_table_get(nstrm->notes, TLS_NETIO_NOTE, NULL);

        /* Make sure that the certificate used, if any, for this data channel
         * handshake is the same as that used for the control channel handshake.
         * This may be too strict of a requirement, though.
         */
        ctrl_cert = SSL_get_peer_certificate(ctrl_ssl);
        data_cert = SSL_get_peer_certificate(ssl);

        if (ctrl_cert != NULL &&
            data_cert != NULL) {
          if (X509_cmp(ctrl_cert, data_cert)) {
            X509_free(ctrl_cert);
            X509_free(data_cert);

            /* Properly shutdown the TLS session. */
            tls_end_sess(ssl, session.d, 0);
            pr_table_remove(tls_data_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
            pr_table_remove(tls_data_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);

            tls_log("%s", "unable to open data connection: control/data "
              "certificate mismatch");

            session.d->xerrno = errno = EPERM;
            return -1;
          }

          if (ctrl_cert) {
            X509_free(ctrl_cert);
          }

          if (data_cert) {
            X509_free(data_cert);
          }
        }

      } else if (tls_sscn_mode == TLS_SSCN_MODE_CLIENT) {
        tls_log("%s", "making TLS connection for data connection");
        if (tls_connect(session.d) < 0) {
          tls_log("%s",
            "unable to open data connection: TLS connection failed");
          session.d->xerrno = errno = EPERM;
          return -1;
        }
     }

#if OPENSSL_VERSION_NUMBER < 0x0090702fL
      /* Make sure blinding is turned on. (For some reason, this only seems
       * to be allowed on SSL objects, not on SSL_CTX objects.  Bummer).
       */
      tls_blinding_on(ssl);
#endif

      tls_flags |= TLS_SESS_ON_DATA;
    }
  }

  return 0;
}

static void tls_data_renegotiate(SSL *ssl) {
  if (tls_flags & TLS_SESS_DATA_RENEGOTIATING) {
    return;
  }

  tls_data_renegotiate_current = session.xfer.total_bytes;

  if (tls_data_renegotiate_limit > 0 &&
      tls_data_renegotiate_current >= tls_data_renegotiate_limit) {

    switch (SSL_version(ssl)) {
# if defined(TLS1_3_VERSION) && !defined(HAVE_LIBRESSL)
      /* If we're a TLSv1.3 session, use SSL_key_update() to request new
       * session keys; TLSv1.3 does not support renegotiations.
       */
      case TLS1_3_VERSION: {
        if (SSL_get_key_update_type(ctrl_ssl) == SSL_KEY_UPDATE_NONE) {
          tls_flags |= TLS_SESS_DATA_RENEGOTIATING;

          tls_log("requesting TLS key updates on data channel "
            "(%" PR_LU " KB data limit)",
            (pr_off_t) (tls_data_renegotiate_limit / 1024));

          if (SSL_key_update(ssl, SSL_KEY_UPDATE_REQUESTED) != 1) {
            tls_log("error requesting TLS key update on data channel: %s",
              tls_get_errors());
          }

        } else {
          pr_trace_msg(trace_channel, 7,
            "TLS key update on data channel already in progress");
        }
      }
      break;
# endif /* TLS1_3_VERSION and no LibreSSL */

      default: {
        tls_flags |= TLS_SESS_DATA_RENEGOTIATING;

        tls_log("requesting TLS renegotiation on data channel "
          "(%" PR_LU " KB data limit)",
          (pr_off_t) (tls_data_renegotiate_limit / 1024));

        if (SSL_renegotiate(ssl) != 1) {
          tls_log("error requesting TLS renegotiation on data channel: %s",
            tls_get_errors());
        }

        pr_timer_add(tls_renegotiate_timeout, -1, &tls_module,
          tls_renegotiate_timeout_cb, "SSL/TLS renegotiation timeout");
      }
    }
  }
}

static int tls_netio_read_cb(pr_netio_stream_t *nstrm, char *buf,
    size_t buflen) {
  SSL *ssl;

  ssl = (SSL *) pr_table_get(nstrm->notes, TLS_NETIO_NOTE, NULL);
  if (ssl != NULL) {
    BIO *rbio, *wbio;
    int bread = 0, bwritten = 0, xerrno = 0;
    ssize_t res = 0;
    unsigned long rbio_rbytes, rbio_wbytes, wbio_rbytes, wbio_wbytes;

    rbio = SSL_get_rbio(ssl);
    rbio_rbytes = BIO_number_read(rbio);
    rbio_wbytes = BIO_number_written(rbio);

    wbio = SSL_get_wbio(ssl);
    wbio_rbytes = BIO_number_read(wbio);
    wbio_wbytes = BIO_number_written(wbio);

    if (nstrm->strm_type == PR_NETIO_STRM_DATA) {
      tls_data_renegotiate(ssl);
    }

    res = tls_read(ssl, buf, buflen);
    xerrno = errno;

    bread = (BIO_number_read(rbio) - rbio_rbytes) +
      (BIO_number_read(wbio) - wbio_rbytes);
    bwritten = (BIO_number_written(rbio) - rbio_wbytes) +
      (BIO_number_written(wbio) - wbio_wbytes);

    /* Manually update session.total_raw_in with the difference between
     * the raw bytes read in versus the non-SSL bytes read in, in order to
     * have %I be accurately represented for the raw traffic.
     */
    if (res > 0) {
      session.total_raw_in += (bread - res);
    }

    /* Manually update session.total_raw_out, in order to have %O be
     * accurately represented for the raw traffic.
     */
    if (bwritten > 0) {
      session.total_raw_out += bwritten;
    }

    errno = xerrno;
    return res;
  }

  return read(nstrm->strm_fd, buf, buflen);
}

static pr_netio_stream_t *tls_netio_reopen_cb(pr_netio_stream_t *nstrm, int fd,
    int mode) {

  if (nstrm->strm_fd != -1) {
    close(nstrm->strm_fd);
  }

  nstrm->strm_fd = fd;
  nstrm->strm_mode = mode;

  /* NOTE: a no-op? */
  return nstrm;
}

static int tls_netio_shutdown_cb(pr_netio_stream_t *nstrm, int how) {

  if (how == 1 ||
      how == 2) {
    /* Closing this stream for writing; we need to send the 'close_notify'
     * alert first, so that the client knows, at the application layer,
     * that the SSL/TLS session is shutting down.
     */

    if (nstrm->strm_mode == PR_NETIO_IO_WR &&
        (nstrm->strm_type == PR_NETIO_STRM_CTRL ||
         nstrm->strm_type == PR_NETIO_STRM_DATA)) {
      SSL *ssl;

      ssl = (SSL *) pr_table_get(nstrm->notes, TLS_NETIO_NOTE, NULL);
      if (ssl != NULL) {
        BIO *rbio, *wbio;
        int bread = 0, bwritten = 0;
        unsigned long rbio_rbytes, rbio_wbytes, wbio_rbytes, wbio_wbytes;
        conn_t *conn;

        rbio = SSL_get_rbio(ssl);
        rbio_rbytes = BIO_number_read(rbio);
        rbio_wbytes = BIO_number_written(rbio);

        wbio = SSL_get_wbio(ssl);
        wbio_rbytes = BIO_number_read(wbio);
        wbio_wbytes = BIO_number_written(wbio);

        if (!(SSL_get_shutdown(ssl) & SSL_SENT_SHUTDOWN)) {

          /* Disable any socket buffering (Nagle, TCP_CORK), so that the alert
           * is sent in a timely manner (avoiding TLS shutdown latency).
           */
          conn = (nstrm->strm_type == PR_NETIO_STRM_DATA) ? session.d :
            session.c;
          if (conn != NULL) {
            if (pr_inet_set_proto_nodelay(conn->pool, conn, 1) < 0) {
              pr_trace_msg(trace_channel, 9,
                "error enabling TCP_NODELAY on conn: %s", strerror(errno));
            }

            if (pr_inet_set_proto_cork(conn->wfd, 0) < 0) {
              pr_trace_msg(trace_channel, 9,
                "error disabling TCP_CORK on fd %d: %s", conn->wfd,
                strerror(errno));
            }
          }

          /* We haven't sent a 'close_notify' alert yet; do so now. */
          SSL_shutdown(ssl);
        }

        bread = (BIO_number_read(rbio) - rbio_rbytes) +
          (BIO_number_read(wbio) - wbio_rbytes);
        bwritten = (BIO_number_written(rbio) - rbio_wbytes) +
          (BIO_number_written(wbio) - wbio_wbytes);

        /* Manually update session.total_raw_in/out, in order to have %I/%O be
         * accurately represented for the raw traffic.
         */
        if (bread > 0) {
          session.total_raw_in += bread;
        }

        if (bwritten > 0) {
          session.total_raw_out += bwritten;
        }

      } else {
        pr_trace_msg(trace_channel, 3,
          "no TLS found in stream notes for '%s'", TLS_NETIO_NOTE);
      }
    }
  }

  return shutdown(nstrm->strm_fd, how);
}

static int tls_netio_write_cb(pr_netio_stream_t *nstrm, char *buf,
    size_t buflen) {
  SSL *ssl;

  ssl = (SSL *) pr_table_get(nstrm->notes, TLS_NETIO_NOTE, NULL);
  if (ssl != NULL) {
    BIO *rbio, *wbio;
    int bread = 0, bwritten = 0, xerrno = 0;
    ssize_t res = 0;
    unsigned long rbio_rbytes, rbio_wbytes, wbio_rbytes, wbio_wbytes;

    rbio = SSL_get_rbio(ssl);
    rbio_rbytes = BIO_number_read(rbio);
    rbio_wbytes = BIO_number_written(rbio);

    wbio = SSL_get_wbio(ssl);
    wbio_rbytes = BIO_number_read(wbio);
    wbio_wbytes = BIO_number_written(wbio);

    if (nstrm->strm_type == PR_NETIO_STRM_DATA) {
      tls_data_renegotiate(ssl);
    }

    res = tls_write(ssl, buf, buflen);
    xerrno = errno;

    bread = (BIO_number_read(rbio) - rbio_rbytes) +
      (BIO_number_read(wbio) - wbio_rbytes);
    bwritten = (BIO_number_written(rbio) - rbio_wbytes) +
      (BIO_number_written(wbio) - wbio_wbytes);

    /* Manually update session.total_raw_in, in order to have %I be
     * accurately represented for the raw traffic.
     */
    if (bread > 0) {
      session.total_raw_in += bread;
    }

    /* Manually update session.total_raw_out with the difference between
     * the raw bytes written out versus the non-SSL bytes written out,
     * in order to have %) be accurately represented for the raw traffic.
     */
    if (res > 0) {
      session.total_raw_out += (bwritten - res);
    }

    errno = xerrno;
    return res;
  }

  return write(nstrm->strm_fd, buf, buflen);
}

static void tls_netio_install_ctrl(void) {
  pr_netio_t *netio;

  if (tls_ctrl_netio) {
    /* If we already have our ctrl netio, then it's been registered, and
     * we don't need to do anything more.
     */
    return;
  }

  tls_ctrl_netio = netio = pr_alloc_netio2(permanent_pool, &tls_module, NULL);

  netio->abort = tls_netio_abort_cb;
  netio->close = tls_netio_close_cb;
  netio->open = tls_netio_open_cb;
  netio->poll = tls_netio_poll_cb;
  netio->postopen = tls_netio_postopen_cb;
  netio->read = tls_netio_read_cb;
  netio->reopen = tls_netio_reopen_cb;
  netio->shutdown = tls_netio_shutdown_cb;
  netio->write = tls_netio_write_cb;

  pr_unregister_netio(PR_NETIO_STRM_CTRL);

  if (pr_register_netio(netio, PR_NETIO_STRM_CTRL) < 0) {
    pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION ": error registering netio: %s",
      strerror(errno));
  }
}

static void tls_netio_install_data(void) {
  pr_netio_t *netio = tls_data_netio ? tls_data_netio :
    (tls_data_netio = pr_alloc_netio2(session.pool ? session.pool :
    permanent_pool, &tls_module, NULL));

  netio->abort = tls_netio_abort_cb;
  netio->close = tls_netio_close_cb;
  netio->open = tls_netio_open_cb;
  netio->poll = tls_netio_poll_cb;
  netio->postopen = tls_netio_postopen_cb;
  netio->read = tls_netio_read_cb;
  netio->reopen = tls_netio_reopen_cb;
  netio->shutdown = tls_netio_shutdown_cb;
  netio->write = tls_netio_write_cb;

  pr_unregister_netio(PR_NETIO_STRM_DATA);

  if (pr_register_netio(netio, PR_NETIO_STRM_DATA) < 0) {
    pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION ": error registering netio: %s",
      strerror(errno));
  }
}

/* Logging functions
 */

static void tls_closelog(void) {

  /* Sanity check */
  if (tls_logfd != -1) {
    close(tls_logfd);
    tls_logfd = -1;
  }

  return;
}

int tls_log(const char *fmt, ...) {
  va_list msg;
  int res;

  /* Sanity check */
  if (tls_logfd < 0) {
    return 0;
  }

  va_start(msg, fmt);
  res = pr_log_vwritefile(tls_logfd, MOD_TLS_VERSION, fmt, msg);
  va_end(msg);

  return res;
}

static int tls_openlog(void) {
  int res = 0, xerrno;
  char *path;

  /* Sanity checks */
  path = get_param_ptr(main_server->conf, "TLSLog", FALSE);
  if (path == NULL ||
      strncasecmp(path, "none", 5) == 0) {
    return 0;
  }

  pr_signals_block();
  PRIVS_ROOT
  res = pr_log_openfile(path, &tls_logfd, PR_LOG_SYSTEM_MODE);
  xerrno = errno;
  PRIVS_RELINQUISH
  pr_signals_unblock();

  errno = xerrno;
  return res;
}

/* Authentication handlers
 */

/* This function does the main authentication work, and is called in the
 * normal course of events:
 *
 *   cmd->argv[0]: user name
 *   cmd->argv[1]: cleartext password
 */
MODRET tls_authenticate(cmd_rec *cmd) {
  if (!tls_engine) {
    return PR_DECLINED(cmd);
  }

  /* Possible authentication combinations:
   *
   *  TLS handshake + passwd (default)
   *  TLS handshake + .tlslogin (passwd ignored)
   */

  if (tls_flags & TLS_SESS_ON_CTRL) {
    config_rec *c;

    if (tls_opts & TLS_OPT_ALLOW_DOT_LOGIN) {
      if (tls_dotlogin_allow(cmd->argv[0])) {
        tls_log("TLS/X509 .tlslogin check successful for user '%s'",
          (char *) cmd->argv[0]);
        pr_log_auth(PR_LOG_NOTICE, "USER %s: TLS/X509 .tlslogin authentication "
          "successful", (char *) cmd->argv[0]);
        session.auth_mech = "mod_tls.c";
        return mod_create_data(cmd, (void *) PR_AUTH_RFC2228_OK);

      } else {
        tls_log("TLS/X509 .tlslogin check failed for user '%s'",
          (char *) cmd->argv[0]);
      }
    }

    c = find_config(main_server->conf, CONF_PARAM, "TLSUserName", FALSE);
    if (c != NULL) {
      if (tls_cert_to_user(cmd->argv[0], c->argv[0])) {
        tls_log("TLS/X509 TLSUserName '%s' check successful for user '%s'",
          (char *) c->argv[0], (char *) cmd->argv[0]);
        pr_log_auth(PR_LOG_NOTICE,
          "USER %s: TLS/X509 TLSUserName authentication successful",
          (char *) cmd->argv[0]);
        session.auth_mech = "mod_tls.c";
        return mod_create_data(cmd, (void *) PR_AUTH_RFC2228_OK);

      } else {
        tls_log("TLS/X509 TLSUserName '%s' check failed for user '%s'",
          (char *) c->argv[0], (char *) cmd->argv[0]);
      }
    }
  }

  return PR_DECLINED(cmd);
}

/* This function is called only when UserPassword is involved, used to
 * override the configured password for a user.
 *
 *  cmd->argv[0]: hashed password (from proftpd.conf)
 *  cmd->argv[1]: user name
 *  cmd->argv[2]: cleartext password
 */
MODRET tls_auth_check(cmd_rec *cmd) {
  if (!tls_engine)
    return PR_DECLINED(cmd);

  /* Possible authentication combinations:
   *
   *  TLS handshake + passwd (default)
   *  TLS handshake + .tlslogin (passwd ignored)
   */

  if (tls_flags & TLS_SESS_ON_CTRL) {
    config_rec *c;

    if (tls_opts & TLS_OPT_ALLOW_DOT_LOGIN) {
      if (tls_dotlogin_allow(cmd->argv[1])) {
        tls_log("TLS/X509 .tlslogin check successful for user '%s'",
          (char *) cmd->argv[0]);
        pr_log_auth(PR_LOG_NOTICE, "USER %s: TLS/X509 .tlslogin authentication "
          "successful", (char *) cmd->argv[1]);
        session.auth_mech = "mod_tls.c";
        return mod_create_data(cmd, (void *) PR_AUTH_RFC2228_OK);

      } else {
        tls_log("TLS/X509 .tlslogin check failed for user '%s'",
          (char *) cmd->argv[1]);
      }
    }

    c = find_config(main_server->conf, CONF_PARAM, "TLSUserName", FALSE);
    if (c != NULL) {
      if (tls_cert_to_user(cmd->argv[0], c->argv[0])) {
        tls_log("TLS/X509 TLSUserName '%s' check successful for user '%s'",
          (char *) c->argv[0], (char *) cmd->argv[0]);
        pr_log_auth(PR_LOG_NOTICE,
          "USER %s: TLS/X509 TLSUserName authentication successful",
          (char *) cmd->argv[0]);
        session.auth_mech = "mod_tls.c";
        return mod_create_data(cmd, (void *) PR_AUTH_RFC2228_OK);

      } else {
        tls_log("TLS/X509 TLSUserName '%s' check failed for user '%s'",
          (char *) c->argv[0], (char *) cmd->argv[0]);
      }
    }
  }

  return PR_DECLINED(cmd);
}

/* Command handlers
 */

MODRET tls_any(cmd_rec *cmd) {
  if (tls_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  /* Some commands need not be hindered. */
  if (pr_cmd_cmp(cmd, PR_CMD_SYST_ID) == 0 ||
      pr_cmd_cmp(cmd, PR_CMD_AUTH_ID) == 0 ||
      pr_cmd_cmp(cmd, PR_CMD_FEAT_ID) == 0 ||
      pr_cmd_cmp(cmd, PR_CMD_HOST_ID) == 0 ||
      pr_cmd_cmp(cmd, PR_CMD_CLNT_ID) == 0 ||
      pr_cmd_cmp(cmd, PR_CMD_QUIT_ID) == 0) {
    return PR_DECLINED(cmd);
  }

  if (tls_required_on_auth == 1 &&
      !(tls_flags & TLS_SESS_ON_CTRL)) {

    if (!(tls_opts & TLS_OPT_ALLOW_PER_USER)) {
      if (pr_cmd_cmp(cmd, PR_CMD_USER_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_PASS_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_ACCT_ID) == 0) {
        tls_log("SSL/TLS required but absent for authentication, "
          "denying %s command", (char *) cmd->argv[0]);
        pr_response_add_err(R_550,
          _("SSL/TLS required on the control channel"));

        pr_cmd_set_errno(cmd, EPERM);
        errno = EPERM;
        return PR_ERROR(cmd);
      }
    }
  }

  if (tls_required_on_ctrl == 1 &&
      !(tls_flags & TLS_SESS_ON_CTRL)) {

    if (!(tls_opts & TLS_OPT_ALLOW_PER_USER)) {
      tls_log("SSL/TLS required but absent on control channel, "
        "denying %s command", (char *) cmd->argv[0]);
      pr_response_add_err(R_550, _("SSL/TLS required on the control channel"));

      pr_cmd_set_errno(cmd, EPERM);
      errno = EPERM;
      return PR_ERROR(cmd);
    }

    if (tls_authenticated != NULL &&
        *tls_authenticated == TRUE) {
      tls_log("SSL/TLS required but absent on control channel, "
        "denying %s command", (char *) cmd->argv[0]);
      pr_response_add_err(R_550,
        _("SSL/TLS required on the control channel"));

      pr_cmd_set_errno(cmd, EPERM);
      errno = EPERM;
      return PR_ERROR(cmd);
    }
  }

  /* TLSRequired checks */

  if (tls_required_on_data == 1) {
    /* TLSRequired encompasses all data transfers for this session, the
     * client did not specify an appropriate PROT, and the command is one
     * which will trigger a data transfer...
     */

    if (!(tls_flags & TLS_SESS_NEED_DATA_PROT)) {
      if (pr_cmd_cmp(cmd, PR_CMD_APPE_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_MLSD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_RETR_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_STOR_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_STOU_ID) == 0) {
        tls_log("SSL/TLS required but absent on data channel, "
          "denying %s command", (char *) cmd->argv[0]);
        pr_response_add_err(R_522, _("SSL/TLS required on the data channel"));

        pr_cmd_set_errno(cmd, EPERM);
        errno = EPERM;
        return PR_ERROR(cmd);
      }
    }

  } else {

    /* TLSRequired is not in effect for all data transfers for this session.
     * If this command will trigger a data transfer, check the current
     * context to see if there's a directory-level TLSRequired for data
     * transfers.
     *
     * XXX ideally, rather than using the current directory location, we'd
     * do the lookup based on the target location.
     */

    if (pr_cmd_cmp(cmd, PR_CMD_APPE_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_MLSD_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_RETR_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_STOR_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_STOU_ID) == 0) {
      config_rec *c;

      c = find_config(CURRENT_CONF, CONF_PARAM, "TLSRequired", FALSE);
      if (c != NULL) {
        int tls_required;

        tls_required = *((int *) c->argv[1]);

        if (tls_required == TRUE &&
            !(tls_flags & TLS_SESS_NEED_DATA_PROT)) {
          tls_log("%s command denied by TLSRequired in directory '%s'",
            (char *) cmd->argv[0],
            session.dir_config ? session.dir_config->name :
              session.anon_config ? session.anon_config->name :
              main_server->ServerName);
          pr_response_add_err(R_522, _("SSL/TLS required on the data channel"));

          pr_cmd_set_errno(cmd, EPERM);
          errno = EPERM;
          return PR_ERROR(cmd);
        }
      }
    }
  }

  return PR_DECLINED(cmd);
}

/* Issue #1931: For TLSv1.3 sessions, we should use SSL_new_session_ticket
 * to request a new ticket (and thus session) before any data transfers.
 *
 * For TLSv1.2 sessions with (and without) tickets, we might try
 * SSL_renegotiate in the future.
 *
 * TODO: Work on solutions for TLSv1.2 and other protocol versions.
 */
MODRET tls_pre_xfer(cmd_rec *cmd) {
#if defined(TLS1_3_VERSION)
  SSL_SESSION *ssl_session;
  unsigned long sess_remaining;
  int request_new_ticket = FALSE, ssl_version;

  if (tls_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (!(tls_flags & TLS_SESS_ON_CTRL)) {
    return PR_DECLINED(cmd);
  }

  if (ctrl_ssl == NULL) {
    return PR_DECLINED(cmd);
  }

  ssl_session = SSL_get_session(ctrl_ssl);
  ssl_version = SSL_SESSION_get_protocol_version(ssl_session);

  if (ssl_version != TLS1_3_VERSION) {
    return PR_DECLINED(cmd);
  }

  sess_remaining = tls_sess_remaining(ssl_session);
  pr_trace_msg(trace_channel, 21,
    "%s: control channel %s session lifetime: %lu %s%s",
    (const char *) cmd->argv[0], SSL_get_version(ctrl_ssl), sess_remaining,
    sess_remaining != 1 ? "seconds" : "second",
    sess_remaining != 0 ? "" : " (EXPIRED)");

  /* If the control session expires in 10 seconds or less, request a new one
   * before proceeding with the data transfer (Issue #1931).
   */
  if (sess_remaining <= 10) {
    request_new_ticket = TRUE;
  }

  if (request_new_ticket == TRUE) {
    int res;

# if defined(OPENSSL_VERSION_MAJOR) && \
     OPENSSL_VERSION_MAJOR >= 3
    res = SSL_new_session_ticket(ctrl_ssl);
# else
    res = 0;
    errno = ENOSYS;
# endif /* OpenSSL 3.x and later */

    if (res == 1) {
      pr_trace_msg(trace_channel, 19,
        "%s: requesting new %s session before data transfer",
        (const char *) cmd->argv[0], SSL_get_version(ctrl_ssl));

    } else {
      pr_trace_msg(trace_channel, 1,
        "%s: error requesting new %s session: %s", (const char *) cmd->argv[0],
        SSL_get_version(ctrl_ssl),
        errno == ENOSYS ? strerror(ENOSYS) : tls_get_errors());
    }
  }
#endif /* TLS1_3_VERSION */

  return PR_DECLINED(cmd);
}

MODRET tls_auth(cmd_rec *cmd) {
  register unsigned int i = 0;
  char *mode;
  unsigned char *authenticated = NULL;

  if (tls_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  /* If we already have protection on the control channel (i.e. AUTH has
   * already been sent by the client and handled), then reject this second
   * AUTH.  Clients that want to renegotiate can either use SSL/TLS's
   * renegotiation facilities, or disconnect and start over.
   */
  if (tls_flags & TLS_SESS_ON_CTRL) {
    tls_log("Unwilling to accept AUTH after AUTH for this session");
    pr_response_add_err(R_503, _("Unwilling to accept second AUTH"));

    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  if (cmd->argc < 2) {
    pr_response_add_err(R_504, _("AUTH requires at least one argument"));

    pr_cmd_set_errno(cmd, EINVAL);
    errno = EINVAL;
    return PR_ERROR(cmd);
  }

  if (tls_flags & TLS_SESS_HAVE_CCC) {
    tls_log("Unwilling to accept AUTH after CCC for this session");
    pr_response_add_err(R_534, _("Unwilling to accept security parameters"));

    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  /* CAN we even handle an AUTH command?  If we do not have the necessary
   * certificates, then we should indicate that we cannot.
   */
  if (tls_rsa_cert_file == NULL &&
      tls_dsa_cert_file == NULL &&
      tls_ec_cert_file == NULL &&
      tls_pkcs12_file == NULL) {
    tls_log("Unable to accept AUTH %s due to lack of certificates", cmd->arg);
    pr_response_add_err(R_431, _("Necessary security resource unavailable"));

    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  /* If the client has already authenticated via USER/PASS, AND if the
   * AllowPerUser TLSOption is NOT in effect, then do not allow the AUTH
   * command.
   */
  authenticated = get_param_ptr(cmd->server->conf, "authenticated", FALSE);
  if (authenticated != NULL &&
      *authenticated == TRUE &&
      !(tls_opts & TLS_OPT_ALLOW_PER_USER)) {
    tls_log("Unwilling to accept AUTH after USER/PASS authentication for this session unless AllowPerUser TLSOption is used");
    pr_response_add_err(R_534, _("Unwilling to accept security parameters"));

    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  /* Convert the parameter to upper case */
  mode = cmd->argv[1];
  for (i = 0; i < strlen(mode); i++) {
    mode[i] = toupper(mode[i]);
  }

  if (strncmp(mode, "TLS", 4) == 0 ||
      strncmp(mode, "TLS-C", 6) == 0) {
    uint64_t start_ms = 0;

    pr_response_send(R_234, _("AUTH %s successful"), (char *) cmd->argv[1]);
    tls_log("%s", "TLS/TLS-C requested, starting TLS handshake");

    if (pr_trace_get_level(timing_channel) > 0) {
      pr_gettimeofday_millis(&start_ms);
    }

    pr_event_generate("mod_tls.ctrl-handshake", session.c);
    if (tls_accept(session.c, FALSE) < 0) {
      tls_log("%s", "TLS/TLS-C negotiation failed on control channel");

      if (tls_required_on_ctrl == 1) {
        pr_response_send_async(R_421, _("TLS handshake failed"));
        pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_CONFIG_ACL,
          "TLSRequired");
      }

      /* If we reach this point, the debug logging may show gibberish
       * commands from the client.  In reality, this gibberish is probably
       * more encrypted data from the client.
       */
      pr_response_send_async(R_421, _("TLS handshake failed"));
      pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_BY_APPLICATION,
        NULL);
    }

#if OPENSSL_VERSION_NUMBER < 0x0090702fL
    /* Make sure blinding is turned on. (For some reason, this only seems
     * to be allowed on SSL objects, not on SSL_CTX objects.  Bummer).
     */
    tls_blinding_on(ctrl_ssl);
#endif

    tls_flags |= TLS_SESS_ON_CTRL;

    if (pr_trace_get_level(timing_channel) >= 4) {
      unsigned long elapsed_ms;
      uint64_t finish_ms;

      pr_gettimeofday_millis(&finish_ms);

      elapsed_ms = (unsigned long) (finish_ms - session.connect_time_ms);
      pr_trace_msg(timing_channel, 4,
        "Time before TLS ctrl handshake: %lu ms", elapsed_ms);

      elapsed_ms = (unsigned long) (finish_ms - start_ms);
      pr_trace_msg(timing_channel, 4,
        "TLS ctrl handshake duration: %lu ms", elapsed_ms);
    }

  } else if (strncmp(mode, "SSL", 4) == 0 ||
             strncmp(mode, "TLS-P", 6) == 0) {
    uint64_t start_ms = 0;

    pr_response_send(R_234, _("AUTH %s successful"), (char *) cmd->argv[1]);
    tls_log("%s", "SSL/TLS-P requested, starting TLS handshake");

    if (pr_trace_get_level(timing_channel) > 0) {
      pr_gettimeofday_millis(&start_ms);
    }

    if (tls_accept(session.c, FALSE) < 0) {
      tls_log("%s", "SSL/TLS-P negotiation failed on control channel");

      if (tls_required_on_ctrl == 1) {
        pr_response_send_async(R_421, _("TLS handshake failed"));
        pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_CONFIG_ACL,
          "TLSRequired");
      }

      /* If we reach this point, the debug logging may show gibberish
       * commands from the client.  In reality, this gibberish is probably
       * more encrypted data from the client.
       */
      pr_response_send_async(R_421, _("TLS handshake failed"));
      pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_BY_APPLICATION,
        NULL);
    }

#if OPENSSL_VERSION_NUMBER < 0x0090702fL
    /* Make sure blinding is turned on. (For some reason, this only seems
     * to be allowed on SSL objects, not on SSL_CTX objects.  Bummer).
     */
    tls_blinding_on(ctrl_ssl);
#endif

    tls_flags |= TLS_SESS_ON_CTRL;
    tls_flags |= TLS_SESS_NEED_DATA_PROT;

    if (pr_trace_get_level(timing_channel) >= 4) {
      unsigned long elapsed_ms;
      uint64_t finish_ms;

      pr_gettimeofday_millis(&finish_ms);

      elapsed_ms = (unsigned long) (finish_ms - session.connect_time_ms);
      pr_trace_msg(timing_channel, 4,
        "Time before TLS ctrl handshake: %lu ms", elapsed_ms);

      elapsed_ms = (unsigned long) (finish_ms - start_ms);
      pr_trace_msg(timing_channel, 4,
        "TLS ctrl handshake duration: %lu ms", elapsed_ms);
    }

  } else {
    tls_log("AUTH %s unsupported, declining", (char *) cmd->argv[1]);

    /* Allow other RFC2228 modules a chance a handling this command. */
    return PR_DECLINED(cmd);
  }

  pr_session_set_protocol("ftps");
  session.rfc2228_mech = "TLS";

  return PR_HANDLED(cmd);
}

MODRET tls_ccc(cmd_rec *cmd) {

  if (!tls_engine ||
      !session.rfc2228_mech ||
      strncmp(session.rfc2228_mech, "TLS", 4) != 0) {
    return PR_DECLINED(cmd);
  }

  if (!(tls_flags & TLS_SESS_ON_CTRL)) {
    pr_response_add_err(R_533,
      _("CCC not allowed on insecure control connection"));

    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  if (tls_required_on_ctrl == 1) {
    pr_response_add_err(R_534, _("Unwilling to accept security parameters"));
    tls_log("%s: unwilling to accept security parameters: "
      "TLSRequired setting does not allow for unprotected control channel",
      (char *) cmd->argv[0]);

    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  /* Check for <Limit> restrictions. */
  if (!dir_check(cmd->tmp_pool, cmd, G_NONE, session.cwd, NULL)) {

    pr_log_debug(DEBUG8, "%s %s denied by <Limit> configuration",
      (char *) cmd->argv[0], cmd->arg);
    tls_log("%s: unwilling to accept security parameters",
      (char *) cmd->argv[0]);
    pr_response_add_err(R_534, _("Unwilling to accept security parameters"));

    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  tls_log("received CCC, clearing control channel protection");

  /* Send the OK response asynchronously; the spec dictates that the
   * response be sent prior to performing the TLS session shutdown.
   */
  pr_response_send_async(R_200, _("Clearing control channel protection"));

  /* Close the TLS session, but only one the control channel.
   * The data channel, if protected, should remain so.
   */

  tls_end_sess(ctrl_ssl, session.c, TLS_SHUTDOWN_FL_BIDIRECTIONAL);
  pr_table_remove(tls_ctrl_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
  pr_table_remove(tls_ctrl_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
  ctrl_ssl = NULL;

  /* Remove our NetIO for the control channel. */
  pr_unregister_netio(PR_NETIO_STRM_CTRL);

  tls_flags &= ~TLS_SESS_ON_CTRL;
  tls_flags |= TLS_SESS_HAVE_CCC;

  return PR_HANDLED(cmd);
}

MODRET tls_pbsz(cmd_rec *cmd) {

  if (!tls_engine ||
      !session.rfc2228_mech ||
      strncmp(session.rfc2228_mech, "TLS", 4) != 0)
    return PR_DECLINED(cmd);

  CHECK_CMD_ARGS(cmd, 2);

  if (!(tls_flags & TLS_SESS_ON_CTRL)) {
    pr_response_add_err(R_503,
      _("PBSZ not allowed on insecure control connection"));

    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  /* We expect "PBSZ 0" */
  if (strncmp(cmd->argv[1], "0", 2) == 0) {
    pr_response_add(R_200, _("PBSZ 0 successful"));

  } else {
    pr_response_add(R_200, _("PBSZ=0 successful"));
  }

  tls_flags |= TLS_SESS_PBSZ_OK;
  return PR_HANDLED(cmd);
}

MODRET tls_post_auth(cmd_rec *cmd) {
  const char *sni = NULL;
  server_rec *named_server = NULL;
  cmd_rec *host_cmd = NULL;

  if (tls_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  /* We are specifically looking for SNI provided by the client during
   * a successful AUTH TLS handshake.
   */
  if (session.rfc2228_mech == NULL ||
      strncmp(session.rfc2228_mech, "TLS", 4) != 0) {
    return PR_DECLINED(cmd);
  }

  sni = pr_table_get(session.notes, "mod_tls.sni", NULL);
  if (sni == NULL) {
    /* No SNI provided. */
    return PR_DECLINED(cmd);
  }

  if (tls_opts & TLS_OPT_IGNORE_SNI) {
    return PR_DECLINED(cmd);
  }

  named_server = pr_namebind_get_server(sni, main_server->addr,
    session.c->local_port);
  if (named_server == NULL) {
    pr_trace_msg(trace_channel, 5,
      "client sent SNI '%s', but no matching host found", sni);
    return PR_DECLINED(cmd);
  }

  if (named_server == main_server) {
    return PR_DECLINED(cmd);
  }

  /* Set a session flag indicating that the main_server pointer changed. */
  pr_log_debug(DEBUG0,
    "Changing to server '%s' (ServerAlias %s) due to TLS SNI",
    named_server->ServerName, sni);
  session.prev_server = main_server;
  main_server = named_server;

  pr_event_generate("core.session-reinit", named_server);

  /* Now we need to inform the modules of the changed config, to let them
   * do their checks.
   */

  host_cmd = pr_cmd_alloc(cmd->tmp_pool, 2, pstrdup(cmd->tmp_pool, C_HOST), sni,
    NULL);
  pr_cmd_dispatch_phase(host_cmd, POST_CMD, 0);
  pr_cmd_dispatch_phase(host_cmd, LOG_CMD, 0);
  pr_response_clear(&resp_list);

  return PR_DECLINED(cmd);
}

MODRET tls_log_auth(cmd_rec *cmd) {
  if (tls_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  /* Ensure any sensitive passphrases are scrubbed. */
  (void) tls_lookup_pkey(main_server, FALSE, TRUE);

  return PR_DECLINED(cmd);
}

MODRET tls_post_pass(cmd_rec *cmd) {
  config_rec *c, *protocols_config;

  if (tls_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  c = find_config(main_server->conf, CONF_PARAM, "TLSOptions", FALSE);
  while (c != NULL) {
    unsigned long opts = 0;

    pr_signals_handle();

    opts = *((unsigned long *) c->argv[0]);
    tls_opts |= opts;

    c = find_config_next(c, c->next, CONF_PARAM, "TLSOptions", FALSE);
  }

  /* At this point, we can look up the Protocols config if the client has been
   * authenticated, which may have been tweaked via mod_ifsession's
   * user/group/class-specific sections.
   */
  protocols_config = find_config(main_server->conf, CONF_PARAM, "Protocols",
    FALSE);

  if (!(tls_opts & TLS_OPT_ALLOW_PER_USER) &&
      protocols_config == NULL) {
    return PR_DECLINED(cmd);
  }

  tls_authenticated = get_param_ptr(cmd->server->conf, "authenticated", FALSE);

  if (tls_authenticated &&
      *tls_authenticated == TRUE) {

    c = find_config(TOPLEVEL_CONF, CONF_PARAM, "TLSRequired", FALSE);
    if (c != NULL) {
      /* Lookup the TLSRequired directive again in this context (which could be
       * <Anonymous>, for example, or modified by mod_ifsession).
       */

      tls_required_on_ctrl = *((int *) c->argv[0]);
      tls_required_on_data = *((int *) c->argv[1]);
      tls_required_on_auth = *((int *) c->argv[2]);

      /* We cannot return PR_ERROR for the PASS command at this point, since
       * this is a POST_CMD handler.  Instead, we will simply check the
       * TLSRequired policy, and if the current session does not make the
       * cut, well, then the session gets cut.
       */
      if ((tls_required_on_ctrl == 1 ||
           tls_required_on_auth == 1) &&
          (!(tls_flags & TLS_SESS_ON_CTRL))) {
        tls_log("SSL/TLS required but absent on control channel, "
          "disconnecting");
        pr_response_send(R_530, "%s", _("Login incorrect."));
        pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_CONFIG_ACL,
          "TLSRequired");
      }
    }

    if (protocols_config != NULL) {
      register unsigned int i;
      int allow_sess = FALSE;
      array_header *protocols;
      char **elts;

      protocols = protocols_config->argv[0];
      elts = protocols->elts;

      for (i = 0; i < protocols->nelts; i++) {
        char *proto;

        proto = elts[i];
        if (proto != NULL) {
          /* We only want to check for 'ftps' in the configured Protocols list
           * if the RFC2228 mechanism is "TLS".
           */

          if (strcasecmp(proto, "ftp") == 0) {
            if (session.rfc2228_mech == NULL ||
                strcmp(session.rfc2228_mech, "TLS") != 0) {
              allow_sess = TRUE;
              break;
            }
          }

          if (strcasecmp(proto, "ftps") == 0) {
            if (session.rfc2228_mech != NULL &&
                strcmp(session.rfc2228_mech, "TLS") == 0) {
              allow_sess = TRUE;
              break;
            }
          }
        }
      }

      if (allow_sess == FALSE) {
        tls_log("%s protocol denied by Protocols config",
          pr_session_get_protocol(0));
        pr_response_send(R_530, "%s", _("Login incorrect."));
        pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_CONFIG_ACL,
          "Denied by Protocols setting");
      }
    }
  }

  return PR_DECLINED(cmd);
}

MODRET tls_post_user(cmd_rec *cmd) {
  const void *ifsess_note;
  config_rec *c;

  if (tls_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  /* Look for a session note, stashed by mod_ifsession when its
   * PerUnauthenticatedUser option is set.  That is our cue to honor any
   * <IfUser>/<IfGroup> TLSRequired settings for this user.
   */
  ifsess_note = pr_table_get(session.notes,
    "mod_ifsession.per-unauthenticated-user", NULL);
  if (ifsess_note == NULL) {
    return PR_DECLINED(cmd);
  }

  c = find_config(main_server->conf, CONF_PARAM, "TLSOptions", FALSE);
  while (c != NULL) {
    unsigned long opts = 0;

    pr_signals_handle();

    opts = *((unsigned long *) c->argv[0]);
    tls_opts |= opts;

    c = find_config_next(c, c->next, CONF_PARAM, "TLSOptions", FALSE);
  }

  if (!(tls_opts & TLS_OPT_ALLOW_PER_USER)) {
    return PR_DECLINED(cmd);
  }

  c = find_config(main_server->conf, CONF_PARAM, "TLSRequired", FALSE);
  if (c != NULL) {
    tls_required_on_ctrl = *((int *) c->argv[0]);
    tls_required_on_data = *((int *) c->argv[1]);
    tls_required_on_auth = *((int *) c->argv[2]);

    /* We cannot return PR_ERROR for the USER command at this point, since
     * this is a POST_CMD handler.  Instead, we will simply check the
     * TLSRequired policy, and if the current session does not make the
     * cut, well, then the session gets cut.
     */
    if ((tls_required_on_ctrl == 1 ||
         tls_required_on_auth == 1) &&
        (!(tls_flags & TLS_SESS_ON_CTRL))) {
      tls_log("SSL/TLS required but absent on control channel, "
        "disconnecting");
      pr_response_send(R_530, "%s", _("Login incorrect."));
      pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_CONFIG_ACL,
        "TLSRequired");
    }
  }

  return PR_DECLINED(cmd);
}

MODRET tls_prot(cmd_rec *cmd) {
  char *prot;

  if (!tls_engine ||
      !session.rfc2228_mech ||
      strncmp(session.rfc2228_mech, "TLS", 4) != 0) {
    return PR_DECLINED(cmd);
  }

  CHECK_CMD_ARGS(cmd, 2);

  if (!(tls_flags & TLS_SESS_ON_CTRL) &&
      !(tls_flags & TLS_SESS_HAVE_CCC)) {
    pr_response_add_err(R_503,
      _("PROT not allowed on insecure control connection"));

    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  /* In theory, we could enforce the RFC 4217 semantics, which require
   * that PBSZ be sent before the PROT command.
   *
   * However, some broken FTPS clients do not send PBSZ before PROT.  And,
   * in practice, since the PBSZ value for FTPS is ALWAYS zero, there is little
   * value in punishing users of these broken clients by refusing to work
   * with their client.
   *
   * Thus we've relaxed our PBSZ requirements, by acting as if PBSZ has been
   * sent already, even if it has not.  For now.
   */

  /* Check for <Limit> restrictions. */
  if (!dir_check(cmd->tmp_pool, cmd, G_NONE, session.cwd, NULL)) {

    pr_log_debug(DEBUG8, "%s %s denied by <Limit> configuration",
      (char *) cmd->argv[0], cmd->arg);
    tls_log("%s: denied by <Limit> configuration", (char *) cmd->argv[0]);
    pr_response_add_err(R_534, _("Unwilling to accept security parameters"));

    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  /* Only PROT C or PROT P is valid with respect to SSL/TLS. */
  prot = cmd->argv[1];
  if (strncmp(prot, "C", 2) == 0) {
    char *mesg = "Protection set to Clear";

    if (tls_required_on_data != 1) {
      /* Only accept this if SSL/TLS is not required, by policy, on data
       * connections.
       */
      tls_flags &= ~TLS_SESS_NEED_DATA_PROT;
      pr_response_add(R_200, "%s", mesg);
      tls_log("%s", mesg);

    } else {
      pr_response_add_err(R_534, _("Unwilling to accept security parameters"));
      tls_log("%s: TLSRequired requires protection for data transfers",
        (char *) cmd->argv[0]);
      tls_log("%s: unwilling to accept security parameter (%s)",
        (char *) cmd->argv[0], prot);

      pr_cmd_set_errno(cmd, EPERM);
      errno = EPERM;
      return PR_ERROR(cmd);
    }

  } else if (strncmp(prot, "P", 2) == 0) {
    char *mesg = "Protection set to Private";

    if (tls_required_on_data != -1) {
      /* Only accept this if SSL/TLS is allowed, by policy, on data
       * connections.
       */
      tls_flags |= TLS_SESS_NEED_DATA_PROT;
      pr_response_add(R_200, "%s", mesg);
      tls_log("%s", mesg);

    } else {
      pr_response_add_err(R_534, _("Unwilling to accept security parameters"));
      tls_log("%s: TLSRequired does not allow protection for data transfers",
        (char *) cmd->argv[0]);
      tls_log("%s: unwilling to accept security parameter (%s)",
        (char *) cmd->argv[0], prot);

      pr_cmd_set_errno(cmd, EPERM);
      errno = EPERM;
      return PR_ERROR(cmd);
    }

  } else if (strncmp(prot, "S", 2) == 0 ||
             strncmp(prot, "E", 2) == 0) {
    pr_response_add_err(R_536, _("PROT %s unsupported"), prot);

    /* By the time the logic reaches this point, there must have been
     * an SSL/TLS session negotiated; other AUTH mechanisms will handle
     * things differently, and when they do, the logic of this handler
     * would not reach this point.  This means that it would not be impolite
     * to return ERROR here, rather than DECLINED: it shows that mod_tls
     * is handling the security mechanism, and that this module does not
     * allow for the unsupported PROT levels.
     */

    pr_cmd_set_errno(cmd, ENOSYS);
    errno = ENOSYS;
    return PR_ERROR(cmd);

  } else {
    pr_response_add_err(R_504, _("PROT %s unsupported"), prot);

    pr_cmd_set_errno(cmd, ENOSYS);
    errno = ENOSYS;
    return PR_ERROR(cmd);
  }

  tls_flags |= TLS_SESS_PBSZ_OK;
  return PR_HANDLED(cmd);
}

MODRET tls_sscn(cmd_rec *cmd) {

  if (tls_engine == FALSE ||
      session.rfc2228_mech == NULL ||
      strncmp(session.rfc2228_mech, "TLS", 4) != 0) {
    return PR_DECLINED(cmd);
  }

  if (cmd->argc > 2) {
    int xerrno = EINVAL;

    tls_log("denying malformed SSCN command: '%s %s'", (char *) cmd->argv[0],
      cmd->arg);
    pr_response_add_err(R_504, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));

    pr_cmd_set_errno(cmd, xerrno);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (!dir_check(cmd->tmp_pool, cmd, cmd->group, session.cwd, NULL)) {
    int xerrno = EPERM;

    pr_log_debug(DEBUG8, "%s %s denied by <Limit> configuration",
      (char *) cmd->argv[0], cmd->arg);
    tls_log("%s denied by <Limit> configuration", (char *) cmd->argv[0]);
    pr_response_add_err(R_550, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));

    pr_cmd_set_errno(cmd, xerrno);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (cmd->argc == 1) {
    /* Client is querying our SSCN mode. */
    pr_response_add(R_200, "%s:%s METHOD", (char *) cmd->argv[0],
      tls_sscn_mode == TLS_SSCN_MODE_SERVER ? "SERVER" : "CLIENT");

  } else {
    /* Parameter MUST be one of: "ON", "OFF. */
    if (strncmp(cmd->argv[1], "ON", 3) == 0) {
      tls_sscn_mode = TLS_SSCN_MODE_CLIENT;
      pr_response_add(R_200, "%s:CLIENT METHOD", (char *) cmd->argv[0]);

    } else if (strncmp(cmd->argv[1], "OFF", 4) == 0) {
      tls_sscn_mode = TLS_SSCN_MODE_SERVER;
      pr_response_add(R_200, "%s:SERVER METHOD", (char *) cmd->argv[0]);

    } else {
      int xerrno = EINVAL;

      tls_log("denying unsupported SSCN command: '%s %s'",
        (char *) cmd->argv[0], (char *) cmd->argv[1]);
      pr_response_add_err(R_501, _("%s: %s"), (char *) cmd->argv[0],
        strerror(xerrno));

      pr_cmd_set_errno(cmd, xerrno);
      errno = xerrno;
      return PR_ERROR(cmd);
    }
  }

  return PR_HANDLED(cmd);
}

/* Configuration handlers
 */

/* usage: TLSCACertificateFile file */
MODRET set_tlscacertfile(cmd_rec *cmd) {
  int res;
  char *path;
  SSL_CTX *ctx;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT

  ctx = SSL_CTX_new(SSLv23_server_method());
  if (ctx != NULL) {
    res = SSL_CTX_load_verify_locations(ctx, path, NULL);
    if (res != 1) {
      unsigned long err_code;
      const char *err_msg;

      PRIVS_RELINQUISH

      /* Unfortunately, if the specified path exists but does not contain
       * any certificate data, the error queue is not helpful.  Thanks,
       * OpenSSL.  Thus we have to peek first.
       */
      err_code = ERR_peek_error();
      if (err_code != 0) {
        err_msg = tls_get_errors2(cmd->tmp_pool);

      } else {
        err_msg = "file contained no certificate data";
      }

      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "unable to use '", path, "': ", err_msg, NULL));
    }

    SSL_CTX_free(ctx);

  } else {
    res = file_exists2(cmd->tmp_pool, path);
    if (res == FALSE) {
      PRIVS_RELINQUISH
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
        NULL));
    }
  }

  PRIVS_RELINQUISH

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
}

/* usage: TLSCACertificatePath path */
MODRET set_tlscacertpath(cmd_rec *cmd) {
  int res;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT
  res = dir_exists2(cmd->tmp_pool, path);
  PRIVS_RELINQUISH

  if (res == FALSE) {
    CONF_ERROR(cmd, "parameter must be a directory path");
  }

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
}

/* usage: TLSCARevocationFile file */
MODRET set_tlscacrlfile(cmd_rec *cmd) {
  int res;
  char *path;
  X509_STORE *store;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT

  store = X509_STORE_new();
  if (store != NULL) {
    res = X509_STORE_load_locations(store, path, NULL);
    if (res != 1) {
      unsigned long err_code;
      const char *err_msg;

      PRIVS_RELINQUISH

      /* Unfortunately, if the specified path exists but does not contain
       * any CRL data, the error queue is not helpful.  Thanks, OpenSSL.
       * Thus we have to peek first.
       */
      err_code = ERR_peek_error();
      if (err_code != 0) {
        err_msg = tls_get_errors2(cmd->tmp_pool);

      } else {
        err_msg = "file contained no CRL data";
      }

      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "unable to use '", path, "': ", err_msg, NULL));
    }

    X509_STORE_free(store);

  } else {
    res = file_exists2(cmd->tmp_pool, path);
    if (res == FALSE) {
      PRIVS_RELINQUISH
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
        NULL));
    }
  }

  PRIVS_RELINQUISH

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
}

/* usage: TLSCARevocationPath path */
MODRET set_tlscacrlpath(cmd_rec *cmd) {
  int res;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT
  res = dir_exists2(cmd->tmp_pool, path);
  PRIVS_RELINQUISH

  if (!res) {
    CONF_ERROR(cmd, "parameter must be a directory path");
  }

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
}

/* usage: TLSCertificateChainFile file */
MODRET set_tlscertchain(cmd_rec *cmd) {
  int res;
  char *path;
  SSL_CTX *ctx;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT

  ctx = SSL_CTX_new(SSLv23_server_method());
  if (ctx != NULL) {
    res = SSL_CTX_use_certificate_chain_file(ctx, path);
    if (res != 1) {
      unsigned long err_code;
      const char *err_msg;

      PRIVS_RELINQUISH

      /* Unfortunately, if the specified path exists but does not contain
       * any certificate data, the error queue is not helpful.  Thanks,
       * OpenSSL.  Thus we have to peek first.
       */
      err_code = ERR_peek_error();
      if (err_code != 0) {
        err_msg = tls_get_errors2(cmd->tmp_pool);

      } else {
        err_msg = "file contained no certificate data";
      }

      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "unable to use '", path, "': ", err_msg, NULL));
    }

    SSL_CTX_free(ctx);

  } else {
    res = file_exists2(cmd->tmp_pool, path);
    if (res == FALSE) {
      PRIVS_RELINQUISH
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
        NULL));
    }
  }

  PRIVS_RELINQUISH

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
}

/* usage: TLSCipherSuite [protocol] string */
MODRET set_tlsciphersuite(cmd_rec *cmd) {
  config_rec *c = NULL;
  char *ciphersuite = NULL;
  int protocol = 0;
  SSL_CTX *ctx;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (cmd->argc-1 == 1) {
    ciphersuite = cmd->argv[1];

    /* Currently, OpenSSL ciphersuite names for TLSv1.3 all use underscores;
     * ciphersuite names for TLSv1.2 and older do NOT use underscores.
     *
     * So if we see an underscore in the configured ciphersuites here, we
     * know that the optional protocol parameter has NOT been used, and that
     * a TLSv1.3 ciphersuite is being configured -- and that this situation
     * will be silently ignored by OpenSSL.
     */
    if (strchr(ciphersuite, '_') != NULL) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "use of TLSv1.3 ciphersuite in '", ciphersuite,
        "' requires protocol parameter; use 'TLSCipherSuite TLSv1.3 ",
        ciphersuite, "'", NULL));
    }

  } else if (cmd->argc-1 == 2) {
    char *protocol_text;

    protocol_text = cmd->argv[1];
    if (strcasecmp(protocol_text, "TLSv1.3") == 0) {
      protocol = TLS_PROTO_TLS_V1_3;

    } else if (strcasecmp(protocol_text, "TLSv1.2") == 0) {
      protocol = TLS_PROTO_TLS_V1_2;

    } else if (strcasecmp(protocol_text, "TLSv1.1") == 0) {
      protocol = TLS_PROTO_TLS_V1_1;

    } else if (strcasecmp(protocol_text, "TLSv1.0") == 0) {
      protocol = TLS_PROTO_TLS_V1;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "unknown/unsupported protocol specifier: ", protocol_text, NULL));
    }

    ciphersuite = cmd->argv[2];
  }

  c = add_config_param(cmd->argv[0], 2, NULL, NULL);

  if (protocol == TLS_PROTO_TLS_V1_3) {
    ciphersuite = pstrdup(c->pool, ciphersuite);

  } else {
    /* Make sure that EXPORT ciphers cannot be used, per Bug#4163. Note that
     * this breaks system profiles, so handle them specially.
     */
    if (strncmp(ciphersuite, "PROFILE=", 8) == 0) {
      ciphersuite = pstrdup(c->pool, ciphersuite);

    } else {
      ciphersuite = pstrcat(c->pool, ciphersuite, ":!EXPORT", NULL);
    }
  }

  /* Check that our construct ciphersuite is acceptable. */
  ctx = SSL_CTX_new(SSLv23_server_method());
  if (ctx != NULL) {
    int res = 1;

    if (protocol == TLS_PROTO_TLS_V1_3) {
#if OPENSSL_VERSION_NUMBER >= 0x10101000L && \
    defined(TLS1_3_VERSION)
      res = SSL_CTX_set_ciphersuites(ctx, ciphersuite);
#endif /* TLS1_3_VERSION */

    } else {
      res = SSL_CTX_set_cipher_list(ctx, ciphersuite);
    }

    if (res != 1) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "unable to use ciphersuite '", ciphersuite, "': ",
        tls_get_errors2(cmd->tmp_pool), NULL));
    }

    SSL_CTX_free(ctx);
  }

  c->argv[0] = ciphersuite;
  c->argv[1] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[1]) = protocol;

  if (pr_module_exists("mod_ifsession.c")) {
    /* These are needed in case this directive is used with mod_ifsession
     * configuration.
     */
    c->flags |= CF_MULTI;
  }

  return PR_HANDLED(cmd);
}

/* usage: TLSControlsACLs actions|all allow|deny user|group list */
MODRET set_tlsctrlsacls(cmd_rec *cmd) {
#if defined(PR_USE_CTRLS)
  char *bad_action = NULL, **actions = NULL;

  CHECK_ARGS(cmd, 4);
  CHECK_CONF(cmd, CONF_ROOT);

  actions = pr_ctrls_parse_acl(cmd->tmp_pool, cmd->argv[1]);

  /* Check the second parameter to make sure it is "allow" or "deny" */
  if (strcmp(cmd->argv[2], "allow") != 0 &&
      strcmp(cmd->argv[2], "deny") != 0) {
    CONF_ERROR(cmd, "second parameter must be 'allow' or 'deny'");
  }

  /* Check the third parameter to make sure it is "user" or "group" */
  if (strcmp(cmd->argv[3], "user") != 0 &&
      strcmp(cmd->argv[3], "group") != 0) {
    CONF_ERROR(cmd, "third parameter must be 'user' or 'group'");
  }

  bad_action = pr_ctrls_set_module_acls(tls_acttab, tls_act_pool, actions,
    cmd->argv[2], cmd->argv[3], cmd->argv[4]);
  if (bad_action != NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown action: '",
      bad_action, "'", NULL));
  }

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", cmd->argv[0],
    " directive requires Controls support (--enable-ctrls)", NULL));
#endif /* PR_USE_CTRLS */
}

/* usage: TLSCryptoDevice driver|"ALL" */
MODRET set_tlscryptodevice(cmd_rec *cmd) {
#if OPENSSL_VERSION_NUMBER > 0x000907000L
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  (void) add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);

  return PR_HANDLED(cmd);

#else /* OpenSSL is too old for ENGINE support */
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", cmd->argv[0],
    "directive cannot be used on the system, as the OpenSSL version is too old",
    NULL));
#endif
}

/* usage: TLSDHParamFile file */
MODRET set_tlsdhparamfile(cmd_rec *cmd) {
  int res;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT
  res = file_exists2(cmd->tmp_pool, path);
  PRIVS_RELINQUISH

  if (res == FALSE) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
      NULL));
  }

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
}

/* usage: TLSDSACertificateFile file */
MODRET set_tlsdsacertfile(cmd_rec *cmd) {
  char *path;
  const char *fingerprint, *errstr = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];
  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  PRIVS_ROOT
  fingerprint = tls_get_fingerprint_from_file(cmd->tmp_pool, path, EVP_PKEY_DSA,
    &errstr);
  PRIVS_RELINQUISH

  if (fingerprint == NULL) {
    if (errstr == NULL) {
      errstr = "does not exist or does not contain a certificate";
    }

    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use '", path, "': ",
      errstr, NULL));
  }

  add_config_param_str(cmd->argv[0], 2, path, fingerprint);
  return PR_HANDLED(cmd);
}

/* usage: TLSDSACertificateKeyFile file */
MODRET set_tlsdsakeyfile(cmd_rec *cmd) {
  int res;
  char *path;
  SSL_CTX *ctx;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT

  ctx = SSL_CTX_new(SSLv23_server_method());
  if (ctx != NULL) {
    /* Note that the configured key file might be passphrase-protected.  We
     * do not necessarily want to prompt for the passphrase here, so if that
     * is the error returned, it is an expected condition, and indicates that
     * the encoding of the key is acceptable.
     */
    SSL_CTX_set_default_passwd_cb(ctx, tls_keyfile_check_cb);

    res = SSL_CTX_use_PrivateKey_file(ctx, path, X509_FILETYPE_PEM);
    if (res != 1) {
      unsigned long err_code;

      err_code = ERR_peek_error();
      switch (ERR_GET_REASON(err_code)) {
        /* These are "expected" error codes from working with
         * passphrase-protected keys.
         */
        case EVP_R_BAD_DECRYPT:
        case PEM_R_BAD_PASSWORD_READ:
          break;

        default: {
          PRIVS_RELINQUISH

          CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use '", path, "': ",
            tls_get_errors2(cmd->tmp_pool), NULL));
        }
      }
    }

    SSL_CTX_free(ctx);

  } else {
    res = file_exists2(cmd->tmp_pool, path);
    if (res == FALSE) {
      PRIVS_RELINQUISH
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
        NULL));
    }
  }

  PRIVS_RELINQUISH

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
}

/* usage: TLSECCertificateFile file */
MODRET set_tlseccertfile(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL_ECC
  char *path;
  const char *fingerprint, *errstr = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];
  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  PRIVS_ROOT
  fingerprint = tls_get_fingerprint_from_file(cmd->tmp_pool, path, EVP_PKEY_EC,
    &errstr);
  PRIVS_RELINQUISH

  if (fingerprint == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path,
      "' does not exist or does not contain a certificate", NULL));
  }

  add_config_param_str(cmd->argv[0], 2, path, fingerprint);
  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", (char *) cmd->argv[0],
    " directive cannot be used on this system, as your OpenSSL version "
    "does not have EC support", NULL));
#endif /* PR_USE_OPENSSL_ECC */
}

/* usage: TLSECCertificateKeyFile file */
MODRET set_tlseckeyfile(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL_ECC
  int res;
  char *path;
  SSL_CTX *ctx;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT

  ctx = SSL_CTX_new(SSLv23_server_method());
  if (ctx != NULL) {
    /* Note that the configured key file might be passphrase-protected.  We
     * do not necessarily want to prompt for the passphrase here, so if that
     * is the error returned, it is an expected condition, and indicates that
     * the encoding of the key is acceptable.
     */
    SSL_CTX_set_default_passwd_cb(ctx, tls_keyfile_check_cb);

    res = SSL_CTX_use_PrivateKey_file(ctx, path, X509_FILETYPE_PEM);
    if (res != 1) {
      unsigned long err_code;

      err_code = ERR_peek_error();
      switch (ERR_GET_REASON(err_code)) {
        /* These are "expected" error codes from working with
         * passphrase-protected keys.
         */
        case EVP_R_BAD_DECRYPT:
        case PEM_R_BAD_PASSWORD_READ:
          break;

        default: {
          PRIVS_RELINQUISH

          CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use '", path, "': ",
            tls_get_errors2(cmd->tmp_pool), NULL));
        }
      }
    }

    SSL_CTX_free(ctx);

  } else {
    res = file_exists2(cmd->tmp_pool, path);
    if (res == FALSE) {
      PRIVS_RELINQUISH
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
        NULL));
    }
  }

  PRIVS_RELINQUISH

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", (char *) cmd->argv[0],
    " directive cannot be used on this system, as your OpenSSL version "
    "does not have EC support", NULL));
#endif /* PR_USE_OPENSSL_ECC */
}

/* usage: TLSECDHCurve name */
MODRET set_tlsecdhcurve(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL_ECC
  char *curve_names = NULL;
# if defined(SSL_CTX_set1_curves_list)
  SSL_CTX *ctx = NULL;
# else
  int curve_nid = -1;
  EC_KEY *ec_key = NULL;
# endif /* No SSL_CTX_set1_curves_list; pre-OpenSSL 1.0.2 */

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  curve_names = cmd->argv[1];

# if defined(SSL_CTX_set1_curves_list)
  if (strcasecmp(curve_names, "auto") != 0) {
    ctx = SSL_CTX_new(SSLv23_server_method());
  }

  if (ctx != NULL) {
    int res;

    res = SSL_CTX_set1_curves_list(ctx, curve_names);
    if (res != 1) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use ECDH curves '",
        curve_names, "': ", tls_get_errors2(cmd->tmp_pool), NULL));
    }

    SSL_CTX_free(ctx);
  }

  (void) add_config_param_str(cmd->argv[0], 1, curve_names);

# else
  /* The special-case handling of these curve names is copied from OpenSSL's
   * apps/ecparam.c code.
   */

  if (strcmp(curve_names, "secp192r1") == 0) {
    curve_nid = NID_X9_62_prime192v1;

  } else if (strcmp(curve_names, "secp256r1") == 0) {
    curve_nid = NID_X9_62_prime256v1;

  } else {
    curve_nid = OBJ_sn2nid(curve_names);
  }

  ec_key = EC_KEY_new_by_curve_name(curve_nid);
  if (ec_key == NULL) {
    char *err_str = "unknown/unsupported curve";

    if (curve_nid > 0) {
      err_str = ERR_error_string(ERR_get_error(), NULL);
    }

    if (strchr(curve_names, ':') != NULL) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "configuring multiple curves '", curve_names,
        "' not supported by OpenSSL version ", OPENSSL_VERSION_TEXT, NULL));

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to create '", curve_names,
        "' EC curve: ", err_str, NULL));
    }
  }

  (void) add_config_param(cmd->argv[0], 1, ec_key);
# endif /* pre-OpenSSL-1.0.2 */

  return PR_HANDLED(cmd);

#else
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", cmd->argv[0],
    " directive cannot be used on this system, as your OpenSSL version "
    "does not have EC support", NULL));
#endif /* PR_USE_OPENSSL_ECC */
}

/* usage: TLSEngine on|off */
MODRET set_tlsengine(cmd_rec *cmd) {
  int engine = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  engine = get_boolean(cmd, 1);
  if (engine == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = engine;

  return PR_HANDLED(cmd);
}

/* usage: TLSLog file */
MODRET set_tlslog(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* usage: TLSMasqueradeAddress ip-addr|dns-name */
MODRET set_tlsmasqaddr(cmd_rec *cmd) {
  config_rec *c = NULL;
  const pr_netaddr_t *masq_addr = NULL;
  unsigned int addr_flags = PR_NETADDR_GET_ADDR_FL_INCL_DEVICE;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL);

  /* We can only masquerade as one address, so we don't need to know if the
   * given name might map to multiple addresses.
   */
  masq_addr = pr_netaddr_get_addr2(cmd->server->pool, cmd->argv[1], NULL,
    addr_flags);
  if (masq_addr == NULL) {
    return PR_ERROR_MSG(cmd, NULL, pstrcat(cmd->tmp_pool, cmd->argv[0],
      ": unable to resolve \"", cmd->argv[1], "\"", NULL));
  }

  c = add_config_param(cmd->argv[0], 2, (void *) masq_addr, NULL);
  c->argv[1] = pstrdup(c->pool, cmd->argv[1]);

  return PR_HANDLED(cmd);
}

/* usage: TLSNextProtocol on|off */
MODRET set_tlsnextprotocol(cmd_rec *cmd) {
#if !defined(OPENSSL_NO_TLSEXT)
  config_rec *c;
  int use_next_protocol = FALSE;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  use_next_protocol = get_boolean(cmd, 1);
  if (use_next_protocol == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = use_next_protocol;
  return PR_HANDLED(cmd);

#else
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", cmd->argv[0],
    " directive cannot be used on this system, as your OpenSSL version "
    "does not have NPN/ALPN support", NULL));
#endif /* !OPENSSL_NO_TLSEXT */
}

/* usage: TLSOptions opt1 opt2 ... */
MODRET set_tlsoptions(cmd_rec *cmd) {
  config_rec *c = NULL;
  register unsigned int i = 0;
  unsigned long opts = 0UL;

  if (cmd->argc-1 == 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  c = add_config_param(cmd->argv[0], 1, NULL);

  for (i = 1; i < cmd->argc; i++) {
    if (strcmp(cmd->argv[i], "AllowDotLogin") == 0) {
      opts |= TLS_OPT_ALLOW_DOT_LOGIN;

    } else if (strcmp(cmd->argv[i], "AllowPerUser") == 0) {
      opts |= TLS_OPT_ALLOW_PER_USER;

    } else if (strcmp(cmd->argv[i], "AllowWeakDH") == 0) {
      opts |= TLS_OPT_ALLOW_WEAK_DH;

    } else if (strcmp(cmd->argv[i], "AllowWeakSecurity") == 0) {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
      opts |= TLS_OPT_ALLOW_WEAK_SECURITY;
#else
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", cmd->argv[i],
        " option cannot be used on this system, as your OpenSSL version "
        "is too old; requires OpenSSL-1.1.0 or later", NULL));
#endif /* OpenSSL-1.1.0 and later */

    } else if (strcmp(cmd->argv[i], "AllowClientRenegotiation") == 0 ||
               strcmp(cmd->argv[i], "AllowClientRenegotiations") == 0) {
      opts |= TLS_OPT_ALLOW_CLIENT_RENEGOTIATIONS;

    } else if (strcmp(cmd->argv[i], "EnableDiags") == 0) {
      opts |= TLS_OPT_ENABLE_DIAGS;

    } else if (strcmp(cmd->argv[i], "ExportCertData") == 0) {
      opts |= TLS_OPT_EXPORT_CERT_DATA;

    } else if (strcmp(cmd->argv[i], "IgnoreSNI") == 0) {
      opts |= TLS_OPT_IGNORE_SNI;

#ifdef SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS
    } else if (strcmp(cmd->argv[i], "NoEmptyFragments") == 0) {
      /* Unlike the other TLSOptions, this option is handled slightly
       * differently, due to the fact that option affects the creation
       * of the SSL_CTX.
       *
       */
      tls_ssl_opts |= SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;
#else
      pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION ": TLSOption NoEmptyFragments not supported (OpenSSL version is too old)");
#endif

    } else if (strcmp(cmd->argv[i], "NoSessionReuseRequired") == 0) {
      opts |= TLS_OPT_NO_SESSION_REUSE_REQUIRED;

    } else if (strcmp(cmd->argv[i], "StdEnvVars") == 0) {
      opts |= TLS_OPT_STD_ENV_VARS;

    } else if (strcmp(cmd->argv[i], "dNSNameRequired") == 0) {
      opts |= TLS_OPT_VERIFY_CERT_FQDN;

    } else if (strcmp(cmd->argv[i], "iPAddressRequired") == 0) {
      opts |= TLS_OPT_VERIFY_CERT_IP_ADDR;

    } else if (strcmp(cmd->argv[i], "UseImplicitSSL") == 0) {
      opts |= TLS_OPT_USE_IMPLICIT_SSL;

    } else if (strcmp(cmd->argv[i], "CommonNameRequired") == 0) {
      opts |= TLS_OPT_VERIFY_CERT_CN;

    } else if (strcmp(cmd->argv[i], "NoAutoECDH") == 0) {
      opts |= TLS_OPT_NO_AUTO_ECDH;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown TLSOption '",
        cmd->argv[i], "'", NULL));
    }
  }

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = opts;

  if (pr_module_exists("mod_ifsession.c")) {
    /* These are needed in case this directive is used with mod_ifsession
     * configuration.
     */
    c->flags |= CF_MULTI;
  }

  return PR_HANDLED(cmd);
}

/* usage: TLSPassPhraseProvider path */
MODRET set_tlspassphraseprovider(cmd_rec *cmd) {
  struct stat st;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT);

  path = cmd->argv[1];

  if (*path != '/') {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "must be a full path: '", path, "'",
      NULL));
  }

  if (stat(path, &st) < 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error checking '", path, "': ",
      strerror(errno), NULL));
  }

  if (!S_ISREG(st.st_mode)) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use '", path,
      ": Not a regular file", NULL));
  }

  tls_passphrase_provider = pstrdup(permanent_pool, path);
  return PR_HANDLED(cmd);
}

/* usage: TLSPKCS12File file */
MODRET set_tlspkcs12file(cmd_rec *cmd) {
  int res;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT
  res = file_exists2(cmd->tmp_pool, path);
  PRIVS_RELINQUISH

  if (!res) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
      NULL));
  }

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
}

/* usage: TLSPreSharedKey name path */
MODRET set_tlspresharedkey(cmd_rec *cmd) {
#if defined(PSK_MAX_PSK_LEN)
  char *identity, *path;
  size_t identity_len, path_len;

  CHECK_ARGS(cmd, 2);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  identity = cmd->argv[1];
  path = cmd->argv[2];

  identity_len = strlen(identity);
  if (identity_len > PSK_MAX_IDENTITY_LEN) {
    char buf[32];

    memset(buf, '\0', sizeof(buf));
    pr_snprintf(buf, sizeof(buf)-1, "%u", (unsigned int) PSK_MAX_IDENTITY_LEN);

    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
      "TLSPreSharedKey identity '", identity, "' exceed maximum length ",
      buf, path, NULL))
  }

  /* Ensure that the given path starts with "hex:", denoting the
   * format of the key at the given path.  Support for other formats, e.g.
   * bcrypt or somesuch, will be added later.
   */
  path_len = strlen(path);
  if (path_len < 5 ||
      strncmp(path, "hex:", 4) != 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
      "unsupported TLSPreSharedKey format: ", path, NULL))
  }

  (void) add_config_param_str(cmd->argv[0], 2, identity, path);
#else
  pr_log_debug(DEBUG0,
    "%s is not supported by this build/version of OpenSSL, ignoring",
    (char *) cmd->argv[0]);
#endif /* PSK_MAX_PSK_LEN */

  return PR_HANDLED(cmd);
}

/* usage: TLSProtocol version1 ... versionN */
MODRET set_tlsprotocol(cmd_rec *cmd) {
  register unsigned int i;
  config_rec *c;
  unsigned int protocols = 0;

  if (cmd->argc-1 == 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (strcasecmp(cmd->argv[1], "all") == 0) {
    /* We're in an additive/subtractive type of configuration. */
    protocols = TLS_PROTO_ALL;

    for (i = 2; i < cmd->argc; i++) {
      int disable = FALSE;
      char *proto_name;

      proto_name = cmd->argv[i];

      if (*proto_name == '+') {
        proto_name++;

      } else if (*proto_name == '-') {
        disable = TRUE;
        proto_name++;

      } else {
        /* Using the additive/subtractive approach requires a +/- prefix;
         * it's malformed without such prefaces.
         */
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "missing required +/- prefix: ",
          proto_name, NULL));
      }

      if (strncasecmp(proto_name, "SSLv3", 6) == 0) {
        if (disable) {
          protocols &= ~TLS_PROTO_SSL_V3;
        } else {
          protocols |= TLS_PROTO_SSL_V3;
        }

      } else if (strncasecmp(proto_name, "TLSv1", 6) == 0 ||
                 strncasecmp(proto_name, "TLSv1.0", 8) == 0) {
        if (disable) {
          protocols &= ~TLS_PROTO_TLS_V1;
        } else {
          protocols |= TLS_PROTO_TLS_V1;
        }

      } else if (strncasecmp(proto_name, "TLSv1.1", 8) == 0) {
#if OPENSSL_VERSION_NUMBER >= 0x10001000L
        if (disable) {
          protocols &= ~TLS_PROTO_TLS_V1_1;
        } else {
          protocols |= TLS_PROTO_TLS_V1_1;
        }
#else
        CONF_ERROR(cmd, "Your OpenSSL installation does not support TLSv1.1");
#endif /* OpenSSL 1.0.1 or later */

      } else if (strncasecmp(proto_name, "TLSv1.2", 8) == 0) {
#if OPENSSL_VERSION_NUMBER >= 0x10001000L
        if (disable) {
          protocols &= ~TLS_PROTO_TLS_V1_2;
        } else {
          protocols |= TLS_PROTO_TLS_V1_2;
        }
#else
        CONF_ERROR(cmd, "Your OpenSSL installation does not support TLSv1.2");
#endif /* OpenSSL 1.0.1 or later */

      } else if (strncasecmp(proto_name, "TLSv1.3", 8) == 0) {
#ifdef TLS1_3_VERSION
        if (disable) {
          protocols &= ~TLS_PROTO_TLS_V1_3;
        } else {
          protocols |= TLS_PROTO_TLS_V1_3;
        }
#else
        CONF_ERROR(cmd, "Your OpenSSL installation does not support TLSv1.3");
#endif /* OpenSSL 1.1.1 or later */

      } else {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown protocol: '",
          cmd->argv[i], "'", NULL));
      }
    }

  } else {
    for (i = 1; i < cmd->argc; i++) {
      if (strncasecmp(cmd->argv[i], "SSLv23", 7) == 0) {
        protocols |= TLS_PROTO_SSL_V3;
        protocols |= TLS_PROTO_TLS_V1;
#ifdef SSL_OP_NO_TLSv1_1
        protocols |= TLS_PROTO_TLS_V1_1;
#endif
#ifdef SSL_OP_NO_TLSv1_2
        protocols |= TLS_PROTO_TLS_V1_2;
#endif
#ifdef SSL_OP_NO_TLSv1_3
        protocols |= TLS_PROTO_TLS_V1_3;
#endif

      } else if (strncasecmp(cmd->argv[i], "SSLv3", 6) == 0) {
        protocols |= TLS_PROTO_SSL_V3;

      } else if (strncasecmp(cmd->argv[i], "TLSv1", 6) == 0 ||
                 strncasecmp(cmd->argv[i], "TLSv1.0", 8) == 0) {
        protocols |= TLS_PROTO_TLS_V1;

      } else if (strncasecmp(cmd->argv[i], "TLSv1.1", 8) == 0) {
#if OPENSSL_VERSION_NUMBER >= 0x10001000L
        protocols |= TLS_PROTO_TLS_V1_1;
#else
        CONF_ERROR(cmd, "Your OpenSSL installation does not support TLSv1.1");
#endif /* OpenSSL 1.0.1 or later */

      } else if (strncasecmp(cmd->argv[i], "TLSv1.2", 8) == 0) {
#if OPENSSL_VERSION_NUMBER >= 0x10001000L
        protocols |= TLS_PROTO_TLS_V1_2;
#else
        CONF_ERROR(cmd, "Your OpenSSL installation does not support TLSv1.2");
#endif /* OpenSSL 1.0.1 or later */

      } else if (strncasecmp(cmd->argv[i], "TLSv1.3", 8) == 0) {
#ifdef TLS1_3_VERSION
        protocols |= TLS_PROTO_TLS_V1_3;
#else
        CONF_ERROR(cmd, "Your OpenSSL installation does not support TLSv1.3");
#endif /* OpenSSL 1.1.1 or later */

      } else {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown protocol: '",
          cmd->argv[i], "'", NULL));
      }
    }
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[0]) = protocols;

  return PR_HANDLED(cmd);
}

/* usage: TLSRandomSeed file */
MODRET set_tlsrandseed(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* usage: TLSRenegotiate [ctrl nsecs] [data nbytes] */
MODRET set_tlsrenegotiate(cmd_rec *cmd) {
#if OPENSSL_VERSION_NUMBER > 0x000907000L
  register unsigned int i = 0;
  config_rec *c = NULL;

  if (cmd->argc-1 < 1 ||
      cmd->argc-1 > 8) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (strncasecmp(cmd->argv[1], "none", 5) == 0) {
    add_config_param(cmd->argv[0], 0);
    return PR_HANDLED(cmd);
  }

  c = add_config_param(cmd->argv[0], 4, NULL, NULL, NULL, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = 0;
  c->argv[1] = pcalloc(c->pool, sizeof(off_t));
  *((off_t *) c->argv[1]) = 0;
  c->argv[2] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[2]) = 0;
  c->argv[3] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[3]) = TRUE;

  for (i = 1; i < cmd->argc;) {
    if (strcmp(cmd->argv[i], "ctrl") == 0) {
      int secs;

      secs = atoi(cmd->argv[i+1]);
      *((int *) c->argv[0]) = secs;

      i += 2;

    } else if (strcmp(cmd->argv[i], "data") == 0) {
      char *ptr = NULL;
      unsigned long kbytes;

      kbytes = strtoul(cmd->argv[i+1], &ptr, 10);
      if (ptr && *ptr) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, cmd->argv[i],
          " must be valid number: '", cmd->argv[i+1], "'", NULL));
      }

      *((off_t *) c->argv[1]) = (off_t) kbytes * 1024;

      i += 2;

    } else if (strcmp(cmd->argv[i], "required") == 0) {
      int required;

      required = get_boolean(cmd, i+1);
      if (required != -1) {
        *((unsigned char *) c->argv[3]) = required;

      } else {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, cmd->argv[i],
          " must be a Boolean value: '", cmd->argv[i+1], "'", NULL));
      }

      i += 2;

    } else if (strcmp(cmd->argv[i], "timeout") == 0) {
      int secs = atoi(cmd->argv[i+1]);

      if (secs > 0) {
        *((int *) c->argv[2]) = secs;

      } else {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, cmd->argv[i],
          " must be greater than zero: '", cmd->argv[i+1], "'", NULL));
      }

      i += 2;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        ": unknown TLSRenegotiate argument '", cmd->argv[i], "'", NULL));
    }
  }

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, " requires OpenSSL-0.9.7 or greater",
    NULL));
#endif
}

/* usage: TLSRequired on|off|both|control|ctrl|[!]data|auth|auth+data */
MODRET set_tlsrequired(cmd_rec *cmd) {
  int required = -1;
  int on_auth = 0, on_ctrl = 0, on_data = 0;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|CONF_DIR|
    CONF_DYNDIR);

  required = get_boolean(cmd, 1);
  if (required == -1) {
    if (strcmp(cmd->argv[1], "control") == 0 ||
        strcmp(cmd->argv[1], "ctrl") == 0) {
      on_auth = 1;
      on_ctrl = 1;

    } else if (strcmp(cmd->argv[1], "data") == 0) {
      on_data = 1;

    } else if (strcmp(cmd->argv[1], "!data") == 0) {
      on_data = -1;

    } else if (strcmp(cmd->argv[1], "both") == 0 ||
               strcmp(cmd->argv[1], "ctrl+data") == 0) {
      on_auth = 1;
      on_ctrl = 1;
      on_data = 1;

    } else if (strcmp(cmd->argv[1], "ctrl+!data") == 0) {
      on_auth = 1;
      on_ctrl = 1;
      on_data = -1;

    } else if (strcmp(cmd->argv[1], "auth") == 0) {
      on_auth = 1;

    } else if (strcmp(cmd->argv[1], "auth+data") == 0) {
      on_auth = 1;
      on_data = 1;

    } else if (strcmp(cmd->argv[1], "auth+!data") == 0) {
      on_auth = 1;
      on_data = -1;

    } else
      CONF_ERROR(cmd, "bad parameter");

  } else {
    if (required == TRUE) {
      on_auth = 1;
      on_ctrl = 1;
      on_data = 1;
    }
  }

  c = add_config_param(cmd->argv[0], 3, NULL, NULL, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = on_ctrl;
  c->argv[1] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[1]) = on_data;
  c->argv[2] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[2]) = on_auth;

  c->flags |= CF_MERGEDOWN;

  return PR_HANDLED(cmd);
}

/* usage: TLSRSACertificateFile file */
MODRET set_tlsrsacertfile(cmd_rec *cmd) {
  char *path;
  const char *fingerprint, *errstr = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];
  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  PRIVS_ROOT
  fingerprint = tls_get_fingerprint_from_file(cmd->tmp_pool, path, EVP_PKEY_RSA,
    &errstr);
  PRIVS_RELINQUISH

  if (fingerprint == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path,
      "' does not exist or does not contain a certificate", NULL));
  }

  add_config_param_str(cmd->argv[0], 2, path, fingerprint);
  return PR_HANDLED(cmd);
}

/* usage: TLSRSACertificateKeyFile file */
MODRET set_tlsrsakeyfile(cmd_rec *cmd) {
  int res;
  char *path;
  SSL_CTX *ctx;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT

  ctx = SSL_CTX_new(SSLv23_server_method());
  if (ctx != NULL) {
    /* Note that the configured key file might be passphrase-protected.  We
     * do not necessarily want to prompt for the passphrase here, so if that
     * is the error returned, it is an expected condition, and indicates that
     * the encoding of the key is acceptable.
     */
    SSL_CTX_set_default_passwd_cb(ctx, tls_keyfile_check_cb);

    res = SSL_CTX_use_PrivateKey_file(ctx, path, X509_FILETYPE_PEM);
    if (res != 1) {
      unsigned long err_code;

      err_code = ERR_peek_error();
      switch (ERR_GET_REASON(err_code)) {
        /* These are "expected" error codes from working with
         * passphrase-protected keys.
         */
        case EVP_R_BAD_DECRYPT:
        case PEM_R_BAD_PASSWORD_READ:
          break;

        default: {
          PRIVS_RELINQUISH

          CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use '", path, "': ",
            tls_get_errors2(cmd->tmp_pool), NULL));
        }
      }
    }

    SSL_CTX_free(ctx);

  } else {
    res = file_exists2(cmd->tmp_pool, path);
    if (res == FALSE) {
      PRIVS_RELINQUISH
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
        NULL));
    }
  }

  PRIVS_RELINQUISH

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
}

/* usage: TLSServerCipherPreference on|off */
MODRET set_tlsservercipherpreference(cmd_rec *cmd) {
  int use_server_prefs = -1;
#ifdef SSL_OP_CIPHER_SERVER_PREFERENCE
  config_rec *c = NULL;
#endif

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  use_server_prefs = get_boolean(cmd, 1);
  if (use_server_prefs == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

#ifdef SSL_OP_CIPHER_SERVER_PREFERENCE
  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = use_server_prefs;

#else
  pr_log_debug(DEBUG0,
    "%s is not supported by this version of OpenSSL, ignoring",
    (char *) cmd->argv[0]);
#endif /* SSL_OP_CIPHER_SERVER_PREFERENCE */

  return PR_HANDLED(cmd);
}

/* usage: TLSServerInfoFile path */
MODRET set_tlsserverinfofile(cmd_rec *cmd) {
#if !defined(OPENSSL_NO_TLSEXT) && OPENSSL_VERSION_NUMBER >= 0x10002000L
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];
  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
#else
  pr_log_debug(DEBUG0,
    "%s is not supported by this version of OpenSSL, ignoring",
    (char *) cmd->argv[0]);
#endif /* OPENSSL_NO_TLSEXT */

  return PR_HANDLED(cmd);
}

/* usage: TLSSessionCache "off"|type:/info [timeout] */
MODRET set_tlssessioncache(cmd_rec *cmd) {
  char *provider = NULL, *info = NULL;
  config_rec *c;
  long timeout = -1;
  int enabled = -1;

  if (cmd->argc < 2 ||
      cmd->argc > 3) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT);

  /* Has session caching been explicitly turned off? */
  enabled = get_boolean(cmd, 1);
  if (enabled != FALSE) {
    char *ptr;

    /* Separate the type/info parameter into pieces. */
    ptr = strchr(cmd->argv[1], ':');
    if (ptr == NULL) {
      CONF_ERROR(cmd, "badly formatted parameter");
    }

    *ptr = '\0';
    provider = cmd->argv[1];
    info = ptr + 1;

    /* Verify that the requested cache type has been registered. */
    if (strncmp(provider, "internal", 9) != 0) {
      if (tls_sess_cache_get_cache(provider) == NULL) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "session cache type '",
        provider, "' not available", NULL));
      }
    }
  }

  if (cmd->argc == 3) {
    char *ptr = NULL;

    timeout = strtol(cmd->argv[2], &ptr, 10);
    if (ptr && *ptr) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", cmd->argv[2],
        "' is not a valid timeout value", NULL));
    }

    if (timeout < 1) {
      CONF_ERROR(cmd, "timeout be greater than 1");
    }

  } else {
    /* Default timeout is 30 min (1800 secs). */
    timeout = 1800;
  }

  c = add_config_param(cmd->argv[0], 3, NULL, NULL, NULL);
  if (provider != NULL) {
    c->argv[0] = pstrdup(c->pool, provider);
  }

  if (info != NULL) {
    c->argv[1] = pstrdup(c->pool, info);
  }

  c->argv[2] = palloc(c->pool, sizeof(long));
  *((long *) c->argv[2]) = timeout;

  return PR_HANDLED(cmd);
}

/* usage: TLSSessionTicketKeys [age secs] [count num] */
MODRET set_tlssessionticketkeys(cmd_rec *cmd) {
#if defined(TLS_USE_SESSION_TICKETS)
  register unsigned int i;
  int max_age = -1, max_nkeys = -1;
  config_rec *c = NULL;

  if (cmd->argc != 3 &&
      cmd->argc != 5) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT);

  for (i = 1; i < cmd->argc; i++) {
    if (strcasecmp(cmd->argv[i], "age") == 0) {
      if (pr_str_get_duration(cmd->argv[i+1], &max_age) < 0) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error parsing age value '",
          cmd->argv[i+1], "': ", strerror(errno), NULL));
      }

      /* Note that we do not allow ticket keys to age out faster than 1
       * minute.  Less than that is a bit ridiculous, no?
       */
      if (max_age < 60) {
        CONF_ERROR(cmd, "max key age must be at least 60sec");
      }

      i++;

    } else if (strcasecmp(cmd->argv[i], "count") == 0) {
      max_nkeys = atoi(cmd->argv[i+1]);
      if (max_nkeys < 0) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error parsing count value '",
          cmd->argv[i+1], "': ", strerror(EINVAL), NULL));
      }

      /* Note that we need at least ONE ticket key for session tickets to
       * even work.
       */
      if (max_nkeys < 2) {
        CONF_ERROR(cmd, "max key count must be at least 1");
      }

      i++;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown parameter: ",
        (char *) cmd->argv[i], NULL));
    }
  }

  c = add_config_param(cmd->argv[0], 2, NULL, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[0]) = max_age;
  c->argv[1] = pcalloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[1]) = max_nkeys;

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", cmd->argv[0],
    " directive cannot be used on this system, as your OpenSSL version "
    "does not have session ticket support", NULL));
#endif /* TLS_USE_SESSION_TICKETS */
}

/* usage; TLSSessionTickets on|off */
MODRET set_tlssessiontickets(cmd_rec *cmd) {
#if defined(TLS_USE_SESSION_TICKETS)
  int session_tickets = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  session_tickets = get_boolean(cmd, 1);
  if (session_tickets == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = session_tickets;

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", cmd->argv[0],
    " directive cannot be used on this system, as your OpenSSL version "
    "does not have session ticket support", NULL));
#endif /* TLS_USE_SESSION_TICKETS */
}

/* usage: TLSStapling on|off */
MODRET set_tlsstapling(cmd_rec *cmd) {
#if defined(PR_USE_OPENSSL_OCSP)
  int stapling = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  stapling = get_boolean(cmd, 1);
  if (stapling == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = stapling;

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", cmd->argv[0],
    " directive cannot be used on this system, as your OpenSSL version "
    "does not have OCSP support", NULL));
#endif /* PR_USE_OPENSSL_OCSP */
}

/* usage: TLSStaplingCache "off"|type:/info */
MODRET set_tlsstaplingcache(cmd_rec *cmd) {
  char *provider = NULL, *info = NULL;
  config_rec *c;
  int enabled = -1;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT);

  /* Has OCSP response caching been explicitly turned off? */
  enabled = get_boolean(cmd, 1);
  if (enabled != FALSE) {
    char *ptr;

    /* Separate the type/info parameter into pieces. */
    ptr = strchr(cmd->argv[1], ':');
    if (ptr == NULL) {
      CONF_ERROR(cmd, "badly formatted parameter");
    }

    *ptr = '\0';
    provider = cmd->argv[1];
    info = ptr + 1;

    /* Verify that the requested cache type has been registered. */
    if (tls_ocsp_cache_get_cache(provider) == NULL) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "OCSP stapling cache type '",
        provider, "' not available", NULL));
    }
  }

  c = add_config_param(cmd->argv[0], 2, NULL, NULL);
  if (provider != NULL) {
    c->argv[0] = pstrdup(c->pool, provider);
  }

  if (info != NULL) {
    c->argv[1] = pstrdup(c->pool, info);
  }

  return PR_HANDLED(cmd);
}

/* usage: TLSStaplingOptions opt1 opt2 ... */
MODRET set_tlsstaplingoptions(cmd_rec *cmd) {
#if defined(PR_USE_OPENSSL_OCSP)
  config_rec *c = NULL;
  register unsigned int i = 0;
  unsigned long opts = 0UL;

  if (cmd->argc-1 == 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  c = add_config_param(cmd->argv[0], 1, NULL);

  for (i = 1; i < cmd->argc; i++) {
    if (strcmp(cmd->argv[i], "NoNonce") == 0) {
      opts |= TLS_STAPLING_OPT_NO_NONCE;

    } else if (strcmp(cmd->argv[i], "NoVerify") == 0) {
      opts |= TLS_STAPLING_OPT_NO_VERIFY;

    } else if (strcmp(cmd->argv[i], "NoFakeTryLater") == 0) {
      opts |= TLS_STAPLING_OPT_NO_FAKE_TRY_LATER;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown TLSStaplingOption '",
        cmd->argv[i], "'", NULL));
    }
  }

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = opts;
#endif /* PR_USE_OPENSSL_OCSP */

  return PR_HANDLED(cmd);
}

/* usage: TLSStaplingResponder url */
MODRET set_tlsstaplingresponder(cmd_rec *cmd) {
#if defined(PR_USE_OPENSSL_OCSP)
  char *host = NULL, *port = NULL, *uri = NULL, *url;
  int use_ssl = 0;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  url = cmd->argv[1];
  if (OCSP_parse_url(url, &host, &port, &uri, &use_ssl) != 1) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error parsing URL '", url, "': ",
      tls_get_errors(), NULL));
  }

  OPENSSL_free(host);
  OPENSSL_free(port);
  OPENSSL_free(uri);

  add_config_param_str(cmd->argv[0], 1, url);
#endif /* PR_USE_OPENSSL_OCSP */

  return PR_HANDLED(cmd);
}

/* usage: TLSStaplingTimeout secs */
MODRET set_tlsstaplingtimeout(cmd_rec *cmd) {
  int timeout = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (pr_str_get_duration(cmd->argv[1], &timeout) < 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error parsing timeout value '",
      cmd->argv[1], "': ", strerror(errno), NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[0]) = timeout;

  return PR_HANDLED(cmd);
}

/* usage: TLSTimeoutHandshake secs */
MODRET set_tlstimeouthandshake(cmd_rec *cmd) {
  int timeout = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (pr_str_get_duration(cmd->argv[1], &timeout) < 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error parsing timeout value '",
      cmd->argv[1], "': ", strerror(errno), NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[0]) = timeout;

  return PR_HANDLED(cmd);
}

/* usage: TLSUserName CommonName|EmailSubjAltName|custom-oid */
MODRET set_tlsusername(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  /* Make sure the parameter is either "CommonName",
   * "EmailSubjAltName", or a custom OID.
   */
  if (strcmp(cmd->argv[1], "CommonName") != 0 &&
      strcmp(cmd->argv[1], "EmailSubjAltName") != 0) {
    register unsigned int i;
    char *param;
    size_t param_len;

    param = cmd->argv[1];
    param_len = strlen(param);
    for (i = 0; i < param_len; i++) {
      if (!PR_ISDIGIT(param[i]) &&
          param[i] != '.') {
        CONF_ERROR(cmd, "badly formatted OID parameter");
      }
    }
  }

  add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* usage: TLSVerifyClient on|off|optional */
MODRET set_tlsverifyclient(cmd_rec *cmd) {
  int verify_client = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  verify_client = get_boolean(cmd, 1);
  if (verify_client == -1) {
    if (strcasecmp(cmd->argv[1], "optional") != 0) {
      CONF_ERROR(cmd, "expected Boolean parameter");
    }

    verify_client = 2;
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = verify_client;

  return PR_HANDLED(cmd);
}

/* usage: TLSVerifyDepth depth */
MODRET set_tlsverifydepth(cmd_rec *cmd) {
  int depth = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  depth = atoi(cmd->argv[1]);
  if (depth < 0) {
    CONF_ERROR(cmd, "depth must be zero or greater");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = depth;

  return PR_HANDLED(cmd);
}

/* usage: TLSVerifyOrder mech1 ... */
MODRET set_tlsverifyorder(cmd_rec *cmd) {
  register unsigned int i = 0;
  config_rec *c = NULL;

  /* We only support two client cert verification mechanisms at the moment:
   * CRLs and OCSP.
   */
  if (cmd->argc-1 < 1 ||
      cmd->argc-1 > 2) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  for (i = 1; i < cmd->argc; i++) {
    char *mech = cmd->argv[i];

    if (strncasecmp(mech, "crl", 4) != 0
#if OPENSSL_VERSION_NUMBER > 0x000907000L
        && strncasecmp(mech, "ocsp", 5) != 0) {
#else
        ) {
#endif
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "unsupported verification mechanism '", mech, "' requested", NULL));
    }
  }

  c = add_config_param(cmd->argv[0], cmd->argc-1, NULL, NULL);
  for (i = 1; i < cmd->argc; i++) {
    char *mech = cmd->argv[i];

    if (strncasecmp(mech, "crl", 4) == 0) {
      c->argv[i-1] = pstrdup(c->pool, "crl");

#if OPENSSL_VERSION_NUMBER > 0x000907000L
    } else if (strncasecmp(mech, "ocsp", 5) == 0) {
      c->argv[i-1] = pstrdup(c->pool, "ocsp");
#endif
    }
  }

  return PR_HANDLED(cmd);
}

/* usage: TLSVerifyServer on|NoReverseDNS|off */
MODRET set_tlsverifyserver(cmd_rec *cmd) {
  int setting = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  setting = get_boolean(cmd, 1);
  if (setting == -1) {
    if (strcasecmp(cmd->argv[1], "NoReverseDNS") != 0) {
      CONF_ERROR(cmd, "expected Boolean parameter");
    }

    setting = 2;
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = setting;

  return PR_HANDLED(cmd);
}

/* Event handlers
 */

#if defined(PR_SHARED_MODULE)
static void tls_mod_unload_ev(const void *event_data, void *user_data) {
  if (strcmp("mod_tls.c", (const char *) event_data) == 0) {
    /* Unregister ourselves from all events. */
    pr_event_unregister(&tls_module, NULL, NULL);

    pr_timer_remove(-1, &tls_module);
# if defined(TLS_USE_SESSION_TICKETS)
    scrub_ticket_keys();
# endif /* TLS_USE_SESSION_TICKETS */

# ifdef PR_USE_CTRLS
    /* Unregister any control actions. */
    pr_ctrls_unregister(&tls_module, "tls");

    destroy_pool(tls_act_pool);
    tls_act_pool = NULL;
# endif /* PR_USE_CTRLS */

    /* Cleanup the OpenSSL stuff. */
    tls_cleanup(0);

    /* Unregister our NetIO handler for the control channel. */
    pr_unregister_netio(PR_NETIO_STRM_CTRL);

    if (tls_ctrl_netio) {
      destroy_pool(tls_ctrl_netio->pool);
      tls_ctrl_netio = NULL;
    }

    if (tls_data_netio) {
      destroy_pool(tls_data_netio->pool);
      tls_data_netio = NULL;
    }

    close(tls_logfd);
    tls_logfd = -1;
  }
}
#endif /* PR_SHARED_MODULE */

static void tls_sess_reinit_ev(const void *event_data, void *user_data) {
  int res;

  /* If the client has already established a TLS session, then do nothing;
   * we cannot easily force a re-handshake using different credentials very
   * easily. (Right?)
   */
  if (session.rfc2228_mech != NULL) {
    pr_trace_msg(trace_channel, 17,
      "ignored 'core.session-reinit' event due to existing SSL session");
    return;
  }

  /* A HOST command changed the main_server pointer; reinitialize ourselves. */

  pr_event_unregister(&tls_module, "core.exit", tls_exit_ev);
  pr_event_unregister(&tls_module, "core.session-reinit", tls_sess_reinit_ev);

  tls_reset_state();

/* XXX How to indicate, to sess_init. to NOT re-register the following event
 * listeners?
 *
 *
 * XXX How to indicate, to sess_init, to NOT re-add the pr_feat_add TLS
 * features.  Same for TLS HELP stuff.
 * ANSWER: Add unit tests for Feature API, Help API for adding duplicates;
 * ensure APIs detect and ignore such duplicates.
 */

  res = tls_sess_init();
  if (res < 0) {
    pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_SESSION_INIT_FAILED,
      NULL);
  }
}

/* Daemon PID */
extern pid_t mpid;

static void tls_shutdown_ev(const void *event_data, void *user_data) {
  if (mpid == getpid()) {
    tls_scrub_pkeys();
#if defined(TLS_USE_SESSION_TICKETS)
    scrub_ticket_keys();
#endif /* TLS_USE_SESSION_TICKETS */
    destroy_pool(tls_pool);
    tls_pool = NULL;
  }

  /* Write out a new RandomSeed file, for use later. */
  if (tls_rand_file) {
    int res;

    res = RAND_write_file(tls_rand_file);
    if (res < 0) {
      pr_log_pri(PR_LOG_WARNING, MOD_TLS_VERSION
        ": error writing PRNG seed data to '%s': %s", tls_rand_file,
        tls_get_errors());

    } else {
      pr_log_debug(DEBUG2, MOD_TLS_VERSION
        ": wrote %d bytes of PRNG seed data to '%s'", res, tls_rand_file);
    }
  }

  if (ssl_ctx != NULL) {
    SSL_CTX_free(ssl_ctx);
    ssl_ctx = NULL;
  }

#if OPENSSL_VERSION_NUMBER < 0x10100000L
  RAND_cleanup();
#endif /* prior to OpenSSL-1.1.x */
}

static void tls_restart_ev(const void *event_data, void *user_data) {
#ifdef PR_USE_CTRLS
  register unsigned int i;
#endif /* PR_USE_CTRLS */

  /* Note: We SHOULD scrub all of the mlock'd passphrases from memory here.
   * However, doing so would require that some outside agency, such as
   * a TLSPassPhraseProvider, provide those passphrases again.  And some
   * sites deem such providers insecure, but do have servers restarted due
   * to e.g. log rotation, without changing the passphrases/certs.  For
   * such sites, then, we will opportunistically scrub the pkeys later,
   * during the phase where such passphrases are obtained as needed;
   * see Bug#4260.
   */

#ifdef PR_USE_CTRLS
  if (tls_act_pool != NULL) {
    destroy_pool(tls_act_pool);
    tls_act_pool = NULL;
  }

  tls_act_pool = make_sub_pool(permanent_pool);
  pr_pool_tag(tls_act_pool, "TLS Controls Pool");

  /* Re-create the controls ACLs. */
  for (i = 0; tls_acttab[i].act_action; i++) {
    tls_acttab[i].act_acl = palloc(tls_act_pool, sizeof(ctrls_acl_t));
    pr_ctrls_init_acl(tls_acttab[i].act_acl);
  }
#endif /* PR_USE_CTRLS */

  tls_closelog();
}

static void tls_exit_ev(const void *event_data, void *user_data) {

  if (ssl_ctx != NULL) {
    time_t now;

    /* Help out with the TLS session cache grooming by flushing any
     * expired sessions out right now.  The client is closing its
     * connection to us anyway, so some additional latency here shouldn't
     * be noticed.  Right?
     */
    now = time(NULL);
    SSL_CTX_flush_sessions(ssl_ctx, (long) now);
  }

  /* If diags are enabled, log some OpenSSL stats. */
  if (ssl_ctx != NULL &&
      (tls_opts & TLS_OPT_ENABLE_DIAGS)) {
    long res;

    res = SSL_CTX_sess_accept(ssl_ctx);
    tls_log("[stat]: SSL/TLS sessions attempted: %ld", res);

    res = SSL_CTX_sess_accept_good(ssl_ctx);
    tls_log("[stat]: SSL/TLS sessions established: %ld", res);

    res = SSL_CTX_sess_accept_renegotiate(ssl_ctx);
    tls_log("[stat]: SSL/TLS sessions renegotiated: %ld", res);

    res = SSL_CTX_sess_hits(ssl_ctx);
    tls_log("[stat]: SSL/TLS sessions resumed: %ld", res);

    res = SSL_CTX_sess_number(ssl_ctx);
    tls_log("[stat]: SSL/TLS sessions in cache: %ld", res);

    res = SSL_CTX_sess_cb_hits(ssl_ctx);
    tls_log("[stat]: SSL/TLS session cache hits: %ld", res);

    res = SSL_CTX_sess_misses(ssl_ctx);
    tls_log("[stat]: SSL/TLS session cache misses: %ld", res);

    res = SSL_CTX_sess_timeouts(ssl_ctx);
    tls_log("[stat]: SSL/TLS session cache timeouts: %ld", res);

    res = SSL_CTX_sess_cache_full(ssl_ctx);
    tls_log("[stat]: SSL/TLS session cache size exceeded: %ld", res);
  }

  if (tls_sni_sess_tab != NULL) {
    (void) pr_table_empty(tls_sni_sess_tab);
    (void) pr_table_free(tls_sni_sess_tab);
    tls_sni_sess_tab = NULL;
  }

  if (tls_pkey != NULL) {
    tls_scrub_pkey(tls_pkey);
    tls_pkey = NULL;
  }

  /* OpenSSL cleanup */
  tls_cleanup(0);

  /* Done with the NetIO objects.  Note that we only really need to
   * destroy the data channel NetIO object; the control channel NetIO
   * object is allocated out of the permanent pool, in the daemon process,
   * and thus we have a read-only copy.
   */

  if (tls_ctrl_netio != NULL) {
    pr_unregister_netio(PR_NETIO_STRM_CTRL);
    destroy_pool(tls_ctrl_netio->pool);
    tls_ctrl_netio = NULL;
  }

  if (tls_data_netio != NULL) {
    pr_unregister_netio(PR_NETIO_STRM_DATA);
    destroy_pool(tls_data_netio->pool);
    tls_data_netio = NULL;
  }

  if (mpid != getpid()) {
    tls_scrub_pkeys();
  }

  tls_closelog();
}

static void tls_timeout_ev(const void *event_data, void *user_data) {

  if (session.c &&
      ctrl_ssl != NULL &&
      (tls_flags & TLS_SESS_ON_CTRL)) {
    /* Try to properly close the TLS session down on the control channel,
     * if there is one.
     */
    tls_end_sess(ctrl_ssl, session.c, 0);
    pr_table_remove(tls_ctrl_rd_nstrm->notes, TLS_NETIO_NOTE, NULL);
    pr_table_remove(tls_ctrl_wr_nstrm->notes, TLS_NETIO_NOTE, NULL);
    ctrl_ssl = NULL;
  }
}

static tls_pkey_t *tls_find_pkey(server_rec *s, int flags) {
  tls_pkey_t *k, *pkey = NULL;

  for (k = tls_pkey_list; k; k = k->next) {
    if (k->sid == s->sid) {
      switch (flags) {
        case TLS_PASSPHRASE_FL_RSA_KEY:
          if (k->rsa_pkey != NULL) {
            pkey = k;
          }
          break;

        case TLS_PASSPHRASE_FL_DSA_KEY:
          if (k->dsa_pkey != NULL) {
            pkey = k;
          }
          break;

        case TLS_PASSPHRASE_FL_EC_KEY:
#ifdef PR_USE_OPENSSL_ECC
          if (k->ec_pkey != NULL) {
            pkey = k;
          }
#endif /* PR_USE_OPENSSL_ECC */
          break;

        case TLS_PASSPHRASE_FL_PKCS12_PASSWD:
          if (k->pkcs12_passwd != NULL) {
            pkey = k;
          }
          break;

        default:
          break;
      }

      if (pkey != NULL) {
        break;
      }
    }
  }

  return pkey;
}

static tls_pkey_t *tls_get_key_passphrase(server_rec *s, const char *path,
    int flags) {
  int res, *pass_len = NULL;
  tls_pkey_t *k = NULL;
  const char *key_type = "unsupported";
  char buf[256], **key_data = NULL;
  void **key_ptr = NULL;

  switch (flags) {
    case TLS_PASSPHRASE_FL_RSA_KEY:
      key_type = "RSA";
      break;

    case TLS_PASSPHRASE_FL_DSA_KEY:
      key_type = "DSA";
      break;

    case TLS_PASSPHRASE_FL_EC_KEY:
      key_type = "EC";
      break;

    case TLS_PASSPHRASE_FL_PKCS12_PASSWD:
      key_type = "PKCS12";
      break;

    default:
      errno = EINVAL;
      return NULL;
  }

  pr_trace_msg(trace_channel, 14,
    "obtaining passphrase/password for %s cert for path %s", key_type, path);

  /* First see if we have an existing (and usable!) passphrase already
   * stored for this server/key type/path.
   */
  k = tls_find_pkey(s, flags);
  if (k != NULL) {
    /* Remove the key from the list; we will be adding this key, or another
     * one, back to the list in its place.
     */
    tls_remove_pkey(k);

    pr_trace_msg(trace_channel, 19,
      "FOUND existing %s pkey found for server ID %u (path %s)", key_type,
      s->sid, k->path);

    /* If this key is for the same path, consider it usable. */
    if (strcmp(path, k->path) == 0) {
      pr_trace_msg(trace_channel, 14,
        "reusing stored %s for %s certificate from path '%s'",
        flags != TLS_PASSPHRASE_FL_PKCS12_PASSWD ? "passphrase" : "password",
        key_type, path);
      return k;
    }

    /* Not for the same path?  Consider this key stale, and scrub it. */
    tls_scrub_pkey(k);
  }

  if (k == NULL) {
    pool *key_pool;

    key_pool = make_sub_pool(tls_pool);
    pr_pool_tag(key_pool, "Private Key Pool");

    k = pcalloc(key_pool, sizeof(tls_pkey_t));
    k->pool = key_pool;
  }

  k->pkeysz = PEM_BUFSIZE;

  switch (flags) {
    case TLS_PASSPHRASE_FL_RSA_KEY:
      key_data = &(k->rsa_pkey);
      key_ptr = &(k->rsa_pkey_ptr);
      pass_len = &(k->rsa_passlen);
      break;

    case TLS_PASSPHRASE_FL_DSA_KEY:
      key_data = &(k->dsa_pkey);
      key_ptr = &(k->dsa_pkey_ptr);
      pass_len = &(k->dsa_passlen);
      break;

    case TLS_PASSPHRASE_FL_EC_KEY:
#ifdef PR_USE_OPENSSL_ECC
      key_data = &(k->ec_pkey);
      key_ptr = &(k->ec_pkey_ptr);
      pass_len = &(k->ec_passlen);
#endif /* PR_USE_OPENSSL_ECC */
      break;

    case TLS_PASSPHRASE_FL_PKCS12_PASSWD:
      key_data = &(k->pkcs12_passwd);
      key_ptr = &(k->pkcs12_passwd_ptr);
      pass_len = &(k->pkcs12_passlen);
      break;

    default:
      errno = EINVAL;
      return NULL;
  }

  res = pr_snprintf(buf, sizeof(buf)-1, "%s %s for the %s#%d (%s) server: ",
    key_type, flags != TLS_PASSPHRASE_FL_PKCS12_PASSWD ? "key" : "password",
    pr_netaddr_get_ipstr(s->addr), s->ServerPort, s->ServerName);
  buf[res] = '\0';
  buf[sizeof(buf)-1] = '\0';

  *key_data = tls_get_page(PEM_BUFSIZE, key_ptr);
  if (*key_data == NULL) {
    pr_log_pri(PR_LOG_ALERT, MOD_TLS_VERSION ": Out of memory!");
    pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_NOMEM, NULL);
  }

  res = tls_get_passphrase(s, path, buf, *key_data, k->pkeysz-1, flags);
  if (res < 0) {
    const char *errors;

    errors = tls_get_errors();
    if (errors == NULL) {
      errors = "Not provided";
    }

    pr_trace_msg(trace_channel, 1, "error reading %s %s: %s", key_type,
      flags != TLS_PASSPHRASE_FL_PKCS12_PASSWD ? "passphrase" : "password",
      errors);
    pr_log_debug(DEBUG0, MOD_TLS_VERSION ": error reading %s %s: %s", key_type,
      flags != TLS_PASSPHRASE_FL_PKCS12_PASSWD ? "passphrase" : "password",
      errors);

    pr_log_pri(PR_LOG_ERR, MOD_TLS_VERSION
      ": unable to use %s certificate %sin '%s', exiting", key_type,
      flags != TLS_PASSPHRASE_FL_PKCS12_PASSWD ? "key " : "", path);
    pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_BY_APPLICATION, NULL);
  }

  *pass_len = res;
  k->path = strdup(path);
  k->sid = s->sid;

  return k;
}

static void tls_get_passphrases(void) {
  server_rec *s = NULL;

  for (s = (server_rec *) server_list->xas_list; s; s = s->next) {
    config_rec *rsa = NULL, *dsa = NULL, *ec = NULL, *pkcs12 = NULL;
    tls_pkey_t *k = NULL;
    const char *path;

    /* Find any TLS*CertificateKeyFile directives.  If they aren't present,
     * look for TLS*CertificateFile directives (when appropriate).
     */
    rsa = find_config(s->conf, CONF_PARAM, "TLSRSACertificateKeyFile", FALSE);
    if (rsa == NULL) {
      rsa = find_config(s->conf, CONF_PARAM, "TLSRSACertificateFile", FALSE);
    }

    dsa = find_config(s->conf, CONF_PARAM, "TLSDSACertificateKeyFile", FALSE);
    if (dsa == NULL) {
      dsa = find_config(s->conf, CONF_PARAM, "TLSDSACertificateFile", FALSE);
    }

    ec = find_config(s->conf, CONF_PARAM, "TLSECCertificateKeyFile", FALSE);
    if (ec == NULL) {
      ec = find_config(s->conf, CONF_PARAM, "TLSECCertificateFile", FALSE);
    }

    pkcs12 = find_config(s->conf, CONF_PARAM, "TLSPKCS12File", FALSE);

    if (rsa == NULL &&
        dsa == NULL &&
        ec == NULL &&
        pkcs12 == NULL) {
      continue;
    }

    if (rsa != NULL) {
      path = rsa->argv[0];
      k = tls_get_key_passphrase(s, path, TLS_PASSPHRASE_FL_RSA_KEY);
    }

    if (dsa != NULL) {
      path = dsa->argv[0];
      k = tls_get_key_passphrase(s, path, TLS_PASSPHRASE_FL_DSA_KEY);
    }

#ifdef PR_USE_OPENSSL_ECC
    if (ec != NULL) {
      path = ec->argv[0];
      k = tls_get_key_passphrase(s, path, TLS_PASSPHRASE_FL_EC_KEY);
    }
#endif /* PR_USE_OPENSSL_ECC */

    if (pkcs12 != NULL) {
      path = pkcs12->argv[0];
      k = tls_get_key_passphrase(s, path, TLS_PASSPHRASE_FL_PKCS12_PASSWD);
    }

    k->next = tls_pkey_list;
    tls_pkey_list = k;
    tls_npkeys++;
  }
}

static void tls_postparse_ev(const void *event_data, void *user_data) {
  server_rec *s = NULL;

  /* Check for incompatible configurations.  For example, configuring:
   *
   *  TLSOptions AllowPerUser
   *  TLSRequired auth
   *
   * cannot be supported; the AllowPerUser means that the requirement of
   * SSL/TLS protection during authentication cannot be enforced.
   */

  for (s = (server_rec *) server_list->xas_list; s; s = s->next) {
    unsigned long *opts;
    config_rec *toplevel_c = NULL, *other_c = NULL;
    int toplevel_auth_requires_ssl = FALSE, other_auth_requires_ssl = TRUE;

    opts = get_param_ptr(s->conf, "TLSOptions", FALSE);
    if (opts == NULL) {
      continue;
    }

    /* The purpose of this check is to watch for configurations such as:
     *
     *  <IfModule mod_tls.c>
     *    ...
     *    TLSRequired on
     *    ...
     *    TLSOptions AllowPerUser
     *    ...
     *  </IfModule>
     *
     * This policy cannot be enforced; we cannot require use of SSL/TLS
     * (specifically at authentication time, when we do NOT know the user)
     * AND also allow per-user SSL/TLS requirements.  It's a chicken-and-egg
     * problem.
     *
     * However, we DO want to allow configurations like:
     *
     *  <IfModule mod_tls.c>
     *    ...
     *    TLSRequired on
     *    ...
     *    TLSOptions AllowPerUser
     *    ...
     *  </IfModule>
     *
     *  <Anonymous ...>
     *    ...
     *    <IfModule mod_tls.c>
     *      TLSRequired off
     *    </IfModule>
     *  </Anonymous>
     *
     * Thus this check is a bit tricky.  We look first in this server_rec's
     * config list for a top-level TLSRequired setting.  If it is 'on' AND
     * if the AllowPerUser TLSOption is set, AND we find no other TLSRequired
     * configs deeper in the server_rec whose value is 'off', then log the
     * error and quit.  Otherwise, let things proceed.
     *
     * If the mod_ifsession module is present, skip this check as well; we
     * will not be able to suss out any TLSRequired settings which are
     * lurking in mod_ifsession's grasp until authentication time.
     *
     * I still regret adding support for the AllowPerUser TLSOption.  Users
     * just cannot seem to wrap their minds around the fact that the user
     * is not known at the time when the SSL/TLS session is done.  Sigh.
     */

    if (pr_module_exists("mod_ifsession.c")) {
      continue;
    }

    toplevel_c = find_config(s->conf, CONF_PARAM, "TLSRequired", FALSE);
    if (toplevel_c) {
      toplevel_auth_requires_ssl = *((int *) toplevel_c->argv[2]);
    }

    /* If this toplevel TLSRequired value is 'off', then we need check no
     * further.
     */
    if (!toplevel_auth_requires_ssl) {
      continue;
    }

    /* This time, we recurse deeper into the server_rec's configs.
     * We need only pay attention to settings we find in the CONF_DIR or
     * CONF_ANON config contexts.  And we need only look until we find such
     * a setting does not require SSL/TLS during authentication, for at that
     * point we know it is not a misconfiguration.
     */
    find_config_set_top(NULL);
    other_c = find_config(s->conf, CONF_PARAM, "TLSRequired", TRUE);
    while (other_c) {
      int auth_requires_ssl;

      pr_signals_handle();

      if (other_c->parent == NULL ||
          (other_c->parent->config_type != CONF_ANON &&
           other_c->parent->config_type != CONF_DIR)) {
        /* Not what we're looking for; continue on. */
        other_c = find_config_next(other_c, other_c->next, CONF_PARAM,
          "TLSRequired", TRUE);
        continue;
      }

      auth_requires_ssl = *((int *) other_c->argv[2]);
      if (!auth_requires_ssl) {
        other_auth_requires_ssl = FALSE;
        break;
      }

      other_c = find_config_next(other_c, other_c->next, CONF_PARAM,
        "TLSRequired", TRUE);
    }

    if ((*opts & TLS_OPT_ALLOW_PER_USER) &&
        toplevel_auth_requires_ssl == TRUE &&
        other_auth_requires_ssl == TRUE) {
      pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION ": Server %s: cannot enforce "
        "both 'TLSRequired auth' and 'TLSOptions AllowPerUser' at the "
        "same time", s->ServerName);
      pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_BAD_CONFIG, NULL);
    }
  }

  if (ServerType == SERVER_INETD) {
    if (tls_set_fips() < 0) {
      pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
        ": error initialising FIPS");
      pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_BY_APPLICATION,
        NULL);
    }
  }

  /* Initialize the OpenSSL context. */
  ssl_ctx = tls_init_ctx(main_server);
  if (ssl_ctx == NULL) {
    pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
      ": error initialising OpenSSL context");
    pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_BY_APPLICATION, NULL);
  }

  if (tls_seed_prng() < 0) {
    pr_log_debug(DEBUG1, MOD_TLS_VERSION ": unable to properly seed PRNG");
  }

  tls_pool = make_sub_pool(permanent_pool);
  pr_pool_tag(tls_pool, MOD_TLS_VERSION);

  tls_ctx_set_session_cache(main_server, ssl_ctx);
  tls_ctx_set_stapling_cache(main_server, ssl_ctx);
  tls_ctx_set_session_id_context(main_server, ssl_ctx);

  /* We can only get the passphrases for certs once OpenSSL has been
   * initialized.
   */
  tls_get_passphrases();

  /* Clean up the pkey list, scrubbing any stale/irrelevant keys. */
  tls_clean_pkeys();

  /* Install our control channel NetIO handlers.  This is done here
   * specifically because we need to cache a pointer to the nstrm that
   * is passed to the open callback().  Ideally we'd only install our
   * custom NetIO handlers if the appropriate AUTH command was given.
   * But by then, the open() callback will have already been called, and
   * we will not have a chance to get that nstrm pointer.
   */
  tls_netio_install_ctrl();
}

static void tls_lookup_dh_params(server_rec *s) {
  config_rec *c;

  c = find_config(s->conf, CONF_PARAM, "TLSDHParamFile", FALSE);
  while (c != NULL) {
    const char *path;
    FILE *fp;
    int xerrno;

    pr_signals_handle();

    path = c->argv[0];

    /* Load the DH params from the file. */
    PRIVS_ROOT
    fp = fopen(path, "r");
    xerrno = errno;
    PRIVS_RELINQUISH

    if (fp != NULL) {
      DH *dh;

      dh = PEM_read_DHparams(fp, NULL, NULL, NULL);
      if (dh != NULL) {
        if (tls_tmp_dhs == NULL) {
          tls_tmp_dhs = make_array(session.pool, 1, sizeof(DH *));
        }
      }

      while (dh != NULL) {
        pr_signals_handle();
        *((DH **) push_array(tls_tmp_dhs)) = dh;
        dh = PEM_read_DHparams(fp, NULL, NULL, NULL);
      }

      fclose(fp);

    } else {
      pr_log_debug(DEBUG3, MOD_TLS_VERSION
        ": unable to open TLSDHParamFile '%s': %s", path, strerror(xerrno));
    }

    c = find_config_next(c, c->next, CONF_PARAM, "TLSDHParamFile", FALSE);
  }
}

static void tls_lookup_psks(server_rec *s) {
#if defined(PSK_MAX_PSK_LEN)
  config_rec *c;

  if (tls_psks != NULL) {
    pr_table_empty(tls_psks);
    pr_table_free(tls_psks);
    tls_psks = NULL;
  }

  c = find_config(s->conf, CONF_PARAM, "TLSPreSharedKey", FALSE);
  while (c != NULL) {
    register int i;
    char key_buf[PR_TUNABLE_BUFFER_SIZE], *identity, *path;
    int fd, key_len, valid_hex = TRUE, res, xerrno;
    struct stat st;
    BIGNUM *bn = NULL;

    pr_signals_handle();

    identity = c->argv[0];
    path = c->argv[1];

    /* Advance past the "hex:" format prefix. */
    path += 4;

    PRIVS_ROOT
    fd = open(path, O_RDONLY);
    xerrno = errno;
    PRIVS_RELINQUISH

    if (fd < 0) {
      pr_log_debug(DEBUG2, MOD_TLS_VERSION
        ": error opening TLSPreSharedKey file '%s': %s", path,
        strerror(xerrno));
      c = find_config_next(c, c->next, CONF_PARAM, "TLSPreSharedKey", FALSE);
      continue;
    }

    if (fstat(fd, &st) < 0) {
      pr_log_debug(DEBUG2, MOD_TLS_VERSION
        ": error checking TLSPreSharedKey file '%s': %s", path,
        strerror(errno));
      (void) close(fd);
      c = find_config_next(c, c->next, CONF_PARAM, "TLSPreSharedKey", FALSE);
      continue;
    }

    /* Check on the permissions of the file; skip it if the permissions
     * are too permissive, e.g. file is world-read/writable.
     */
    if (st.st_mode & S_IROTH) {
      pr_log_debug(DEBUG2, MOD_TLS_VERSION
        ": unable to use TLSPreSharedKey file '%s': file is world-readable",
        path);
      (void) close(fd);
      c = find_config_next(c, c->next, CONF_PARAM, "TLSPreSharedKey", FALSE);
      continue;
    }

    if (st.st_mode & S_IWOTH) {
      pr_log_debug(DEBUG2, MOD_TLS_VERSION
        ": unable to use TLSPreSharedKey file '%s': file is world-writable",
        path);
      (void) close(fd);
      c = find_config_next(c, c->next, CONF_PARAM, "TLSPreSharedKey", FALSE);
      continue;
    }

    /* Read the entire key into memory. */
    key_len = read(fd, key_buf, sizeof(key_buf)-1);
    (void) close(fd);

    if (key_len < 0) {
      pr_log_debug(DEBUG2, MOD_TLS_VERSION
        ": error reading TLSPreSharedKey file '%s': %s", path,
        strerror(xerrno));
      c = find_config_next(c, c->next, CONF_PARAM, "TLSPreSharedKey", FALSE);
      continue;

    } else if (key_len == 0) {
      pr_log_debug(DEBUG2, MOD_TLS_VERSION
        ": read zero bytes from TLSPreSharedKey file '%s', ignoring", path);
      c = find_config_next(c, c->next, CONF_PARAM, "TLSPreSharedKey", FALSE);
      continue;

    } else if (key_len < TLS_MIN_PSK_LEN) {
      pr_log_debug(DEBUG2, MOD_TLS_VERSION
        ": read %d bytes from TLSPreSharedKey file '%s', need at least %d "
        "bytes of key data, ignoring", key_len, path, TLS_MIN_PSK_LEN);
      c = find_config_next(c, c->next, CONF_PARAM, "TLSPreSharedKey", FALSE);
      continue;
    }

    key_buf[key_len] = '\0';
    key_buf[sizeof(key_buf)-1] = '\0';

    /* Ignore any trailing newlines. */
    if (key_buf[key_len-1] == '\n') {
      key_buf[key_len-1] = '\0';
      key_len--;
    }

    if (key_buf[key_len-1] == '\r') {
      key_buf[key_len-1] = '\0';
      key_len--;
    }

    /* Ensure that it is all hex encoded data */
    for (i = 0; i < key_len; i++) {
      if (PR_ISXDIGIT((int) key_buf[i]) == 0) {
        valid_hex = FALSE;
        break;
      }
    }

    if (valid_hex == FALSE) {
      pr_log_debug(DEBUG2, MOD_TLS_VERSION
        ": unable to use '%s': not a hex number", key_buf);
      c = find_config_next(c, c->next, CONF_PARAM, "TLSPreSharedKey", FALSE);
      continue;
    }

    res = BN_hex2bn(&bn, key_buf);
    if (res == 0) {
      pr_log_debug(DEBUG2, MOD_TLS_VERSION
        ": failed to convert '%s' to BIGNUM: %s", key_buf,
        tls_get_errors());

      if (bn != NULL) {
        BN_free(bn);
      }

      c = find_config_next(c, c->next, CONF_PARAM, "TLSPreSharedKey", FALSE);
      continue;
    }

    if (tls_psks == NULL) {
      tls_psks = pr_table_nalloc(session.pool, 0, 2);
    }

    if (pr_table_add(tls_psks, identity, bn, sizeof(BIGNUM *)) < 0) {
      pr_log_debug(DEBUG0, MOD_TLS_VERSION
        ": error stashing key for identity '%s': %s", identity,
        strerror(errno));
      BN_free(bn);
    }

    c = find_config_next(c, c->next, CONF_PARAM, "TLSPreSharedKey", FALSE);
  }
#endif /* PSK_MAX_PSK_LEN */
}

static void tls_lookup_renegotiate(server_rec *s) {
#if OPENSSL_VERSION_NUMBER > 0x000907000L
  config_rec *c = NULL;

  /* Lookup/process any configured TLSRenegotiate parameters. */
  c = find_config(s->conf, CONF_PARAM, "TLSRenegotiate", FALSE);
  if (c == NULL) {
    return;
  }

  if (c->argc == 0) {
    /* Disable all server-side requested renegotiations; clients can still
     * request renegotiations.
     */
    tls_ctrl_renegotiate_timeout = 0;
    tls_data_renegotiate_limit = 0;
    tls_renegotiate_timeout = 0;
    tls_renegotiate_required = FALSE;

  } else {
    int ctrl_timeout = *((int *) c->argv[0]);
    off_t data_limit = *((off_t *) c->argv[1]);
    int renegotiate_timeout = *((int *) c->argv[2]);
    unsigned char renegotiate_required = *((unsigned char *) c->argv[3]);

    if (data_limit > 0) {
      tls_data_renegotiate_limit = data_limit;
    }

    if (renegotiate_timeout > 0) {
      tls_renegotiate_timeout = renegotiate_timeout;
    }

    tls_renegotiate_required = renegotiate_required;

    /* Set any control channel renegotiation timers, if need be. */
    pr_timer_add(ctrl_timeout ? ctrl_timeout : tls_ctrl_renegotiate_timeout,
      -1, &tls_module, tls_ctrl_renegotiate_cb, "SSL/TLS renegotiation");
  }
#endif
}

static void tls_lookup_stapling(server_rec *s) {
#if defined(PR_USE_OPENSSL_OCSP)
  config_rec *c;

  /* Reset to defaults. */
  tls_stapling_opts = 0UL;

  c = find_config(s->conf, CONF_PARAM, "TLSStaplingOptions", FALSE);
  while (c != NULL) {
    unsigned long opts = 0;

    pr_signals_handle();

    opts = *((unsigned long *) c->argv[0]);
    tls_stapling_opts |= opts;

    c = find_config_next(c, c->next, CONF_PARAM, "TLSStaplingOptions", FALSE);
  }

  c = find_config(s->conf, CONF_PARAM, "TLSStaplingResponder", FALSE);
  if (c != NULL) {
    tls_stapling_responder = c->argv[0];

  } else {
    /* Reset to default. */
    tls_stapling_responder = NULL;
  }

  c = find_config(s->conf, CONF_PARAM, "TLSStaplingTimeout", FALSE);
  if (c != NULL) {
    tls_stapling_timeout = *((unsigned int *) c->argv[0]);

  } else {
    /* Reset the default. */
    tls_stapling_timeout = TLS_DEFAULT_STAPLING_TIMEOUT;
  }

  /* If a TLSStaplingCache has been configured, then TLSStapling should
   * be enabled by default.
   */
  if (tls_ocsp_cache != NULL) {
    tls_stapling = TRUE;
  }

  c = find_config(s->conf, CONF_PARAM, "TLSStapling", FALSE);
  if (c != NULL) {
    tls_stapling = *((int *) c->argv[0]);

  } else {
    /* Reset the default. */
    tls_stapling = FALSE;
  }
#endif /* PR_USE_OPENSSL_OCSP */
}

static void tls_lookup_verify(server_rec *s) {
  config_rec *c;

  /* Clear client verification-related flags. */
  tls_flags &= ~TLS_SESS_VERIFY_CLIENT_REQUIRED;
  tls_flags &= ~TLS_SESS_VERIFY_CLIENT_OPTIONAL;

  /* TLSVerifyClient */
  c = find_config(s->conf, CONF_PARAM, "TLSVerifyClient", FALSE);
  if (c != NULL) {
    unsigned char verify_client;

    verify_client = *((unsigned char *) c->argv[0]);
    switch (verify_client) {
      case 0:
        break;

      case 1:
        tls_flags |= TLS_SESS_VERIFY_CLIENT_REQUIRED;
        break;

      case 2:
        tls_flags |= TLS_SESS_VERIFY_CLIENT_OPTIONAL;
        break;

      default:
        break;
    }
  }

  /* Clear server verification-related flags. */
  tls_flags &= ~TLS_SESS_VERIFY_SERVER_NO_DNS;
  tls_flags &= ~TLS_SESS_VERIFY_SERVER;

  /* TLSVerifyServer */
  c = find_config(s->conf, CONF_PARAM, "TLSVerifyServer", FALSE);
  if (c != NULL) {
    int setting;

    setting = *((int *) c->argv[0]);
    switch (setting) {
      case 2:
        tls_flags |= TLS_SESS_VERIFY_SERVER_NO_DNS;
        break;

      case 1:
        tls_flags |= TLS_SESS_VERIFY_SERVER;
        break;
    }

  } else {
    tls_flags |= TLS_SESS_VERIFY_SERVER;
  }

  /* If TLSVerifyClient/Server is on, look up the verification depth. */
  if (tls_flags & (TLS_SESS_VERIFY_CLIENT_REQUIRED|TLS_SESS_VERIFY_SERVER|TLS_SESS_VERIFY_SERVER_NO_DNS)) {
    int *depth = NULL;

    depth = get_param_ptr(s->conf, "TLSVerifyDepth", FALSE);
    if (depth != NULL) {
      tls_verify_depth = *depth;

    } else {
      /* Reset the default. */
      tls_verify_depth = TLS_DEFAULT_VERIFY_DEPTH;
    }
  }
}

static void tls_lookup_all(server_rec *s) {
  config_rec *c;

  /* TLSCACertificate{File,Path} */
  tls_ca_file = get_param_ptr(s->conf, "TLSCACertificateFile", FALSE);
  tls_ca_path = get_param_ptr(s->conf, "TLSCACertificatePath", FALSE);

  /* TLSCARevocation{File,Path} */
  tls_crl_file = get_param_ptr(s->conf, "TLSCARevocationFile", FALSE);
  tls_crl_path = get_param_ptr(s->conf, "TLSCARevocationPath", FALSE);

  /* TLSCertificateChainFile */
  tls_ca_chain = get_param_ptr(s->conf, "TLSCertificateChainFile", FALSE);

  /* TLS*CertificateFile.  Assume that, if no separate key files are configured,
   * the keys are in the same file as the corresponding certificate.
   */
  tls_dsa_cert_file = get_param_ptr(s->conf, "TLSDSACertificateFile", FALSE);
  tls_dsa_key_file = get_param_ptr(s->conf, "TLSDSACertificateKeyFile", FALSE);

  tls_ec_cert_file = get_param_ptr(s->conf, "TLSECCertificateFile", FALSE);
  tls_ec_key_file = get_param_ptr(s->conf, "TLSECCertificateKeyFile", FALSE);

  tls_pkcs12_file = get_param_ptr(s->conf, "TLSPKCS12File", FALSE);

  tls_rsa_cert_file = get_param_ptr(s->conf, "TLSRSACertificateFile", FALSE);
  tls_rsa_key_file = get_param_ptr(s->conf, "TLSRSACertificateKeyFile", FALSE);

  /* TLSCipherSuite */
  c = find_config(s->conf, CONF_PARAM, "TLSCipherSuite", FALSE);
  while (c != NULL) {
    int protocol;

    pr_signals_handle();

    protocol = *((int *) c->argv[1]);

#if OPENSSL_VERSION_NUMBER >= 0x10101000L && \
    defined(TLS1_3_VERSION)
    if (protocol == TLS_PROTO_TLS_V1_3) {
      tlsv13_cipher_suite = c->argv[0];

    } else {
      tls_cipher_suite = c->argv[0];
    }
#else
    tls_cipher_suite = c->argv[0];
#endif /* TLS1_3_VERSION */

    c = find_config_next(c, c->next, CONF_PARAM, "TLSCipherSuite", FALSE);
  }

  if (tls_cipher_suite == NULL) {
    tls_cipher_suite = TLS_DEFAULT_CIPHER_SUITE;
  }

  tls_lookup_dh_params(s);

  /* TLSECDHCurve */
#if defined(PR_USE_OPENSSL_ECC)
  c = find_config(s->conf, CONF_PARAM, "TLSECDHCurve", FALSE);
  if (c != NULL) {
    tls_ecdh_curve = (void *) c->argv[0];

  } else {
    /* Reset the default. */
    tls_ecdh_curve = NULL;
  }
#endif /* PR_USE_OPENSSL_ECC */

  /* TLSNextProtocol */
#if !defined(OPENSSL_NO_TLSEXT)
  c = find_config(s->conf, CONF_PARAM, "TLSNextProtocol", FALSE);
  if (c != NULL) {
    tls_use_next_protocol = *((int *) c->argv[0]);

  } else {
    /* Reset the default. */
    tls_use_next_protocol = TRUE;
  }
#endif /* !OPENSSL_NO_TLSEXT */

  /* TLSOptions */
  c = find_config(s->conf, CONF_PARAM, "TLSOptions", FALSE);
  while (c != NULL) {
    unsigned long opts = 0;

    pr_signals_handle();

    opts = *((unsigned long *) c->argv[0]);
    tls_opts |= opts;

    c = find_config_next(c, c->next, CONF_PARAM, "TLSOptions", FALSE);
  }

  /* If UseReverseDNS is set to off, disable TLS_OPT_VERIFY_CERT_FQDN. */
  if (!ServerUseReverseDNS &&
      ((tls_opts & TLS_OPT_VERIFY_CERT_FQDN) ||
       (tls_opts & TLS_OPT_VERIFY_CERT_CN))) {

    if (tls_opts & TLS_OPT_VERIFY_CERT_FQDN) {
      tls_opts &= ~TLS_OPT_VERIFY_CERT_FQDN;
      tls_log("%s", "reverse DNS off, disabling TLSOption dNSNameRequired");
    }

    if (tls_opts & TLS_OPT_VERIFY_CERT_CN) {
      tls_opts &= ~TLS_OPT_VERIFY_CERT_CN;
      tls_log("%s", "reverse DNS off, disabling TLSOption CommonNameRequired");
    }
  }

  /* TLSProtocol */
  c = find_config(s->conf, CONF_PARAM, "TLSProtocol", FALSE);
  if (c != NULL) {
    tls_protocol = *((unsigned int *) c->argv[0]);

  } else {
    /* Reset the default. */
    tls_protocol = TLS_PROTO_DEFAULT;
  }

  tls_lookup_psks(s);

  tls_lookup_renegotiate(s);

  /* TLSRequired */
  c = find_config(s->conf, CONF_PARAM, "TLSRequired", FALSE);
  if (c != NULL) {
    tls_required_on_ctrl = *((int *) c->argv[0]);
    tls_required_on_data = *((int *) c->argv[1]);
    tls_required_on_auth = *((int *) c->argv[2]);

  } else {
    /* Reset the default. */
    tls_required_on_ctrl = 0;
    tls_required_on_data = 0;
    tls_required_on_auth = 0;
  }

  /* TLSServerCipherPreference */
#if defined(SSL_OP_CIPHER_SERVER_PREFERENCE)
  c = find_config(s->conf, CONF_PARAM, "TLSServerCipherPreference",
    FALSE);
  if (c != NULL) {
    tls_use_server_cipher_preference = *((int *) c->argv[0]);

  } else {
    /* Reset the default. */
    tls_use_server_cipher_preference = TRUE;
  }
#endif /* SSL_OP_CIPHER_SERVER_PREFERENCE */

  /* TLSServerInfoFile */
#if !defined(OPENSSL_NO_TLSEXT)
# if OPENSSL_VERSION_NUMBER >= 0x10002000L && \
     !defined(HAVE_LIBRESSL)
  c = find_config(s->conf, CONF_PARAM, "TLSServerInfoFile", FALSE);
  if (c != NULL) {
    tls_serverinfo_file = c->argv[0];

  } else {
    /* Reset the default. */
    tls_serverinfo_file = NULL;
  }
# endif /* OpenSSL-1.0.2 and later */
#endif /* !OPENSSL_NO_TLSEXT */

  /* TLSSessionTickets */
#if defined(TLS_USE_SESSION_TICKETS)
  c = find_config(s->conf, CONF_PARAM, "TLSSessionTickets", FALSE);
  if (c != NULL) {
    tls_use_session_tickets = *((int *) c->argv[0]);

  } else {
    /* Reset the default. */
    tls_use_session_tickets = FALSE;
  }
#endif /* TLS_USE_SESSION_TICKETS */

  tls_lookup_stapling(s);

  /* TLSTimeoutHandshake */
  c = find_config(s->conf, CONF_PARAM, "TLSTimeoutHandshake", FALSE);
  if (c != NULL) {
    tls_handshake_timeout = *((unsigned int *) c->argv[0]);

  } else {
    /* Reset the default. */
    tls_handshake_timeout = TLS_DEFAULT_HANDSHAKE_TIMEOUT;
  }

  tls_lookup_verify(s);
}

/* SSL setters */

static int tls_ssl_set_cert_chain(SSL *ssl) {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
  int res;

  if (tls_ca_chain == NULL) {
    return 0;
  }

  tls_log("adding certs from '%s' to SSL certificate chain", tls_ca_chain);
  PRIVS_ROOT
  res = SSL_use_certificate_chain_file(ssl, tls_ca_chain);
  PRIVS_RELINQUISH

  if (res != 1) {
    tls_log("unable to read certificate chain '%s': %s", tls_ca_chain,
      tls_get_errors());
    return -1;
  }
#endif /* OpenSSL 1.1.x and later */

  return 0;
}

static int tls_ssl_set_ciphers(SSL *ssl) {
  SSL_set_cipher_list(ssl, tls_cipher_suite);

#if OPENSSL_VERSION_NUMBER >= 0x10101000L && \
    defined(TLS1_3_VERSION)
  if (tlsv13_cipher_suite != NULL) {
    SSL_set_ciphersuites(ssl, tlsv13_cipher_suite);
  }
#endif /* TLS1_3_VERSION */

  return 0;
}

static int tls_ssl_set_ecdh_curve(SSL *ssl) {
#if defined(PR_USE_OPENSSL_ECC)
# if defined(SSL_CTX_set_ecdh_auto)
  if (tls_opts & TLS_OPT_NO_AUTO_ECDH) {
    SSL_set_ecdh_auto(ssl, 0);
  }
# endif

  if (tls_ecdh_curve != NULL) {
# if defined(SSL_CTX_set1_curves_list)
    char *curve_names;

    curve_names = tls_ecdh_curve;
    if (strcasecmp(curve_names, "auto") != 0) {
      SSL_set1_curves_list(ssl, curve_names);
    }
# else
    const EC_KEY *ec_key;

    ec_key = tls_ecdh_curve;
    SSL_set_tmp_ecdh(ssl, ec_key);
# endif /* pre OpenSSL-1.0.2 */

    SSL_set_options(ssl, SSL_OP_SINGLE_ECDH_USE);
  } else {
# if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_set_tmp_ecdh_callback(ssl, tls_ecdh_cb);
# endif /* Before OpenSSL-1.1.x */
  }
#endif /* PR_USE_OPENSS_ECC */

  return 0;
}

static int tls_ssl_set_psks(SSL *ssl) {
#if defined(PSK_MAX_PSK_LEN)
  if (tls_psks == NULL ||
      pr_table_count(tls_psks) == 0) {
    SSL_set_psk_server_callback(ssl, NULL);
    return 0;
  }

  pr_trace_msg(trace_channel, 9,
    "enabling support for PSK identities (%d)", pr_table_count(tls_psks));
  SSL_set_psk_server_callback(ssl, tls_lookup_psk);
#endif /* PSK_MAX_PSK_LEN */

  return 0;
}

static int tls_ssl_set_options(SSL *ssl) {

#if OPENSSL_VERSION_NUMBER > 0x009080cfL
  SSL_clear_options(ssl, SSL_OP_CIPHER_SERVER_PREFERENCE);

  /* The OpenSSL team realized that the flag added in 0.9.8l, the
   * SSL3_FLAGS_ALLOW_UNSAFE_LEGACY_RENEGOTIATION flag, was a bad idea.
   * So in later versions, it was changed to a context flag,
   * SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION.
   */
  SSL_clear_options(ssl, SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
  if (tls_opts & TLS_OPT_ALLOW_CLIENT_RENEGOTIATIONS) {
    SSL_set_options(ssl, SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
  }
#endif

#if defined(SSL_OP_CIPHER_SERVER_PREFERENCE)
  if (tls_use_server_cipher_preference == TRUE) {
    SSL_set_options(ssl, SSL_OP_CIPHER_SERVER_PREFERENCE);
  }
#endif /* SSL_OP_CIPHER_SERVER_PREFERENCE */

#if OPENSSL_VERSION_NUMBER > 0x000907000L
  /* Install a callback for logging OpenSSL message information, if requested.
   */
  if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
    tls_log("%s",
      "TLSOption EnableDiags enabled, setting diagnostics callback");
    SSL_set_msg_callback(ssl, tls_msg_cb);

  } else {
    SSL_set_msg_callback(ssl, NULL);
  }
#endif

  return 0;
}

static int tls_ssl_set_next_protocol(SSL *ssl) {
#if !defined(OPENSSL_NO_TLSEXT)
  register unsigned int i;
  const char *proto = TLS_DEFAULT_NEXT_PROTO;
  size_t encoded_protolen, proto_len;
  unsigned char *encoded_proto;
  struct tls_next_proto *next_proto;
  SSL_CTX *ctx;

  ctx = SSL_get_SSL_CTX(ssl);

  if (tls_use_next_protocol == FALSE) {
# if defined(PR_USE_OPENSSL_NPN)
    SSL_CTX_set_next_protos_advertised_cb(ctx, NULL, NULL);
# endif /* NPN */

# if defined(PR_USE_OPENSSL_ALPN)
    SSL_CTX_set_alpn_select_cb(ctx, NULL, NULL);
# endif /* ALPN */

    return 0;
  }

  proto_len = strlen(proto);
  encoded_protolen = proto_len + 1;
  encoded_proto = palloc(session.pool, encoded_protolen);
  encoded_proto[0] = proto_len;
  for (i = 0; i < proto_len; i++) {
    encoded_proto[i+1] = proto[i];
  }

  next_proto = palloc(session.pool, sizeof(struct tls_next_proto));
  next_proto->proto = pstrdup(session.pool, proto);
  next_proto->encoded_proto = encoded_proto;
  next_proto->encoded_protolen = encoded_protolen;

# if defined(PR_USE_OPENSSL_NPN)
  SSL_CTX_set_next_protos_advertised_cb(ctx, tls_npn_advertised_cb, next_proto);
# endif /* NPN */

# if defined(PR_USE_OPENSSL_ALPN)
  SSL_CTX_set_alpn_select_cb(ctx, tls_alpn_select_cb, next_proto);
# endif /* ALPN */
#endif /* !OPENSSL_NO_TLSEXT */

  return 0;
}

static int tls_ssl_set_protocol(server_rec *s, SSL *ssl) {
  int all_proto, disabled_proto;
  unsigned int enabled_proto_count = 0;
  const char *enabled_proto_str = NULL;

  /* First, make sure ALL protocol versions are disabled by default. */
  all_proto = get_disabled_protocols(0);
  SSL_set_options(ssl, all_proto);

  disabled_proto = get_disabled_protocols(tls_protocol);

  /* Per the comments in <ssl/ssl.h>, SSL_CTX_set_options() uses |= on
   * the previous value.  This means we can easily OR in our new option
   * values with any previously set values.
   */
  enabled_proto_str = tls_get_proto_str(s->pool, tls_protocol,
    &enabled_proto_count);

  pr_log_debug(DEBUG8, MOD_TLS_VERSION ": supporting %s %s", enabled_proto_str,
    enabled_proto_count != 1 ? "protocols" : "protocol only");

  /* Now we clear all of the NO_ protocol version options EXCEPT for the
   * enabled versions.
   *
   * This is more convoluted than it should be, because of the expression
   * of enabled protocol versions via OP_NO_ negations, and bitmasking.
   */
#if OPENSSL_VERSION_NUMBER > 0x000908000L
  SSL_clear_options(ssl, all_proto|disabled_proto);
#endif
  SSL_set_options(ssl, disabled_proto);

  return 0;
}

static int tls_ssl_set_renegotiations(SSL *ssl) {
#if defined(SSL_OP_NO_RENEGOTIATION)
  if (tls_ctrl_renegotiate_timeout == 0 &&
      tls_data_renegotiate_limit == 0 &&
      tls_renegotiate_timeout == 0 &&
      tls_renegotiate_required == FALSE) {
    /* If server-requested renegotiations are disabled, reject
     * client-initiated renegotiations, too.
     */
    SSL_set_options(ssl, SSL_OP_NO_RENEGOTIATION);
  }
#endif /* SSL_OP_NO_RENEGOTIATION */

  return 0;
}

static int tls_ssl_set_session_tickets(SSL *ssl) {
#if defined(TLS_USE_SESSION_TICKETS) && \
    defined(SSL_OP_NO_TICKET)
  if (tls_use_session_tickets == TRUE) {
    /* Ensure that tickets are enabled in the SSL options. */
    SSL_clear_options(ssl, SSL_OP_NO_TICKET);

  } else {
# if defined(TLS1_3_VERSION)
    /* Disable session tickets unless it's a TLSv1.3 session. */
    if (SSL_version(ssl) != TLS1_3_VERSION) {
      SSL_set_options(ssl, SSL_OP_NO_TICKET);
    }
# else
    /* Disable session tickets. */
    SSL_set_options(ssl, SSL_OP_NO_TICKET);
# endif /* TLS1_3_VERSION */
  }
#endif /* TLS_USE_SESSION_TICKETS and SSL_OP_NO_TICKET */

  return 0;
}

static int tls_ssl_set_verify(SSL *ssl) {
  int verify_mode = 0;

  if (tls_flags & TLS_SESS_VERIFY_CLIENT_OPTIONAL) {
    verify_mode = SSL_VERIFY_PEER;
  }

  if (tls_flags & TLS_SESS_VERIFY_CLIENT_REQUIRED) {
    /* If we are verifying clients, make sure the client sends a cert;
     * the protocol allows for the client to disregard a request for
     * its cert by the server.
     */
    verify_mode |= (SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT);
  }

  if (verify_mode != 0) {
    SSL_set_verify(ssl, verify_mode, tls_verify_cb);

    /* Note: we add one to the configured depth purposefully.  As noted
     * in the OpenSSL man pages, the verification process will silently
     * stop at the configured depth, and the error messages ensuing will
     * be that of an incomplete certificate chain, rather than the
     * "chain too long" error that might be expected.  To log the "chain
     * too long" condition, we add one to the configured depth, and catch,
     * in the verify callback, the exceeding of the actual depth.
     */
    SSL_set_verify_depth(ssl, tls_verify_depth + 1);
  }

  return 0;
}

static int tls_ssl_set_session_id_context(server_rec *s, SSL *ssl) {
  SSL_SESSION *sess;

  /* Update the session ID context to use.  This is important; it ensures
   * that the session IDs for this particular vhost will differ from those
   * for another vhost.  An external TLS session cache will possibly
   * cache sessions from all vhosts together, and we need to keep them
   * separate.
   */

  pr_trace_msg(trace_channel, 19,
    "setting session ID context '%u' (%lu bytes) on SSL %p", s->sid,
    sizeof(s->sid), ssl);
  SSL_set_session_id_context(ssl, (unsigned char *) &(s->sid), sizeof(s->sid));

#if defined(PR_USE_OPENSSL_SSL_SESSION_SET1_ID_CONTEXT)
  sess = SSL_get_session(ssl);
  if (sess != NULL) {
    pr_trace_msg(trace_channel, 19,
      "setting session ID context '%u' (%lu bytes) on SSL_SESSION %p (SSL %p)",
      s->sid, sizeof(s->sid), sess, ssl);
    SSL_SESSION_set1_id_context(sess, (unsigned char *) &(s->sid),
      sizeof(s->sid));
  }
#endif

  return 0;
}

static char *get_sess_id_text(BIO *bio, const unsigned char *id,
    unsigned int idsz) {
  char *data = NULL;
  long datalen;

  if (idsz > 0) {
    register unsigned int i;

    for (i = 0; i < idsz; i++) {
      BIO_printf(bio, "%02x", id[i]);
    }

  } else {
    BIO_printf(bio, "%s", "NONE");
  }

  datalen = BIO_get_mem_data(bio, &data);
  if (data != NULL) {
    data[datalen] = '\0';

  } else {
    data = "UNKNOWN";
  }

  return data;
}

static int tls_sni_sess_tab_add_cb(SSL *ssl, SSL_SESSION *sess) {
  unsigned char *sess_id;
  unsigned int sess_id_len;
  void *key;

#if OPENSSL_VERSION_NUMBER > 0x000908000L
  sess_id = (unsigned char *) SSL_SESSION_get_id(sess, &sess_id_len);
#else
  /* XXX Directly accessing these fields cannot be a Good Thing. */
  sess_id = sess->session_id;
  sess_id_len = sess->session_id_length;
#endif

  key = pr_table_pcalloc(tls_sni_sess_tab, sess_id_len);
  memcpy(key, sess_id, sess_id_len);

  if (pr_table_kadd(tls_sni_sess_tab, key, (size_t) sess_id_len,
      sess, sizeof(sess)) < 0) {
    pr_trace_msg(trace_channel, 3, "error adding SSL_SESSION to SNI table: %s",
      strerror(errno));
    return 0;
  }

  if (pr_trace_get_level(trace_channel) >= 29) {
    BIO *bio;
    char *data = NULL;
    long datalen;

    bio = BIO_new(BIO_s_mem());
    SSL_SESSION_print(bio, sess);

    datalen = BIO_get_mem_data(bio, &data);
    if (data != NULL) {
      data[datalen] = '\0';

      pr_trace_msg(trace_channel, 29, "added session to SNI table:\n%.*s",
        (int) datalen, data);
    }

    BIO_free(bio);

  } else {
    BIO *bio;
    char *text;

    bio = BIO_new(BIO_s_mem());
    text = get_sess_id_text(bio, sess_id, sess_id_len);
    pr_trace_msg(trace_channel, 9, "added session (ID %s) to SNI table", text);
    BIO_free(bio);
  }

  return 0;
}

static SSL_SESSION *tls_sni_sess_tab_get_cb(SSL *ssl,
    const unsigned char *sess_id, int sess_id_len, int *do_copy) {
  SSL_SESSION *sess = NULL;
  const void *val;
  BIO *bio;
  char *text;

  /* Indicate to OpenSSL that the ref count should not be incremented
   * by setting the do_copy pointer to zero.
   */
  *do_copy = 0;

  bio = BIO_new(BIO_s_mem());
  text = get_sess_id_text(bio, sess_id, sess_id_len);
  pr_trace_msg(trace_channel, 9, "getting session (ID %s) from SNI table",
    text);

  val = pr_table_kget(tls_sni_sess_tab, (const void *) sess_id,
    (size_t) sess_id_len, NULL);
  if (val == NULL) {
    pr_trace_msg(trace_channel, 9, "session (ID %s) not found in SNI table",
      text);
    BIO_free(bio);

    errno = ENOENT;
    return NULL;
  }

  sess = (SSL_SESSION *) val;

  if (pr_trace_get_level(trace_channel) >= 29) {
    char *data = NULL;
    long datalen;

    BIO_free(bio);
    bio = BIO_new(BIO_s_mem());
    SSL_SESSION_print(bio, sess);

    datalen = BIO_get_mem_data(bio, &data);
    if (data != NULL) {
      data[datalen] = '\0';

      pr_trace_msg(trace_channel, 29, "found session in SNI table:\n%.*s",
        (int) datalen, data);
    }

  } else {
    pr_trace_msg(trace_channel, 9, "found session (ID %s) in SNI table", text);
  }

  BIO_free(bio);
  return sess;
}

static void tls_sni_sess_tab_delete_cb(SSL_CTX *ctx, SSL_SESSION *sess) {
  unsigned char *sess_id;
  unsigned int sess_id_len;
  const void *val;
  BIO *bio;
  char *text;

#if OPENSSL_VERSION_NUMBER > 0x000908000L
  sess_id = (unsigned char *) SSL_SESSION_get_id(sess, &sess_id_len);
#else
  /* XXX Directly accessing these fields cannot be a Good Thing. */
  sess_id = sess->session_id;
  sess_id_len = sess->session_id_length;
#endif

  bio = BIO_new(BIO_s_mem());
  text = get_sess_id_text(bio, sess_id, sess_id_len);
  pr_trace_msg(trace_channel, 9, "removing session (ID %s) from SNI table",
    text);

  val = pr_table_kremove(tls_sni_sess_tab, (const void *) sess_id,
    (size_t) sess_id_len, NULL);
  if (val == NULL) {
    if (errno == ENOENT) {
      pr_trace_msg(trace_channel, 9, "no session (ID %s) found in SNI table",
        text);

    } else {
      pr_trace_msg(trace_channel, 9,
        "error removing session (ID %s) from SNI table: %s", text,
          strerror(errno));
    }
  }

  BIO_free(bio);
}

static int tls_ssl_set_all(server_rec *s, SSL *ssl) {
  SSL_CTX *ctx = NULL;
  long cache_mode;

  /* This is the key to switching CAs, cert chains, etc for the SSL object
   * for SNI support: build an entirely new SSL_CTX, using the new/proper CAs
   * et al, and set that on the SSL object.
   */
  ctx = tls_init_ctx(s);
  if (ctx == NULL) {
    return -1;
  }

  pr_trace_msg(trace_channel, 19,
    "setting new SSL_CTX for future data transfers");

  /* Update the SSL_CTX for the possibly changed <VirtualHost> config.
   *
   * Even though our SSL has already been created from this SSL_CTX,
   * which means any SSL_CTX changes WILL NOT be conveyed to our SSL,
   * we update it for any data transfer SSL objects later.
   */
  if (tls_ctx_set_all(s, ctx) < 0) {
    return -1;
  }

  /* Copy the existing session cache settings into the new SSL_CTX. */
  cache_mode = SSL_CTX_get_session_cache_mode(ssl_ctx);
  SSL_CTX_set_session_cache_mode(ctx, cache_mode);

  if (cache_mode == SSL_SESS_CACHE_OFF) {
    /* Make sure we automatically relax the "SSL session reuse required
     * for data connections" requirement; to enforce such a requirement
     * TLS session caching MUST be enabled.  So if session caching has been
     * explicitly disabled, relax that policy as well.
     */
    tls_opts |= TLS_OPT_NO_SESSION_REUSE_REQUIRED;
  }

  /* Use an in-memory table-based session store to preserve sessions
   * across the SNI-mandated SSL_CTX changes, in order to preserve
   * session resumption for data transfers (Issue #924).
   *
   * Note that this is only necessary if a TLSSessionCache has not been
   * configured.
   */

  if (SSL_CTX_sess_get_new_cb(ssl_ctx) == NULL) {
    tls_sni_sess_tab = pr_table_alloc(session.pool, 0);

    /* Yes, it looks strange to be setting these callbacks on the old
     * SSL_CTX, the one we will be replacing with our new SNI SSL_CTX.
     *
     * However, that old SSL_CTX is preserved (and used!) in the SSL object
     * via the initial_ctx, and thus its callbacks are used when adding
     * sessions.  And later, for data transfers, the SNI SSL_CTX callbacks
     * will be used for session lookup.  Fun, huh?
     */
    SSL_CTX_sess_set_new_cb(ssl_ctx, tls_sni_sess_tab_add_cb);
    SSL_CTX_sess_set_get_cb(ssl_ctx, tls_sni_sess_tab_get_cb);
    SSL_CTX_sess_set_remove_cb(ssl_ctx, tls_sni_sess_tab_delete_cb);
  }

  SSL_CTX_sess_set_new_cb(ctx, SSL_CTX_sess_get_new_cb(ssl_ctx));
  SSL_CTX_sess_set_get_cb(ctx, SSL_CTX_sess_get_get_cb(ssl_ctx));
  SSL_CTX_sess_set_remove_cb(ctx, SSL_CTX_sess_get_remove_cb(ssl_ctx));
  SSL_CTX_set_timeout(ctx, SSL_CTX_get_timeout(ssl_ctx));

  /* We MUST update the session ID context for the current SSL before we
   * replace its SSL_CTX, due to logic in the SSL_set_SSL_CTX internals.
   */
  if (tls_ssl_set_session_id_context(s, ssl) < 0 ||
      tls_ctx_set_session_id_context(s, ctx) < 0) {
    return -1;
  }

  /* Inexplicable OpenSSL errors occur if the cert chain is updated after
   * calling SSL_set_SSL_CTX, so we do it beforehand.
   */
  if (tls_ssl_set_cert_chain(ssl) < 0) {
    return -1;
  }

#if OPENSSL_VERSION_NUMBER > 0x009080cfL
  /* Note that it is important that we update the SSL with the new SSL_CTX
   * AFTER it has been provisioned.  That way, the new/changed certs in the
   * SSL_CTX will be properly copied/updated in the SSL object.
   */
  ctx = SSL_set_SSL_CTX(ssl, ctx);
#endif

  if (ssl_ctx != NULL) {
    /* Try not to leak memory. */
    SSL_CTX_free(ssl_ctx);
  }

  ssl_ctx = ctx;

  pr_trace_msg(trace_channel, 19, "resetting SSL for ctrl connection");

  if (tls_ssl_set_ciphers(ssl) < 0) {
    return -1;
  }

  if (tls_ssl_set_ecdh_curve(ssl) < 0) {
    return -1;
  }

  if (tls_ssl_set_psks(ssl) < 0) {
    return -1;
  }

  if (tls_ssl_set_options(ssl) < 0) {
    return -1;
  }

  if (tls_ssl_set_next_protocol(ssl) < 0) {
    return -1;
  }

  if (tls_ssl_set_protocol(s, ssl) < 0) {
    return -1;
  }

  if (tls_ssl_set_renegotiations(ssl) < 0) {
    return -1;
  }

  /* Note that we set this after setting the protocol versions, since the
   * supported protocol version flags will affect session tickets, due to
   * TLSv1.3' use of tickets.
   */
  if (tls_ssl_set_session_tickets(ssl) < 0) {
    return -1;
  }

  if (tls_ssl_set_verify(ssl) < 0) {
    return -1;
  }

  return 0;
}

/* SSL_CTX setters */

static int tls_ctx_set_ca_certs(SSL_CTX *ctx) {
  if (tls_ca_file == NULL &&
      tls_ca_path == NULL) {
    /* Default to using locations set in the OpenSSL config file. */
    pr_trace_msg(trace_channel, 9,
      "using default OpenSSL verification locations (see $SSL_CERT_DIR "
      "environment variable)");

    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      tls_log("error setting default verification locations: %s",
        tls_get_errors());
    }

    return 0;
  }

  /* Set the locations used for verifying certificates. */
  PRIVS_ROOT
  if (SSL_CTX_load_verify_locations(ctx, tls_ca_file, tls_ca_path) != 1) {
    PRIVS_RELINQUISH

    tls_log("unable to set CA verification using file '%s' or directory '%s': "
      "%s", tls_ca_file ? tls_ca_file : "(none)",
      tls_ca_path ? tls_ca_path : "(none)", tls_get_errors());
      return -1;
  }
  PRIVS_RELINQUISH

  if (tls_ca_file != NULL) {
    pr_trace_msg(trace_channel, 19, "using CA verification file '%s'",
      tls_ca_file);
  }

  if (tls_ca_path != NULL) {
    pr_trace_msg(trace_channel, 19, "using CA verification directory '%s'",
      tls_ca_path);
  }

  if (tls_ca_file != NULL) {
    STACK_OF(X509_NAME) *sk;

    /* Use SSL_load_client_CA_file() to load all of the CA certs (since
     * there can be more than one) from the TLSCACertificateFile.  The
     * entire list of CAs in that file will be present to the client as
     * the "acceptable client CA" list.
     */

    PRIVS_ROOT
    sk = SSL_load_client_CA_file(tls_ca_file);
    PRIVS_RELINQUISH

    if (sk != NULL) {
      SSL_CTX_set_client_CA_list(ctx, sk);

    } else {
      tls_log("unable to read certificates in '%s': %s", tls_ca_file,
        tls_get_errors());
    }
  }

  if (tls_ca_path != NULL) {
    DIR *cacertdir = NULL;

    PRIVS_ROOT
    cacertdir = opendir(tls_ca_path);
    PRIVS_RELINQUISH

    if (cacertdir != NULL) {
      struct dirent *cadent = NULL;
      pool *tmp_pool = make_sub_pool(permanent_pool);

      while ((cadent = readdir(cacertdir)) != NULL) {
        FILE *cacertf;
        char *cacertname;

        pr_signals_handle();

        /* Skip dot directories. */
        if (is_dotdir(cadent->d_name)) {
          continue;
        }

        cacertname = pdircat(tmp_pool, tls_ca_path, cadent->d_name, NULL);

        PRIVS_ROOT
        cacertf = fopen(cacertname, "r");
        PRIVS_RELINQUISH

        if (cacertf != NULL) {
          X509 *x509;

          x509 = PEM_read_X509(cacertf, NULL, NULL, NULL);
          if (x509 != NULL) {
            if (SSL_CTX_add_client_CA(ctx, x509) != 1) {
              tls_log("error adding '%s' to client CA list: %s", cacertname,
                tls_get_errors());
            }

          } else {
            tls_log("unable to read '%s': %s", cacertname, tls_get_errors());
          }

          fclose(cacertf);

        } else {
          tls_log("unable to open '%s': %s", cacertname, strerror(errno));
        }
      }

      destroy_pool(tmp_pool);
      closedir(cacertdir);

    } else {
      tls_log("unable to add CAs in '%s': %s", tls_ca_path, strerror(errno));
    }
  }

  return 0;
}

static int tls_ctx_set_dsa_cert(SSL_CTX *ctx, X509 **dsa_cert) {
  X509 *cert;
  FILE *fh = NULL;
  int res, xerrno;
  const char *key_file = NULL;

  if (tls_dsa_cert_file == NULL) {
    return 0;
  }

  PRIVS_ROOT
  fh = fopen(tls_dsa_cert_file, "r");
  xerrno = errno;
  if (fh == NULL) {
    PRIVS_RELINQUISH
    tls_log("error reading TLSDSACertificateFile '%s': %s", tls_dsa_cert_file,
      strerror(xerrno));
    errno = xerrno;
    return -1;
  }

  /* Assume that if no separate TLSDSACertificateKeyFile was configured (or
   * that if the configured TLSDSACertificateKeyFile is to the same path as
   * the TLSDSACertificateFile), that the cert and key are in the same file.
   * In that case, the file contains sensitive data, and we do not want it
   * lingering around in stdio buffers.
   */
  if (tls_dsa_key_file == NULL ||
      strcmp(tls_dsa_cert_file, tls_dsa_key_file) == 0) {
    (void) setvbuf(fh, NULL, _IONBF, 0);
  }

  cert = read_cert(fh, ctx);
  if (cert == NULL) {
    PRIVS_RELINQUISH
    tls_log("error reading TLSDSACertificateFile '%s': %s", tls_dsa_cert_file,
      tls_get_errors());
    fclose(fh);
    return -1;
  }

  fclose(fh);

  /* SSL_CTX_use_certificate() will increment the refcount on cert, so we
   * can safely call X509_free() on it.  However, we need to keep that
   * pointer around until after the handling of a cert chain file.
   */
  res = SSL_CTX_use_certificate(ctx, cert);
  if (res <= 0) {
    PRIVS_RELINQUISH

    tls_log("error loading TLSDSACertificateFile '%s': %s", tls_dsa_cert_file,
      tls_get_errors());
    return -1;
  }

  *dsa_cert = cert;
  pr_trace_msg(trace_channel, 19, "loaded DSA server certificate from '%s'",
    tls_dsa_cert_file);

  key_file = tls_dsa_key_file;
  if (key_file == NULL) {
    /* Assume the private key is in the cert file. */
    key_file = tls_dsa_cert_file;
  }

  if (tls_pkey != NULL) {
    tls_pkey->flags |= TLS_PKEY_USE_DSA;
    tls_pkey->flags &= ~(TLS_PKEY_USE_RSA|TLS_PKEY_USE_EC);
  }

  res = SSL_CTX_use_PrivateKey_file(ctx, key_file, X509_FILETYPE_PEM);
  if (res <= 0) {
    PRIVS_RELINQUISH

    tls_log("error loading TLSDSACertificateKeyFile '%s': %s", key_file,
      tls_get_errors());
    return -1;
  }

  res = SSL_CTX_check_private_key(ctx);
  if (res != 1) {
    PRIVS_RELINQUISH

    tls_log("error checking key from TLSDSACertificateKeyFile '%s': %s",
      key_file, tls_get_errors());
    return -1;
  }
  PRIVS_RELINQUISH

  return 0;
}

static int tls_ctx_set_ec_cert(SSL_CTX *ctx, X509 **ec_cert) {
#if defined(PR_USE_OPENSSL_ECC)
  X509 *cert;
  FILE *fh = NULL;
  int res, xerrno;
  const char *key_file = NULL;

  if (tls_ec_cert_file == NULL) {
    return 0;
  }

  PRIVS_ROOT
  fh = fopen(tls_ec_cert_file, "r");
  xerrno = errno;
  if (fh == NULL) {
    PRIVS_RELINQUISH
    tls_log("error reading TLSECCertificateFile '%s': %s", tls_ec_cert_file,
      strerror(xerrno));
    errno = xerrno;
    return -1;
  }

  /* Assume that if no separate TLSECCertificateKeyFile was configured (or
   * that if the configured TLSECCertificateKeyFile is to the same path as
   * the TLSECCertificateFile), that the cert and key are in the same file.
   * In that case, the file contains sensitive data, and we do not want it
   * lingering around in stdio buffers.
   */
  if (tls_ec_key_file == NULL ||
      strcmp(tls_ec_cert_file, tls_ec_key_file) == 0) {
    (void) setvbuf(fh, NULL, _IONBF, 0);
  }

  cert = read_cert(fh, ctx);
  if (cert == NULL) {
    PRIVS_RELINQUISH
    tls_log("error reading TLSECCertificateFile '%s': %s", tls_ec_cert_file,
      tls_get_errors());
    fclose(fh);
    return -1;
  }

  fclose(fh);

  /* SSL_CTX_use_certificate() will increment the refcount on cert, so we
   * can safely call X509_free() on it.  However, we need to keep that
   * pointer around until after the handling of a cert chain file.
   */
  res = SSL_CTX_use_certificate(ctx, cert);
  if (res <= 0) {
    PRIVS_RELINQUISH

    tls_log("error loading TLSECCertificateFile '%s': %s", tls_ec_cert_file,
      tls_get_errors());
    return -1;
  }

  *ec_cert = cert;
  pr_trace_msg(trace_channel, 19, "loaded EC server certificate from '%s'",
    tls_ec_cert_file);

  key_file = tls_ec_key_file;
  if (key_file == NULL) {
    /* Assume the private key is in the cert file. */
    key_file = tls_ec_cert_file;
  }

  if (tls_pkey != NULL) {
    tls_pkey->flags |= TLS_PKEY_USE_EC;
    tls_pkey->flags &= ~(TLS_PKEY_USE_RSA|TLS_PKEY_USE_DSA);
  }

  res = SSL_CTX_use_PrivateKey_file(ctx, key_file, X509_FILETYPE_PEM);
  if (res <= 0) {
    PRIVS_RELINQUISH

    tls_log("error loading TLSECCertificateKeyFile '%s': %s", key_file,
      tls_get_errors());
    return -1;
  }

  res = SSL_CTX_check_private_key(ctx);
  if (res != 1) {
    PRIVS_RELINQUISH

    tls_log("error checking key from TLSECCertificateKeyFile '%s': %s",
      key_file, tls_get_errors());
    return -1;
  }

  PRIVS_RELINQUISH
#endif /* PR_USE_OPENSSL_ECC */

  return 0;
}

static int tls_ctx_set_pkcs12_cert(SSL_CTX *ctx, X509 **dsa_cert,
    X509 **ec_cert, X509 **rsa_cert) {
  PKCS12 *p12 = NULL;
  X509 *cert = NULL;
  EVP_PKEY *pkey = NULL;
  FILE *fh;
  int res, xerrno;
  char *passwd = "";

  if (tls_pkcs12_file == NULL) {
    return 0;
  }

  if (tls_pkey) {
    passwd = tls_pkey->pkcs12_passwd;
  }

  PRIVS_ROOT
  fh = fopen(tls_pkcs12_file, "r");
  xerrno = errno;
  if (fh == NULL) {
    PRIVS_RELINQUISH

    tls_log("error opening TLSPKCS12File '%s': %s", tls_pkcs12_file,
      strerror(xerrno));
    errno = xerrno;
    return -1;
  }

  /* As the file may contain sensitive data, we do not want it lingering
   * around in stdio buffers.
   */
  (void) setvbuf(fh, NULL, _IONBF, 0);

  /* Note that this should NOT fail; we will have already parsed the PKCS12
   * file, in order to get the password and key passphrases.
   */
  p12 = d2i_PKCS12_fp(fh, NULL);
  if (p12 == NULL) {
    PRIVS_RELINQUISH

    tls_log("error reading TLSPKCS12File '%s': %s", tls_pkcs12_file,
      tls_get_errors());
    fclose(fh);
    return -1;
  }

  fclose(fh);

  /* XXX Need to add support for any CA certs contained in the PKCS12 file. */
  if (PKCS12_parse(p12, passwd, &pkey, &cert, NULL) != 1) {
    PRIVS_RELINQUISH

    tls_log("error parsing info in TLSPKCS12File '%s': %s", tls_pkcs12_file,
      tls_get_errors());

    PKCS12_free(p12);

    if (cert != NULL) {
      X509_free(cert);
    }

    if (pkey != NULL) {
      EVP_PKEY_free(pkey);
    }

    return -1;
  }

  /* Note: You MIGHT be tempted to call SSL_CTX_check_private_key(ctx) here,
   * to make sure that the private key can be used.  But doing so will not
   * work as expected, and will error out with:
   *
   *  SSL_CTX_check_private_key:no certificate assigned
   *
   * To make PKCS#12 support work, do not make that call here.
   */

  res = SSL_CTX_use_certificate(ctx, cert);
  if (res <= 0) {
    PRIVS_RELINQUISH

    tls_log("error loading certificate from TLSPKCS12File '%s' %s",
      tls_pkcs12_file, tls_get_errors());
    PKCS12_free(p12);

    if (cert != NULL) {
      X509_free(cert);
    }

    if (pkey != NULL) {
      EVP_PKEY_free(pkey);
    }

    return -1;
  }

  if (pkey != NULL &&
      tls_pkey != NULL) {
    switch (get_pkey_type(pkey)) {
      case EVP_PKEY_RSA:
        tls_pkey->flags |= TLS_PKEY_USE_RSA;
        tls_pkey->flags &= ~(TLS_PKEY_USE_DSA|TLS_PKEY_USE_EC);
        break;

      case EVP_PKEY_DSA:
        tls_pkey->flags |= TLS_PKEY_USE_DSA;
        tls_pkey->flags &= ~(TLS_PKEY_USE_RSA|TLS_PKEY_USE_EC);
        break;

#ifdef PR_USE_OPENSSL_ECC
      case EVP_PKEY_EC:
        tls_pkey->flags |= TLS_PKEY_USE_EC;
        tls_pkey->flags &= ~(TLS_PKEY_USE_RSA|TLS_PKEY_USE_DSA);
        break;
#endif /* PR_USE_OPENSSL_ECC */
    }
  }

  res = SSL_CTX_use_PrivateKey(ctx, pkey);
  if (res <= 0) {
    PRIVS_RELINQUISH

    tls_log("error loading key from TLSPKCS12File '%s' %s", tls_pkcs12_file,
      tls_get_errors());

    PKCS12_free(p12);

    if (cert != NULL) {
      X509_free(cert);
    }

    if (pkey != NULL) {
      EVP_PKEY_free(pkey);
    }

    return -1;
  }

  res = SSL_CTX_check_private_key(ctx);
  if (res != 1) {
    PRIVS_RELINQUISH

    tls_log("error checking key from TLSPKCS12File '%s': %s", tls_pkcs12_file,
      tls_get_errors());

    PKCS12_free(p12);

    if (cert != NULL) {
      X509_free(cert);
    }

    if (pkey != NULL) {
      EVP_PKEY_free(pkey);
    }

    return -1;
  }

  /* SSL_CTX_use_certificate() will increment the refcount on cert, so we
   * can safely call X509_free() on it.  However, we need to keep that
   * pointer around until after the handling of a cert chain file.
   */
  if (pkey != NULL) {
    switch (get_pkey_type(pkey)) {
      case EVP_PKEY_RSA:
        *rsa_cert = cert;
        break;

      case EVP_PKEY_DSA:
        *dsa_cert = cert;
        break;

#ifdef PR_USE_OPENSSL_ECC
      case EVP_PKEY_EC:
        *ec_cert = cert;
        break;
#endif /* PR_USE_OPENSSL_ECC */
    }
  }

  if (pkey != NULL) {
    EVP_PKEY_free(pkey);
  }

  if (p12 != NULL) {
    PKCS12_free(p12);
  }

  PRIVS_RELINQUISH

  return 0;
}

static int tls_ctx_set_rsa_cert(SSL_CTX *ctx, X509 **rsa_cert) {
  X509 *cert;
  FILE *fh = NULL;
  int res, xerrno;
  const char *key_file = NULL;

  if (tls_rsa_cert_file == NULL) {
    return 0;
  }

  PRIVS_ROOT
  fh = fopen(tls_rsa_cert_file, "r");
  xerrno = errno;
  if (fh == NULL) {
    PRIVS_RELINQUISH
    pr_log_pri(PR_LOG_DEBUG, MOD_TLS_VERSION
      ": error reading TLSRSACertificateFile '%s': %s", tls_rsa_cert_file,
      strerror(xerrno));
    errno = xerrno;
    return -1;
  }

  /* Assume that if no separate TLSRSACertificateKeyFile was configured (or
   * that if the configured TLSRSACertificateKeyFile is to the same path as
   * the TLSRSACertificateFile), that the cert and key are in the same file.
   * In that case, the file contains sensitive data, and we do not want it
   * lingering around in stdio buffers.
   */
  if (tls_rsa_key_file == NULL ||
      strcmp(tls_rsa_cert_file, tls_rsa_key_file) == 0) {
    (void) setvbuf(fh, NULL, _IONBF, 0);
  }

  cert = read_cert(fh, ctx);
  if (cert == NULL) {
    PRIVS_RELINQUISH
    pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
      ": error reading TLSRSACertificateFile '%s': %s", tls_rsa_cert_file,
      tls_get_errors());
    fclose(fh);
    return -1;
  }

  fclose(fh);

  /* SSL_CTX_use_certificate() will increment the refcount on cert, so we
   * can safely call X509_free() on it.  However, we need to keep that
   * pointer around until after the handling of a cert chain file.
   */
  res = SSL_CTX_use_certificate(ctx, cert);
  if (res <= 0) {
    PRIVS_RELINQUISH

    pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
      ": error loading TLSRSACertificateFile '%s': %s", tls_rsa_cert_file,
      tls_get_errors());
    return -1;
  }

  *rsa_cert = cert;
  pr_trace_msg(trace_channel, 19, "loaded RSA server certificate from '%s'",
    tls_rsa_cert_file);

  key_file = tls_rsa_key_file;
  if (key_file == NULL) {
    /* Assume the private key is in the cert file. */
    key_file = tls_rsa_cert_file;
  }

  if (tls_pkey != NULL) {
    tls_pkey->flags |= TLS_PKEY_USE_RSA;
    tls_pkey->flags &= ~(TLS_PKEY_USE_DSA|TLS_PKEY_USE_EC);
  }

  res = SSL_CTX_use_PrivateKey_file(ctx, key_file, X509_FILETYPE_PEM);
  if (res <= 0) {
    const char *errors;

    PRIVS_RELINQUISH
    errors = tls_get_errors();

    pr_trace_msg(trace_channel, 3,
      "error loading TLSRSACertificateKeyFile '%s': %s", key_file, errors);
    pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
      ": error loading TLSRSACertificateKeyFile '%s': %s", key_file, errors);
    return -1;
  }

  res = SSL_CTX_check_private_key(ctx);
  if (res != 1) {
    const char *errors;

    PRIVS_RELINQUISH
    errors = tls_get_errors();

    pr_trace_msg(trace_channel, 3,
      "error checking key from TLSRSACertificateKeyFile '%s': %s", key_file,
      errors);
    pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
      ": error checking key from TLSRSACertificateKeyFile '%s': %s", key_file,
      errors);
    return -1;
  }
  PRIVS_RELINQUISH

  return 0;
}

static int tls_ctx_set_cert_chain(SSL_CTX *ctx, X509 *dsa_cert, X509 *ec_cert,
    X509 *rsa_cert) {
#if defined(SSL_CTRL_CHAIN_CERT)
  BIO *bio;
  X509 *cert;
  unsigned int count = 0;

  if (tls_ca_chain == NULL) {
    return 0;
  }

  PRIVS_ROOT
  bio = BIO_new_file(tls_ca_chain, "r");
  PRIVS_RELINQUISH

  if (bio == NULL) {
    tls_log("unable to read certificate chain '%s': %s", tls_ca_chain,
      tls_get_errors());
    return 0;
  }

  if (SSL_CTX_clear_chain_certs(ctx) != 1) {
    tls_log("error clearing SSL_CTX chain certs: %s", tls_get_errors());
    BIO_free(bio);
    return -1;
  }

  cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  while (cert != NULL) {
    pr_signals_handle();

    if (rsa_cert != NULL) {
      /* Skip this cert if it is the same as the configured RSA server cert. */
      if (X509_cmp(rsa_cert, cert) == 0) {
        X509_free(cert);
        cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        continue;
      }
    }

    if (dsa_cert != NULL) {
      /* Skip this cert if it is the same as the configured DSA server cert. */
      if (X509_cmp(dsa_cert, cert) == 0) {
        X509_free(cert);
        cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        continue;
      }
    }

    if (ec_cert != NULL) {
      /* Skip this cert if it is the same as the configured EC server cert. */
      if (X509_cmp(ec_cert, cert) == 0) {
        X509_free(cert);
        cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        continue;
      }
    }

    if (SSL_CTX_add1_chain_cert(ctx, cert) != 1) {
      tls_log("error adding cert to SSL_CTX certificate chain: %s",
        tls_get_errors());
      X509_free(cert);
      break;
    }

    count++;
    cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  }

  BIO_free(bio);
  ERR_clear_error();

  tls_log("added %u certs from '%s' to SSL_CTX certificate chain", count,
    tls_ca_chain);
#endif /* SSL_CTRL_CHAIN_CERT */

  return 0;
}

static int tls_ctx_set_certs(SSL_CTX *ctx, X509 **dsa_cert, X509 **ec_cert,
    X509 **rsa_cert) {
  if (tls_ctx_set_dsa_cert(ctx, dsa_cert) < 0 ||
      tls_ctx_set_ec_cert(ctx, ec_cert) < 0 ||
      tls_ctx_set_pkcs12_cert(ctx, dsa_cert, ec_cert, rsa_cert) < 0 ||
      tls_ctx_set_rsa_cert(ctx, rsa_cert) < 0) {
    return -1;
  }

  /* Log a warning if the server was badly misconfigured, and has no server
   * certs at all.  The client will probably see this situation as something
   * like:
   *
   *  error:14094410:SSL routines:SSL3_READ_BYTES:sslv3 alert handshake failure
   *
   * And the TLSLog will show the error as:
   *
   *  error:1408A0C1:SSL routines:SSL3_GET_CLIENT_HELLO:no shared cipher
   */
  if (tls_rsa_cert_file == NULL &&
      tls_dsa_cert_file == NULL &&
      tls_ec_cert_file == NULL &&
      tls_pkcs12_file == NULL) {
    tls_log("no TLSRSACertificateFile, TLSDSACertificateFile, "
      "TLSECCertificateFile, or TLSPKCS12File configured; "
      "unable to handle SSL/TLS connections");
    pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
      ": no TLSRSACertificateFile, TLSDSACertificateFile, "
      "TLSECCertificateFile or TLSPKCS12File configured; "
      "unable to handle SSL/TLS connections");
  }

  return 0;
}

static int tls_ctx_set_ciphers(SSL_CTX *ctx) {
  SSL_CTX_set_cipher_list(ctx, tls_cipher_suite);

#if OPENSSL_VERSION_NUMBER >= 0x10101000L && \
    defined(TLS1_3_VERSION)
  if (tlsv13_cipher_suite != NULL) {
    SSL_CTX_set_ciphersuites(ctx, tlsv13_cipher_suite);
  }
#endif /* TLS1_3_VERSION */

  return 0;
}

static int tls_ctx_set_crls(SSL_CTX *ctx) {
  X509_STORE *store;

  if (tls_crl_file == NULL &&
      tls_crl_path == NULL) {
    return 0;
  }

  store = SSL_CTX_get_cert_store(ctx);
  if (store == NULL) {
    tls_log("error getting SSL_CTX store: %s", tls_get_errors());
    return -1;
  }

  PRIVS_ROOT
  if (X509_STORE_load_locations(store, tls_crl_file, tls_crl_path) != 1) {
    if (tls_crl_file != NULL &&
        tls_crl_path == NULL) {
      tls_log("error loading TLSCARevocationFile '%s': %s", tls_crl_file,
        tls_get_errors());

    } else if (tls_crl_file == NULL &&
               tls_crl_path != NULL) {
      tls_log("error loading TLSCARevocationPath '%s': %s", tls_crl_path,
        tls_get_errors());

    } else {
      tls_log("error loading TLSCARevocationFile '%s', "
        "TLSCARevocationPath '%s': %s", tls_crl_file, tls_crl_path,
        tls_get_errors());
    }
  }

  PRIVS_RELINQUISH

  if (tls_crl_file != NULL) {
    pr_trace_msg(trace_channel, 19, "using CRL file '%s'", tls_crl_file);
  }

  if (tls_crl_path != NULL) {
    pr_trace_msg(trace_channel, 19, "using CRL directory '%s'", tls_crl_path);
  }

  /* Make sure we enable CRL checking, now that we've loaded them. */
  X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK|X509_V_FLAG_CRL_CHECK_ALL);
  X509_STORE_set_purpose(store, X509_PURPOSE_SSL_CLIENT);

  return 0;
}

static int tls_ctx_set_ecdh_curve(SSL_CTX *ctx) {
#if defined(PR_USE_OPENSSL_ECC)
# if defined(SSL_CTX_set_ecdh_auto)
  if (tls_opts & TLS_OPT_NO_AUTO_ECDH) {
    SSL_CTX_set_ecdh_auto(ctx, 0);
  }
# endif

  if (tls_ecdh_curve != NULL) {
# if defined(SSL_CTX_set1_curves_list)
    char *curve_names;

    curve_names = tls_ecdh_curve;
    if (strcasecmp(curve_names, "auto") != 0) {
      SSL_CTX_set1_curves_list(ctx, curve_names);
    }
# else
    const EC_KEY *ec_key;

    ec_key = tls_ecdh_curve;
    SSL_CTX_set_tmp_ecdh(ctx, ec_key);
# endif /* pre OpenSSL-1.0.2 */

    SSL_CTX_set_options(ctx, SSL_OP_SINGLE_ECDH_USE);
  } else {
# if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_CTX_set_tmp_ecdh_callback(ctx, tls_ecdh_cb);
# endif /* Before OpenSSL-1.1.x */
  }
#endif /* PR_USE_OPENSS_ECC */

  return 0;
}

static int tls_ctx_set_next_protocol(SSL_CTX *ctx) {
#if !defined(OPENSSL_NO_TLSEXT)
  register unsigned int i;
  const char *proto = TLS_DEFAULT_NEXT_PROTO;
  size_t encoded_protolen, proto_len;
  unsigned char *encoded_proto;
  struct tls_next_proto *next_proto;

  if (tls_use_next_protocol == FALSE) {
    return 0;
  }

  proto_len = strlen(proto);
  encoded_protolen = proto_len + 1;
  encoded_proto = palloc(session.pool, encoded_protolen);
  encoded_proto[0] = proto_len;
  for (i = 0; i < proto_len; i++) {
    encoded_proto[i+1] = proto[i];
  }

  next_proto = palloc(session.pool, sizeof(struct tls_next_proto));
  next_proto->proto = pstrdup(session.pool, proto);
  next_proto->encoded_proto = encoded_proto;
  next_proto->encoded_protolen = encoded_protolen;

# if defined(PR_USE_OPENSSL_NPN)
  SSL_CTX_set_next_protos_advertised_cb(ctx, tls_npn_advertised_cb, next_proto);
# endif /* NPN */

# if defined(PR_USE_OPENSSL_ALPN)
  SSL_CTX_set_alpn_select_cb(ctx, tls_alpn_select_cb, next_proto);
# endif /* ALPN */
#endif /* !OPENSSL_NO_TLSEXT */

  return 0;
}

static int tls_ctx_set_options(SSL_CTX *ctx) {
#if OPENSSL_VERSION_NUMBER > 0x009080cfL
# if defined(SSL_OP_CIPHER_SERVER_PREFERENCE)
  SSL_CTX_clear_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
# endif /* SSL_OP_CIPHER_SERVER_PREFERENCE */

  /* The OpenSSL team realized that the flag added in 0.9.8l, the
   * SSL3_FLAGS_ALLOW_UNSAFE_LEGACY_RENEGOTIATION flag, was a bad idea.
   * So in later versions, it was changed to a context flag,
   * SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION.
   */
  SSL_CTX_clear_options(ctx, SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
  if (tls_opts & TLS_OPT_ALLOW_CLIENT_RENEGOTIATIONS) {
    SSL_CTX_set_options(ctx, SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
  }
#endif

#if defined(SSL_OP_CIPHER_SERVER_PREFERENCE)
  if (tls_use_server_cipher_preference == TRUE) {
    SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
  }
#endif /* SSL_OP_CIPHER_SERVER_PREFERENCE */

#if OPENSSL_VERSION_NUMBER > 0x000907000L
  /* Install a callback for logging OpenSSL message information, if requested.
   */
  if (tls_opts & TLS_OPT_ENABLE_DIAGS) {
    tls_log("%s",
      "TLSOption EnableDiags enabled, setting diagnostics callback");
    SSL_CTX_set_msg_callback(ctx, tls_msg_cb);
  }
#endif

  return 0;
}

static int tls_ctx_set_pkey(SSL_CTX *ctx) {
  if (tls_pkey != NULL) {
    SSL_CTX_set_default_passwd_cb(ctx, tls_pkey_cb);
    SSL_CTX_set_default_passwd_cb_userdata(ctx, (void *) tls_pkey);
  }

  return 0;
}

static int tls_ctx_set_protocol(server_rec *s, SSL_CTX *ctx) {
  int all_proto, disabled_proto;
  unsigned int enabled_proto_count = 0;
  const char *enabled_proto_str = NULL;

  /* First, make sure ALL protocol versions are disabled by default. */
  all_proto = get_disabled_protocols(0);
  SSL_CTX_set_options(ctx, all_proto);

  disabled_proto = get_disabled_protocols(tls_protocol);

  /* Per the comments in <ssl/ssl.h>, SSL_CTX_set_options() uses |= on
   * the previous value.  This means we can easily OR in our new option
   * values with any previously set values.
   */
  enabled_proto_str = tls_get_proto_str(s->pool, tls_protocol,
    &enabled_proto_count);

  pr_log_debug(DEBUG8, MOD_TLS_VERSION ": supporting %s %s", enabled_proto_str,
    enabled_proto_count != 1 ? "protocols" : "protocol only");

  /* Now we clear all of the NO_ protocol version options EXCEPT for the
   * enabled versions.
   *
   * This is more convoluted than it should be, because of the expression
   * of enabled protocol versions via OP_NO_ negations, and bitmasking.
   */
#if OPENSSL_VERSION_NUMBER > 0x009080cfL
  SSL_CTX_clear_options(ctx, all_proto|disabled_proto);
#endif
  SSL_CTX_set_options(ctx, disabled_proto);

  return 0;
}

static int tls_ctx_set_psks(SSL_CTX *ctx) {
#if defined(PSK_MAX_PSK_LEN)
  if (tls_psks != NULL &&
      pr_table_count(tls_psks) > 0) {
    pr_trace_msg(trace_channel, 9,
      "enabling support for PSK identities (%d)", pr_table_count(tls_psks));
    SSL_CTX_set_psk_server_callback(ctx, tls_lookup_psk);
  }
#endif /* PSK_MAX_PSK_LEN */

  return 0;
}

static int tls_ctx_set_renegotiations(SSL_CTX *ctx) {
#if defined(SSL_OP_NO_RENEGOTIATION)
  if (tls_ctrl_renegotiate_timeout == 0 &&
      tls_data_renegotiate_limit == 0 &&
      tls_renegotiate_timeout == 0 &&
      tls_renegotiate_required == FALSE) {
    /* If server-requested renegotiations are disabled, reject
     * client-initiated renegotiations, too.
     */
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
  }
#endif /* SSL_OP_NO_RENEGOTIATION */

  return 0;
}

static int tls_ctx_set_serverinfo(SSL_CTX *ctx) {
#if !defined(OPENSSL_NO_TLSEXT)
# if OPENSSL_VERSION_NUMBER >= 0x10002000L && \
     !defined(HAVE_LIBRESSL)
  if (tls_serverinfo_file == NULL) {
    return 0;
  }

  if (SSL_CTX_use_serverinfo_file(ctx, tls_serverinfo_file) != 1) {
    tls_log("error setting server info using '%s': %s", tls_serverinfo_file,
      tls_get_errors());
  }
# endif /* OpenSSL-1.0.2 and later */
#endif /* !OPENSSL_NO_TLSEXT */

  return 0;
}

static int tls_ctx_set_session_id_context(server_rec *s, SSL_CTX *ctx) {
  /* Update the session ID context to use.  This is important; it ensures
   * that the session IDs for this particular vhost will differ from those
   * for another vhost.  An external TLS session cache will possibly
   * cache sessions from all vhosts together, and we need to keep them
   * separate.
   */
  pr_trace_msg(trace_channel, 19,
    "setting session ID context '%u' (%lu bytes) on SSL_CTX %p", s->sid,
    sizeof(s->sid), ctx);
  SSL_CTX_set_session_id_context(ctx, (unsigned char *) &(s->sid),
    sizeof(s->sid));

  return 0;
}

static int tls_ctx_set_session_cache(server_rec *s, SSL_CTX *ctx) {
  config_rec *c;
  const char *provider;
  long timeout;

  c = find_config(s->conf, CONF_PARAM, "TLSSessionCache", FALSE);
  if (c == NULL) {
    /* No explicit external session cache configured. */

#if OPENSSL_VERSION_NUMBER > 0x000907000L
    /* If we support renegotiations, then make the cache lifetime be 10%
     * longer than the control connection renegotiation timer.
     */
    timeout = 15840;
#else
    /* If we are not supporting reneogtiations because the OpenSSL version
     * is too old, then set the default cache lifetime to 30 minutes.
     */
    timeout = 1800;
#endif /* OpenSSL older than 0.9.7. */

    /* Make sure that we enable OpenSSL internal caching, AND that the
     * cache timeout is longer than the default control channel
     * renegotiation.
     */

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_timeout(ctx, timeout);

    return 0;
  }

  /* Look up and initialize the configured session cache provider. */
  provider = c->argv[0];
  timeout = *((long *) c->argv[2]);

  if (provider != NULL) {
    if (strncmp(provider, "internal", 9) != 0) {
      tls_sess_cache = tls_sess_cache_get_cache(provider);

      pr_log_debug(DEBUG8, MOD_TLS_VERSION ": opening '%s' TLSSessionCache",
        provider);

      if (tls_sess_cache_open(c->argv[1], timeout) == 0) {
        long cache_mode, cache_flags;

        cache_mode = SSL_SESS_CACHE_SERVER;

        /* We could force OpenSSL to use ONLY the configured external session
         * caching mechanism by using the SSL_SESS_CACHE_NO_INTERNAL mode flag
         * (available in OpenSSL 0.9.6h and later).
         *
         * However, consider the case where the serialized session data is
         * too large for the external cache, or the external cache refuses
         * to add the session for some reason.  If OpenSSL is using only our
         * external cache, that session is lost (which could cause problems
         * e.g. for later protected data transfers, which require that the
         * SSL session from the control connection be reused).
         *
         * If the external cache can be reasonably sure that session data
         * can be added, then the NO_INTERNAL flag is a good idea; it keeps
         * OpenSSL from allocating more memory than necessary.  Having both
         * an internal and an external cache of the same data is a bit
         * unresourceful.  Thus we ask the external cache mechanism what
         * additional cache mode flags to use.
         */

        cache_flags = tls_sess_cache_get_cache_mode();
        cache_mode |= cache_flags;

        SSL_CTX_set_session_cache_mode(ctx, cache_mode);
        SSL_CTX_set_timeout(ctx, timeout);

        SSL_CTX_sess_set_new_cb(ctx, tls_sess_cache_add_sess_cb);
        SSL_CTX_sess_set_get_cb(ctx, tls_sess_cache_get_sess_cb);
        SSL_CTX_sess_set_remove_cb(ctx, tls_sess_cache_delete_sess_cb);

      } else {
        pr_log_debug(DEBUG1, MOD_TLS_VERSION
          ": error opening '%s' TLSSessionCache: %s", provider,
          strerror(errno));

        /* Default to using OpenSSL's own internal session caching. */
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
      }

    } else {
      /* Default to using OpenSSL's own internal session caching. */
      SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
      SSL_CTX_set_timeout(ctx, timeout);
    }

  } else {
    /* SSL session caching has been explicitly turned off. */

    pr_log_debug(DEBUG3, MOD_TLS_VERSION
      ": TLSSessionCache off, disabling TLS session caching and setting "
      "NoSessionReuseRequired TLSOption");

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);

    /* Make sure we automatically relax the "SSL session reuse required
     * for data connections" requirement; to enforce such a requirement
     * TLS session caching MUST be enabled.  So if session caching has been
     * explicitly disabled, relax that policy as well.
     */
    tls_opts |= TLS_OPT_NO_SESSION_REUSE_REQUIRED;
  }

  return 0;
}

static int tls_ctx_set_session_tickets(SSL_CTX *ctx) {
#if defined(TLS_USE_SESSION_TICKETS) && \
    defined(SSL_OP_NO_TICKET)
  if (tls_use_session_tickets == TRUE) {
    if (SSL_CTX_set_tlsext_ticket_key_cb(ctx, tls_ticket_key_cb) == 0) {
      pr_log_pri(PR_LOG_WARNING, MOD_TLS_VERSION
        ": mod_tls compiled with Session Ticket support, but linked to "
        "an OpenSSL library without tlsext support, therefore Session "
        "Tickets are not available");
    }

    /* Ensure that tickets are enabled in the SSL_CTX options. */
    SSL_CTX_clear_options(ctx, SSL_OP_NO_TICKET);

  } else {
# if defined(TLS1_3_VERSION)
    /* If we might handle TLSv1.3 sessions, we need the callback for session
     * resumption; TLSv1.3 prefers stateless session tickets vs stateful
     * server-side session IDs.
     */
    if (SSL_CTX_get_options(ctx) & SSL_OP_NO_TLSv1_3) {
      SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
      SSL_CTX_set_tlsext_ticket_key_cb(ctx, NULL);

    } else {
      if (SSL_CTX_set_tlsext_ticket_key_cb(ctx, tls_ticket_key_cb) == 0) {
        pr_log_pri(PR_LOG_WARNING, MOD_TLS_VERSION
          ": mod_tls compiled with Session Ticket support, but linked to "
          "an OpenSSL library without tlsext support, therefore Session "
          "Tickets are not available");
      }

      SSL_CTX_clear_options(ctx, SSL_OP_NO_TICKET);
    }
# else
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    SSL_CTX_set_tlsext_ticket_key_cb(ctx, NULL);
# endif /* TLS1_3_VERSION */
  }
#endif /* TLS_USE_SESSION_TICKETS and SSL_OP_NO_TICKET */

  return 0;
}

static int tls_ctx_set_stapling(SSL_CTX *ctx) {
#if defined(PR_USE_OPENSSL_OCSP)
  SSL_CTX_set_tlsext_status_cb(ctx, tls_ocsp_cb);
  SSL_CTX_set_tlsext_status_arg(ctx, NULL);
#endif /* PR_USE_OPENSSL_OCSP */

  return 0;
}

static int tls_ctx_set_stapling_cache(server_rec *s, SSL_CTX *ctx) {
#if defined(PR_USE_OPENSSL_OCSP)
  config_rec *c;
  const char *provider;

  c = find_config(s->conf, CONF_PARAM, "TLSStaplingCache", FALSE);
  if (c == NULL) {
    return 0;
  }

  /* Look up and initialize the configured OCSP cache provider. */
  provider = c->argv[0];

  if (provider != NULL) {
    tls_ocsp_cache = tls_ocsp_cache_get_cache(provider);

    pr_log_debug(DEBUG8, MOD_TLS_VERSION ": opening '%s' TLSStaplingCache",
      provider);

    if (tls_ocsp_cache_open(c->argv[1]) < 0 &&
        errno != ENOSYS) {
      pr_log_debug(DEBUG1, MOD_TLS_VERSION
        ": error opening '%s' TLSStaplingCache: %s", provider,
        strerror(errno));
      tls_ocsp_cache = NULL;
    }
  }
#endif /* PR_USE_OPENSSL_OCSP */

  return 0;
}

static int tls_ctx_set_verify(SSL_CTX *ctx) {
  int verify_mode = 0;

  if (tls_flags & TLS_SESS_VERIFY_CLIENT_OPTIONAL) {
    verify_mode = SSL_VERIFY_PEER;
  }

  if (tls_flags & TLS_SESS_VERIFY_CLIENT_REQUIRED) {
    /* If we are verifying clients, make sure the client sends a cert;
     * the protocol allows for the client to disregard a request for
     * its cert by the server.
     */
    verify_mode |= (SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT);
  }

  if (verify_mode != 0) {
    SSL_CTX_set_verify(ctx, verify_mode, tls_verify_cb);

    /* Note: we add one to the configured depth purposefully.  As noted
     * in the OpenSSL man pages, the verification process will silently
     * stop at the configured depth, and the error messages ensuing will
     * be that of an incomplete certificate chain, rather than the
     * "chain too long" error that might be expected.  To log the "chain
     * too long" condition, we add one to the configured depth, and catch,
     * in the verify callback, the exceeding of the actual depth.
     */
    SSL_CTX_set_verify_depth(ctx, tls_verify_depth + 1);
  }

  return 0;
}

static int tls_ctx_set_all(server_rec *s, SSL_CTX *ctx) {
  X509 *dsa_cert = NULL, *ec_cert = NULL, *rsa_cert = NULL;

  if (tls_opts & TLS_OPT_ALLOW_WEAK_SECURITY) {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
    SSL_CTX_set_security_level(ctx, 0);
#endif /* OpenSSL-1.1.0 and later */
  }

  if (tls_ctx_set_pkey(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_ca_certs(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_certs(ctx, &dsa_cert, &ec_cert, &rsa_cert) < 0) {
    return -1;
  }

  /* Handle a CertificateChainFile.  We need to do this here, after the
   * server cert has been loaded, so that we can decide whether the
   * CertificateChainFile contains another copy of the server cert (or not).
   */
  if (tls_ctx_set_cert_chain(ctx, dsa_cert, ec_cert, rsa_cert) < 0) {
    return -1;
  }

  if (dsa_cert != NULL) {
    X509_free(dsa_cert);
    dsa_cert = NULL;
  }

  if (ec_cert != NULL) {
    X509_free(ec_cert);
    ec_cert = NULL;
  }

  if (rsa_cert != NULL) {
    X509_free(rsa_cert);
    rsa_cert = NULL;
  }

  if (tls_ctx_set_ciphers(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_crls(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_ecdh_curve(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_psks(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_options(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_next_protocol(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_protocol(s, ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_renegotiations(ctx) < 0) {
    return -1;
  }

  /* Note that we set this after setting the protocol versions, since the
   * supported protocol version flags will affect session tickets, due to
   * TLSv1.3' use of tickets.
   */
  if (tls_ctx_set_session_tickets(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_serverinfo(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_stapling(ctx) < 0) {
    return -1;
  }

  if (tls_ctx_set_verify(ctx) < 0) {
    return -1;
  }

  return 0;
}

/* Initialization routines
 */

static int tls_init(void) {
  unsigned long openssl_version;

  /* Check that the OpenSSL headers used match the version of the
   * OpenSSL library used.
   *
   * For now, we only log if there is a difference.
   */

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    defined(HAVE_LIBRESSL)
  openssl_version = SSLeay();
#else
  openssl_version = OpenSSL_version_num();
#endif /* prior to OpenSSL-1.1.x */

  if (openssl_version != OPENSSL_VERSION_NUMBER) {
    int unexpected_version_mismatch = TRUE;

    if (OPENSSL_VERSION_NUMBER >= 0x1000000fL) {
      /* OpenSSL versions after 1.0.0 try to maintain ABI compatibility.
       * So we will warn about header/library version mismatches only if
       * the library is older than the headers.
       */
      if (openssl_version >= OPENSSL_VERSION_NUMBER) {
        unexpected_version_mismatch = FALSE;
      }
    }

    if (unexpected_version_mismatch == TRUE) {
      const char *version_text = NULL;

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    defined(HAVE_LIBRESSL)
      version_text = SSLeay_version(SSLEAY_VERSION);
#else
      version_text = OpenSSL_version(OPENSSL_VERSION);
#endif /* prior to OpenSSL-1.1.x */

      pr_log_pri(PR_LOG_WARNING, MOD_TLS_VERSION
        ": compiled using OpenSSL version '%s' headers, but linked to "
        "OpenSSL version '%s' library", OPENSSL_VERSION_TEXT, version_text);
      tls_log("compiled using OpenSSL version '%s' headers, but linked to "
        "OpenSSL version '%s' library", OPENSSL_VERSION_TEXT, version_text);
    }
  }

  pr_log_debug(DEBUG2, MOD_TLS_VERSION ": using " OPENSSL_VERSION_TEXT);

#if defined(PR_SHARED_MODULE)
  pr_event_register(&tls_module, "core.module-unload", tls_mod_unload_ev, NULL);
#endif /* PR_SHARED_MODULE */
  pr_event_register(&tls_module, "core.postparse", tls_postparse_ev, NULL);
  pr_event_register(&tls_module, "core.restart", tls_restart_ev, NULL);
  pr_event_register(&tls_module, "core.shutdown", tls_shutdown_ev, NULL);

#if OPENSSL_VERSION_NUMBER < 0x10100000L
  OPENSSL_config(NULL);

  /* It looks like calling OpenSSL_add_all_algorithms() is necessary for
   * handling some algorithms (e.g. PKCS12 files) which are NOT added by
   * just calling SSL_library_init().
   */
  OpenSSL_add_all_algorithms();

  SSL_load_error_strings();
  SSL_library_init();
  ERR_load_crypto_strings();
#endif /* prior to OpenSSL-1.1.x */

#ifdef PR_USE_CTRLS
  if (pr_ctrls_register(&tls_module, "tls", "query/tune mod_tls settings",
      tls_handle_tls) < 0) {
    pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
      ": error registering 'tls' control: %s", strerror(errno));

  } else {
    register unsigned int i;

    tls_act_pool = make_sub_pool(permanent_pool);
    pr_pool_tag(tls_act_pool, "TLS Controls Pool");

    for (i = 0; tls_acttab[i].act_action; i++) {
      tls_acttab[i].act_acl = palloc(tls_act_pool, sizeof(ctrls_acl_t));
      pr_ctrls_init_acl(tls_acttab[i].act_acl);
    }
  }
#endif /* PR_USE_CTRLS */

  return 0;
}

static int tls_sess_init(void) {
  int res = 0;
  unsigned char *tmp = NULL;
  config_rec *c = NULL;

#if defined(TLS_USE_SESSION_TICKETS)
  lock_ticket_keys();
#endif /* TLS_USE_SESSION_TICKETS */

  pr_event_register(&tls_module, "core.session-reinit", tls_sess_reinit_ev,
    NULL);

  /* First, check to see whether mod_tls is even enabled. */
  tmp = get_param_ptr(main_server->conf, "TLSEngine", FALSE);
  if (tmp != NULL &&
      *tmp == TRUE) {
    tls_engine = TRUE;
  }

  if (tls_engine == FALSE) {
    /* If we have no ServerAlias vhosts at all, then it is OK to clean up
     * all of the TLS/OpenSSL-related code from this process.  Otherwise,
     * a client MIGHT send a HOST command for a TLS-enabled vhost; if we
     * were to always cleanup OpenSSL here, that HOST command would inevitably
     * lead to a problem.
     */

    res = pr_namebind_count(main_server);
    if (res == 0) {
      /* No need for this module's control channel NetIO handlers anymore. */
      pr_unregister_netio(PR_NETIO_STRM_CTRL);

      /* No need for all the OpenSSL stuff in this process space, either. */
      tls_cleanup(TLS_CLEANUP_FL_SESS_INIT);
      tls_scrub_pkeys();
    }

    return 0;
  }

  /* Open the TLSLog, if configured */
  res = tls_openlog();
  if (res < 0) {
    if (res == -1) {
      pr_log_pri(PR_LOG_NOTICE, MOD_TLS_VERSION
        ": notice: unable to open TLSLog: %s", strerror(errno));

    } else if (res == PR_LOG_WRITABLE_DIR) {
      pr_log_pri(PR_LOG_WARNING, MOD_TLS_VERSION
        ": notice: unable to open TLSLog: parent directory is world-writable");

    } else if (res == PR_LOG_SYMLINK) {
      pr_log_pri(PR_LOG_WARNING, MOD_TLS_VERSION
        ": notice: unable to open TLSLog: cannot log to a symbolic link");
    }
  }

  tls_lookup_all(main_server);

  /* Check to see if a passphrase has been entered for this server. */
  tls_pkey = tls_lookup_pkey(main_server, TRUE, FALSE);

  if (tls_ctx_set_all(main_server, ssl_ctx) < 0) {
    tls_log("%s", "error initializing OpenSSL context for this session");
    return -1;
  }

#if !defined(OPENSSL_NO_TLSEXT) && defined(TLSEXT_MAXLEN_host_name)
  SSL_CTX_set_tlsext_servername_callback(ssl_ctx, tls_sni_cb);
  SSL_CTX_set_tlsext_servername_arg(ssl_ctx, NULL);
#endif /* !OPENSSL_NO_TLSEXT */

#if defined(PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK)
  /* Generate random ticket appdata that will uniquely (hopefully) identify
   * this particular FTP session.
   *
   * Note that we only want to do this once, so that the identifying data
   * does not change across SNI/HOST.
   */
  if (tls_ctrl_ticket_appdatasz == 0) {
    tls_ctrl_ticket_appdatasz = tls_data_ticket_appdatasz = 32;

    if (tls_ctrl_ticket_appdata == NULL) {
      tls_ctrl_ticket_appdata = palloc(session.pool,
        tls_ctrl_ticket_appdatasz);
    }

    /* Allocate space for the data connection ticket appdata as well; we
     * only need one buffer for this.
     */
    if (tls_data_ticket_appdata == NULL) {
      tls_data_ticket_appdata = palloc(session.pool,
        tls_data_ticket_appdatasz);
    }

    if (RAND_bytes((unsigned char *) tls_ctrl_ticket_appdata,
        tls_ctrl_ticket_appdatasz) != 1) {
      tls_log("error generating %lu bytes of random ticket appdata: %s",
        (unsigned long) tls_ctrl_ticket_appdatasz, tls_get_errors());
      tls_ctrl_ticket_appdata_len = 0;

    } else {
      tls_ctrl_ticket_appdata_len = tls_ctrl_ticket_appdatasz;
    }
  }
#endif /* PR_USE_OPENSSL_SSL_SESSION_TICKET_CALLBACK */

  /* We need to check for FIPS mode in the child process as well, in order
   * to re-seed the FIPS PRNG for this process ID.  Annoying, isn't it?
   */
  if (ServerType == SERVER_STANDALONE) {
    if (tls_set_fips() < 0) {
      return -1;
    }
  }

#if OPENSSL_VERSION_NUMBER > 0x000907000L
  /* Handle any requested crypto accelerators/drivers. */
  c = find_config(main_server->conf, CONF_PARAM, "TLSCryptoDevice", FALSE);
  if (c != NULL) {
# if defined(PR_USE_OPENSSL_ENGINE)
    tls_crypto_device = (const char *) c->argv[0];

    if (strcasecmp(tls_crypto_device, "ALL") == 0) {
      /* Load all ENGINE implementations bundled with OpenSSL. */
      ENGINE_load_builtin_engines();
      ENGINE_register_all_complete();
#if OPENSSL_VERSION_NUMBER < 0x10100000L
      OPENSSL_config(NULL);
#endif /* prior to OpenSSL-1.1.x */

      tls_log("%s", "enabled all builtin crypto devices");

    } else {
      ENGINE *e;

      /* Load all ENGINE implementations bundled with OpenSSL. */
      ENGINE_load_builtin_engines();
#if OPENSSL_VERSION_NUMBER < 0x10100000L
      OPENSSL_config(NULL);
#endif /* prior to OpenSSL-1.1.x */

      e = ENGINE_by_id(tls_crypto_device);
      if (e != NULL) {
        if (ENGINE_init(e)) {
          if (ENGINE_set_default(e, ENGINE_METHOD_ALL)) {
            ENGINE_finish(e);
            ENGINE_free(e);

            tls_log("using TLSCryptoDevice '%s'", tls_crypto_device);

          } else {
            /* The requested driver could not be used as the default for
             * some odd reason.
             */
            tls_log("unable to register TLSCryptoDevice '%s' as the "
              "default: %s", tls_crypto_device, tls_get_errors());

            ENGINE_finish(e);
            ENGINE_free(e);
            e = NULL;
            tls_crypto_device = NULL;
          }

        } else {
          /* The requested driver could not be initialized. */
          tls_log("unable to initialize TLSCryptoDevice '%s': %s",
            tls_crypto_device, tls_get_errors());

          ENGINE_free(e);
          e = NULL;
          tls_crypto_device = NULL;
        }

      } else {
        /* The requested driver is not available. */
        tls_log("TLSCryptoDevice '%s' is not available", tls_crypto_device);
        tls_crypto_device = NULL;
      }
    }
# else
    tls_log("%s", "TLSCryptoDevice not supported by OpenSSL");
# endif /* PR_USE_OPENSSL_ENGINE */
  }
#endif

  /* Install our data channel NetIO handlers. */
  tls_netio_install_data();

  pr_event_register(&tls_module, "core.exit", tls_exit_ev, NULL);

  /* There are several timeouts which can cause the client to be disconnected;
   * register a listener for them which can politely/cleanly shut the SSL/TLS
   * session down before the connection is closed.
   */
  pr_event_register(&tls_module, "core.timeout-idle", tls_timeout_ev, NULL);
  pr_event_register(&tls_module, "core.timeout-login", tls_timeout_ev, NULL);
  pr_event_register(&tls_module, "core.timeout-no-transfer", tls_timeout_ev,
    NULL);
  pr_event_register(&tls_module, "core.timeout-session", tls_timeout_ev, NULL);
  pr_event_register(&tls_module, "core.timeout-stalled", tls_timeout_ev, NULL);

  /* Add the additional features implemented by this module into the
   * list, to be displayed in response to a FEAT command.
   */
  pr_feat_add("AUTH TLS");
  pr_feat_add("CCC");
  pr_feat_add("PBSZ");
  pr_feat_add("PROT");
  pr_feat_add("SSCN");

  /* Add the commands handled by this module to the HELP list. */
  pr_help_add(C_AUTH, _("<sp> base64-data"), TRUE);
  pr_help_add(C_PBSZ, _("<sp> protection buffer size"), TRUE);
  pr_help_add(C_PROT, _("<sp> protection code"), TRUE);

  if (tls_opts & TLS_OPT_USE_IMPLICIT_SSL) {
    uint64_t start_ms = 0;

    tls_log("%s", "TLSOption UseImplicitSSL in effect, starting SSL/TLS "
      "handshake");

    if (pr_trace_get_level(timing_channel) > 0) {
      pr_gettimeofday_millis(&start_ms);
    }

    if (tls_accept(session.c, FALSE) < 0) {
      tls_log("%s", "implicit SSL/TLS negotiation failed on control channel");

      /* Rather than returning an error to the init callback, we disconnect
       * the session ourselves here.  Makes for slightly nicer logging.
       */
      pr_response_send_async(R_421, _("TLS handshake failed"));
      pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_CLIENT_EOF,
        "Failed TLS handshake");
    }

    tls_flags |= TLS_SESS_ON_CTRL;

    if (tls_required_on_data != -1) {
      tls_flags |= TLS_SESS_NEED_DATA_PROT;
    }

    if (pr_trace_get_level(timing_channel) >= 4) {
      unsigned long elapsed_ms;
      uint64_t finish_ms;

      pr_gettimeofday_millis(&finish_ms);

      elapsed_ms = (unsigned long) (finish_ms - session.connect_time_ms);
      pr_trace_msg(timing_channel, 4,
        "Time before TLS ctrl handshake: %lu ms", elapsed_ms);

      elapsed_ms = (unsigned long) (finish_ms - start_ms);
      pr_trace_msg(timing_channel, 4,
        "TLS ctrl handshake duration: %lu ms", elapsed_ms);
    }

    pr_session_set_protocol("ftps");
    session.rfc2228_mech = "TLS";
  }

  return 0;
}

#if defined(PR_USE_CTRLS)
static ctrls_acttab_t tls_acttab[] = {
  { "clear", NULL, NULL, NULL },
  { "info", NULL, NULL, NULL },
  { "ocspcache", NULL, NULL, NULL },
  { "sesscache", NULL, NULL, NULL },

  { NULL, NULL, NULL, NULL }
};
#endif /* PR_USE_CTRLS */

/* Module API tables
 */

static conftable tls_conftab[] = {
  { "TLSCACertificateFile",	set_tlscacertfile,	NULL },
  { "TLSCACertificatePath",	set_tlscacertpath,	NULL },
  { "TLSCARevocationFile",      set_tlscacrlfile,       NULL },
  { "TLSCARevocationPath",      set_tlscacrlpath,       NULL },
  { "TLSCertificateChainFile",	set_tlscertchain,	NULL },
  { "TLSCipherSuite",		set_tlsciphersuite,	NULL },
  { "TLSControlsACLs",		set_tlsctrlsacls,	NULL },
  { "TLSCryptoDevice",		set_tlscryptodevice,	NULL },
  { "TLSDHParamFile",		set_tlsdhparamfile,	NULL },
  { "TLSDSACertificateFile",	set_tlsdsacertfile,	NULL },
  { "TLSDSACertificateKeyFile",	set_tlsdsakeyfile,	NULL },
  { "TLSECCertificateFile",	set_tlseccertfile,	NULL },
  { "TLSECCertificateKeyFile",	set_tlseckeyfile,	NULL },
  { "TLSECDHCurve",		set_tlsecdhcurve,	NULL },
  { "TLSEngine",		set_tlsengine,		NULL },
  { "TLSLog",			set_tlslog,		NULL },
  { "TLSMasqueradeAddress",	set_tlsmasqaddr,	NULL },
  { "TLSNextProtocol",		set_tlsnextprotocol,	NULL },
  { "TLSOptions",		set_tlsoptions,		NULL },
  { "TLSPassPhraseProvider",	set_tlspassphraseprovider, NULL },
  { "TLSPKCS12File", 		set_tlspkcs12file,	NULL },
  { "TLSPreSharedKey",		set_tlspresharedkey,	NULL },
  { "TLSProtocol",		set_tlsprotocol,	NULL },
  { "TLSRandomSeed",		set_tlsrandseed,	NULL },
  { "TLSRenegotiate",		set_tlsrenegotiate,	NULL },
  { "TLSRequired",		set_tlsrequired,	NULL },
  { "TLSRSACertificateFile",	set_tlsrsacertfile,	NULL },
  { "TLSRSACertificateKeyFile",	set_tlsrsakeyfile,	NULL },
  { "TLSServerCipherPreference",set_tlsservercipherpreference, NULL },
  { "TLSServerInfoFile",	set_tlsserverinfofile,	NULL },
  { "TLSSessionCache",		set_tlssessioncache,	NULL },
  { "TLSSessionTicketKeys",	set_tlssessionticketkeys, NULL },
  { "TLSSessionTickets",	set_tlssessiontickets,	NULL },
  { "TLSStapling",		set_tlsstapling,	NULL },
  { "TLSStaplingCache",		set_tlsstaplingcache,	NULL },
  { "TLSStaplingOptions",	set_tlsstaplingoptions,	NULL },
  { "TLSStaplingResponder",	set_tlsstaplingresponder, NULL },
  { "TLSStaplingTimeout",	set_tlsstaplingtimeout,	NULL },
  { "TLSTimeoutHandshake",	set_tlstimeouthandshake,NULL },
  { "TLSUserName",		set_tlsusername,	NULL },
  { "TLSVerifyClient",		set_tlsverifyclient,	NULL },
  { "TLSVerifyDepth",		set_tlsverifydepth,	NULL },
  { "TLSVerifyOrder",		set_tlsverifyorder,	NULL },
  { "TLSVerifyServer",		set_tlsverifyserver,	NULL },
  { NULL , NULL, NULL}
};

static cmdtable tls_cmdtab[] = {
  { PRE_CMD,	C_ANY,	G_NONE,	tls_any,	FALSE,	FALSE },
  { PRE_CMD,	C_APPE,	G_NONE,	tls_pre_xfer,	FALSE,	FALSE },
  { PRE_CMD,	C_LIST,	G_NONE,	tls_pre_xfer,	FALSE,	FALSE },
  { PRE_CMD,	C_MLSD,	G_NONE,	tls_pre_xfer,	FALSE,	FALSE },
  { PRE_CMD,	C_NLST,	G_NONE,	tls_pre_xfer,	FALSE,	FALSE },
  { PRE_CMD,	C_RETR,	G_NONE,	tls_pre_xfer,	FALSE,	FALSE },
  { PRE_CMD,	C_STOR,	G_NONE,	tls_pre_xfer,	FALSE,	FALSE },
  { PRE_CMD,	C_STOU,	G_NONE,	tls_pre_xfer,	FALSE,	FALSE },
  { CMD,	C_AUTH,	G_NONE,	tls_auth,	FALSE,	FALSE,	CL_SEC },
  { CMD,	C_CCC,	G_NONE,	tls_ccc,	TRUE,	FALSE,	CL_SEC },
  { CMD,	C_PBSZ,	G_NONE,	tls_pbsz,	FALSE,	FALSE,	CL_SEC },
  { CMD,	C_PROT,	G_NONE,	tls_prot,	FALSE,	FALSE,	CL_SEC },
  { CMD,	"SSCN",	G_NONE,	tls_sscn,	TRUE,	FALSE,	CL_SEC },
  { POST_CMD,	C_PASS,	G_NONE,	tls_post_pass,	FALSE,	FALSE },
  { POST_CMD,	C_USER,	G_NONE,	tls_post_user,	FALSE,	FALSE },
  { POST_CMD,	C_AUTH,	G_NONE,	tls_post_auth,	FALSE,	FALSE },
  { LOG_CMD,	C_AUTH,	G_NONE,	tls_log_auth,	FALSE,	FALSE },
  { LOG_CMD_ERR,C_AUTH,	G_NONE,	tls_log_auth,	FALSE,	FALSE },
  { 0,	NULL }
};

static authtable tls_authtab[] = {
  { 0, "auth",			tls_authenticate	},
  { 0, "check",			tls_auth_check		},
  { 0, "requires_pass",		tls_authenticate	},
  { 0, NULL }
};

module tls_module = {

  /* Always NULL */
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "tls",

  /* Module configuration handler table */
  tls_conftab,

  /* Module command handler table */
  tls_cmdtab,

  /* Module authentication handler table */
  tls_authtab,

  /* Module initialization */
  tls_init,

  /* Session initialization */
  tls_sess_init,

  /* Module version */
  MOD_TLS_VERSION
};
