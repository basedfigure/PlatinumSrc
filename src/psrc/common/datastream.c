#include "datastream.h"
#include "filesystem.h"
#include "logging.h"
#include "../debug.h"
#include "../platform.h"

#include <string.h>
#include <stdlib.h>
#ifndef PSRC_COMMON_DATASTREAM_USESTDIO
    #include <fcntl.h>
    #include <unistd.h>
    #ifndef O_BINARY
        #define O_BINARY 0
    #endif
#else
    #include <stdio.h>
#endif

void ds_openmem(void* b, size_t sz, ds_mem_freecb freecb, void* freectx, struct datastream* ds) {
    ds->buf = b;
    ds->pos = 0;
    ds->passed = 0;
    ds->datasz = sz;
    ds->mem.free = freecb;
    ds->mem.freectx = freectx;
    ds->path = "<memory>";
    ds->atend = 0;
    ds->unget = 0;
    ds->mode = DS_MODE_MEM;
}
bool ds_openfile(const char* p, size_t bufsz, struct datastream* ds) {
    {
        int tmp = isFile(p);
        if (tmp < 1) {
            if (tmp) {
                #if DEBUG(1)
                plog(LL_ERROR | LF_DEBUG | LF_FUNC, LE_NOEXIST(p));
                #endif
            } else {
                plog(LL_ERROR | LF_FUNC, LE_ISDIR(p));
            }
            return false;
        }
    }
    #ifndef PSRC_COMMON_DATASTREAM_USESTDIO
    if ((ds->file.fd = open(p, O_RDONLY | O_BINARY, 0)) < 0) {
        plog(LL_WARN | LF_FUNC, LE_CANTOPEN(p, errno));
        return false;
    }
    #else
    if (!(ds->file.f = fopen(p, "rb"))) {
        plog(LL_WARN | LF_FUNC, LE_CANTOPEN(p, errno));
        return false;
    }
    #endif
    if (!bufsz) {
        #if PLATFORM == PLAT_DREAMCAST
        bufsz = 2048 * 4; // 4 CD sectors (8192)
        #else
        bufsz = 512 * 8; // 8 HDD sectors (4096)
        #endif
    }
    ds->buf = malloc(bufsz);
    if (!ds->buf) {
        #ifndef PSRC_COMMON_DATASTREAM_USESTDIO
        close(ds->file.fd);
        #else
        fclose(ds->file.f);
        #endif
        return false;
    }
    ds->pos = 0;
    ds->passed = 0;
    ds->datasz = 0;
    ds->bufsz = bufsz;
    ds->path = strpath(p);
    ds->atend = 0;
    ds->unget = 0;
    ds->mode = DS_MODE_FILE;
    return true;
}
bool ds_opencb(ds_cb_readcb readcb, void* readctx, size_t bufsz, ds_cb_closecb closecb, void* closectx, struct datastream* ds) {
    if (!bufsz) bufsz = 4096;
    ds->buf = malloc(bufsz);
    if (!ds->buf) return false;
    ds->pos = 0;
    ds->passed = 0;
    ds->datasz = 0;
    ds->bufsz = bufsz;
    ds->cb.read = readcb;
    ds->cb.readctx = readctx;
    ds->cb.close = closecb;
    ds->cb.closectx = closectx;
    ds->path = "<callback>";
    ds->atend = 0;
    ds->unget = 0;
    ds->mode = DS_MODE_CB;
    return true;
}
bool ds_opensect(struct datastream* ids, size_t lim, size_t bufsz, struct datastream* ds) {
    if (!bufsz) bufsz = 256;
    ds->buf = malloc(bufsz);
    if (!ds->buf) return false;
    ds->pos = 0;
    ds->passed = 0;
    ds->datasz = 0;
    ds->bufsz = bufsz;
    ds->sect.ds = ids;
    ds->sect.lim = lim;
    ds->path = ids->path;
    ds->atend = 0;
    ds->unget = 0;
    ds->mode = DS_MODE_SECT;
    return true;
}

void ds_close(struct datastream* ds) {
    switch (ds->mode) {
        case DS_MODE_MEM:
            if (ds->mem.free) ds->mem.free(ds->mem.freectx, ds->buf);
            break;
        case DS_MODE_FILE:
            free(ds->buf);
            #ifndef PSRC_COMMON_DATASTREAM_USESTDIO
            close(ds->file.fd);
            #else
            fclose(ds->file.f);
            #endif
            free(ds->path);
            break;
        case DS_MODE_CB:
            free(ds->buf);
            if (ds->cb.close) ds->cb.close(ds->cb.closectx);
            break;
        case DS_MODE_SECT:
            free(ds->buf);
            break;
    }
}

int ds_text_getc(struct datastream* ds) {
    int c;
    do {
        c = ds_text__getc_inline(ds);
    } while (c == '\r' || !c);
    return c;
}

size_t ds_bin_read(struct datastream* ds, size_t l, void* b) {
    if (!l) return 0;
    size_t r;
    #if 0
    if (ds->unget) {
        ds->unget = 0;
        *(uint8_t*)b = ds->last;
        if (l == 1) return 1;
        r = 1;
    } else {
        r = 0;
    }
    #else
    r = 0;
    #endif
    size_t a = ds->datasz - ds->pos;
    if (!a) {
        if (!ds__refill(ds)) return r;
        a = ds->datasz/* - ds->pos*/;
        if (!a) return r;
    }
    while (a < l) {
        memcpy((uint8_t*)b + r, ds->buf + ds->pos, a);
        r += a;
        l -= a;
        if (!ds__refill(ds)) return r;
        a = ds->datasz/* - ds->pos*/;
        if (!a) return r;
    }
    memcpy((uint8_t*)b + r, ds->buf + ds->pos, l);
    ds->pos += l;
    return r + l;
}
size_t ds_bin_skip(struct datastream* ds, size_t l) {
    if (!l) return 0;
    size_t r = 0;
    size_t a = ds->datasz - ds->pos;
    if (!a) {
        if (!ds__refill(ds)) return r;
        a = ds->datasz/* - ds->pos*/;
        if (!a) return r;
    }
    while (a < l) {
        r += a;
        l -= a;
        if (!ds__refill(ds)) return r;
        a = ds->datasz/* - ds->pos*/;
        if (!a) return r;
    }
    ds->pos += l;
    return r + l;
}

int ds_text__getc(struct datastream* ds) {
    return ds_text__getc_inline(ds);
}

bool ds__refill(struct datastream* ds) {
    if (ds->atend) return false;
    if (ds->mode == DS_MODE_MEM) {
        ds->atend = 1;
        return false;
    }
    ds->passed += ds->datasz;
    ds->pos = 0;
    if (ds->mode == DS_MODE_FILE) {
        #ifndef PSRC_COMMON_DATASTREAM_USESTDIO
            ssize_t r = read(ds->file.fd, ds->buf, ds->bufsz);
            if (r == 0 || r == -1) {
                ds->datasz = 0;
                ds->atend = 1;
                return false;
            }
            ds->datasz = r;
        #else
            if (feof(ds->file.f)) {
                ds->datasz = 0;
                ds->atend = 1;
                return false;
            }
            ds->datasz = fread(ds->buf, 1, ds->bufsz, ds->file.f);
        #endif
    } else if (ds->mode == DS_MODE_CB) {
        if (!ds->cb.read(ds->cb.readctx, ds->buf, ds->bufsz, &ds->datasz)) {
            ds->datasz = 0;
            ds->atend = 1;
            return false;
        }
    } else {
        size_t tmp = ds->sect.lim - ds->passed;
        if (!tmp) {
            ds->datasz = 0;
            ds->atend = 1;
            return false;
        }
        if (tmp > ds->bufsz) tmp = ds->bufsz;
        size_t r = ds_bin_read(ds->sect.ds, tmp, ds->buf);
        if (!r && ds_bin_atend(ds->sect.ds)) {
            ds->datasz = 0;
            ds->atend = 1;
            return false;
        }
        ds->datasz = r;
    }
    return true;
}
