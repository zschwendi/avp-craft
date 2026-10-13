/* Copyright 2026 mcopt-shade contributors. Licensed under Apache-2.0. */
#ifndef MC_PRESENT_SLOTS_H
#define MC_PRESENT_SLOTS_H
#include <stdatomic.h>

/* One render-thread producer; present completion/mailbox replacement only frees slots.
 * A mailbox replacement can leave a free slot behind the cyclic next index.
 * Scan at most the fixed ring size before dropping a fresh image. */
static inline int mc_present_available_slot(const _Atomic int *busy, int count, int next) {
    for (int offset = 0; offset < count; offset++) {
        int slot = (next + offset) % count;
        if (!atomic_load(&busy[slot])) return slot;
    }
    return -1;
}
#endif
