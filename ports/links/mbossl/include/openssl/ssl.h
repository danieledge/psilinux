/* openssl/ssl.h - mbossl: the part of OpenSSL's API that Links 2.30 uses,
 * written over Mbed TLS 3.6 (mbossl.c). Not a general OpenSSL replacement:
 * only what Links calls, with OpenSSL's meanings where Links depends on them
 * (SSL_get_error after SSL_connect/SSL_read/SSL_write, non-blocking sockets).
 *
 * What is left out on purpose, so that Links' configure and its #ifdefs skip
 * it: client certificates (OPENSSL_NO_STDIO), protocol downgrades
 * (no SSL_OP_NO_TLSv1_x), RAND_*, CRYPTO_set_mem_functions, X509 objects.
 * Certificates are checked by Mbed TLS during the handshake; Links asks the
 * outcome through mbossl_verify_result() and mbossl_issuer() (the patch's
 * https.c), and decides as its ssl.certificates option says. */
#ifndef MBOSSL_SSL_H
#define MBOSSL_SSL_H

#include <stddef.h>

#define MBOSSL 1
#define OPENSSL_NO_STDIO 1
#define OPENSSL_NO_SSL3 1

typedef struct mbossl_ctx SSL_CTX;
typedef struct mbossl SSL;
typedef struct mbossl_session SSL_SESSION;
typedef struct mbossl_method { int client; } SSL_METHOD;

#define SSL_ERROR_NONE		0
#define SSL_ERROR_SSL		1
#define SSL_ERROR_WANT_READ	2
#define SSL_ERROR_WANT_WRITE	3
#define SSL_ERROR_SYSCALL	5
#define SSL_ERROR_ZERO_RETURN	6

#define SSL_OP_ALL		0x80000850UL
#define SSL_OP_NO_COMPRESSION	0x00020000UL
#define SSL_MODE_ENABLE_PARTIAL_WRITE	0x1UL
#define SSL_CTRL_SET_TLSEXT_HOSTNAME	55

#define X509_V_OK		0

#define OPENSSL_VERSION		0
#define OpenSSL_version(t)	mbossl_version()
#define OpenSSL_add_ssl_algorithms()	mbossl_library_init()
#define SSL_load_error_strings()	do { } while (0)
#define SSL_CTX_set_default_passwd_cb(ctx, cb)	do { (void)(ctx); (void)(cb); } while (0)

const SSL_METHOD *SSLv23_client_method(void);
int mbossl_library_init(void);
const char *mbossl_version(void);

SSL_CTX *SSL_CTX_new(const SSL_METHOD *m);
void SSL_CTX_free(SSL_CTX *ctx);
unsigned long SSL_CTX_set_options(SSL_CTX *ctx, unsigned long op);
unsigned long SSL_CTX_set_mode(SSL_CTX *ctx, unsigned long mode);
int SSL_CTX_set_default_verify_paths(SSL_CTX *ctx);

SSL *SSL_new(SSL_CTX *ctx);
void SSL_free(SSL *s);
unsigned long SSL_set_options(SSL *s, unsigned long op);
unsigned long SSL_set_mode(SSL *s, unsigned long mode);
int SSL_set_fd(SSL *s, int fd);
int SSL_set_tlsext_host_name(SSL *s, const char *name);
int SSL_connect(SSL *s);
int SSL_read(SSL *s, void *buf, int num);
int SSL_write(SSL *s, const void *buf, int num);
int SSL_pending(const SSL *s);
int SSL_get_error(const SSL *s, int ret);
int SSL_shutdown(SSL *s);
void SSL_set_quiet_shutdown(SSL *s, int mode);
const char *SSL_get_version(const SSL *s);
const char *SSL_get_cipher_name(const SSL *s);
int SSL_get_cipher_bits(const SSL *s, int *alg_bits);
long SSL_get_verify_result(const SSL *s);

SSL_SESSION *SSL_get1_session(SSL *s);
int SSL_set_session(SSL *s, SSL_SESSION *session);
void SSL_SESSION_free(SSL_SESSION *session);
int SSL_session_reused(const SSL *s);

unsigned long ERR_get_error(void);
unsigned long ERR_peek_error(void);
void ERR_error_string_n(unsigned long e, char *buf, size_t len);

/* mbossl's own (Links' https.c, patched) */
/* Check the server's certificate in the handshakes that follow (or not, when
   Links' ssl.certificates says accept anything: the check is skipped). */
void mbossl_set_verify(int on);
/* the Mbed TLS verification flags of the handshake (0: valid, the name too) */
unsigned mbossl_verify_result(const SSL *s);
/* the issuers of the chain, from the root down: the organisation (or common
   name) of number i, NULL past the last */
const char *mbossl_issuer(const SSL *s, int i);

#endif
