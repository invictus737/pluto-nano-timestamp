/*
 * Nexus-BS RTOS SSH server (milestone M2e): wolfSSH on the libbsd stack, RTEMS shell sessions
 * on pseudo-terminals, single-command exec and an SFTP subsystem on the root file system.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEXUS_SSH_SERVER_H
#define NEXUS_SSH_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the SSH service (TCP 22). Call once after rtems_bsd_initialize(); returns at once.
 * Returns 0 when the listener runs, -1 if the image was built without wolfSSH or the start
 * failed (the reason is printed on the console).
 */
int nexus_ssh_start(void);

/*
 * Password check for SSH logins: the nexus2 dashboard credentials ([dashboard.auth] in
 * nexus2.toml). check(user, pass) gets NUL-terminated strings and returns nonzero to accept;
 * it is called from the SSH worker threads (several may run at once) and should compare in
 * constant time. SSH users log in with the dashboard user name or as "root" with the same
 * password, so a checker should accept both names. NULL restores the built-in checker, which
 * accepts the nexus2 defaults admin/nexus (and root/nexus). May be called before or after
 * nexus_ssh_start(). Public keys from rtos/ssh/authorized_keys work for any user name.
 */
void nexus_ssh_set_password_checker(int (*check)(const char *user, const char *pass));

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_SSH_SERVER_H */
