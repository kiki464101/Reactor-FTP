/**
 * @file ipc_shm.h
 * @brief Shared memory IPC for server monitoring
 *
 * Parent process creates a shared-memory segment holding an array of
 * client_info_t.  Child processes write their own entry on login and
 * clear it on disconnect.  The main process TUI reads the table every
 * 1 second for display.
 */

#ifndef IPC_SHM_H
#define IPC_SHM_H

#include <stdbool.h>

#define MAX_CLIENTS 16
#define STATUS_LEN  64

/** One entry in the shared client table. */
typedef struct {
    int     pid;                /**< child-process PID */
    char    ip[32];             /**< client IP string */
    int     port;               /**< client port */
    char    status[STATUS_LEN]; /**< "Idle","Downloading","Uploading","Online" */
    bool    active;             /**< false = slot free */
} client_info_t;

/**
 * Initialise shared memory.
 * Called once by the parent process before entering the poll loop.
 * @return pointer to the shared client_info_t array, or NULL on failure.
 */
client_info_t *shm_init(void);

/**
 * Add / update a client entry (called from child process after LOGIN).
 * Scans for an empty slot or one matching pid.
 */
void shm_add_client(client_info_t *shm, int pid,
                    const char *ip, int port, const char *status);

/** Update the status string of the entry matching @p pid. */
void shm_update_status(client_info_t *shm, int pid, const char *status);

/** Mark the slot for @p pid as inactive. */
void shm_remove_client(client_info_t *shm, int pid);

#endif /* IPC_SHM_H */
