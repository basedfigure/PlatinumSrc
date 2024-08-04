#include "filesystem.h"
#include "string.h"
#include "logging.h"

#include <stddef.h>
#include <ctype.h>
#if PLATFORM == PLAT_NXDK
    #include <windows.h>
#elif PLATFORM == PLAT_DREAMCAST
    #include <dirent.h>
#else
    #include <sys/types.h>
    #include <sys/stat.h>
    #if !(PLATFLAGS & PLATFLAG_WINDOWSLIKE)
        #include <dirent.h>
        #include <unistd.h>
    #else
        #include <windows.h>
    #endif
    #ifndef PSRC_USESDL1
        #if PLATFORM == PLAT_NXDK || PLATFORM == PLAT_GDK
            #include <SDL.h>
        #else
            #include <SDL2/SDL.h>
        #endif
    #endif
#endif
#include <stdio.h>
#include <stdbool.h>

#include "../glue.h"

int isFile(const char* p) {
    #if !(PLATFLAGS & PLATFLAG_WINDOWSLIKE)
        struct stat s;
        if (stat(p, &s)) return -1;
        if (S_ISREG(s.st_mode)) return 1;
        if (S_ISDIR(s.st_mode)) return 0;
        return 2;
    #else
        DWORD a = GetFileAttributes(p);
        if (a == INVALID_FILE_ATTRIBUTES) return -1;
        if (a & FILE_ATTRIBUTE_DIRECTORY) return 0;
        if (a & FILE_ATTRIBUTE_DEVICE) return 2;
        return 1;
    #endif
}

long getFileSize(FILE* f, bool c) {
    if (!f) return -1;
    long ret = -1;
    long tmp;
    if (!c) tmp = ftell(f);
    if (!fseek(f, 0, SEEK_END)) ret = ftell(f);
    if (c) fclose(f);
    else fseek(f, tmp, SEEK_SET);
    return ret;
}

void replpathsep(struct charbuf* b, const char* s, bool first) {
    if (first) {
        #if !(PLATFLAGS & PLATFLAG_WINDOWSLIKE)
        if (*s == '/') {
            cb_add(b, '/');
            ++s;
        }
        #endif
    }
    while (ispathsep(*s)) ++s;
    bool sep = false;
    while (1) {
        char c = *s;
        if (!c) break;
        ++s;
        if (ispathsep(c)) {
            sep = true;
        } else {
            if (sep) {
                cb_add(b, PATHSEP);
                sep = false;
            }
            cb_add(b, c);
        }
    }
}
char* mkpath(const char* s, ...) {
    struct charbuf b;
    cb_init(&b, 256);
    if (s) {
        replpathsep(&b, s, true);
    }
    va_list v;
    va_start(v, s);
    if (!s) {
        if ((s = va_arg(v, char*))) {
            replpathsep(&b, s, false);
        } else {
            goto ret;
        }
    }
    while ((s = va_arg(v, char*))) {
        cb_add(&b, PATHSEP);
        replpathsep(&b, s, false);
    }
    ret:;
    va_end(v);
    return cb_finalize(&b);
}
char* strpath(const char* s) {
    struct charbuf b;
    cb_init(&b, 256);
    replpathsep(&b, s, true);
    return cb_finalize(&b);
}
char* strrelpath(const char* s) {
    struct charbuf b;
    cb_init(&b, 256);
    replpathsep(&b, s, false);
    return cb_finalize(&b);
}
void sanfilename_cb(const char* s, char r, struct charbuf* cb) {
    char c;
    #if (PLATFLAGS & PLATFLAG_WINDOWSLIKE)
    int b = cb->len;
    #endif
    while ((c = *s)) {
        if (ispathsep(c)) {if (r) cb_add(cb, r);}
        #if (PLATFLAGS & PLATFLAG_WINDOWSLIKE)
        else if ((c >= 1 && c <= 31) || c == '<' || c == '>' || c == ':' ||
            c == '"' || c == '|' || c == '?' || c == '*') {if (r) cb_add(cb, r);}
        #endif
        else cb_add(cb, c);
        ++s;
    }
    #if (PLATFLAGS & PLATFLAG_WINDOWSLIKE)
    if (b == cb->len) return;
    int i = b;
    while (i < cb->len) {
        if (cb->data[i] == ' ') cb->data[i] = '_';
        else break;
        ++i;
    }
    if (i == b) {
        char* d = cb->data + b;
        int l = cb->len - b;
        if (((l == 3 || (l > 3 && d[3] == '.')) &&
             (!strncasecmp(d, "con", 3) || !strncasecmp(d, "prn", 3) || !strncasecmp(d, "aux", 3) || !strncasecmp(d, "nul", 3))) ||
            ((l == 4 || (l > 4 && d[4] == '.')) &&
             (!strncasecmp(d, "com", 3) || !strncasecmp(d, "lpt", 3)) &&
             ((c >= '0' && c <= '9') || c == '\xB2' || c == '\xB3' || c == '\xB9')) ||
            ((l == 5 || (l > 5 && d[5] == '.')) &&
             (!strncasecmp(d, "com", 3) || !strncasecmp(d, "lpt", 3)) && d[4] == '\xC2' &&
             (c == '\xB2' || c == '\xB3' || c == '\xB9')) ||
            ((l == 6 || (l > 6 && d[6] == '.')) &&
             !strncasecmp(d, "conin$", 6)) ||
            ((l == 7 || (l > 7 && d[7] == '.')) &&
             !strncasecmp(d, "conout$", 7))) d[2] = (r) ? r : '_';
        if ((l == 1 && d[0] == '.') ||
            (l == 2 && d[0] == '.' && d[1] == '.')) return;
        if (l > 2 && d[0] == '.' && d[1] == '.' && d[2] == '.') d[2] = (r && r != '.') ? r : '_';
    }
    i = cb->len - 1;
    while (i >= b) {
        if (cb->data[i] == '.') --cb->len;
        else break;
        --i;
    }
    while (i >= b) {
        if (cb->data[i] == ' ') cb->data[i] = '_';
        else break;
        --i;
    }
    #endif
}
char* sanfilename(const char* s, char r) {
    struct charbuf cb;
    cb_init(&cb, 64);
    sanfilename_cb(s, r, &cb);
    return cb_finalize(&cb);
}
char* restrictpath(const char* s, const char* inseps, char outsep, char outrepl) {
    struct charbuf cb;
    cb_init(&cb, 256);
    int ct;
    char** dl = splitstr(s, inseps, false, &ct);
    for (int i = 0; i < ct; ++i) {
        char* d = dl[i];
        if (!*d) goto skip;
        if (d[0] == '.') {
            if (!d[1]) {
                goto skip;
            } else if (d[1] == '.' && !d[2]) {
                while (cb.len > 0) {
                    --cb.len;
                    if (cb.data[cb.len] == outsep) break;
                }
                goto skip;
            }
        }
        cb_add(&cb, outsep);
        #if !(PLATFLAGS & PLATFLAG_WINDOWSLIKE)
        cb_addstr(&cb, d);
        #else
        sanfilename_cb(d, outrepl, &cb);
        #endif
        skip:;
        free(d);
    }
    free(dl);
    return cb_finalize(&cb);
}

bool md(const char* p) {
    struct charbuf cb;
    cb_init(&cb, 256);
    char c = *p;
    if (ispathsep(c)) {cb_add(&cb, c); ++p;}
    while ((c == *p)) {
        if (ispathsep(c)) {
            int t = isFile(cb_peek(&cb));
            if (t < 0) {
                bool cond;
                #if PLATFORM != PLAT_NXDK
                cond = (mkdir(cb.data) < 0);
                #else
                cond = (!CreateDirectory(cb.data, NULL));
                #endif
                if (cond) {
                    cb_dump(&cb);
                    return false;
                }
            } else if (t > 0) {
                // is a file
                cb_dump(&cb);
                return false;
            }
        }
        cb_add(&cb, c);
        ++p;
    }
    cb_dump(&cb);
    return true;
}

char** ls(const char* p, bool ln, int* l) {
    #if !(PLATFLAGS & PLATFLAG_WINDOWSLIKE)
    DIR* d = opendir(p);
    if (!d) return NULL;
    #else
    #endif
    struct charbuf names, statname;
    cb_init(&names, 256);
    cb_init(&statname, 256);
    cb_addstr(&statname, p);
    if (statname.len > 0 && !ispathsep(statname.data[statname.len - 1])) cb_add(&statname, PATHSEP);
    int snl = statname.len;
    char** data = malloc(16 * sizeof(*data));
    int len = 1;
    int size = 16;
    #if !(PLATFLAGS & PLATFLAG_WINDOWSLIKE)
    struct dirent* de;
    while ((de = readdir(d))) {
        char* n = de->d_name;
        if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]))) continue; // skip . and ..
        cb_addstr(&statname, n);
        struct stat s;
        if (!stat(cb_peek(&statname), &s)) {
            char i = (S_ISDIR(s.st_mode)) ? LS_ISDIR : 0;
            if (S_ISCHR(s.st_mode)) i |= LS_ISSPECIAL;
            else if (S_ISBLK(s.st_mode)) i |= LS_ISSPECIAL;
            else if (S_ISFIFO(s.st_mode)) i |= LS_ISSPECIAL;
            #ifdef S_ISSOCK
            else if (S_ISSOCK(s.st_mode)) i |= LS_ISSPECIAL;
            #endif
            #ifdef S_ISLNK
            lstat(statname.data, &s);
            if (S_ISLNK(s.st_mode)) i |= LS_ISLNK;
            #endif
            cb_add(&names, i);
            if (len == size) {
                size *= 2;
                data = realloc(data, size * sizeof(*data));
            }
            data[len++] = (char*)(uintptr_t)names.len;
            cb_addstr(&names, (ln) ? statname.data : n);
            cb_add(&names, 0);
        }
        statname.len = snl;
    }
    closedir(d);
    #else
    #endif
    cb_dump(&statname);
    names.data = realloc(names.data, names.len);
    data = realloc(data, (len + 1) * sizeof(*data));
    *data = names.data;
    data[len] = NULL;
    --len;
    ++data;
    for (int i = 0; i < len; ++i) {
        data[i] = (char*)(((uintptr_t)data[i]) + (uintptr_t)names.data);
    }
    if (l) *l = len;
    return data;
}
void freels(char** d) {
    --d;
    free(*d);
    free(d);
}

bool rm(const char* p) {
    
}
