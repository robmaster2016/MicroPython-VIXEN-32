/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
#ifndef VIXEN_SQLITE_H
#define VIXEN_SQLITE_H

/* SQLite OS layer (vixen_sqlite.c): memory on the gc heap, RAM-backed files */
void vixen_sqlite_init(void);   /* once, before the first connect() */
void vixen_sqlite_reset(void);  /* soft reset, before gc_sweep_all() */

#endif
