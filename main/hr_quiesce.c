#include "hr_quiesce.h"

void hr_quiesce_reopen(hr_quiesce_t *q)
{
    atomic_store(&q->closing, false);
}

bool hr_quiesce_enter(hr_quiesce_t *q)
{
    /* Count first, check second - see the header for why the order matters. */
    atomic_fetch_add(&q->users, 1);
    if (atomic_load(&q->closing)) {
        atomic_fetch_sub(&q->users, 1);
        return false;
    }
    return true;
}

void hr_quiesce_leave(hr_quiesce_t *q)
{
    atomic_fetch_sub(&q->users, 1);
}

void hr_quiesce_close(hr_quiesce_t *q)
{
    atomic_store(&q->closing, true);
}

bool hr_quiesce_idle(const hr_quiesce_t *q)
{
    return atomic_load(&q->closing) && atomic_load(&q->users) == 0;
}

bool hr_quiesce_closing(const hr_quiesce_t *q)
{
    return atomic_load(&q->closing);
}

int hr_quiesce_users(const hr_quiesce_t *q)
{
    return atomic_load(&q->users);
}
