/*
 * Quiesce gate: let a resource be torn down only once nobody is inside it.
 *
 * Every user brackets its access with hr_quiesce_enter()/hr_quiesce_leave().
 * The task about to tear the resource down calls hr_quiesce_close(), after
 * which enter() refuses newcomers, and then waits (bounded, its own choice
 * of how) until hr_quiesce_idle() says the last user has left.
 *
 * Written for the capture filesystem: esp_vfs_spiffs_unregister() frees the
 * SPIFFS control block, its lock and the VFS entry with no reference count
 * of its own, so unmounting while another task is inside a read(), stat() or
 * fclose() on that partition is a use-after-free. The writer task is already
 * serialised by a mutex; this covers the paths that were not - the HTTP
 * download readers, the logbook on the main task, and the bare stat() calls.
 *
 * Lock-free and portable (C11 atomics, no RTOS), so the ordering argument is
 * host-testable: enter() increments before it checks the flag, close() sets
 * the flag before it reads the count, both sequentially consistent, so either
 * the entrant sees the closed flag and backs out, or the closer sees the
 * entrant and waits. Nesting is fine - it is a count, not a flag.
 */
#ifndef HR_QUIESCE_H
#define HR_QUIESCE_H

#include <stdatomic.h>
#include <stdbool.h>

typedef struct {
    atomic_int  users;
    atomic_bool closing;
} hr_quiesce_t;

#define HR_QUIESCE_INIT { 0, false }

/* Admit entrants again after a close() - the count is untouched, so users
 * who were inside throughout are still accounted for. */
void hr_quiesce_reopen(hr_quiesce_t *q);

/* Claim a slot. Returns false - and claims nothing - once the gate is
 * closing; the caller must then not touch the resource. */
bool hr_quiesce_enter(hr_quiesce_t *q);

/* Release a slot claimed by a successful enter(). */
void hr_quiesce_leave(hr_quiesce_t *q);

/* Refuse new entrants from now on. Idempotent. */
void hr_quiesce_close(hr_quiesce_t *q);

/* True once the gate is closing and the last user has left: the resource may
 * be torn down. */
bool hr_quiesce_idle(const hr_quiesce_t *q);

bool hr_quiesce_closing(const hr_quiesce_t *q);
int  hr_quiesce_users(const hr_quiesce_t *q);

#endif /* HR_QUIESCE_H */
