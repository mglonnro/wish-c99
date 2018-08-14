/**
 * Copyright (C) 2018, ControlThings Oy Ab
 * Copyright (C) 2018, André Kaustell
 * Copyright (C) 2018, Jan Nyman
 * Copyright (C) 2018, Jepser Lökfors
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * @license Apache-2.0
 */
#include "wish_fs.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

/* Unix port specific file I/O functions implemented using Posix sys
 * calls */

wish_file_t my_fs_open(const char *pathname) {
    return open(pathname, O_RDWR| O_CREAT, S_IRUSR|S_IWUSR);
}

int32_t my_fs_read(wish_file_t fd, void* buf, size_t count) {
    return read(fd, buf, count);
}

int32_t my_fs_write(wish_file_t fd, const void *buf, size_t count) {
    return write(fd, buf, count);
}

wish_offset_t my_fs_lseek(wish_file_t fd, wish_offset_t offset, int whence) {
    return lseek(fd, offset, whence);
}

int32_t my_fs_close(wish_file_t fd) {
    return close(fd);
}

int32_t my_fs_rename(const char *oldpath, const char *newpath) {
    return rename(oldpath, newpath);
}

int32_t my_fs_remove(const char *path) {
    return remove(path);
}
