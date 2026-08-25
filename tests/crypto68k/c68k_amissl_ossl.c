/*
 * AmiNetXDuo, the AmiSSL side of tests/crypto68k/c68k_amissl.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>

#include <proto/amisslmaster.h>
#include <proto/amissl.h>

#include <amissl/amissl.h>

#include <string.h>

#include "c68k_amissl.h"


/* The three library bases the inline macros dereference.  Declared here and
   nowhere else; the orchestrator never touches AmiSSL. */
struct Library *AmiSSLMasterBase;
struct Library *AmiSSLBase;
struct Library *AmiSSLExtBase;

/* AmiSSL's inline stubs reach for this when a socket or C-library call inside
   the library fails.  No networking happens here, but the tag wants an address
   that outlives the call. */
static int      a_errno;

static char     a_version[128];
static char     a_libid[128];

int a_ossl_open_master(char *err, unsigned long err_len)
{

    err[0] = '\0';

    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library",
                                   AMISSLMASTER_MIN_VERSION);
    if (AmiSSLMasterBase == NULL)
    {
        strncpy(err, "LIBS:amisslmaster.library would not open", err_len - 1);
        err[err_len - 1] = '\0';
        return(1);
    }

    return(0);
}


int a_ossl_open_ssl(char *err, unsigned long err_len)
{

struct TagItem  tags[5];


    err[0] = '\0';

    tags[0].ti_Tag = AmiSSL_UsesOpenSSLStructs; tags[0].ti_Data = (ULONG)FALSE;
    tags[1].ti_Tag = AmiSSL_InitAmiSSL;         tags[1].ti_Data = (ULONG)FALSE;
    tags[2].ti_Tag = AmiSSL_GetAmiSSLBase;      tags[2].ti_Data = (ULONG)&AmiSSLBase;
    tags[3].ti_Tag = TAG_DONE;                  tags[3].ti_Data = 0UL;

    if (OpenAmiSSLTagList(AMISSL_CURRENT_VERSION, tags) != 0)
    {
        strncpy(err, "OpenAmiSSLTagList failed, is LIBS:AmiSSL/ populated?",
                err_len - 1);
        err[err_len - 1] = '\0';
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        return(1);
    }

    return(0);
}


int a_ossl_init_ssl(char *err, unsigned long err_len)
{

const char     *v;
struct TagItem  tags[5];


    err[0] = '\0';

    tags[0].ti_Tag = AmiSSL_GetAmiSSLBase;    tags[0].ti_Data = (ULONG)&AmiSSLBase;
    tags[1].ti_Tag = AmiSSL_GetAmiSSLExtBase; tags[1].ti_Data = (ULONG)&AmiSSLExtBase;
    tags[2].ti_Tag = AmiSSL_ErrNoPtr;         tags[2].ti_Data = (ULONG)&a_errno;
    tags[3].ti_Tag = TAG_DONE;                tags[3].ti_Data = 0UL;

    if (InitAmiSSLA(tags) != 0)
    {
        strncpy(err, "InitAmiSSLA failed", err_len - 1);
        err[err_len - 1] = '\0';
        return(1);
    }

    v = (const char *)OpenSSL_version(OPENSSL_VERSION);
    strncpy(a_version, (v != NULL) ? v : "?", sizeof(a_version) - 1);
    a_version[sizeof(a_version) - 1] = '\0';

    if (AmiSSLBase -> lib_IdString != NULL)
    {
        strncpy(a_libid, (const char *)AmiSSLBase -> lib_IdString,
                sizeof(a_libid) - 1);
        a_libid[sizeof(a_libid) - 1] = '\0';
    }
    else
    {
        strcpy(a_libid, "?");
    }

    return(0);
}


void a_ossl_close(void)
{

    if (AmiSSLBase != NULL)
    {
        CloseAmiSSL();
        AmiSSLBase    = NULL;
        AmiSSLExtBase = NULL;
    }

    if (AmiSSLMasterBase != NULL)
    {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
}


void a_ossl_touch(void)
{

BIGNUM *b;


    b = BN_new();
    if (b != NULL)
    {
        BN_free(b);
    }
}


const char *a_ossl_version(void)
{

    return(a_version);
}


const char *a_ossl_library_id(void)
{

    return(a_libid);
}

static RSA     *a_rsa;

int a_ossl_rsa_setup(const unsigned char *n, const unsigned char *e,
                     const unsigned char *d, const unsigned char *p,
                     const unsigned char *q, int blinding)
{

BIGNUM *bn_n = NULL, *bn_e = NULL, *bn_d = NULL, *bn_p = NULL, *bn_q = NULL;
BIGNUM *dmp1 = NULL, *dmq1 = NULL, *iqmp = NULL, *t = NULL;
BN_CTX *ctx  = NULL;
int     ok   = 1;


    a_ossl_rsa_free();

    a_rsa = RSA_new();
    ctx   = BN_CTX_new();
    if ((a_rsa == NULL) || (ctx == NULL))
    {
        goto out;
    }

    bn_n = BN_bin2bn((unsigned char *)n, 256, NULL);
    bn_e = BN_bin2bn((unsigned char *)e, 3,   NULL);
    bn_d = BN_bin2bn((unsigned char *)d, 256, NULL);
    bn_p = BN_bin2bn((unsigned char *)p, 128, NULL);
    bn_q = BN_bin2bn((unsigned char *)q, 128, NULL);
    if ((bn_n == NULL) || (bn_e == NULL) || (bn_d == NULL) ||
        (bn_p == NULL) || (bn_q == NULL))
    {
        goto out;
    }

    t    = BN_new();
    dmp1 = BN_new();
    dmq1 = BN_new();
    iqmp = BN_new();
    if ((t == NULL) || (dmp1 == NULL) || (dmq1 == NULL) || (iqmp == NULL))
    {
        goto out;
    }

    if (BN_copy(t, bn_p) == NULL)       { goto out; }
    if (!BN_sub_word(t, 1))             { goto out; }
    if (!BN_mod(dmp1, bn_d, t, ctx))    { goto out; }

    if (BN_copy(t, bn_q) == NULL)       { goto out; }
    if (!BN_sub_word(t, 1))             { goto out; }
    if (!BN_mod(dmq1, bn_d, t, ctx))    { goto out; }

    if (BN_mod_inverse(iqmp, bn_q, bn_p, ctx) == NULL) { goto out; }

    if (!RSA_set0_key(a_rsa, bn_n, bn_e, bn_d))        { goto out; }
    bn_n = bn_e = bn_d = NULL;
    if (!RSA_set0_factors(a_rsa, bn_p, bn_q))          { goto out; }
    bn_p = bn_q = NULL;
    if (!RSA_set0_crt_params(a_rsa, dmp1, dmq1, iqmp)) { goto out; }
    dmp1 = dmq1 = iqmp = NULL;

    if (!blinding)
    {
        RSA_blinding_off(a_rsa);        /* returns void */
    }

    ok = 0;

out:
    BN_free(bn_n); BN_free(bn_e); BN_free(bn_d);
    BN_free(bn_p); BN_free(bn_q);
    BN_free(dmp1); BN_free(dmq1); BN_free(iqmp);
    BN_free(t);
    BN_CTX_free(ctx);

    if (ok != 0)
    {
        a_ossl_rsa_free();
    }

    return(ok);
}


void a_ossl_rsa_free(void)
{

    if (a_rsa != NULL)
    {
        RSA_free(a_rsa);
        a_rsa = NULL;
    }
}


int a_ossl_rsa_public(const unsigned char *in, unsigned char *out)
{

    if (a_rsa == NULL)
    {
        return(1);
    }

    return((RSA_public_encrypt(256, (unsigned char *)in, out, a_rsa,
                               RSA_NO_PADDING) == 256) ? 0 : 1);
}


int a_ossl_rsa_private(const unsigned char *in, unsigned char *out)
{

    if (a_rsa == NULL)
    {
        return(1);
    }

    return((RSA_private_decrypt(256, (unsigned char *)in, out, a_rsa,
                                RSA_NO_PADDING) == 256) ? 0 : 1);
}

static BIGNUM  *a_mx_n;
static BIGNUM  *a_mx_x;
static BIGNUM  *a_mx_e;
static BIGNUM  *a_mx_r;
static BN_CTX  *a_mx_ctx;
static BN_MONT_CTX *a_mx_mont;

int a_ossl_modexp_setup(const unsigned char *n, const unsigned char *x)
{

    a_ossl_modexp_free();

    a_mx_ctx  = BN_CTX_new();
    a_mx_n    = BN_bin2bn((unsigned char *)n, 256, NULL);
    a_mx_x    = BN_bin2bn((unsigned char *)x, 256, NULL);
    a_mx_e    = BN_new();
    a_mx_r    = BN_new();
    a_mx_mont = BN_MONT_CTX_new();

    if ((a_mx_ctx == NULL) || (a_mx_n == NULL) || (a_mx_x == NULL) ||
        (a_mx_e == NULL) || (a_mx_r == NULL) || (a_mx_mont == NULL))
    {
        a_ossl_modexp_free();
        return(1);
    }

    if (!BN_set_word(a_mx_e, 65537UL))
    {
        a_ossl_modexp_free();
        return(1);
    }

    if (!BN_MONT_CTX_set(a_mx_mont, a_mx_n, a_mx_ctx))
    {
        a_ossl_modexp_free();
        return(1);
    }

    return(0);
}


void a_ossl_modexp_free(void)
{

    BN_free(a_mx_n);    a_mx_n = NULL;
    BN_free(a_mx_x);    a_mx_x = NULL;
    BN_free(a_mx_e);    a_mx_e = NULL;
    BN_free(a_mx_r);    a_mx_r = NULL;
    BN_MONT_CTX_free(a_mx_mont); a_mx_mont = NULL;
    BN_CTX_free(a_mx_ctx);       a_mx_ctx = NULL;
}


int a_ossl_modexp_public(int cached, unsigned char *out)
{

    if (a_mx_n == NULL)
    {
        return(1);
    }

    if (!BN_mod_exp_mont(a_mx_r, a_mx_x, a_mx_e, a_mx_n, a_mx_ctx,
                         cached ? a_mx_mont : NULL))
    {
        return(1);
    }

    return((BN_bn2binpad(a_mx_r, out, 256) == 256) ? 0 : 1);
}


int a_ossl_modexp_private(int cached, const unsigned char *d,
                          unsigned char *out)
{

BIGNUM *e;
int     rc;


    if (a_mx_n == NULL)
    {
        return(1);
    }

    e = BN_bin2bn((unsigned char *)d, 256, NULL);
    if (e == NULL)
    {
        return(1);
    }

    BN_set_flags(e, BN_FLG_CONSTTIME);

    rc = BN_mod_exp_mont(a_mx_r, a_mx_x, e, a_mx_n, a_mx_ctx,
                         cached ? a_mx_mont : NULL) ? 0 : 1;

    BN_free(e);

    if (rc != 0)
    {
        return(rc);
    }

    return((BN_bn2binpad(a_mx_r, out, 256) == 256) ? 0 : 1);
}

static EC_KEY   *a_ec_verify;       /* public key only, for ECDSA verify   */
static EC_KEY   *a_ec_dh;           /* our ECDH key pair                   */
static EC_GROUP *a_ec_group;        /* an independent group for k*G        */
static BN_CTX   *a_ec_ctx;


int a_ossl_ec_setup(const unsigned char *pub65,
                    const unsigned char *ecdh_priv32,
                    const unsigned char *ecdh_pub65)
{

const EC_GROUP *g;
EC_POINT       *pt   = NULL;
BIGNUM         *priv = NULL;
int             ok   = 1;


    a_ossl_ec_free();

    a_ec_ctx = BN_CTX_new();
    if (a_ec_ctx == NULL)
    {
        goto out;
    }

    a_ec_group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    if (a_ec_group == NULL)
    {
        goto out;
    }

    a_ec_verify = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (a_ec_verify == NULL)
    {
        goto out;
    }
    g  = EC_KEY_get0_group(a_ec_verify);
    pt = EC_POINT_new(g);
    if ((g == NULL) || (pt == NULL))
    {
        goto out;
    }
    if (!EC_POINT_oct2point(g, pt, (unsigned char *)pub65, 65, a_ec_ctx))
    {
        goto out;
    }
    if (!EC_KEY_set_public_key(a_ec_verify, pt))
    {
        goto out;
    }
    EC_POINT_free(pt);
    pt = NULL;

    a_ec_dh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (a_ec_dh == NULL)
    {
        goto out;
    }
    g  = EC_KEY_get0_group(a_ec_dh);
    pt = EC_POINT_new(g);
    if ((g == NULL) || (pt == NULL))
    {
        goto out;
    }
    if (!EC_POINT_oct2point(g, pt, (unsigned char *)ecdh_pub65, 65, a_ec_ctx))
    {
        goto out;
    }
    if (!EC_KEY_set_public_key(a_ec_dh, pt))
    {
        goto out;
    }

    priv = BN_bin2bn((unsigned char *)ecdh_priv32, 32, NULL);
    if (priv == NULL)
    {
        goto out;
    }
    if (!EC_KEY_set_private_key(a_ec_dh, priv))
    {
        goto out;
    }

    ok = 0;

out:
    EC_POINT_free(pt);
    BN_free(priv);

    if (ok != 0)
    {
        a_ossl_ec_free();
    }

    return(ok);
}


void a_ossl_ec_free(void)
{

    if (a_ec_verify != NULL) { EC_KEY_free(a_ec_verify);     a_ec_verify = NULL; }
    if (a_ec_dh     != NULL) { EC_KEY_free(a_ec_dh);         a_ec_dh     = NULL; }
    if (a_ec_group  != NULL) { EC_GROUP_free(a_ec_group);    a_ec_group  = NULL; }
    if (a_ec_ctx    != NULL) { BN_CTX_free(a_ec_ctx);        a_ec_ctx    = NULL; }
}


int a_ossl_ecdsa_verify(const unsigned char *hash32,
                        const unsigned char *sig, int sig_len)
{

    if (a_ec_verify == NULL)
    {
        return(1);
    }

    /* 1 is a good signature, 0 a bad one, -1 an error.  A benchmark that
       accepted -1 would be timing an early return. */
    return((ECDSA_verify(0, (unsigned char *)hash32, 32,
                         (unsigned char *)sig, sig_len, a_ec_verify) == 1)
           ? 0 : 1);
}


int a_ossl_ecdh(const unsigned char *peer65, unsigned char *out32)
{

const EC_GROUP *g;
EC_POINT       *peer;
int             len;


    if (a_ec_dh == NULL)
    {
        return(1);
    }

    g = EC_KEY_get0_group(a_ec_dh);
    peer = EC_POINT_new(g);
    if (peer == NULL)
    {
        return(1);
    }

    if (!EC_POINT_oct2point(g, peer, (unsigned char *)peer65, 65, a_ec_ctx))
    {
        EC_POINT_free(peer);
        return(1);
    }

    /* NULL KDF gives the raw x coordinate, which is what TLS's pre-master
       secret is and what our vector holds. */
    len = ECDH_compute_key(out32, 32, peer, a_ec_dh, NULL);

    EC_POINT_free(peer);

    return((len == 32) ? 0 : 1);
}


int a_ossl_ec_mul_g(const unsigned char *k32, int consttime,
                    unsigned char *out65)
{

BIGNUM   *k;
EC_POINT *r;
int       rc = 1;


    if (a_ec_group == NULL)
    {
        return(1);
    }

    k = BN_bin2bn((unsigned char *)k32, 32, NULL);
    r = EC_POINT_new(a_ec_group);
    if ((k == NULL) || (r == NULL))
    {
        goto out;
    }

    if (consttime)
    {
        BN_set_flags(k, BN_FLG_CONSTTIME);
    }

    if (!EC_POINT_mul(a_ec_group, r, k, NULL, NULL, a_ec_ctx))
    {
        goto out;
    }

    if (EC_POINT_point2oct(a_ec_group, r, POINT_CONVERSION_UNCOMPRESSED,
                           out65, 65, a_ec_ctx) != 65)
    {
        goto out;
    }

    rc = 0;

out:
    BN_free(k);
    EC_POINT_free(r);

    return(rc);
}


int a_ossl_ec_have_precompute(void)
{

    if (a_ec_group == NULL)
    {
        return(0);
    }

    return(EC_GROUP_have_precompute_mult(a_ec_group) ? 1 : 0);
}

int a_ossl_aes128_cbc(const unsigned char *key16, const unsigned char *iv16,
                      const unsigned char *in, unsigned char *out,
                      unsigned long len)
{

EVP_CIPHER_CTX *ctx;
int             outl = 0;
int             rc   = 1;


    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL)
    {
        return(1);
    }

    if (!EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL,
                            (unsigned char *)key16, (unsigned char *)iv16))
    {
        goto out;
    }

    /* The input is a whole number of blocks, so padding would only add a
       block we are not comparing against. */
    (void)EVP_CIPHER_CTX_set_padding(ctx, 0);

    if (!EVP_EncryptUpdate(ctx, out, &outl, (unsigned char *)in, (int)len))
    {
        goto out;
    }

    rc = ((unsigned long)outl == len) ? 0 : 1;

out:
    EVP_CIPHER_CTX_free(ctx);

    return(rc);
}

int a_ossl_aes128_cbc_dec(const unsigned char *key16, const unsigned char *iv16,
                          const unsigned char *in, unsigned char *out,
                          unsigned long len)
{

EVP_CIPHER_CTX *ctx;
int             outl = 0;
int             rc   = 1;


    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL)
    {
        return(1);
    }

    if (!EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL,
                            (unsigned char *)key16, (unsigned char *)iv16))
    {
        goto out;
    }

    (void)EVP_CIPHER_CTX_set_padding(ctx, 0);

    if (!EVP_DecryptUpdate(ctx, out, &outl, (unsigned char *)in, (int)len))
    {
        goto out;
    }

    rc = ((unsigned long)outl == len) ? 0 : 1;

out:
    EVP_CIPHER_CTX_free(ctx);

    return(rc);
}


int a_ossl_hmac_sha256(const unsigned char *key, int key_len,
                       const unsigned char *in, unsigned long len,
                       unsigned char *out32)
{

unsigned int out_len = 0;


    if (HMAC(EVP_sha256(), (unsigned char *)key, key_len,
             (unsigned char *)in, (size_t)len, out32, &out_len) == NULL)
    {
        return(1);
    }

    return((out_len == 32u) ? 0 : 1);
}
