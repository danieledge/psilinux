/* mbossl.c - the part of OpenSSL's API that Links 2.30 uses, over Mbed TLS
 * 3.6 (see include/openssl/ssl.h). For PsiLinux: Links in graphics mode on
 * the Psion's X desktop.
 *
 * Certificates. The trusted roots are the PEM bundle at $SSL_CERT_FILE, or
 * else the first there is of /etc/ssl/certs/ca-certificates.crt, and
 * ca-certificates.crt beside the program or in ../share/links/ from it (the
 * Psion's \Linux\X\, the root's /usr/bin). Parsing all of it (about 140 roots)
 * would hold some 300 KB and cost the 36 MHz ARM a second or two, so it is
 * only indexed, once: each root's subject (a hash of its DER) and where its
 * PEM block is in the file, 12 bytes a root. Mbed TLS asks for the roots
 * that may have issued a certificate (the trusted-CA callback), and only
 * those few are read from the file and parsed, then let go.
 *
 * Part of PsiLinux (MIT, see LICENSE at the top of the repository). */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/asn1.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"	/* (the MBEDTLS_ERR_NET_ codes only) */
#include "mbedtls/oid.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <openssl/ssl.h>

#define N_ISSUERS	6
#define ISSUER_LEN	64

struct mbossl_ctx {
	mbedtls_ssl_config conf[2];	/* [1] checks certificates, [0] does not */
	int conf_ready[2];
};

struct mbossl {
	mbedtls_ssl_context ssl;
	int fd;
	int last;			/* Mbed TLS's last result */
	int eof;			/* the peer closed */
	int sys_errno;			/* a socket error (SSL_ERROR_SYSCALL) */
	int quiet;
	int ticket;			/* a TLS 1.3 ticket came: the session can be kept */
	int n_issuers;
	char issuer[N_ISSUERS][ISSUER_LEN];
};

struct mbossl_session {
	mbedtls_ssl_session s;
};

static const SSL_METHOD client_method = { 1 };
static int lib_ready;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static int verify_peer = 1;

/* --- the trusted roots, indexed --------------------------------------- */

struct ca_ent {
	uint32_t hash;			/* FNV-1a of the subject's DER */
	uint32_t off, len;		/* the PEM block in the file */
};

static struct ca_ent *ca_idx;
static int ca_n = -1;			/* -1: not read yet */
static char ca_path[256];

static uint32_t fnv(const unsigned char *p, size_t n)
{
	uint32_t h = 2166136261u;
	while (n--) {
		h ^= *p++;
		h *= 16777619u;
	}
	return h;
}

/* the subject (tag, length and contents) of a certificate in DER */
static int der_subject(unsigned char *p, size_t n, const unsigned char **subj, size_t *subj_len)
{
	unsigned char *end = p + n, *q;
	size_t l;
	int k;
	if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) return -1;
	end = p + l;
	if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) return -1;
	end = p + l;
	if (p < end && *p == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED | 0)) {
		if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED | 0)) return -1;
		p += l;
	}
	if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_INTEGER)) return -1;	/* serial */
	p += l;
	for (k = 0; k < 3; k++) {	/* signature algorithm, issuer, validity */
		if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) return -1;
		p += l;
	}
	q = p;
	if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) return -1;
	*subj = q;
	*subj_len = (size_t)(p + l - q);
	return 0;
}

static const char pem_begin[] = "-----BEGIN CERTIFICATE-----";
static const char pem_end[] = "-----END CERTIFICATE-----";

static void ca_index(void)
{
	FILE *f;
	long size;
	char *text = NULL, *s;
	unsigned char *der = NULL;
	int cap = 0;
	ca_n = 0;
	if (!ca_path[0]) return;
	if (!(f = fopen(ca_path, "rb"))) return;
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)
	    || !(text = malloc((size_t)size + 1)) || fread(text, 1, (size_t)size, f) != (size_t)size) {
		fclose(f);
		free(text);
		return;
	}
	fclose(f);
	text[size] = 0;
	for (s = text; (s = strstr(s, pem_begin)); ) {
		char *b = s + sizeof pem_begin - 1, *e = strstr(b, pem_end);
		size_t dl;
		const unsigned char *subj;
		size_t subj_len;
		if (!e) break;
		der = realloc(der, (size_t)(e - b));
		if (der && !mbedtls_base64_decode(der, (size_t)(e - b), &dl, (unsigned char *)b, (size_t)(e - b))
		    && !der_subject(der, dl, &subj, &subj_len)) {
			if (ca_n == cap) {
				struct ca_ent *n = realloc(ca_idx, (cap = cap ? cap * 2 : 64) * sizeof *ca_idx);
				if (!n) break;
				ca_idx = n;
			}
			ca_idx[ca_n].hash = fnv(subj, subj_len);
			ca_idx[ca_n].off = (uint32_t)(s - text);
			ca_idx[ca_n].len = (uint32_t)(e + sizeof pem_end - 1 - s);
			ca_n++;
		}
		s = e;
	}
	free(der);
	free(text);
}

/* Mbed TLS's trusted-CA callback: the roots whose subject is child's issuer */
static int ca_cb(void *unused, mbedtls_x509_crt const *child, mbedtls_x509_crt **candidates)
{
	uint32_t h;
	int i;
	FILE *f = NULL;
	mbedtls_x509_crt *out = NULL;
	(void)unused;
	*candidates = NULL;
	if (ca_n < 0) ca_index();
	h = fnv(child->issuer_raw.p, child->issuer_raw.len);
	for (i = 0; i < ca_n; i++) {
		char *pem;
		mbedtls_x509_crt one;
		if (ca_idx[i].hash != h) continue;
		if (!f && !(f = fopen(ca_path, "rb"))) break;
		if (!(pem = malloc(ca_idx[i].len + 1))) break;
		if (fseek(f, (long)ca_idx[i].off, SEEK_SET) || fread(pem, 1, ca_idx[i].len, f) != ca_idx[i].len) {
			free(pem);
			continue;
		}
		pem[ca_idx[i].len] = 0;
		mbedtls_x509_crt_init(&one);
		if (!mbedtls_x509_crt_parse(&one, (unsigned char *)pem, ca_idx[i].len + 1)
		    && one.subject_raw.len == child->issuer_raw.len
		    && !memcmp(one.subject_raw.p, child->issuer_raw.p, one.subject_raw.len)) {
			if (!out && (out = calloc(1, sizeof *out))) mbedtls_x509_crt_init(out);
			if (out) mbedtls_x509_crt_parse_der(out, one.raw.p, one.raw.len);
		}
		mbedtls_x509_crt_free(&one);
		free(pem);
	}
	if (f) fclose(f);
	*candidates = out;
	return 0;
}

/* --- the library ---------------------------------------------------------- */

int mbossl_library_init(void)
{
	if (lib_ready) return 1;
	if (psa_crypto_init() != PSA_SUCCESS) return 0;
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"links", 5)) return 0;
	lib_ready = 1;
	return 1;
}

const char *mbossl_version(void)
{
	return MBEDTLS_VERSION_STRING_FULL " (mbossl)";
}

const SSL_METHOD *SSLv23_client_method(void)
{
	return &client_method;
}

void mbossl_set_verify(int on)
{
	verify_peer = !!on;
}

/* --- contexts ------------------------------------------------------------- */

SSL_CTX *SSL_CTX_new(const SSL_METHOD *m)
{
	SSL_CTX *ctx;
	if (!m || !mbossl_library_init()) return NULL;
	if (!(ctx = calloc(1, sizeof *ctx))) return NULL;
	return ctx;
}

void SSL_CTX_free(SSL_CTX *ctx)
{
	int i;
	if (!ctx) return;
	for (i = 0; i < 2; i++)
		if (ctx->conf_ready[i]) mbedtls_ssl_config_free(&ctx->conf[i]);
	free(ctx);
}

unsigned long SSL_CTX_set_options(SSL_CTX *ctx, unsigned long op) { (void)ctx; return op; }
unsigned long SSL_CTX_set_mode(SSL_CTX *ctx, unsigned long mode) { (void)ctx; return mode; }

/* the bundle: $SSL_CERT_FILE, the system's, or one that came with the program */
static const char *find_bundle(char *buf, size_t size)
{
	const char *p = getenv("SSL_CERT_FILE");
	static const char *const rel[] = { "/ca-certificates.crt", "/../share/links/ca-certificates.crt" };
	ssize_t n;
	char *slash;
	size_t i;
	if (p && *p) return p;
	p = "/etc/ssl/certs/ca-certificates.crt";
	if (!access(p, R_OK)) return p;
	if ((n = readlink("/proc/self/exe", buf, size - 1)) <= 0) return p;
	buf[n] = 0;
	if (!(slash = strrchr(buf, '/'))) return p;
	for (i = 0; i < sizeof rel / sizeof *rel; i++) {
		if ((size_t)(slash - buf) + strlen(rel[i]) >= size) continue;
		strcpy(slash, rel[i]);
		if (!access(buf, R_OK)) return buf;
	}
	return p;
}

int SSL_CTX_set_default_verify_paths(SSL_CTX *ctx)
{
	char buf[256];
	const char *p = find_bundle(buf, sizeof buf);
	(void)ctx;
	if (strlen(p) >= sizeof ca_path) return 0;
	if (strcmp(p, ca_path)) {
		strcpy(ca_path, p);
		free(ca_idx);
		ca_idx = NULL;
		ca_n = -1;
	}
	return 1;
}

static mbedtls_ssl_config *get_conf(SSL_CTX *ctx, int verify)
{
	mbedtls_ssl_config *c = &ctx->conf[verify];
	if (ctx->conf_ready[verify]) return c;
	mbedtls_ssl_config_init(c);
	if (mbedtls_ssl_config_defaults(c, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) {
		mbedtls_ssl_config_free(c);
		return NULL;
	}
	mbedtls_ssl_conf_rng(c, mbedtls_ctr_drbg_random, &drbg);
	/* (OPTIONAL: the handshake goes on, and Links decides afterwards) */
	mbedtls_ssl_conf_authmode(c, verify ? MBEDTLS_SSL_VERIFY_OPTIONAL : MBEDTLS_SSL_VERIFY_NONE);
	if (verify) mbedtls_ssl_conf_ca_cb(c, ca_cb, NULL);
#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_PROTO_TLS1_3)
	mbedtls_ssl_conf_tls13_enable_signal_new_session_tickets(c, MBEDTLS_SSL_TLS1_3_SIGNAL_NEW_SESSION_TICKETS_ENABLED);
#endif
	ctx->conf_ready[verify] = 1;
	return c;
}

/* --- connections ---------------------------------------------------------- */

/* the chain's issuers, as Mbed TLS checks it (from the root down) */
static int verify_cb(void *p, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
	SSL *s = p;
	const mbedtls_x509_name *n, *cn = NULL, *o = NULL;
	(void)depth;
	(void)flags;
	for (n = &crt->issuer; n; n = n->next) {
		if (!MBEDTLS_OID_CMP(MBEDTLS_OID_AT_ORGANIZATION, &n->oid)) { if (!o) o = n; }
		else if (!MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid)) { if (!cn) cn = n; }
	}
	if (!o) o = cn;
	if (o && s->n_issuers < N_ISSUERS) {
		size_t l = o->val.len < ISSUER_LEN - 1 ? o->val.len : ISSUER_LEN - 1;
		memcpy(s->issuer[s->n_issuers], o->val.p, l);
		s->issuer[s->n_issuers][l] = 0;
		s->n_issuers++;
	}
	return 0;
}

SSL *SSL_new(SSL_CTX *ctx)
{
	SSL *s;
	mbedtls_ssl_config *c;
	if (!ctx || !(c = get_conf(ctx, verify_peer))) return NULL;
	if (!(s = calloc(1, sizeof *s))) return NULL;
	mbedtls_ssl_init(&s->ssl);
	if (mbedtls_ssl_setup(&s->ssl, c)) {
		mbedtls_ssl_free(&s->ssl);
		free(s);
		return NULL;
	}
	if (verify_peer) mbedtls_ssl_set_verify(&s->ssl, verify_cb, s);
	s->fd = -1;
	return s;
}

void SSL_free(SSL *s)
{
	if (!s) return;
	mbedtls_ssl_free(&s->ssl);
	free(s);
}

unsigned long SSL_set_options(SSL *s, unsigned long op) { (void)s; return op; }
unsigned long SSL_set_mode(SSL *s, unsigned long mode) { (void)s; return mode; }

static int bio_send(void *p, const unsigned char *buf, size_t len)
{
	SSL *s = p;
	ssize_t n = send(s->fd, buf, len, MSG_NOSIGNAL);
	if (n >= 0) return (int)n;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
	s->sys_errno = errno;
	return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *p, unsigned char *buf, size_t len)
{
	SSL *s = p;
	ssize_t n = recv(s->fd, buf, len, 0);
	if (n >= 0) return (int)n;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
	s->sys_errno = errno;
	return MBEDTLS_ERR_NET_RECV_FAILED;
}

int SSL_set_fd(SSL *s, int fd)
{
	s->fd = fd;
	mbedtls_ssl_set_bio(&s->ssl, s, bio_send, bio_recv, NULL);
	return 1;
}

int SSL_set_tlsext_host_name(SSL *s, const char *name)
{
	/* (SNI, and the name the certificate must carry) */
	return !mbedtls_ssl_set_hostname(&s->ssl, name);
}

static int result(SSL *s, int r)
{
	s->last = r;
	if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) {
		s->eof = 1;
		return 0;
	}
	if (r != MBEDTLS_ERR_NET_SEND_FAILED && r != MBEDTLS_ERR_NET_RECV_FAILED) s->sys_errno = 0;
	return -1;
}

int SSL_connect(SSL *s)
{
	int r;
	while ((r = mbedtls_ssl_handshake(&s->ssl)) == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
		s->ticket = 1;
	if (!r) {
		s->last = 0;
		return 1;
	}
	if (result(s, r) == 0) s->eof = 0, s->last = MBEDTLS_ERR_SSL_CONN_EOF;	/* (a handshake cut short is an error) */
	return -1;
}

int SSL_read(SSL *s, void *buf, int num)
{
	int r;
	if (num <= 0) return 0;
	while ((r = mbedtls_ssl_read(&s->ssl, buf, (size_t)num)) == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
		s->ticket = 1;
	if (r > 0) {
		s->last = 0;
		return r;
	}
	if (r == 0) {
		s->eof = 1;
		s->last = 0;
		return 0;
	}
	return result(s, r);
}

int SSL_write(SSL *s, const void *buf, int num)
{
	int r;
	if (num <= 0) return 0;
	while ((r = mbedtls_ssl_write(&s->ssl, buf, (size_t)num)) == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
		s->ticket = 1;
	if (r > 0) {
		s->last = 0;
		return r;
	}
	return result(s, r);
}

int SSL_pending(const SSL *s)
{
	return (int)mbedtls_ssl_get_bytes_avail(&s->ssl);
}

int SSL_get_error(const SSL *s, int ret)
{
	if (ret > 0) return SSL_ERROR_NONE;
	if (s->last == MBEDTLS_ERR_SSL_WANT_READ) return SSL_ERROR_WANT_READ;
	if (s->last == MBEDTLS_ERR_SSL_WANT_WRITE) return SSL_ERROR_WANT_WRITE;
	if (s->eof) return SSL_ERROR_ZERO_RETURN;
	if (s->sys_errno) {
		errno = s->sys_errno;
		return SSL_ERROR_SYSCALL;
	}
	return SSL_ERROR_SSL;
}

int SSL_shutdown(SSL *s)
{
	/* (Links asks for a quiet shutdown: no close_notify, the socket just goes) */
	if (!s->quiet && s->fd >= 0) mbedtls_ssl_close_notify(&s->ssl);
	return 1;
}

void SSL_set_quiet_shutdown(SSL *s, int mode)
{
	s->quiet = mode;
}

const char *SSL_get_version(const SSL *s)
{
	return mbedtls_ssl_get_version(&s->ssl);
}

const char *SSL_get_cipher_name(const SSL *s)
{
	return mbedtls_ssl_get_ciphersuite(&s->ssl);
}

int SSL_get_cipher_bits(const SSL *s, int *alg_bits)
{
	const char *c = mbedtls_ssl_get_ciphersuite(&s->ssl);
	int bits = 128;
	if (c && (strstr(c, "AES-256") || strstr(c, "CHACHA20"))) bits = 256;
	if (alg_bits) *alg_bits = bits;
	return bits;
}

long SSL_get_verify_result(const SSL *s)
{
	return mbedtls_ssl_get_verify_result(&s->ssl) ? 1 : X509_V_OK;
}

unsigned mbossl_verify_result(const SSL *s)
{
	return (unsigned)mbedtls_ssl_get_verify_result(&s->ssl);
}

const char *mbossl_issuer(const SSL *s, int i)
{
	return i >= 0 && i < s->n_issuers ? s->issuer[i] : NULL;
}

/* --- sessions (resumed: no certificate or key exchange to pay for again) -- */

SSL_SESSION *SSL_get1_session(SSL *s)
{
	SSL_SESSION *ss;
	if (!mbedtls_ssl_is_handshake_over(&s->ssl)) return NULL;
	/* (TLS 1.3: only once a ticket has come, after the handshake) */
	if (mbedtls_ssl_get_version_number(&s->ssl) == MBEDTLS_SSL_VERSION_TLS1_3 && !s->ticket) return NULL;
	if (!(ss = calloc(1, sizeof *ss))) return NULL;
	mbedtls_ssl_session_init(&ss->s);
	if (mbedtls_ssl_get_session(&s->ssl, &ss->s)) {
		mbedtls_ssl_session_free(&ss->s);
		free(ss);
		return NULL;
	}
	return ss;
}

int SSL_set_session(SSL *s, SSL_SESSION *ss)
{
	return ss && !mbedtls_ssl_set_session(&s->ssl, &ss->s);
}

void SSL_SESSION_free(SSL_SESSION *ss)
{
	if (!ss) return;
	mbedtls_ssl_session_free(&ss->s);
	free(ss);
}

int SSL_session_reused(const SSL *s)
{
	(void)s;
	return 0;
}

/* --- errors: Links only clears and logs them ------------------------------ */

unsigned long ERR_get_error(void) { return 0; }
unsigned long ERR_peek_error(void) { return 0; }

void ERR_error_string_n(unsigned long e, char *buf, size_t len)
{
	if (len) snprintf(buf, len, "mbossl error %lu", e);
}
