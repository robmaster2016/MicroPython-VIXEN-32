/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 VIXEN-32 MicroPython port contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* VIXEN-32 disks as a MicroPython VFS: vixen.Disk(unit).
 *
 * The filesystem is NOT parsed here. Every operation goes through the resident
 * kernel's disk services (devkit common/kernel_api.h), the same FatFs instance
 * BASIC/DOS use. That keeps the kernel the only writer of the FAT: a second
 * FatFs on the same card (MicroPython's VfsFat over raw sectors) would work
 * from stale caches and could corrupt it. Long file names and sub-directories
 * come for free.
 *
 * Units (KUNIT_*): 0 = A:, 1 = B: (disk images mounted from the F12 menu),
 * 3 = the SD card itself. Unit 3 only answers if the installed BOOT.BIN
 * enables it (VX_DRIVE_C); otherwise mounting it raises ENODEV.
 *
 * SD CARD OWNERSHIP. Any access to unit 3 makes the kernel claim the card from
 * the MEGA65's QNICE side, and it keeps it until kdisk_release(). While it is
 * held the F12 file browser cannot reach the card -- it stops the core with
 * "SD Card: Unknown error while trying to browse". So, like BASIC, every
 * operation releases the card again as soon as nothing of ours is open on it
 * (disk_done()). Open files keep it claimed: releasing would unmount the
 * filesystem their handles depend on.
 *
 * Because a release unmounts the kernel's filesystem, the kernel's own current
 * directory cannot be relied on. Each Disk object keeps its cwd itself and
 * hands the kernel absolute, normalised paths only.
 *
 * Limitations of the kernel API, surfaced as-is:
 *  - open() modes: 'r' and 'w' only. The kernel has no append and no
 *    read+write handle, so 'a' and '+' raise EOPNOTSUPP.
 *  - Data written is committed when the file is closed (no fsync service).
 *  - One directory listing at a time: ilistdir() reads the whole directory
 *    and closes it before returning.
 */

#include <string.h>

#include "py/runtime.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "py/objstr.h"
#include "extmod/vfs.h"

#include "kernel_api.h"
#include "uart.h"

#if MICROPY_VFS

/* ---- kernel calls ------------------------------------------------------ */

/* The kernel table has no safety net: an entry this BOOT.BIN lacks is NULL
 * and calling it jumps to 0. Check before every service we use. */
static void disk_require(uint32_t entry) {
    if (KERNEL_ENTRY(entry) == NULL) {
        mp_raise_OSError(MP_ENODEV);
    }
}

/* Filesystem calls return ChaN's FRESULT negated (kernel_api.h). */
static int disk_errno(int r) {
    switch (-r) {
        case 3:  /* FR_NOT_READY */
        case 11: /* FR_INVALID_DRIVE */
        case 12: /* FR_NOT_ENABLED */
        case 13: /* FR_NO_FILESYSTEM */
            return MP_ENODEV;
        case 4:  /* FR_NO_FILE */
        case 5:  /* FR_NO_PATH */
            return MP_ENOENT;
        case 6:  /* FR_INVALID_NAME */
        case 9:  /* FR_INVALID_OBJECT */
        case 19: /* FR_INVALID_PARAMETER */
            return MP_EINVAL;
        case 7:  /* FR_DENIED (also: directory not empty) */
            return MP_EACCES;
        case 8:  /* FR_EXIST */
            return MP_EEXIST;
        case 10: /* FR_WRITE_PROTECTED */
            return MP_EROFS;
        case 15: /* FR_TIMEOUT */
            return MP_ETIMEDOUT;
        case 16: /* FR_LOCKED */
            return MP_EBUSY;
        case 17: /* FR_NOT_ENOUGH_CORE */
            return MP_ENOMEM;
        case 18: /* FR_TOO_MANY_OPEN_FILES */
            return MP_EMFILE;
        default: /* FR_DISK_ERR, FR_INT_ERR, FR_MKFS_ABORTED, ... */
            return MP_EIO;
    }
}

/* Files of ours currently open on the SD card unit; while > 0 the card stays
 * claimed. Reset on soft reboot: the GC finaliser of every file closes it. */
static uint32_t sd_open_files;

/* End of an operation on `unit`: hand the SD card back to the F12 browser
 * unless one of our files still needs it. */
static void disk_done(uint32_t unit) {
    if (unit == KUNIT_SD && sd_open_files == 0u && KERNEL_ENTRY(KENT_DISK_RELEASE) != NULL) {
        kdisk_release(KUNIT_SD);
    }
}

/* disk_done(), then raise if the kernel call failed. */
static void disk_finish(uint32_t unit, int r) {
    disk_done(unit);
    if (r < 0) {
        mp_raise_OSError(disk_errno(r));
    }
}

/* ---- the VFS object ---------------------------------------------------- */

typedef struct _vixen_disk_obj_t {
    mp_obj_base_t base;
    uint32_t unit;
    bool readonly;
    vstr_t cwd;          /* absolute, normalised, "/" at the root */
} vixen_disk_obj_t;

/* Turn a path from the VFS ("/x" absolute on the unit, "x" relative to our
 * cwd, "" = the cwd) into an absolute path without "." / ".." / "//". */
static const char *disk_abs(vixen_disk_obj_t *self, const char *path, vstr_t *out) {
    vstr_init(out, 32);
    vstr_add_char(out, '/');
    for (int pass = (path[0] == '/') ? 1 : 0; pass < 2; pass++) {
        const char *p = (pass == 0) ? vstr_null_terminated_str(&self->cwd) : path;
        while (*p) {
            while (*p == '/') {
                p++;
            }
            const char *seg = p;
            while (*p && *p != '/') {
                p++;
            }
            size_t n = (size_t)(p - seg);
            if (n == 0 || (n == 1 && seg[0] == '.')) {
                continue;
            }
            if (n == 2 && seg[0] == '.' && seg[1] == '.') {
                while (out->len > 1 && out->buf[out->len - 1] != '/') {
                    out->len--;
                }
                if (out->len > 1) {
                    out->len--;   /* drop the separator too */
                }
                continue;
            }
            if (out->len > 1) {
                vstr_add_char(out, '/');
            }
            vstr_add_strn(out, seg, n);
        }
    }
    return vstr_null_terminated_str(out);
}

static bool disk_is_dir(vixen_disk_obj_t *self, const char *abs_path) {
    if (kdisk_dir_open(self->unit, abs_path) < 0) {
        return false;
    }
    kdisk_dir_close();
    return true;
}

static mp_obj_t vixen_disk_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    mp_int_t unit = mp_obj_get_int(args[0]);
    if (unit < 0 || unit >= (mp_int_t)KUNIT_COUNT) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid unit"));
    }
    vixen_disk_obj_t *self = mp_obj_malloc(vixen_disk_obj_t, type);
    self->unit = (uint32_t)unit;
    self->readonly = false;
    vstr_init(&self->cwd, 16);
    vstr_add_char(&self->cwd, '/');
    return MP_OBJ_FROM_PTR(self);
}

static void vixen_disk_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "Disk(%u)", (unsigned)self->unit);
}

static mp_obj_t vixen_disk_mount(mp_obj_t self_in, mp_obj_t readonly, mp_obj_t mkfs) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (mp_obj_is_true(mkfs)) {
        mp_raise_OSError(MP_EPERM);
    }
    self->readonly = mp_obj_is_true(readonly);
    /* Probe: refuse to mount a unit with no filesystem behind it. */
    disk_require(KENT_DISK_DIR_OPEN);
    disk_require(KENT_DISK_DIR_CLOSE);
    int r = kdisk_dir_open(self->unit, "/");
    if (r >= 0) {
        kdisk_dir_close();
    }
    disk_finish(self->unit, r);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(vixen_disk_mount_obj, vixen_disk_mount);

static mp_obj_t vixen_disk_umount(mp_obj_t self_in) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    disk_done(self->unit);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vixen_disk_umount_obj, vixen_disk_umount);

static void disk_require_writable(vixen_disk_obj_t *self) {
    if (self->readonly) {
        mp_raise_OSError(MP_EROFS);
    }
}

/* ---- files ------------------------------------------------------------- */

typedef struct _vixen_file_obj_t {
    mp_obj_base_t base;
    int handle;          /* kernel handle, -1 once closed */
    uint32_t unit;
    bool writable;
    uint32_t pos;        /* tracked here: the kernel has no tell */
    uint32_t size;       /* at open (read) or high-water mark (write) */
} vixen_file_obj_t;

static mp_uint_t vixen_file_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    vixen_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle < 0 || self->writable) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    int r = kdisk_read(self->handle, buf, size);
    if (r < 0) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    self->pos += (uint32_t)r;
    return (mp_uint_t)r;
}

static mp_uint_t vixen_file_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    vixen_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle < 0 || !self->writable) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    int r = kdisk_write(self->handle, buf, size);
    if (r < 0) {
        *errcode = disk_errno(r);
        return MP_STREAM_ERROR;
    }
    self->pos += (uint32_t)r;
    if (self->pos > self->size) {
        self->size = self->pos;
    }
    return (mp_uint_t)r;
}

static mp_uint_t vixen_file_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    vixen_file_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (request == MP_STREAM_CLOSE) {
        if (self->handle >= 0) {
            kdisk_close(self->handle);
            self->handle = -1;
            if (self->unit == KUNIT_SD && sd_open_files > 0u) {
                sd_open_files--;
            }
            disk_done(self->unit);
        }
        return 0;
    }
    if (self->handle < 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    switch (request) {
        case MP_STREAM_FLUSH:
            /* no sync service: data is committed on close */
            return 0;
        case MP_STREAM_SEEK: {
            struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
            mp_off_t base = 0;
            if (s->whence == MP_SEEK_CUR) {
                base = self->pos;
            } else if (s->whence == MP_SEEK_END) {
                base = self->size;
            }
            mp_off_t target = base + s->offset;
            if (target < 0) {
                *errcode = MP_EINVAL;
                return MP_STREAM_ERROR;
            }
            int r = kdisk_seek(self->handle, (uint32_t)target);
            if (r < 0) {
                *errcode = MP_EIO;
                return MP_STREAM_ERROR;
            }
            self->pos = (uint32_t)target;
            s->offset = target;
            return 0;
        }
        default:
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
    }
}

static void vixen_file_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    mp_printf(print, "<io.%s>", mp_obj_get_type_str(self_in));
}

static const mp_rom_map_elem_t vixen_file_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(vixen_file_locals_dict, vixen_file_locals_dict_table);

static const mp_stream_p_t vixen_fileio_stream_p = {
    .read = vixen_file_read,
    .write = vixen_file_write,
    .ioctl = vixen_file_ioctl,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    vixen_type_fileio,
    MP_QSTR_FileIO,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, vixen_file_print,
    protocol, &vixen_fileio_stream_p,
    locals_dict, &vixen_file_locals_dict
    );

static const mp_stream_p_t vixen_textio_stream_p = {
    .read = vixen_file_read,
    .write = vixen_file_write,
    .ioctl = vixen_file_ioctl,
    .is_text = true,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    vixen_type_textio,
    MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, vixen_file_print,
    protocol, &vixen_textio_stream_p,
    locals_dict, &vixen_file_locals_dict
    );

static mp_obj_t vixen_disk_open(mp_obj_t self_in, mp_obj_t path_in, mp_obj_t mode_in) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *mode = mp_obj_str_get_str(mode_in);
    const mp_obj_type_t *type = &vixen_type_textio;
    bool writable = false;

    for (; *mode; mode++) {
        switch (*mode) {
            case 'r': writable = false; break;
            case 'w': writable = true; break;
            case 'b': type = &vixen_type_fileio; break;
            case 't': type = &vixen_type_textio; break;
            case 'a': /* the kernel has no append / read+write handle */
            case '+':
            case 'x':
                mp_raise_OSError(MP_EOPNOTSUPP);
            default:
                mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
        }
    }
    if (writable) {
        disk_require_writable(self);
    }
    disk_require(KENT_DISK_OPEN);
    disk_require(KENT_DISK_CLOSE);
    disk_require(KENT_DISK_FILE_SIZE);
    vstr_t abs;
    const char *path = disk_abs(self, mp_obj_str_get_str(path_in), &abs);
    /* Allocate before touching the kernel, so a MemoryError can neither leak
     * a handle nor leave the card claimed. */
    vixen_file_obj_t *o = mp_obj_malloc_with_finaliser(vixen_file_obj_t, type);
    o->handle = -1;
    o->unit = self->unit;
    o->writable = writable;
    o->pos = 0;
    o->size = 0;

    if (!writable) {
        int r = kdisk_file_size(self->unit, path);
        if (r < 0) {
            disk_finish(self->unit, r);
        }
        o->size = (uint32_t)r;
    }
    int h = kdisk_open(self->unit, path, writable ? 1u : 0u);
    if (h < 0) {
        disk_finish(self->unit, h);
    }
    o->handle = h;
    if (self->unit == KUNIT_SD) {
        sd_open_files++;
    }
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_3(vixen_disk_open_obj, vixen_disk_open);

/* ---- directories ------------------------------------------------------- */

static mp_obj_t vixen_disk_ilistdir(mp_obj_t self_in, mp_obj_t path_in) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    bool is_str = mp_obj_get_type(path_in) == &mp_type_str;
    disk_require(KENT_DISK_DIR_OPEN);
    disk_require(KENT_DISK_DIR_NEXT);
    disk_require(KENT_DISK_DIR_CLOSE);
    vstr_t abs;
    const char *path = disk_abs(self, mp_obj_str_get_str(path_in), &abs);
    mp_obj_t list = mp_obj_new_list(0, NULL);

    /* The kernel has a single directory cursor that holds the card while it
     * is open, so read the whole listing now, close it and release the card
     * again -- also if building the list raises (MemoryError). */
    int r = kdisk_dir_open(self->unit, path);
    if (r < 0) {
        disk_finish(self->unit, r);
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        kdirent_t ent;
        while ((r = kdisk_dir_next(&ent)) > 0) {
            ent.name[sizeof(ent.name) - 1] = '\0';
            const char *fn = ent.name;
            if (fn[0] == '.' && (fn[1] == '\0' || (fn[1] == '.' && fn[2] == '\0'))) {
                continue;
            }
            mp_obj_t t[4];
            t[0] = is_str ? mp_obj_new_str_from_cstr(fn)
                          : mp_obj_new_bytes((const byte *)fn, strlen(fn));
            t[1] = MP_OBJ_NEW_SMALL_INT(ent.is_dir ? MP_S_IFDIR : MP_S_IFREG);
            t[2] = MP_OBJ_NEW_SMALL_INT(0);
            t[3] = mp_obj_new_int_from_uint(ent.size);
            mp_obj_list_append(list, mp_obj_new_tuple(4, t));
        }
        nlr_pop();
        kdisk_dir_close();
        disk_finish(self->unit, r);
    } else {
        kdisk_dir_close();
        disk_done(self->unit);
        nlr_jump(nlr.ret_val);
    }
    return mp_getiter(list, NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_2(vixen_disk_ilistdir_obj, vixen_disk_ilistdir);

static mp_obj_t vixen_disk_chdir(mp_obj_t self_in, mp_obj_t path_in) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    disk_require(KENT_DISK_DIR_OPEN);
    disk_require(KENT_DISK_DIR_CLOSE);
    vstr_t abs;
    const char *path = disk_abs(self, mp_obj_str_get_str(path_in), &abs);
    bool ok = disk_is_dir(self, path);
    disk_done(self->unit);
    if (!ok) {
        mp_raise_OSError(MP_ENOENT);
    }
    vstr_reset(&self->cwd);
    vstr_add_str(&self->cwd, path);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vixen_disk_chdir_obj, vixen_disk_chdir);

static mp_obj_t vixen_disk_getcwd(mp_obj_t self_in) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_str(self->cwd.buf, self->cwd.len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(vixen_disk_getcwd_obj, vixen_disk_getcwd);

static mp_obj_t vixen_disk_mkdir(mp_obj_t self_in, mp_obj_t path_in) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    disk_require_writable(self);
    disk_require(KENT_DISK_MKDIR);
    vstr_t abs;
    disk_finish(self->unit, kdisk_mkdir(self->unit, disk_abs(self, mp_obj_str_get_str(path_in), &abs)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vixen_disk_mkdir_obj, vixen_disk_mkdir);

/* FatFs' f_unlink removes files and empty directories alike. */
static mp_obj_t vixen_disk_remove(mp_obj_t self_in, mp_obj_t path_in) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    disk_require_writable(self);
    disk_require(KENT_DISK_DELETE);
    vstr_t abs;
    disk_finish(self->unit, kdisk_delete(self->unit, disk_abs(self, mp_obj_str_get_str(path_in), &abs)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vixen_disk_remove_obj, vixen_disk_remove);

static mp_obj_t vixen_disk_rename(mp_obj_t self_in, mp_obj_t old_in, mp_obj_t new_in) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    disk_require_writable(self);
    disk_require(KENT_DISK_RENAME);
    vstr_t abs_old, abs_new;
    const char *old_path = disk_abs(self, mp_obj_str_get_str(old_in), &abs_old);
    const char *new_path = disk_abs(self, mp_obj_str_get_str(new_in), &abs_new);
    disk_finish(self->unit, kdisk_rename(self->unit, old_path, new_path));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(vixen_disk_rename_obj, vixen_disk_rename);

static mp_obj_t vixen_disk_stat(mp_obj_t self_in, mp_obj_t path_in) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    disk_require(KENT_DISK_DIR_OPEN);
    disk_require(KENT_DISK_DIR_CLOSE);
    disk_require(KENT_DISK_FILE_SIZE);
    vstr_t abs;
    const char *path = disk_abs(self, mp_obj_str_get_str(path_in), &abs);
    mp_int_t mode = MP_S_IFDIR;
    uint32_t size = 0;

    if (!disk_is_dir(self, path)) {
        int r = kdisk_file_size(self->unit, path);
        disk_finish(self->unit, r);
        mode = MP_S_IFREG;
        size = (uint32_t)r;
    } else {
        disk_done(self->unit);
    }
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    for (size_t i = 0; i < 10; i++) {
        t->items[i] = MP_OBJ_NEW_SMALL_INT(0);
    }
    t->items[0] = MP_OBJ_NEW_SMALL_INT(mode);
    t->items[6] = mp_obj_new_int_from_uint(size);
    return MP_OBJ_FROM_PTR(t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(vixen_disk_stat_obj, vixen_disk_stat);

static mp_obj_t vixen_disk_statvfs(mp_obj_t self_in, mp_obj_t path_in) {
    vixen_disk_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)path_in;
    disk_require(KENT_DISK_SECTORS);
    disk_require(KENT_DISK_FREE);
    uint32_t sectors = kdisk_sectors(self->unit);
    uint32_t free_bytes = 0;
    disk_finish(self->unit, kdisk_free(self->unit, &free_bytes));

    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    t->items[0] = MP_OBJ_NEW_SMALL_INT(512);                    /* f_bsize */
    t->items[1] = MP_OBJ_NEW_SMALL_INT(512);                    /* f_frsize */
    t->items[2] = mp_obj_new_int_from_uint(sectors);            /* f_blocks */
    t->items[3] = mp_obj_new_int_from_uint(free_bytes / 512u);  /* f_bfree */
    t->items[4] = t->items[3];                                  /* f_bavail */
    t->items[5] = MP_OBJ_NEW_SMALL_INT(0);                      /* f_files */
    t->items[6] = MP_OBJ_NEW_SMALL_INT(0);                      /* f_ffree */
    t->items[7] = MP_OBJ_NEW_SMALL_INT(0);                      /* f_favail */
    t->items[8] = MP_OBJ_NEW_SMALL_INT(self->readonly ? 1 : 0); /* f_flag: ST_RDONLY */
    t->items[9] = MP_OBJ_NEW_SMALL_INT(255);                    /* f_namemax */
    return MP_OBJ_FROM_PTR(t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(vixen_disk_statvfs_obj, vixen_disk_statvfs);

static mp_import_stat_t vixen_disk_import_stat(void *self_in, const char *path) {
    vixen_disk_obj_t *self = self_in;
    if (KERNEL_ENTRY(KENT_DISK_DIR_OPEN) == NULL || KERNEL_ENTRY(KENT_DISK_DIR_CLOSE) == NULL
        || KERNEL_ENTRY(KENT_DISK_FILE_SIZE) == NULL) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    vstr_t abs;
    path = disk_abs(self, path, &abs);
    mp_import_stat_t st = MP_IMPORT_STAT_NO_EXIST;
    if (disk_is_dir(self, path)) {
        st = MP_IMPORT_STAT_DIR;
    } else if (kdisk_file_size(self->unit, path) >= 0) {
        st = MP_IMPORT_STAT_FILE;
    }
    disk_done(self->unit);
    return st;
}

static const mp_rom_map_elem_t vixen_disk_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&vixen_disk_mount_obj) },
    { MP_ROM_QSTR(MP_QSTR_umount), MP_ROM_PTR(&vixen_disk_umount_obj) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&vixen_disk_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&vixen_disk_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&vixen_disk_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_ilistdir), MP_ROM_PTR(&vixen_disk_ilistdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&vixen_disk_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&vixen_disk_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&vixen_disk_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&vixen_disk_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&vixen_disk_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_statvfs), MP_ROM_PTR(&vixen_disk_statvfs_obj) },
};
static MP_DEFINE_CONST_DICT(vixen_disk_locals_dict, vixen_disk_locals_dict_table);

static const mp_vfs_proto_t vixen_disk_proto = {
    .import_stat = vixen_disk_import_stat,
};

MP_DEFINE_CONST_OBJ_TYPE(
    vixen_type_disk,
    MP_QSTR_Disk,
    MP_TYPE_FLAG_NONE,
    make_new, vixen_disk_make_new,
    print, vixen_disk_print,
    protocol, &vixen_disk_proto,
    locals_dict, &vixen_disk_locals_dict
    );

/* ---- automount at boot ------------------------------------------------- */

/* Mount whatever answers: the SD card at /sd, the F12 disk images at /a and
 * /b. A unit that is absent, disabled or unformatted is silently skipped.
 * MicroPython's own folder on the card is /sd/vixen-32/apps/micropython (see
 * main.c); its lib/ and examples/ are added to sys.path, so modules there
 * import directly (`import lvgl_demo`). */
void vixen_disk_automount(void) {
    static const struct {
        uint8_t unit;
        qstr mount_point;
    } units[] = {
        { KUNIT_SD, MP_QSTR__slash_sd },
        { KUNIT_A, MP_QSTR__slash_a },
        { KUNIT_B, MP_QSTR__slash_b },
    };
    sd_open_files = 0u;   /* soft reboot: the previous session's files are gone */
    if (KERNEL_ENTRY(KENT_DISK_DIR_OPEN) == NULL || KERNEL_ENTRY(KENT_DISK_DIR_CLOSE) == NULL) {
        return;   /* kernel without disk services */
    }
    for (size_t i = 0; i < MP_ARRAY_SIZE(units); i++) {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_obj_t args[2] = {
                MP_OBJ_NEW_SMALL_INT(units[i].unit),
                MP_OBJ_NEW_QSTR(units[i].mount_point),
            };
            args[0] = vixen_disk_make_new(&vixen_type_disk, 1, 0, args);
            mp_vfs_mount(2, args, (mp_map_t *)&mp_const_empty_map);
            if (units[i].unit == KUNIT_SD) {
                mp_obj_list_append(mp_sys_path, mp_obj_new_str_from_cstr("/sd/vixen-32/apps/micropython/lib"));
                mp_obj_list_append(mp_sys_path, mp_obj_new_str_from_cstr("/sd/vixen-32/apps/micropython/examples"));
            }
            nlr_pop();
        }
        /* else: unit not usable -- skip it */
    }
    disk_done(KUNIT_SD);   /* whatever happened above, the card goes back */
}

/* Make MicroPython's folder on the card the current directory, if it exists
 * (main.c, before the autostart). Quietly does nothing otherwise. */
void vixen_disk_enter_app_dir(void) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_vfs_chdir(mp_obj_new_str_from_cstr("/sd/vixen-32/apps/micropython"));
        nlr_pop();
    }
}

/* ---- module vixen ------------------------------------------------------ */

#if VIXEN_LVGL
extern const mp_obj_fun_builtin_var_t vixen_lv_display_obj;
extern const mp_obj_fun_builtin_fixed_t vixen_lv_dither_obj;
extern const mp_obj_fun_builtin_fixed_t vixen_lv_keys_obj;
#endif

extern const mp_obj_fun_builtin_fixed_t vixen_add_entropy_obj;
extern const mp_obj_fun_builtin_var_t vixen_seed_file_obj;

/* vixen.tile_scroll(x, y): text-layer scroll in pixels, to align the console
 * with the screen edges (see uart.c). */
static mp_obj_t vixen_tile_scroll(mp_obj_t x_in, mp_obj_t y_in) {
    uart_set_tile_scroll((uint32_t)mp_obj_get_int(x_in) & 0x1FFu, (uint32_t)mp_obj_get_int(y_in) & 0x1FFu);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vixen_tile_scroll_obj, vixen_tile_scroll);

static const mp_rom_map_elem_t vixen_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_vixen) },
    { MP_ROM_QSTR(MP_QSTR_Disk), MP_ROM_PTR(&vixen_type_disk) },
    { MP_ROM_QSTR(MP_QSTR_UNIT_A), MP_ROM_INT(KUNIT_A) },
    { MP_ROM_QSTR(MP_QSTR_UNIT_B), MP_ROM_INT(KUNIT_B) },
    { MP_ROM_QSTR(MP_QSTR_UNIT_SD), MP_ROM_INT(KUNIT_SD) },
    { MP_ROM_QSTR(MP_QSTR_tile_scroll), MP_ROM_PTR(&vixen_tile_scroll_obj) },
    { MP_ROM_QSTR(MP_QSTR_add_entropy), MP_ROM_PTR(&vixen_add_entropy_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_file), MP_ROM_PTR(&vixen_seed_file_obj) },
    #if VIXEN_LVGL
    { MP_ROM_QSTR(MP_QSTR_lv_display), MP_ROM_PTR(&vixen_lv_display_obj) },
    { MP_ROM_QSTR(MP_QSTR_lv_dither), MP_ROM_PTR(&vixen_lv_dither_obj) },
    { MP_ROM_QSTR(MP_QSTR_lv_keys), MP_ROM_PTR(&vixen_lv_keys_obj) },
    #endif
};
static MP_DEFINE_CONST_DICT(vixen_module_globals, vixen_module_globals_table);

const mp_obj_module_t mp_module_vixen = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&vixen_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_vixen, mp_module_vixen);

#endif // MICROPY_VFS
