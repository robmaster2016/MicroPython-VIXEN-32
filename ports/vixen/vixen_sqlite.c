/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/*
 * SQLite on the VIXEN-32: the OS layer under the sqlite3 module
 * (deps/micropython-sqlite3, built with SQLITE_OS_OTHER=1).
 *
 * MEMORY. SQLite allocates from the MicroPython gc heap, so it gets the whole
 * heap (growing into the Zorro RAM) instead of the small libc heap that
 * mbedtls lives in. Every block carries a header that links it into one list
 * hanging off a root pointer: the gc sees all of SQLite's memory as reachable
 * and scans it, which also keeps the database buffers below alive. gc_alloc
 * returns NULL instead of raising, so no MemoryError ever unwinds through
 * SQLite's C frames -- SQLite reports SQLITE_NOMEM itself.
 *
 * FILES. The kernel's file service (vixen_disk.c) opens a file either for
 * reading or for writing from scratch: there is no read+write handle and no
 * fsync. SQLite needs both, so a database lives in RAM while it is open:
 *   - xOpen reads the whole file into a buffer,
 *   - SQLite reads and writes that buffer,
 *   - xSync (the end of every commit) writes the buffer to "<name>-vxtmp",
 *     removes the database and renames the temporary file over it.
 * The card therefore always holds the last committed state. A power cut
 * during the write leaves either the old database (and a stale -vxtmp, deleted
 * on the next open) or only the complete -vxtmp, which the next open renames
 * back. Rollback journals and temp files never touch the card: they live in
 * RAM, and xAccess reports no journal, so there is never a hot journal to
 * replay. Two connections to the same file share one buffer and lock it like
 * SQLite's own memdb VFS.
 * Cost: opening reads the whole file, each commit writes the whole file.
 * Fine for the kilobytes-to-megabytes databases a home computer keeps.
 *
 * Every call into MicroPython (open/read/write/stat/rename) runs under its own
 * nlr frame: an OSError becomes an SQLite error code, never a longjmp across
 * SQLite.
 *
 * Soft reset (Ctrl-D): main.c calls vixen_sqlite_reset() before gc_sweep_all()
 * frees the heap. Committed data is on the card already; what is lost is what
 * a Python program had not committed.
 */

#include <string.h>

#include "py/runtime.h"
#include "py/gc.h"
#include "py/stream.h"
#include "py/mphal.h"
#include "py/builtin.h"
#include "extmod/vfs.h"

#include "sqlite3.h"

#include "vixen_sqlite.h"

#define VX_MAXPATH  256
#define VX_TMPSUFFIX "-vxtmp"
#define VX_CHUNK    4096

/* ---- memory: SQLite on the gc heap ------------------------------------ */

/* 16 bytes: gc blocks are 16-byte aligned, so the payload is too (SQLite
 * needs 8). */
typedef struct vx_mblk {
    struct vx_mblk *prev;
    struct vx_mblk *next;
    size_t size;
    uint32_t pad;
} vx_mblk_t;

MP_REGISTER_ROOT_POINTER(void *vixen_sqlite_mem);

#define VX_MEM_HEAD (*(vx_mblk_t **)&MP_STATE_VM(vixen_sqlite_mem))

static void vx_link(vx_mblk_t *b) {
    b->prev = NULL;
    b->next = VX_MEM_HEAD;
    if (b->next) {
        b->next->prev = b;
    }
    VX_MEM_HEAD = b;
}

static void vx_unlink(vx_mblk_t *b) {
    if (b->prev) {
        b->prev->next = b->next;
    } else {
        VX_MEM_HEAD = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
}

static void *vx_malloc(int n) {
    vx_mblk_t *b = gc_alloc(sizeof(vx_mblk_t) + (size_t)n, 0);
    if (b == NULL) {
        return NULL;
    }
    b->size = (size_t)n;
    vx_link(b);
    return b + 1;
}

static void vx_free(void *p) {
    if (p == NULL) {
        return;
    }
    vx_mblk_t *b = (vx_mblk_t *)p - 1;
    vx_unlink(b);
    gc_free(b);
}

static void *vx_realloc(void *p, int n) {
    vx_mblk_t *b = (vx_mblk_t *)p - 1;
    /* The block stays linked (and so reachable) while gc_realloc may collect;
     * if it moves, the neighbours are pointed at the new address. */
    vx_mblk_t *nb = gc_realloc(b, sizeof(vx_mblk_t) + (size_t)n, true);
    if (nb == NULL) {
        return NULL;
    }
    nb->size = (size_t)n;
    if (nb != b) {
        if (nb->prev) {
            nb->prev->next = nb;
        } else {
            VX_MEM_HEAD = nb;
        }
        if (nb->next) {
            nb->next->prev = nb;
        }
    }
    return nb + 1;
}

static int vx_size(void *p) {
    return p == NULL ? 0 : (int)((vx_mblk_t *)p - 1)->size;
}

static int vx_roundup(int n) {
    return (n + 7) & ~7;
}

static int vx_mem_init(void *unused) {
    (void)unused;
    return SQLITE_OK;
}

static void vx_mem_shutdown(void *unused) {
    (void)unused;
}

static const sqlite3_mem_methods vx_mem_methods = {
    vx_malloc, vx_free, vx_realloc, vx_size, vx_roundup,
    vx_mem_init, vx_mem_shutdown, NULL,
};

/* ---- MicroPython file calls, exceptions caught ------------------------- */

/* errno of a caught OSError, -1 for any other exception */
static int vx_exc_errno(void *exc) {
    mp_obj_t o = MP_OBJ_FROM_PTR(exc);
    if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(o)), MP_OBJ_FROM_PTR(&mp_type_OSError))) {
        mp_obj_t v = mp_obj_exception_get_value(o);
        if (mp_obj_is_small_int(v)) {
            return MP_OBJ_SMALL_INT_VALUE(v);
        }
    }
    return -1;
}

/* 1 = exists (size in *size), 0 = does not exist, -1 = error */
static int vx_stat(const char *path, sqlite3_int64 *size) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t st = mp_vfs_stat(mp_obj_new_str(path, strlen(path)));
        mp_obj_t *items;
        size_t n;
        mp_obj_get_array(st, &n, &items);
        if (size) {
            *size = n > 6 ? mp_obj_get_int(items[6]) : 0;
        }
        nlr_pop();
        return 1;
    }
    return vx_exc_errno(nlr.ret_val) == MP_ENOENT ? 0 : -1;
}

/* remove; 0 also when the file was not there */
static int vx_remove(const char *path) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_vfs_remove(mp_obj_new_str(path, strlen(path)));
        nlr_pop();
        return 0;
    }
    return vx_exc_errno(nlr.ret_val) == MP_ENOENT ? 0 : -1;
}

static int vx_rename(const char *from, const char *to) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_vfs_rename(mp_obj_new_str(from, strlen(from)), mp_obj_new_str(to, strlen(to)));
        nlr_pop();
        return 0;
    }
    return -1;
}

static mp_obj_t vx_open(const char *path, const char *mode) {
    mp_obj_t args[2] = {
        mp_obj_new_str(path, strlen(path)),
        mp_obj_new_str(mode, strlen(mode)),
    };
    return mp_builtin_open(2, args, (mp_map_t *)&mp_const_empty_map);
}

/* Write len bytes as the whole new content of path. 0 or -1. */
static int vx_write_file(const char *path, const uint8_t *data, size_t len) {
    nlr_buf_t nlr;
    volatile mp_obj_t f = MP_OBJ_NULL;
    if (nlr_push(&nlr) == 0) {
        f = vx_open(path, "wb");
        while (len > 0) {
            size_t n = len < VX_CHUNK ? len : VX_CHUNK;
            int err;
            mp_uint_t got = mp_stream_rw(f, (void *)data, n, &err, MP_STREAM_RW_WRITE);
            if (got != n) {
                mp_raise_OSError(err ? err : MP_EIO);
            }
            data += n;
            len -= n;
        }
        mp_obj_t g = f;
        f = MP_OBJ_NULL;
        mp_stream_close(g);
        nlr_pop();
        return 0;
    }
    if (f != MP_OBJ_NULL) {
        nlr_buf_t nlr2;
        if (nlr_push(&nlr2) == 0) {
            mp_stream_close(f);
            nlr_pop();
        }
    }
    return -1;
}

/* Read the whole file into *out (sqlite3_malloc64). 0 or -1. */
static int vx_read_file(const char *path, sqlite3_int64 size, uint8_t **out, sqlite3_int64 *out_len) {
    uint8_t *buf = sqlite3_malloc64(size > 0 ? (sqlite3_uint64)size : 1);
    if (buf == NULL) {
        return -1;
    }
    nlr_buf_t nlr;
    volatile mp_obj_t f = MP_OBJ_NULL;
    if (nlr_push(&nlr) == 0) {
        f = vx_open(path, "rb");
        sqlite3_int64 have = 0;
        while (have < size) {
            sqlite3_int64 want = size - have;
            size_t n = want < VX_CHUNK ? (size_t)want : VX_CHUNK;
            int err;
            mp_uint_t got = mp_stream_rw(f, buf + have, n, &err, MP_STREAM_RW_READ | MP_STREAM_RW_ONCE);
            if (got == MP_STREAM_ERROR) {
                mp_raise_OSError(err);
            }
            if (got == 0) {
                break;   /* shorter than stat said */
            }
            have += got;
        }
        mp_obj_t g = f;
        f = MP_OBJ_NULL;
        mp_stream_close(g);
        nlr_pop();
        *out = buf;
        *out_len = have;
        return 0;
    }
    if (f != MP_OBJ_NULL) {
        nlr_buf_t nlr2;
        if (nlr_push(&nlr2) == 0) {
            mp_stream_close(f);
            nlr_pop();
        }
    }
    sqlite3_free(buf);
    return -1;
}

/* ---- the RAM-backed files --------------------------------------------- */

typedef struct vx_store {
    struct vx_store *next;     /* list of the card-backed stores */
    char *path;                /* NULL: volatile (journal, temp) */
    uint8_t *data;
    sqlite3_int64 size;
    sqlite3_int64 alloc;
    int refs;
    int n_rd_lock;
    int n_wr_lock;
    bool dirty;
    bool readonly;
} vx_store_t;

typedef struct {
    sqlite3_file base;
    vx_store_t *store;
    int lock;
} vx_file_t;

static vx_store_t *vx_stores;

static bool vx_has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Names SQLite derives from a database name for its side files. */
static bool vx_is_side_file(const char *name) {
    return vx_has_suffix(name, "-journal") || vx_has_suffix(name, "-wal")
           || vx_has_suffix(name, "-shm") || vx_has_suffix(name, VX_TMPSUFFIX);
}

static int vx_persist(vx_store_t *s) {
    if (s->path == NULL || !s->dirty || s->readonly) {
        return SQLITE_OK;
    }
    char tmp[VX_MAXPATH + sizeof(VX_TMPSUFFIX)];
    strcpy(tmp, s->path);
    strcat(tmp, VX_TMPSUFFIX);
    if (vx_write_file(tmp, s->data, (size_t)s->size) != 0) {
        vx_remove(tmp);
        return SQLITE_IOERR_WRITE;
    }
    /* kernel FatFs: rename does not replace, so remove first */
    if (vx_remove(s->path) != 0) {
        vx_remove(tmp);
        return SQLITE_IOERR_DELETE;
    }
    if (vx_rename(tmp, s->path) != 0) {
        /* the data is safe in tmp; the next open recovers it */
        return SQLITE_IOERR_FSYNC;
    }
    s->dirty = false;
    return SQLITE_OK;
}

static void vx_store_release(vx_store_t *s) {
    if (--s->refs > 0) {
        return;
    }
    if (s->path) {
        for (vx_store_t **pp = &vx_stores; *pp; pp = &(*pp)->next) {
            if (*pp == s) {
                *pp = s->next;
                break;
            }
        }
    }
    sqlite3_free(s->data);
    sqlite3_free(s->path);
    sqlite3_free(s);
}

static int vx_close(sqlite3_file *f) {
    vx_file_t *p = (vx_file_t *)f;
    /* With PRAGMA synchronous=OFF there is no xSync: write on close. SQLite
     * has rolled back any open transaction by now. */
    int rc = vx_persist(p->store);
    vx_store_release(p->store);
    p->store = NULL;
    return rc;
}

static int vx_read(sqlite3_file *f, void *buf, int amt, sqlite3_int64 ofs) {
    vx_store_t *s = ((vx_file_t *)f)->store;
    if (ofs + amt > s->size) {
        memset(buf, 0, (size_t)amt);
        if (ofs < s->size) {
            memcpy(buf, s->data + ofs, (size_t)(s->size - ofs));
        }
        return SQLITE_IOERR_SHORT_READ;
    }
    memcpy(buf, s->data + ofs, (size_t)amt);
    return SQLITE_OK;
}

static int vx_grow(vx_store_t *s, sqlite3_int64 need) {
    if (need <= s->alloc) {
        return SQLITE_OK;
    }
    sqlite3_int64 n = s->alloc ? s->alloc : 4096;
    while (n < need) {
        n *= 2;
    }
    uint8_t *d = sqlite3_realloc64(s->data, (sqlite3_uint64)n);
    if (d == NULL) {
        return SQLITE_IOERR_NOMEM;
    }
    s->data = d;
    s->alloc = n;
    return SQLITE_OK;
}

static int vx_write(sqlite3_file *f, const void *buf, int amt, sqlite3_int64 ofs) {
    vx_store_t *s = ((vx_file_t *)f)->store;
    if (s->readonly) {
        return SQLITE_READONLY;
    }
    int rc = vx_grow(s, ofs + amt);
    if (rc != SQLITE_OK) {
        return rc;
    }
    if (ofs > s->size) {
        memset(s->data + s->size, 0, (size_t)(ofs - s->size));
    }
    memcpy(s->data + ofs, buf, (size_t)amt);
    if (ofs + amt > s->size) {
        s->size = ofs + amt;
    }
    s->dirty = true;
    return SQLITE_OK;
}

static int vx_truncate(sqlite3_file *f, sqlite3_int64 size) {
    vx_store_t *s = ((vx_file_t *)f)->store;
    if (size < s->size) {
        s->size = size;
        s->dirty = true;
    }
    return SQLITE_OK;
}

static int vx_sync(sqlite3_file *f, int flags) {
    (void)flags;
    return vx_persist(((vx_file_t *)f)->store);
}

static int vx_file_size(sqlite3_file *f, sqlite3_int64 *size) {
    *size = ((vx_file_t *)f)->store->size;
    return SQLITE_OK;
}

/* Locking between connections of this interpreter: SQLite's memdb scheme. */
static int vx_lock(sqlite3_file *f, int lock) {
    vx_file_t *p = (vx_file_t *)f;
    vx_store_t *s = p->store;
    if (lock <= p->lock) {
        return SQLITE_OK;
    }
    if (lock > SQLITE_LOCK_SHARED && s->readonly) {
        return SQLITE_READONLY;
    }
    switch (lock) {
        case SQLITE_LOCK_SHARED:
            if (s->n_wr_lock > 0) {
                return SQLITE_BUSY;
            }
            s->n_rd_lock++;
            break;
        case SQLITE_LOCK_RESERVED:
        case SQLITE_LOCK_PENDING:
            if (p->lock == SQLITE_LOCK_SHARED) {
                if (s->n_wr_lock > 0) {
                    return SQLITE_BUSY;
                }
                s->n_wr_lock = 1;
            }
            break;
        default: /* EXCLUSIVE */
            if (s->n_rd_lock > 1) {
                return SQLITE_BUSY;
            }
            if (p->lock == SQLITE_LOCK_SHARED) {
                s->n_wr_lock = 1;
            }
            break;
    }
    p->lock = lock;
    return SQLITE_OK;
}

static int vx_unlock(sqlite3_file *f, int lock) {
    vx_file_t *p = (vx_file_t *)f;
    vx_store_t *s = p->store;
    if (lock >= p->lock) {
        return SQLITE_OK;
    }
    if (p->lock > SQLITE_LOCK_SHARED) {
        s->n_wr_lock--;
    }
    if (lock == SQLITE_LOCK_NONE) {
        s->n_rd_lock--;
    }
    p->lock = lock;
    return SQLITE_OK;
}

static int vx_check_reserved(sqlite3_file *f, int *out) {
    *out = ((vx_file_t *)f)->store->n_wr_lock > 0;
    return SQLITE_OK;
}

static int vx_file_control(sqlite3_file *f, int op, void *arg) {
    (void)f;
    (void)op;
    (void)arg;
    return SQLITE_NOTFOUND;
}

static int vx_sector_size(sqlite3_file *f) {
    (void)f;
    return 512;
}

static int vx_device_characteristics(sqlite3_file *f) {
    (void)f;
    return SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SAFE_APPEND | SQLITE_IOCAP_SEQUENTIAL;
}

static const sqlite3_io_methods vx_io_methods = {
    1,
    vx_close, vx_read, vx_write, vx_truncate, vx_sync, vx_file_size,
    vx_lock, vx_unlock, vx_check_reserved, vx_file_control,
    vx_sector_size, vx_device_characteristics,
};

/* Load (or create) the card file behind a new store. */
static int vx_store_load(vx_store_t *s, int flags) {
    char tmp[VX_MAXPATH + sizeof(VX_TMPSUFFIX)];
    strcpy(tmp, s->path);
    strcat(tmp, VX_TMPSUFFIX);

    sqlite3_int64 size = 0;
    int st = vx_stat(s->path, &size);
    if (st < 0) {
        return SQLITE_CANTOPEN;
    }
    if (st == 0 && vx_stat(tmp, NULL) == 1) {
        /* power cut between remove and rename: the -vxtmp is complete */
        if (vx_rename(tmp, s->path) != 0) {
            return SQLITE_CANTOPEN;
        }
        st = vx_stat(s->path, &size);
        if (st != 1) {
            return SQLITE_CANTOPEN;
        }
    } else if (st == 1 && !s->readonly) {
        /* power cut while writing -vxtmp: the database is the old one */
        vx_remove(tmp);
    }
    if (st == 0) {
        if (!(flags & SQLITE_OPEN_CREATE)) {
            return SQLITE_CANTOPEN;
        }
        /* create the empty file now, like any other VFS: a bad directory
         * shows at connect() and not at the first commit */
        if (vx_write_file(s->path, NULL, 0) != 0) {
            return SQLITE_CANTOPEN;
        }
        return SQLITE_OK;
    }
    if (vx_read_file(s->path, size, &s->data, &s->size) != 0) {
        return SQLITE_IOERR_READ;
    }
    s->alloc = size > 0 ? size : 1;
    return SQLITE_OK;
}

static int vx_vfs_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *f, int flags, int *out_flags) {
    (void)vfs;
    vx_file_t *p = (vx_file_t *)f;
    p->base.pMethods = NULL;
    p->store = NULL;
    p->lock = SQLITE_LOCK_NONE;

    bool on_card = name != NULL && (flags & SQLITE_OPEN_MAIN_DB);
    vx_store_t *s = NULL;
    if (on_card) {
        for (s = vx_stores; s; s = s->next) {
            if (strcmp(s->path, name) == 0) {
                break;
            }
        }
    }
    if (s != NULL) {
        s->refs++;
    } else {
        s = sqlite3_malloc(sizeof(*s));
        if (s == NULL) {
            return SQLITE_NOMEM;
        }
        memset(s, 0, sizeof(*s));
        s->refs = 1;
        if (on_card) {
            s->readonly = (flags & SQLITE_OPEN_READONLY) != 0;
            s->path = sqlite3_mprintf("%s", name);
            if (s->path == NULL) {
                sqlite3_free(s);
                return SQLITE_NOMEM;
            }
            int rc = vx_store_load(s, flags);
            if (rc != SQLITE_OK) {
                sqlite3_free(s->data);
                sqlite3_free(s->path);
                sqlite3_free(s);
                return rc;
            }
            s->next = vx_stores;
            vx_stores = s;
        }
    }
    p->store = s;
    p->base.pMethods = &vx_io_methods;
    if (out_flags) {
        *out_flags = flags;
    }
    return SQLITE_OK;
}

static int vx_vfs_delete(sqlite3_vfs *vfs, const char *name, int sync_dir) {
    (void)vfs;
    (void)sync_dir;
    if (vx_is_side_file(name)) {
        return SQLITE_OK;    /* journals live in RAM, freed on close */
    }
    return vx_remove(name) == 0 ? SQLITE_OK : SQLITE_IOERR_DELETE;
}

static int vx_vfs_access(sqlite3_vfs *vfs, const char *name, int flags, int *out) {
    (void)vfs;
    (void)flags;
    if (vx_is_side_file(name)) {
        *out = 0;            /* never a hot journal on the card */
        return SQLITE_OK;
    }
    *out = vx_stat(name, NULL) == 1;
    return SQLITE_OK;
}

static int vx_vfs_full_pathname(sqlite3_vfs *vfs, const char *name, int n_out, char *out) {
    (void)vfs;
    if (name[0] == '/') {
        sqlite3_snprintf(n_out, out, "%s", name);
    } else {
        /* absolute, so a later os.chdir() does not move the database */
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            const char *cwd = mp_obj_str_get_str(mp_vfs_getcwd());
            size_t n = strlen(cwd);
            sqlite3_snprintf(n_out, out, "%s%s%s", cwd, (n && cwd[n - 1] == '/') ? "" : "/", name);
            nlr_pop();
        } else {
            sqlite3_snprintf(n_out, out, "%s", name);
        }
    }
    return (int)strlen(out) + 1 >= n_out ? SQLITE_CANTOPEN : SQLITE_OK;
}

static int vx_vfs_randomness(sqlite3_vfs *vfs, int n, char *out) {
    (void)vfs;
    mp_hal_get_random((size_t)n, (uint8_t *)out);
    return n;
}

static int vx_vfs_sleep(sqlite3_vfs *vfs, int us) {
    (void)vfs;
    mp_hal_delay_us((mp_uint_t)us);
    return us;
}

/* No wall clock yet: 2000-01-01 00:00 UTC plus the time since power-up. */
static int vx_vfs_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *out) {
    (void)vfs;
    *out = (sqlite3_int64)211813444800000LL + (sqlite3_int64)mp_hal_ticks_ms();
    return SQLITE_OK;
}

static int vx_vfs_current_time(sqlite3_vfs *vfs, double *out) {
    sqlite3_int64 ms;
    vx_vfs_current_time_int64(vfs, &ms);
    *out = (double)ms / 86400000.0;
    return SQLITE_OK;
}

static int vx_vfs_get_last_error(sqlite3_vfs *vfs, int n, char *out) {
    (void)vfs;
    if (n > 0) {
        out[0] = 0;
    }
    return 0;
}

static sqlite3_vfs vx_vfs = {
    2,                      /* iVersion */
    sizeof(vx_file_t),      /* szOsFile */
    VX_MAXPATH,             /* mxPathname */
    NULL,                   /* pNext */
    "vixen",                /* zName */
    NULL,                   /* pAppData */
    vx_vfs_open,
    vx_vfs_delete,
    vx_vfs_access,
    vx_vfs_full_pathname,
    NULL, NULL, NULL, NULL, /* no extensions */
    vx_vfs_randomness,
    vx_vfs_sleep,
    vx_vfs_current_time,
    vx_vfs_get_last_error,
    vx_vfs_current_time_int64,
    NULL, NULL, NULL,
};

/* ---- SQLITE_OS_OTHER entry points -------------------------------------- */

int sqlite3_os_init(void) {
    return sqlite3_vfs_register(&vx_vfs, 1);
}

int sqlite3_os_end(void) {
    vx_stores = NULL;
    return SQLITE_OK;
}

/* ---- port hooks (main.c) ----------------------------------------------- */

void vixen_sqlite_init(void) {
    VX_MEM_HEAD = NULL;
    sqlite3_config(SQLITE_CONFIG_MALLOC, &vx_mem_methods);
}

void vixen_sqlite_reset(void) {
    /* Connections still open are abandoned: their memory goes with the gc
     * sweep that follows. sqlite3_shutdown resets SQLite's globals so the
     * next connect() initialises from scratch. */
    sqlite3_shutdown();
    vx_stores = NULL;
    VX_MEM_HEAD = NULL;
}
