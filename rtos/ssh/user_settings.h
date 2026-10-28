/*
 * wolfSSL + wolfSSH build configuration for the Nexus-BS RTEMS image (Pluto Nano, Zynq-7010,
 * arm-rtems6, libbsd sockets). Shared by rtos/ci/wolfssh.sh (library build) and
 * rtos/app/Makefile (application), so both sides see the same structure layouts.
 *
 * SSH only: wolfCrypt without TLS. Host key ssh-ed25519, user keys ssh-ed25519 and
 * ecdsa-sha2-nistp256, KEX curve25519-sha256 / ecdh-sha2-nistp256, ciphers AES-GCM / AES-CTR,
 * MAC hmac-sha2-256/512. wolfSSH has no chacha20-poly1305, so it is not built.
 */
#ifndef NEXUS_WOLF_USER_SETTINGS_H
#define NEXUS_WOLF_USER_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- platform ---- */
#ifdef __rtems__
    #define SIZEOF_LONG 4
    #define SIZEOF_LONG_LONG 8
    #define WOLFSSL_GENERAL_ALIGNMENT 4
#endif
#define WOLFSSL_PTHREADS          /* RTEMS POSIX threads and mutexes */
#define NO_WOLFSSL_DIR            /* wolfSSL never scans directories */
#define WOLFSSL_NO_SOCK           /* wolfCrypt only: no wolfSSL socket I/O */
#define WC_NO_ASYNC_THREADING
#define NO_ERROR_STRINGS          /* wolfSSH has its own error names */
#define WOLFSSL_IGNORE_FILE_WARN

/* Seed source: the Zynq-7010 has no TRNG and the BSP getentropy() is a CPU counter, so the
 * application supplies a seed that mixes several sources (rtos/app/ssh_server.c). Hash_DRBG
 * runs on top. */
#define HAVE_HASHDRBG
#define CUSTOM_RAND_GENERATE_SEED nexus_ssh_rand_seed
extern int nexus_ssh_rand_seed(unsigned char *output, unsigned int sz);

/* ---- features ---- */
#define WOLFCRYPT_ONLY
#define WOLFSSL_WOLFSSH
#define WOLFSSL_ASN_TEMPLATE
#define WOLFSSL_PUBLIC_MP
#define WOLFSSL_BASE64_ENCODE

#define TFM_TIMING_RESISTANT
#define ECC_TIMING_RESISTANT

/* ---- asymmetric ---- */
#define NO_RSA
#define NO_DH
#define NO_DSA

#define HAVE_ECC                  /* ecdsa-sha2-nistp256 user keys, ecdh-sha2-nistp256 KEX */
#define ECC_USER_CURVES           /* P-256 only */
#define ECC_SHAMIR

#define HAVE_CURVE25519
#define HAVE_ED25519
#define WOLFSSL_ED25519_STREAMING_VERIFY
#define HAVE_ED25519_KEY_IMPORT
#define HAVE_ED25519_KEY_EXPORT
#define HAVE_CURVE25519_KEY_IMPORT
#define HAVE_CURVE25519_KEY_EXPORT

/* ---- math: single precision, portable C (32-bit) ---- */
#define WOLFSSL_SP_MATH
#define WOLFSSL_SP_SMALL
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_NO_2048
#define WOLFSSL_SP_NO_3072

/* ---- symmetric ---- */
#define HAVE_AESGCM
#define GCM_TABLE_4BIT
#define WOLFSSL_AES_COUNTER
#define WOLFSSL_AES_DIRECT

/* ---- hashes: SHA-256 and SHA-512 (Ed25519); SHA-1 kept for wolfCrypt internals ---- */
#define WOLFSSL_SHA512
#define WOLFSSL_SHA384
#define WOLFSSL_NO_SHAKE128
#define WOLFSSL_NO_SHAKE256

#define NO_PWDBASED
#define NO_PKCS12

/* ---- disabled ---- */
#define NO_DES3
#define NO_MD4
#define NO_MD5
#define NO_RC4
#define NO_PSK
#define NO_OLD_TLS
#define NO_DES3_TLS_SUITES
#define WOLFSSL_NO_TLS12
#define NO_SESSION_CACHE

/* ---- wolfSSH ---- */
#define WOLFSSH_SFTP              /* SFTP server subsystem on the root file system */
#define WOLFSSH_TERM              /* accept pty-req, pass the window size to the shell */
#define WOLFSSH_SHELL             /* window-change requests, exit-status on close */
#define NO_TERMIOS                /* no wolfSSH termios mode mapping (RTEMS shell owns it) */
#define WOLFSSH_MAX_FILENAME 256

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_WOLF_USER_SETTINGS_H */
