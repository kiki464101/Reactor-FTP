#ifndef SYS_AUTH_H
#define SYS_AUTH_H

/**
 * Verify username / password against users.conf.
 * Opens the file read-only, applies flock(LOCK_SH), compares line by line.
 * @return 0 on success, -1 on failure.
 */
int verify_user(const char *username, const char *password);

#endif /* SYS_AUTH_H */
