/*
 * Nexus-BS RTOS SSH server (milestone M2e).
 *
 * wolfSSH over the libbsd TCP/IP stack. One listener thread on TCP 22 hands each connection
 * to a free session slot; every slot owns a worker thread (SSH protocol, crypto, relay) and
 * an RTEMS shell task bound to its own pseudo-terminal (/dev/pty1N, the RTEMS telnetd pty
 * driver). The worker and the pty are joined by an AF_UNIX socket pair, so the shell sees a
 * real terminal (raw mode, line editor, window size) while the bytes travel encrypted.
 *
 *   ssh root@pluto            interactive RTEMS shell (pty)
 *   ssh root@pluto 'cpuuse'   one command, exit status of the command
 *   sftp root@pluto           SFTP on the root file system (IMFS until the QSPI data
 *                             partition exists); OpenSSH scp >= 9 uses SFTP as well
 *
 * Authentication, the same credentials as the nexus2 dashboard: a password checked by a
 * pluggable checker (nexus_ssh_set_password_checker; nexus2 registers one reading
 * [dashboard.auth] of nexus2.toml, the built-in default accepts admin/nexus), for the
 * dashboard user name and for root. Public keys compiled in from rtos/ssh/authorized_keys
 * are accepted for any user name. The host key comes from the generated ssh_secrets.h
 * (rtos/ssh/gen_secrets.py).
 * All waits are blocking (accept, poll with a timeout, semaphores): no polling loops.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ssh_server.h"

#include <stdio.h>

#ifndef NEXUS_SSH

int nexus_ssh_start(void)
{
  printf("ssh: not in this image (built without wolfSSH)\n");
  return -1;
}

void nexus_ssh_set_password_checker(int (*check)(const char *user, const char *pass))
{
  (void)check;
}

#else /* NEXUS_SSH */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>
#endif

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssh/ssh.h>
#include <wolfssh/wolfsftp.h>

#include "ssh_secrets.h" /* generated in the build directory, never in the repository */

#ifndef NEXUS_SSH_PORT
#define NEXUS_SSH_PORT 22
#endif
#define SSH_SLOTS 3                 /* concurrent sessions */
#define SSH_DEFAULT_USER "admin"     /* nexus2 [dashboard.auth] defaults */
#define SSH_DEFAULT_PASSWORD "nexus"
#define SSH_MAX_CRED 128
#define SSH_IDLE_S (15 * 60)        /* no channel data in either direction */
#define SSH_LOGIN_GRACE_S 60        /* TCP accept to authenticated session */
#define SSH_RCV_TIMEOUT_S 10        /* socket receive timeout: bounds every blocking read */
#define SSH_PW_TRIES 3
#define SSH_WORKER_PRIO 10          /* POSIX SCHED_FIFO (larger = more urgent): low */
#define SSH_WORKER_STACK (96 * 1024)
#define SSH_SHELL_STACK (64 * 1024)

enum sess_mode { SESS_NONE, SESS_SHELL, SESS_EXEC };

struct ssh_slot {
  int idx;
  int busy;                 /* guarded by slots_lock */
  int tcp;                  /* connection socket */
  char peer[INET_ADDRSTRLEN];
  char user[33];            /* login name, for the log */
  sem_t go;                 /* listener -> worker: new connection */
  WOLFSSH *ssh;
  int pw_tries;
  char key_comment[64];     /* which authorized key logged in */
  unsigned cols, rows;      /* terminal size from pty-req / window-change */
  /* worker -> backend (shell task) */
  enum sess_mode mode;
  int fd;                   /* backend end of the socket pair */
  char *cmd;                /* exec command line, ends with '\n' */
  int exit_code;
#ifdef __rtems__
  struct ssh_backend_rtems *be;
#else
  void *be;
#endif
};

static struct ssh_slot slots[SSH_SLOTS];
static pthread_mutex_t slots_lock = PTHREAD_MUTEX_INITIALIZER;
static WOLFSSH_CTX *ssh_ctx;

static double mono_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------------------------------ */
/* Seed for wolfCrypt's Hash_DRBG (CUSTOM_RAND_GENERATE_SEED in rtos/ssh/user_settings.h).     */
/* The Zynq-7010 has no TRNG and the BSP getentropy() only reads a CPU counter, so the seed     */
/* hashes: a per-build random salt (not in the repository), getentropy(), wall/monotonic time,  */
/* a call counter and the timing jitter of memory accesses measured with the 333 MHz global     */
/* timer. Each SSH session seeds its own DRBG at accept time, which adds the network timing.    */
/* ------------------------------------------------------------------------------------------ */

#ifdef __rtems__
#include <rtems/counter.h>
int getentropy(void *buf, size_t n); /* newlib declares it only with __BSD_VISIBLE */
static uint32_t seed_counter(void)
{
  return (uint32_t)rtems_counter_read();
}
#else
static uint32_t seed_counter(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)ts.tv_nsec;
}
#endif

int nexus_ssh_rand_seed(unsigned char *output, unsigned int sz)
{
  static uint32_t calls;
  static volatile uint32_t scratch[1024];
  wc_Sha256 pool;
  uint8_t ent[32];
  uint8_t digest[WC_SHA256_DIGEST_SIZE];
  struct timespec ts[2];
  uint32_t jitter[64];
  uint32_t n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);

  if (wc_InitSha256(&pool) != 0) {
    return -1;
  }
  wc_Sha256Update(&pool, nexus_ssh_seed_salt, sizeof(nexus_ssh_seed_salt));
  wc_Sha256Update(&pool, (const uint8_t *)&n, sizeof(n));
  memset(ent, 0, sizeof(ent));
  if (getentropy(ent, sizeof(ent)) == 0) {
    wc_Sha256Update(&pool, ent, sizeof(ent));
  }
  clock_gettime(CLOCK_REALTIME, &ts[0]);
  clock_gettime(CLOCK_MONOTONIC, &ts[1]);
  wc_Sha256Update(&pool, (const uint8_t *)ts, sizeof(ts));
  for (int round = 0; round < 8; round++) {
    uint32_t prev = seed_counter();
    for (int i = 0; i < 64; i++) {
      /* data-dependent walk over a 4 KiB buffer: cache and DDR timing vary the counter */
      uint32_t k = (prev * 2654435761u + (uint32_t)i) & 1023u;
      scratch[k] += prev;
      uint32_t now = seed_counter();
      jitter[i] = now - prev;
      prev = now;
    }
    wc_Sha256Update(&pool, (const uint8_t *)jitter, sizeof(jitter));
  }
  wc_Sha256Final(&pool, digest);

  /* expand: output block i = SHA-256(digest || i) */
  for (unsigned int off = 0, i = 0; off < sz; off += WC_SHA256_DIGEST_SIZE, i++) {
    uint8_t block[WC_SHA256_DIGEST_SIZE];
    unsigned int take = sz - off < WC_SHA256_DIGEST_SIZE ? sz - off : WC_SHA256_DIGEST_SIZE;
    wc_InitSha256(&pool);
    wc_Sha256Update(&pool, digest, sizeof(digest));
    wc_Sha256Update(&pool, (const uint8_t *)&i, sizeof(i));
    wc_Sha256Final(&pool, block);
    memcpy(output + off, block, take);
    memset(block, 0, sizeof(block));
  }
  wc_Sha256Free(&pool);
  memset(digest, 0, sizeof(digest));
  memset(ent, 0, sizeof(ent));
  return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Session backends: an RTEMS shell task per slot bound to a pty (RTEMS image), or a test      */
/* backend supplied by a host build (NEXUS_SSH_TEST_BACKEND).                                  */
/*   backend_init(slot)        once at start                                                   */
/*   backend_run(slot, fd)     shell (slot->mode SESS_SHELL) or command (SESS_EXEC) on fd;     */
/*                             the backend owns fd from here on and closes it when done        */
/*   backend_wait(slot)        blocks until the backend finished; slot->exit_code is valid     */
/*   backend_winsize(slot)     terminal size changed (slot->cols, slot->rows)                  */
/* ------------------------------------------------------------------------------------------ */

#if defined(__rtems__)

#include <rtems.h>
#include <rtems/libio.h>
#include <rtems/pty.h>
#include <rtems/shell.h>
#include <rtems/userenv.h>

struct ssh_backend_rtems {
  rtems_pty_context pty;
  rtems_id task;
  sem_t run;
  sem_t done;
  char taskname[8];
};

static struct ssh_backend_rtems backends[SSH_SLOTS];

static void backend_exec(struct ssh_slot *s)
{
  FILE *saved_in = stdin, *saved_out = stdout, *saved_err = stderr;
  FILE *in = fmemopen(s->cmd, strlen(s->cmd), "r");
  FILE *out = fdopen(s->fd, "w");
  int rc = 127;

  if (in != NULL && out != NULL) {
    rtems_shell_env_t env;
    setvbuf(out, NULL, _IOLBF, 0);
    stdin = in;
    stdout = out;
    stderr = out;
    rtems_shell_dup_current_env(&env);
    env.taskname = s->be->taskname;
    env.devname = "ssh";
    env.login_check = NULL;
    env.forever = false;
    env.exit_on_error = true;
    env.exit_code = &rc;
    rc = 0;
    /* returns false at the end of the input (normal here); rc holds the command status */
    (void)rtems_shell_run_main_loop(&env, false, NULL);
    fflush(stdout);
    stdin = saved_in;
    stdout = saved_out;
    stderr = saved_err;
  }
  if (in != NULL) {
    fclose(in);
  }
  if (out != NULL) {
    fclose(out); /* closes s->fd: the worker sees the end of the output */
  } else {
    close(s->fd);
  }
  s->fd = -1;
  s->exit_code = rc;
}

static void backend_shell(struct ssh_slot *s)
{
  rtems_shell_env_t env;

  rtems_shell_dup_current_env(&env);
  env.devname = rtems_pty_get_path(&s->be->pty);
  env.taskname = s->be->taskname;
  env.login_check = NULL;
  env.forever = false;
  rtems_shell_main_loop(&env);
  s->exit_code = 0;
  /* closes the pty end of the socket pair: the worker sees the shell has ended */
  rtems_pty_close_socket(&s->be->pty);
}

static void backend_task(rtems_task_argument arg)
{
  struct ssh_slot *s = (struct ssh_slot *)arg;
  const char *path = rtems_pty_get_path(&s->be->pty);

  /* private current directory and std streams per session task, as RTEMS telnetd does */
  if (rtems_libio_set_private_env() != RTEMS_SUCCESSFUL) {
    printf("ssh: slot %d: no private environment\n", s->idx);
    rtems_task_exit();
  }
  stdin = fopen(path, "r+");
  stdout = fopen(path, "r+");
  stderr = fopen(path, "r+");
  if (stdin == NULL || stdout == NULL || stderr == NULL) {
    printf("ssh: slot %d: cannot open %s\n", s->idx, path);
    rtems_task_exit();
  }
  for (;;) {
    while (sem_wait(&s->be->run) != 0) {
    }
    if (s->mode == SESS_EXEC) {
      backend_exec(s);
    } else {
      backend_shell(s);
    }
    sem_post(&s->be->done);
  }
}

static int backend_init(struct ssh_slot *s)
{
  struct ssh_backend_rtems *be = &backends[s->idx];
  rtems_status_code sc;

  s->be = be;
  snprintf(be->taskname, sizeof(be->taskname), "SSH%d", s->idx);
  if (sem_init(&be->run, 0, 0) != 0 || sem_init(&be->done, 0, 0) != 0) {
    return -1;
  }
  /* /dev/pty10.. so the numbers never meet an RTEMS telnetd started later (pty0..) */
  if (rtems_pty_initialize(&be->pty, 10 + (uintptr_t)s->idx) == NULL) {
    printf("ssh: slot %d: cannot create pty\n", s->idx);
    return -1;
  }
  sc = rtems_task_create(rtems_build_name('S', 'S', 'H', '0' + s->idx), 200, SSH_SHELL_STACK,
                         RTEMS_DEFAULT_MODES, RTEMS_FLOATING_POINT, &be->task);
  if (sc != RTEMS_SUCCESSFUL) {
    printf("ssh: slot %d: cannot create shell task (%d)\n", s->idx, (int)sc);
    return -1;
  }
  sc = rtems_task_start(be->task, backend_task, (rtems_task_argument)s);
  return sc == RTEMS_SUCCESSFUL ? 0 : -1;
}

static void backend_winsize(struct ssh_slot *s)
{
  /* read back by the pty TIOCGWINSZ ioctl (rtems_shell_winsize before every command) */
  s->be->pty.width = (int)s->cols;
  s->be->pty.height = (int)s->rows;
}

static void backend_run(struct ssh_slot *s, int fd)
{
  if (s->mode == SESS_SHELL) {
    backend_winsize(s);
    /* the pty takes fd; it announces telnet ECHO (IAC WILL ECHO), filtered by the worker */
    rtems_pty_set_socket(&s->be->pty, fd);
    s->fd = -1;
  } else {
    s->fd = fd;
  }
  sem_post(&s->be->run);
}

static void backend_wait(struct ssh_slot *s)
{
  while (sem_wait(&s->be->done) != 0) {
  }
}

/* The RTEMS pty driver speaks telnet: shell output may carry IAC sequences. */
#define BACKEND_TELNET 1

#elif defined(NEXUS_SSH_TEST_BACKEND)
#include NEXUS_SSH_TEST_BACKEND
#else
#error "nexus ssh: no session backend for this platform"
#endif

/* ------------------------------------------------------------------------------------------ */
/* Authentication                                                                             */
/* ------------------------------------------------------------------------------------------ */

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) {
    d |= (uint8_t)(a[i] ^ b[i]);
  }
  return d == 0;
}

/* Compares in time independent of the content (the length of the secret still shows). */
static int ct_streq(const char *a, const char *secret)
{
  size_t la = strlen(a), ls = strlen(secret);
  uint8_t pad[SSH_MAX_CRED + 1];
  memset(pad, 0, sizeof(pad));
  memcpy(pad, a, la < SSH_MAX_CRED ? la : SSH_MAX_CRED);
  int ok = ct_equal(pad, (const uint8_t *)secret, ls < SSH_MAX_CRED ? ls + 1 : SSH_MAX_CRED);
  memset(pad, 0, sizeof(pad));
  return ok && la == ls;
}

/* Built-in checker: the nexus2 dashboard defaults, admin/nexus (root accepted as well). */
static int default_password_checker(const char *user, const char *pass)
{
  int user_ok = ct_streq(user, SSH_DEFAULT_USER) | ct_streq(user, "root");
  int pass_ok = ct_streq(pass, SSH_DEFAULT_PASSWORD);
  return user_ok & pass_ok;
}

static int (*volatile password_checker)(const char *user, const char *pass) =
    default_password_checker;

void nexus_ssh_set_password_checker(int (*check)(const char *user, const char *pass))
{
  password_checker = check != NULL ? check : default_password_checker;
}

static int auth_types(WOLFSSH *ssh, void *ctx)
{
  (void)ssh;
  (void)ctx;
  return WOLFSSH_USERAUTH_PUBLICKEY | WOLFSSH_USERAUTH_PASSWORD;
}

static int auth_user(byte type, WS_UserAuthData *ad, void *ctx)
{
  struct ssh_slot *s = (struct ssh_slot *)ctx;

  if (s == NULL || ad == NULL || ad->usernameSz == 0 || ad->usernameSz >= sizeof(s->user) ||
      memchr(ad->username, 0, ad->usernameSz) != NULL) {
    return WOLFSSH_USERAUTH_INVALID_USER;
  }
  memcpy(s->user, ad->username, ad->usernameSz);
  s->user[ad->usernameSz] = '\0';

  if (type == WOLFSSH_USERAUTH_PUBLICKEY) {
    /* Called once without and once with a signature; wolfSSH verifies the signature
     * against this blob itself, the callback only decides whether the key is allowed. */
    const WS_UserAuthData_PublicKey *pk = &ad->sf.publicKey;
    for (size_t i = 0; i < NEXUS_SSH_AUTHORIZED_COUNT; i++) {
      const struct nexus_ssh_authorized_key *k = &nexus_ssh_authorized[i];
      if (pk->publicKeySz == k->len && memcmp(pk->publicKey, k->blob, k->len) == 0) {
        if (pk->hasSignature) {
          snprintf(s->key_comment, sizeof(s->key_comment), "key %s", k->comment);
        }
        return WOLFSSH_USERAUTH_SUCCESS;
      }
    }
    return WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
  }

  if (type == WOLFSSH_USERAUTH_PASSWORD) {
    char pass[SSH_MAX_CRED + 1];
    word32 n = ad->sf.password.passwordSz;
    int ok = 0;
    if (s->pw_tries >= SSH_PW_TRIES) {
      return WOLFSSH_USERAUTH_REJECTED;
    }
    s->pw_tries++;
    if (n <= SSH_MAX_CRED && (n == 0 || memchr(ad->sf.password.password, 0, n) == NULL)) {
      memcpy(pass, ad->sf.password.password, n);
      pass[n] = '\0';
      ok = password_checker(s->user, pass) != 0;
    }
    memset(pass, 0, sizeof(pass));
    if (ok) {
      snprintf(s->key_comment, sizeof(s->key_comment), "password");
      return WOLFSSH_USERAUTH_SUCCESS;
    }
    return s->pw_tries >= SSH_PW_TRIES ? WOLFSSH_USERAUTH_REJECTED
                                       : WOLFSSH_USERAUTH_INVALID_PASSWORD;
  }
  return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
}

static int term_size(WOLFSSH *ssh, word32 cols, word32 rows, word32 px, word32 py, void *ctx)
{
  struct ssh_slot *s = (struct ssh_slot *)ctx;
  (void)ssh;
  (void)px;
  (void)py;
  if (s != NULL && cols > 0 && rows > 0) {
    s->cols = cols;
    s->rows = rows;
    if (s->mode == SESS_SHELL) {
      backend_winsize(s);
    }
  }
  return WS_SUCCESS;
}

/* ------------------------------------------------------------------------------------------ */
/* Relay between the SSH channel and the backend socket                                        */
/* ------------------------------------------------------------------------------------------ */

/* poll(), not select(): newlib's fd_set holds descriptors 0..63 only, and the descriptor table
 * is larger than that (CONFIGURE_MAXIMUM_FILE_DESCRIPTORS) */
static int wait_readable(int fd, int timeout_s)
{
  struct pollfd p = {.fd = fd, .events = POLLIN};
  int n = poll(&p, 1, timeout_s * 1000);
  return n > 0 ? 1 : n; /* 1 readable (or hung up: the read reports it), 0 timeout, -1 error */
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) {
      continue;
    }
    if (w <= 0) {
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

struct relay {
  struct ssh_slot *s;
  WOLFSSH *ssh;
  word32 chan;
  int fd;            /* worker end of the socket pair, -1 once closed */
  int stdin_closed;  /* client EOF forwarded */
  int telnet;        /* filter the pty's telnet IAC sequences */
  int iac;           /* IAC filter state */
  double last;       /* last channel data, either direction */
};

/* Moves client data that wolfSSH buffered for the channel to the backend. */
static int relay_drain_rx(struct relay *r)
{
  uint8_t in[1024], out[2048];
  for (;;) {
    int n = wolfSSH_ChannelIdRead(r->ssh, r->chan, in, sizeof(in));
    if (n <= 0) {
      return 0;
    }
    r->last = mono_s();
    if (r->fd < 0 || r->stdin_closed || r->s->mode != SESS_SHELL) {
      continue; /* exec: the command reads no input */
    }
    size_t m = 0;
    for (int i = 0; i < n; i++) {
      out[m++] = in[i];
      if (in[i] == 0xff && r->telnet) {
        out[m++] = 0xff; /* a literal 0xff is IAC IAC on a telnet pty */
      }
    }
    if (write_all(r->fd, out, m) != 0) {
      return -1;
    }
  }
}

/* One wolfSSH step after the TCP socket became readable. Returns 0 to go on, 1 when the
 * peer closed the channel, -1 on error. */
static int relay_ssh_step(struct relay *r)
{
  int rc = wolfSSH_worker(r->ssh, NULL);
  int err = wolfSSH_get_error(r->ssh);

  if (rc == WS_CHANNEL_CLOSED || err == WS_CHANNEL_CLOSED) {
    return 1;
  }
  if (rc < 0 && rc != WS_CHAN_RXD && err != WS_CHAN_RXD && err != WS_WANT_READ &&
      err != WS_WANT_WRITE && err != WS_REKEYING && err != WS_WINDOW_FULL) {
    return err == WS_EOF ? 1 : -1;
  }
  if (relay_drain_rx(r) != 0) {
    return -1;
  }
  if (!r->stdin_closed && r->s->mode == SESS_SHELL) {
    WOLFSSH_CHANNEL *ch = wolfSSH_ChannelFind(r->ssh, r->chan, WS_CHANNEL_ID_SELF);
    if (ch != NULL && wolfSSH_ChannelGetEof(ch)) {
      /* client sent EOF (ssh -T with stdin from a file): end of shell input */
      r->stdin_closed = 1;
      if (r->fd >= 0) {
        shutdown(r->fd, SHUT_WR);
      }
    }
  }
  return 0;
}

/* Sends backend output to the client; waits for window space without spinning. */
static int relay_send(struct relay *r, const uint8_t *p, size_t n)
{
  int stalls = 0;
  while (n > 0) {
    int w = wolfSSH_ChannelIdSend(r->ssh, r->chan, (byte *)p, (word32)n);
    if (w > 0) {
      p += w;
      n -= (size_t)w;
      stalls = 0;
      continue;
    }
    int err = wolfSSH_get_error(r->ssh);
    if (w == WS_WINDOW_FULL || w == WS_WANT_WRITE || w == WS_REKEYING ||
        err == WS_WINDOW_FULL || err == WS_WANT_WRITE || err == WS_REKEYING) {
      /* the client must open the window (or finish the rekey): read from it */
      int rd = wait_readable(r->s->tcp, SSH_RCV_TIMEOUT_S);
      if (rd < 0 || (rd == 0 && ++stalls * SSH_RCV_TIMEOUT_S > SSH_IDLE_S)) {
        return -1;
      }
      if (rd > 0 && relay_ssh_step(r) != 0) {
        return -1;
      }
      continue;
    }
    return -1;
  }
  return 0;
}

/* Removes the pty's telnet negotiation (IAC WILL ECHO and replies) from the output stream. */
static size_t iac_filter(struct relay *r, uint8_t *buf, size_t n)
{
  size_t m = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = buf[i];
    switch (r->iac) {
    case 0:
      if (c == 0xff) {
        r->iac = 1;
      } else {
        buf[m++] = c;
      }
      break;
    case 1: /* after IAC */
      if (c == 0xff) {
        buf[m++] = c;
        r->iac = 0;
      } else if (c >= 251 && c <= 254) {
        r->iac = 2; /* WILL WONT DO DONT + option */
      } else if (c == 250) {
        r->iac = 3; /* SB ... IAC SE */
      } else {
        r->iac = 0;
      }
      break;
    case 2:
      r->iac = 0;
      break;
    case 3:
      if (c == 0xff) {
        r->iac = 4;
      }
      break;
    default: /* 4: IAC inside SB */
      r->iac = (c == 240) ? 0 : 3;
      break;
    }
  }
  return m;
}

/* Shell or exec: relays until the backend ends, the client leaves or the session idles. */
static void relay_session(struct ssh_slot *s, int fd)
{
  struct relay r = {.s = s, .ssh = s->ssh, .fd = fd, .last = mono_s()};
  uint8_t buf[2048];
  WOLFSSH_CHANNEL *ch = wolfSSH_ChannelNext(s->ssh, NULL);

#ifdef BACKEND_TELNET
  r.telnet = (s->mode == SESS_SHELL);
#endif
  if (ch == NULL || wolfSSH_ChannelGetId(ch, &r.chan, WS_CHANNEL_ID_SELF) != WS_SUCCESS) {
    printf("ssh: slot %d: no session channel\n", s->idx);
    close(fd);
    return;
  }
  /* client data that arrived with the channel request */
  if (relay_drain_rx(&r) != 0) {
    close(fd);
    return;
  }
  for (;;) {
    struct pollfd pf[2] = {{.fd = s->tcp, .events = POLLIN}, {.fd = r.fd, .events = POLLIN}};
    int n = poll(pf, 2, 30 * 1000);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (n == 0) {
      if (mono_s() - r.last > SSH_IDLE_S) {
        static const char msg[] = "\r\nssh: idle timeout\r\n";
        relay_send(&r, (const uint8_t *)msg, sizeof(msg) - 1);
        printf("ssh: slot %d: idle timeout\n", s->idx);
        break;
      }
      continue;
    }
    if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t got = read(r.fd, buf, sizeof(buf));
      if (got <= 0) {
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) {
          continue;
        }
        break; /* the shell exited or the command finished */
      }
      size_t len = (size_t)got;
      if (r.telnet) {
        len = iac_filter(&r, buf, len);
      }
      if (len > 0) {
        r.last = mono_s();
        if (relay_send(&r, buf, len) != 0) {
          break;
        }
      }
    }
    if ((pf[0].revents & (POLLIN | POLLHUP | POLLERR)) && relay_ssh_step(&r) != 0) {
      break;
    }
  }
  close(r.fd); /* the backend sees EOF if it still runs (client left) */
}

/* SFTP subsystem: the wolfSSH example server loop (echoserver sftp_worker) with an idle limit. */
static void sftp_session(struct ssh_slot *s)
{
  WOLFSSH *ssh = s->ssh;
  int ret, err, timeout = 1;
  double last = mono_s();
  uint8_t peek[1];

  ret = err = wolfSSH_get_error(ssh);
  while (err == WS_WANT_WRITE) {
    ret = wolfSSH_worker(ssh, NULL);
    err = wolfSSH_get_error(ssh);
  }
  do {
    if (ret == WS_WANT_WRITE || ret == WS_CHAN_RXD || wolfSSH_SFTP_PendingSend(ssh)) {
      ret = wolfSSH_SFTP_read(ssh);
      err = wolfSSH_get_error(ssh);
      last = mono_s();
      timeout = (ret == WS_REKEYING) ? 1 : (err == WS_WINDOW_FULL) ? SSH_RCV_TIMEOUT_S : 0;
      if (err == WS_WANT_READ || err == WS_WANT_WRITE || err == WS_CHAN_RXD ||
          err == WS_REKEYING || err == WS_WINDOW_FULL) {
        ret = err;
      }
      if (err == WS_WANT_WRITE || wolfSSH_SFTP_PendingSend(ssh)) {
        continue;
      }
      if (err == WS_EOF) {
        break;
      }
    }

    int rd = wait_readable(s->tcp, timeout);
    if (rd < 0) {
      break;
    }
    if (rd == 0) {
      timeout = SSH_RCV_TIMEOUT_S;
      if (mono_s() - last > SSH_IDLE_S) {
        printf("ssh: slot %d: sftp idle timeout\n", s->idx);
        break;
      }
    } else {
      ret = wolfSSH_worker(ssh, NULL);
      err = wolfSSH_get_error(ssh);
      if (ret == WS_REKEYING) {
        timeout = 1;
        continue;
      }
      if (err == WS_WANT_READ || err == WS_WANT_WRITE || err == WS_WINDOW_FULL) {
        timeout = 1;
        ret = err;
      }
      if (err == WS_EOF || ret == WS_CHANNEL_CLOSED) {
        break;
      }
      if (ret != WS_SUCCESS && ret != WS_CHAN_RXD) {
        if (ret == WS_WANT_WRITE) {
          ret = wolfSSH_worker(ssh, NULL);
          continue;
        }
        if (ret != WS_WANT_READ && ret != WS_WINDOW_FULL) {
          break;
        }
      }
    }

    ret = wolfSSH_stream_peek(ssh, peek, sizeof(peek));
    if (ret > 0) {
      ret = wolfSSH_SFTP_read(ssh);
      err = wolfSSH_get_error(ssh);
      last = mono_s();
      timeout = (ret == WS_REKEYING) ? 1 : 0;
      if (err == WS_WANT_READ || err == WS_WANT_WRITE || err == WS_CHAN_RXD ||
          err == WS_REKEYING || err == WS_WINDOW_FULL) {
        ret = err;
      }
      if (err == WS_EOF) {
        break;
      }
      continue;
    } else if (ret == WS_REKEYING) {
      timeout = 1;
      continue;
    } else if (ret < 0) {
      err = wolfSSH_get_error(ssh);
      if (err == WS_EOF) {
        break;
      }
    }
    if (ret == WS_FATAL_ERROR && err == 0) {
      WOLFSSH_CHANNEL *ch = wolfSSH_ChannelNext(ssh, NULL);
      if (ch != NULL && wolfSSH_ChannelGetEof(ch)) {
        break;
      }
    }
  } while (ret != WS_FATAL_ERROR);
}

/* ------------------------------------------------------------------------------------------ */
/* Connection handling                                                                        */
/* ------------------------------------------------------------------------------------------ */

static int ssh_accept_with_grace(struct ssh_slot *s)
{
  double t0 = mono_s();
  for (;;) {
    int rc = wolfSSH_accept(s->ssh);
    if (rc == WS_SUCCESS || rc == WS_SFTP_COMPLETE) {
      return rc;
    }
    int err = wolfSSH_get_error(s->ssh);
    if ((err == WS_WANT_READ || err == WS_WANT_WRITE) && mono_s() - t0 < SSH_LOGIN_GRACE_S) {
      continue; /* the socket receive timeout expired: resume the handshake */
    }
    if (err == WS_USER_AUTH_E) {
      wolfSSH_SendDisconnect(s->ssh, WOLFSSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE);
    }
    printf("ssh: %s: login failed (%s)\n", s->peer,
           (err == WS_WANT_READ || err == WS_WANT_WRITE) ? "timeout" : wolfSSH_ErrorToName(err));
    return WS_FATAL_ERROR;
  }
}

static void ssh_close(struct ssh_slot *s, int exit_code)
{
  wolfSSH_SetExitStatus(s->ssh, (word32)exit_code);
  int rc = wolfSSH_shutdown(s->ssh);
  for (int i = 0; i < 3 && rc != WS_SUCCESS && rc != WS_CHANNEL_CLOSED; i++) {
    int err = wolfSSH_get_error(s->ssh);
    if (rc == WS_SOCKET_ERROR_E || err == WS_SOCKET_ERROR_E ||
        (err != WS_WANT_READ && err != WS_WANT_WRITE)) {
      break;
    }
    rc = wolfSSH_worker(s->ssh, NULL);
  }
}

static void handle_connection(struct ssh_slot *s)
{
  struct timeval tv = {.tv_sec = SSH_RCV_TIMEOUT_S, .tv_usec = 0};
  int one = 1;

  setsockopt(s->tcp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s->tcp, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  setsockopt(s->tcp, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  s->ssh = wolfSSH_new(ssh_ctx);
  if (s->ssh == NULL) {
    printf("ssh: slot %d: out of memory\n", s->idx);
    return;
  }
  s->pw_tries = 0;
  s->key_comment[0] = '\0';
  s->user[0] = '\0';
  s->cols = 80;
  s->rows = 24;
  s->mode = SESS_NONE;
  wolfSSH_set_fd(s->ssh, s->tcp);
  wolfSSH_SetUserAuthCtx(s->ssh, s);
  wolfSSH_SetTerminalResizeCb(s->ssh, term_size);
  wolfSSH_SetTerminalResizeCtx(s->ssh, s);
  wolfSSH_SFTP_SetDefaultPath(s->ssh, "/");

  int rc = ssh_accept_with_grace(s);
  int exit_code = 0;
  if (rc == WS_SFTP_COMPLETE) {
    printf("ssh: %s %s (%s) sftp, slot %d\n", s->peer, s->user, s->key_comment, s->idx);
    sftp_session(s);
  } else if (rc == WS_SUCCESS) {
    WS_SessionType type = wolfSSH_GetSessionType(s->ssh);
    const char *cmd = wolfSSH_GetSessionCommand(s->ssh);
    int sv[2];

    if (type == WOLFSSH_SESSION_EXEC && cmd != NULL) {
      size_t len = strlen(cmd);
      s->cmd = malloc(len + 2);
      if (s->cmd != NULL) {
        memcpy(s->cmd, cmd, len);
        s->cmd[len] = '\n';
        s->cmd[len + 1] = '\0';
        s->mode = SESS_EXEC;
      }
    } else if (type == WOLFSSH_SESSION_SHELL || type == WOLFSSH_SESSION_TERMINAL) {
      s->mode = SESS_SHELL;
    }
    if (s->mode == SESS_NONE) {
      printf("ssh: %s: unsupported session request\n", s->peer);
      exit_code = 1;
    } else if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
      printf("ssh: slot %d: socketpair failed (%d)\n", s->idx, errno);
      exit_code = 1;
    } else {
      printf("ssh: %s %s (%s) %s%s, slot %d\n", s->peer, s->user, s->key_comment,
             s->mode == SESS_SHELL ? "shell" : "exec: ", s->mode == SESS_SHELL ? "" : cmd,
             s->idx);
      backend_run(s, sv[0]);
      relay_session(s, sv[1]);
      if (s->mode == SESS_EXEC) {
        /* exit status first, then close the channel */
        backend_wait(s);
        exit_code = s->exit_code;
      }
    }
  }
  if (rc == WS_SUCCESS || rc == WS_SFTP_COMPLETE) {
    ssh_close(s, exit_code);
  }
  close(s->tcp);
  s->tcp = -1;
  if (s->mode == SESS_SHELL) {
    /* the shell task notices the closed socket at its next read; the slot is reused only
     * after it has returned to its idle state */
    backend_wait(s);
  }
  wolfSSH_free(s->ssh);
  s->ssh = NULL;
  free(s->cmd);
  s->cmd = NULL;
  s->mode = SESS_NONE;
}

static void *slot_worker(void *arg)
{
  struct ssh_slot *s = (struct ssh_slot *)arg;
  for (;;) {
    while (sem_wait(&s->go) != 0) {
    }
    handle_connection(s);
    printf("ssh: slot %d closed\n", s->idx);
    pthread_mutex_lock(&slots_lock);
    s->busy = 0;
    pthread_mutex_unlock(&slots_lock);
  }
  return NULL;
}

static void *ssh_listener(void *arg)
{
  int ls = (int)(intptr_t)arg;
  for (;;) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int c = accept(ls, (struct sockaddr *)&peer, &plen);
    if (c < 0) {
      if (errno != EINTR) {
        sleep(1); /* resource shortage: retry after a pause */
      }
      continue;
    }
    struct ssh_slot *s = NULL;
    pthread_mutex_lock(&slots_lock);
    for (int i = 0; i < SSH_SLOTS; i++) {
      if (!slots[i].busy) {
        s = &slots[i];
        s->busy = 1;
        break;
      }
    }
    pthread_mutex_unlock(&slots_lock);
    if (s == NULL) {
      static const char msg[] = "SSH-2.0-nexus too many sessions\r\n";
      (void)write(c, msg, sizeof(msg) - 1);
      close(c);
      continue;
    }
    s->tcp = c;
    if (inet_ntop(AF_INET, &peer.sin_addr, s->peer, sizeof(s->peer)) == NULL) {
      strcpy(s->peer, "?");
    }
    sem_post(&s->go);
  }
  return NULL;
}

static int spawn_thread(void *(*fn)(void *), void *arg, size_t stack)
{
  pthread_attr_t at;
  struct sched_param sp = {.sched_priority = SSH_WORKER_PRIO};
  pthread_t t;
  int rc;

  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, stack);
#ifdef __rtems__
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&at, SCHED_FIFO);
  pthread_attr_setschedparam(&at, &sp);
#else
  (void)sp;
#endif
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  rc = pthread_create(&t, &at, fn, arg);
  pthread_attr_destroy(&at);
  return rc == 0 ? 0 : -1;
}

static void set_algos(void)
{
  /* modern subset only; defaults stay if a name is unknown to this wolfSSH */
  if (wolfSSH_CTX_SetAlgoListKex(ssh_ctx, "curve25519-sha256,curve25519-sha256@libssh.org,"
                                          "ecdh-sha2-nistp256") != WS_SUCCESS) {
    printf("ssh: default KEX list kept\n");
  }
  if (wolfSSH_CTX_SetAlgoListCipher(ssh_ctx, "aes256-gcm@openssh.com,aes128-gcm@openssh.com,"
                                             "aes256-ctr,aes128-ctr") != WS_SUCCESS) {
    printf("ssh: default cipher list kept\n");
  }
  if (wolfSSH_CTX_SetAlgoListMac(ssh_ctx, "hmac-sha2-256,hmac-sha2-512") != WS_SUCCESS) {
    printf("ssh: default MAC list kept\n");
  }
  if (wolfSSH_CTX_SetAlgoListKeyAccepted(ssh_ctx, "ssh-ed25519,ecdsa-sha2-nistp256") !=
      WS_SUCCESS) {
    printf("ssh: default user key list kept\n");
  }
}

int nexus_ssh_start(void)
{
  static int started;
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(NEXUS_SSH_PORT)};
  int one = 1, ls;

  if (started) {
    return 0;
  }
  if (wolfSSH_Init() != WS_SUCCESS) {
    printf("ssh: wolfSSH init failed\n");
    return -1;
  }
  ssh_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
  if (ssh_ctx == NULL) {
    printf("ssh: no context\n");
    return -1;
  }
  wolfSSH_SetUserAuth(ssh_ctx, auth_user);
  wolfSSH_SetUserAuthTypes(ssh_ctx, auth_types);
  if (wolfSSH_CTX_UsePrivateKey_buffer(ssh_ctx, nexus_ssh_hostkey_der,
                                       (word32)sizeof(nexus_ssh_hostkey_der),
                                       WOLFSSH_FORMAT_ASN1) != WS_SUCCESS) {
    printf("ssh: host key rejected\n");
    wolfSSH_CTX_free(ssh_ctx);
    ssh_ctx = NULL;
    return -1;
  }
  set_algos();

  for (int i = 0; i < SSH_SLOTS; i++) {
    struct ssh_slot *s = &slots[i];
    s->idx = i;
    s->tcp = -1;
    s->fd = -1;
    if (sem_init(&s->go, 0, 0) != 0 || backend_init(s) != 0 ||
        spawn_thread(slot_worker, s, SSH_WORKER_STACK) != 0) {
      printf("ssh: slot %d not available\n", i);
      pthread_mutex_lock(&slots_lock);
      s->busy = 1; /* never handed out */
      pthread_mutex_unlock(&slots_lock);
    }
  }

#ifdef __rtems__
  a.sin_len = sizeof(a);
#endif
  ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) {
    printf("ssh: no socket\n");
    return -1;
  }
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 2) != 0) {
    printf("ssh: cannot listen on :%d (%d)\n", NEXUS_SSH_PORT, errno);
    close(ls);
    return -1;
  }
  if (spawn_thread(ssh_listener, (void *)(intptr_t)ls, 16 * 1024) != 0) {
    printf("ssh: no listener thread\n");
    close(ls);
    return -1;
  }
  started = 1;
  printf("ssh: listening on :%d, dashboard credentials (%s), %u key(s), host key ed25519 "
         "%s%s\n", NEXUS_SSH_PORT,
         password_checker == default_password_checker ? "default admin" : "nexus2",
         (unsigned)NEXUS_SSH_AUTHORIZED_COUNT, nexus_ssh_hostkey_fp,
         NEXUS_SSH_HOSTKEY_EPHEMERAL ? " (ephemeral, this build only)" : "");
  return 0;
}

#endif /* NEXUS_SSH */
