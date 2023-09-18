#include "resource.h"
#include "game.h"

#include "../psrc_aux/logging.h"
#include "../psrc_aux/string.h"
#include "../psrc_aux/filesystem.h"
#include "../psrc_aux/threading.h"
#include "../psrc_aux/config.h"
#include "../psrc_aux/crc.h"

#include "../debug.h"

#include "../stb/stb_image.h"
#include "../stb/stb_image_resize.h"
#include "../stb/stb_vorbis.h"
#include "../minimp3/minimp3_ex.h"

#if PLATFORM != PLAT_XBOX
    #include <SDL2/SDL.h>
#else
    #include <SDL.h>
#endif

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../glue.h"

#undef loadResource
#undef freeResource
#undef grabResource
#undef releaseResource

static mutex_t rclock;

struct __attribute__((packed)) rcdata {
    struct rcheader header;
    union __attribute__((packed)) {
        struct __attribute__((packed)) {
            struct rc_config config;
            //struct rcopt_config configopt;
        };
        struct __attribute__((packed)) {
            //struct rc_consolescript consolescript;
            //struct rcopt_consolescript consolescriptopt;
        };
        struct __attribute__((packed)) {
            struct rc_entity entity;
            //struct rcopt_entity entityopt;
        };
        struct __attribute__((packed)) {
            //struct rc_gamescript gamescript;
            //struct rcopt_gamescript gamescriptopt;
        };
        struct __attribute__((packed)) {
            struct rc_map map;
            struct rcopt_map mapopt;
        };
        struct __attribute__((packed)) {
            struct rc_material material;
            struct rcopt_material materialopt;
        };
        struct __attribute__((packed)) {
            struct rc_model model;
            struct rcopt_model modelopt;
        };
        struct __attribute__((packed)) {
            //struct rc_playermodel playermodel;
            //struct rcopt_playermodel playermodelopt;
        };
        struct __attribute__((packed)) {
            //struct rc_prop prop;
            //struct rcopt_prop propopt;
        };
        struct __attribute__((packed)) {
            struct rc_sound sound;
            //struct rcopt_sound soundopt;
        };
        struct __attribute__((packed)) {
            struct rc_texture texture;
            struct rcopt_texture textureopt;
        };
    };
};

struct __attribute__((packed)) rcgroup {
    int len;
    int size;
    struct rcdata** data;
};

static struct rcgroup groups[RC__COUNT];
int groupsizes[RC__COUNT] = {4, 2, 8, 8, 1, 16, 8, 2, 16, 16, 16};

struct rcopt_material materialopt_default = {
    RCOPT_TEXTURE_QLT_HIGH
};

struct rcopt_texture textureopt_default = {
    false, RCOPT_TEXTURE_QLT_HIGH
};

static struct {
    int len;
    int size;
    char** paths;
    mutex_t lock;
} modinfo;

static char** extlist[RC__COUNT] = {
    (char*[3]){".cfg", ".txt", NULL},
    (char*[3]){".psh", ".txt", NULL},
    (char*[2]){".txt", NULL},
    (char*[3]){".pgs", ".txt", NULL},
    (char*[2]){".pmf", NULL},
    (char*[2]){".txt", NULL},
    (char*[2]){".p3m", NULL},
    (char*[2]){".txt", NULL},
    (char*[2]){".txt", NULL},
    (char*[4]){".ogg", ".mp3", ".wav", NULL},
    (char*[6]){".png", ".jpg", ".tga", ".bmp", "", NULL}
};

static struct {
    bool sound_decodewhole;
} opt;

static inline int getRcPath_try(struct charbuf* tmpcb, enum rctype type, char** ext, const char* s, ...) {
    cb_addstr(tmpcb, s);
    va_list v;
    va_start(v, s);
    while ((s = va_arg(v, char*))) {
        cb_add(tmpcb, PATHSEP);
        cb_addstr(tmpcb, s);
    }
    va_end(v);
    char** exts = extlist[type];
    char* tmp;
    while ((tmp = *exts)) {
        int len = 0;
        char c;
        while ((c = *tmp)) {
            ++tmp;
            ++len;
            cb_add(tmpcb, c);
        }
        int status = isFile(cb_peek(tmpcb));
        if (status >= 1) {
            if (ext) *ext = *exts;
            return status;
        }
        cb_undo(tmpcb, len);
        ++exts;
    }
    return -1;
}
static char* getRcPath(const char* uri, enum rctype type, char** ext) {
    struct charbuf tmpcb;
    cb_init(&tmpcb, 256);
    const char* tmp = uri;
    enum rcprefix prefix;
    while (1) {
        char c = *tmp;
        if (c) {
            if (c == ':') {
                uri = ++tmp;
                char* srcstr = cb_reinit(&tmpcb, 256);
                if (!*srcstr || !strcmp(srcstr, "self")) {
                    prefix = RCPREFIX_SELF;
                } else if (!strcmp(srcstr, "common")) {
                    prefix = RCPREFIX_COMMON;
                } else if (!strcmp(srcstr, "game")) {
                    prefix = RCPREFIX_GAME;
                } else if (!strcmp(srcstr, "mod")) {
                    prefix = RCPREFIX_MOD;
                } else if (!strcmp(srcstr, "user")) {
                    prefix = RCPREFIX_USER;
                } else if (!strcmp(srcstr, "engine")) {
                    prefix = RCPREFIX_ENGINE;
                } else {
                    free(srcstr);
                    return NULL;
                }
                free(srcstr);
                break;
            } else {
                cb_add(&tmpcb, c);
            }
        } else {
            cb_clear(&tmpcb);
            tmp = uri;
            prefix = RCPREFIX_SELF;
            break;
        }
        ++tmp;
    }
    int level = 0;
    char* path = strrelpath(tmp);
    char* tmp2 = path;
    int lastlen = 0;
    while (1) {
        char c = *tmp2;
        if (c == PATHSEP || !c) {
            char* tmp3 = &(cb_peek(&tmpcb))[lastlen];
            if (!strcmp(tmp3, "..")) {
                --level;
                if (level < 0) {
                    plog(LL_ERROR, "%s reaches out of bounds", path);
                    cb_dump(&tmpcb);
                    free(path);
                    return NULL;
                }
                tmpcb.len -= 2;
                if (tmpcb.len > 0) {
                    --tmpcb.len;
                    while (tmpcb.len > 0 && tmpcb.data[tmpcb.len - 1] != PATHSEP) {
                        --tmpcb.len;
                    }
                }
            } else if (!strcmp(tmp3, ".")) {
                tmpcb.len -= 1;
            } else {
                ++level;
                if (c) cb_add(&tmpcb, PATHSEP);
            }
            if (!c) break;
            lastlen = tmpcb.len;
        } else {
            cb_add(&tmpcb, c);
        }
        ++tmp2;
    }
    free(path);
    path = cb_reinit(&tmpcb, 256);
    int filestatus = -1;
    switch ((int8_t)prefix) {
        case RCPREFIX_SELF: {
            for (int i = 0; i < modinfo.len; ++i) {
                if ((filestatus = getRcPath_try(&tmpcb, type, ext, modinfo.paths[i], "games", gamedir, path, NULL)) >= 1) goto found;
                cb_clear(&tmpcb);
            }
            if ((filestatus = getRcPath_try(&tmpcb, type, ext, maindir, "games", gamedir, path, NULL)) >= 1) goto found;
        } break;
        case RCPREFIX_COMMON: {
            for (int i = 0; i < modinfo.len; ++i) {
                if ((filestatus = getRcPath_try(&tmpcb, type, ext, modinfo.paths[i], "common", path, NULL)) >= 1) goto found;
                cb_clear(&tmpcb);
            }
            if ((filestatus = getRcPath_try(&tmpcb, type, ext, maindir, "common", path, NULL)) >= 1) goto found;
        } break;
        case RCPREFIX_ENGINE: {
            for (int i = 0; i < modinfo.len; ++i) {
                if ((filestatus = getRcPath_try(&tmpcb, type, ext, modinfo.paths[i], "engine", path, NULL)) >= 1) goto found;
                cb_clear(&tmpcb);
            }
            if ((filestatus = getRcPath_try(&tmpcb, type, ext, maindir, "engine", path, NULL)) >= 1) goto found;
        } break;
        case RCPREFIX_GAME: {
            for (int i = 0; i < modinfo.len; ++i) {
                if ((filestatus = getRcPath_try(&tmpcb, type, ext, modinfo.paths[i], "games", path, NULL)) >= 1) goto found;
                cb_clear(&tmpcb);
            }
            if ((filestatus = getRcPath_try(&tmpcb, type, ext, maindir, "games", path, NULL)) >= 1) goto found;
        } break;
        case RCPREFIX_MOD: {
            if ((filestatus = getRcPath_try(&tmpcb, type, ext, userdir, "mods", path, NULL)) >= 1) goto found;
            cb_clear(&tmpcb);
            if ((filestatus = getRcPath_try(&tmpcb, type, ext, maindir, "mods", path, NULL)) >= 1) goto found;
        } break;
        case RCPREFIX_USER: {
            if ((filestatus = getRcPath_try(&tmpcb, type, ext, userdir, path, NULL)) >= 1) goto found;
        } break;
    }
    free(path);
    cb_dump(&tmpcb);
    return NULL;
    found:;
    free(path);
    path = cb_finalize(&tmpcb);
    if (filestatus > 1) {
        plog(LL_WARN, LW_SPECIALFILE(path));
    }
    return path;
}

static struct rcdata* loadResource_newptr(enum rctype t, struct rcgroup* g, const char* p, uint32_t pcrc) {
    struct rcdata* ptr = NULL;
    size_t size = sizeof(struct rcheader);
    switch ((uint8_t)t) {
        case RC_CONFIG:
            size += sizeof(struct rc_config);
            break;
        case RC_ENTITY:
            size += sizeof(struct rc_entity);
            break;
        case RC_MAP:
            size += sizeof(struct rc_map) + sizeof(struct rcopt_map);
            break;
        case RC_MATERIAL:
            size += sizeof(struct rc_material) + sizeof(struct rcopt_material);
            break;
        case RC_MODEL:
            size += sizeof(struct rc_model) + sizeof(struct rcopt_model);
            break;
        case RC_SOUND:
            size += sizeof(struct rc_sound);
            break;
        case RC_TEXTURE:
            size += sizeof(struct rc_texture) + sizeof(struct rcopt_texture);
            break;
    }
    for (int i = 0; i < g->len; ++i) {
        struct rcdata* d = g->data[i];
        if (!d) {
            ptr = malloc(size);
            ptr->header.index = i;
            g->data[i] = ptr;
        }
    }
    if (!ptr) {
        if (g->len == g->size) {
            g->size *= 2;
            g->data = realloc(g->data, g->size * sizeof(*g->data));
        }
        ptr = malloc(size);
        ptr->header.index = g->len;
        g->data[g->len++] = ptr;
    }
    ptr->header.type = t;
    ptr->header.path = strdup(p);
    ptr->header.pathcrc = pcrc;
    ptr->header.refs = 1;
    return ptr;
}

static struct rcdata* loadResource_internal(enum rctype, const char*, union rcopt);

static union resource loadResource_union(enum rctype t, const char* uri, union rcopt o) {
    struct rcdata* r = loadResource_internal(t, uri, o);
    if (!r) return (union resource){.ptr = NULL};
    return (union resource){.ptr = (void*)r + sizeof(struct rcheader)};
}
#define loadResource_union(t, p, o) loadResource_union((t), (p), (union rcopt){.ptr = (void*)(o)})

static struct rcdata* loadResource_internal(enum rctype t, const char* uri, union rcopt o) {
    char* ext;
    char* p = getRcPath(uri, t, &ext);
    if (!p) {
        plog(LL_ERROR, "Could not find resource %s", uri);
        return NULL;
    }
    uint32_t pcrc = strcrc32(p);
    struct rcdata* d;
    struct rcgroup* g = &groups[t];
    for (int i = 0; i < g->len; ++i) {
        d = g->data[i];
        if (d && d->header.pathcrc == pcrc && !strcmp(p, d->header.path)) {
            switch ((uint8_t)t) {
                case RC_MATERIAL: {
                    if (!o.ptr) o.material = &materialopt_default;
                    if (o.material->quality != d->materialopt.quality) goto nomatch;
                } break;
                case RC_TEXTURE: {
                    if (!o.ptr) o.texture = &textureopt_default;
                    if (o.texture->needsalpha && d->texture.channels != RC_TEXTURE_FRMT_RGBA) goto nomatch;
                    if (o.texture->quality != d->textureopt.quality) goto nomatch;
                } break;
            }
            ++d->header.refs;
            free(p);
            return d;
            nomatch:;
        }
    }
    d = NULL;
    switch ((uint8_t)t) {
        case RC_CONFIG: {
            struct cfg* cfg = cfg_open(p);
            if (cfg) {
                d = loadResource_newptr(t, g, p, pcrc);
                d->config.config = cfg;
            }
        } break;
        case RC_MATERIAL: {
            struct cfg* mat = cfg_open(p);
            if (mat) {
                d = loadResource_newptr(t, g, p, pcrc);
                char* tmp = cfg_getvar(mat, NULL, "base");
                if (tmp) {
                    char* tmp2 = getRcPath(tmp, RC_MATERIAL, NULL);
                    free(tmp);
                    if (tmp2) {
                        cfg_merge(mat, tmp2, false);
                        free(tmp2);
                    }
                }
                tmp = cfg_getvar(mat, NULL, "texture");
                if (tmp) {
                    struct rcopt_texture texopt = {false, o.material->quality};
                    d->material.texture = loadResource_union(RC_TEXTURE, tmp, &texopt).texture;
                    free(tmp);
                } else {
                    d->material.texture = NULL;
                }
                d->material.color[0] = 1.0;
                d->material.color[1] = 1.0;
                d->material.color[2] = 1.0;
                d->material.color[3] = 1.0;
                tmp = cfg_getvar(mat, NULL, "color");
                if (tmp) {
                    sscanf(tmp, "%f,%f,%f", &d->material.color[0], &d->material.color[1], &d->material.color[2]);
                }
                tmp = cfg_getvar(mat, NULL, "alpha");
                if (tmp) {
                    sscanf(tmp, "%f", &d->material.color[3]);
                }
            } 
        } break;
        case RC_SOUND: {
            FILE* f = fopen(p, "r");
            if (f) {
                if (!strcmp(ext, ".ogg")) {
                    fseek(f, 0, SEEK_END);
                    long sz = ftell(f);
                    if (sz > 0) {
                        uint8_t* data = malloc(sz);
                        fseek(f, 0, SEEK_SET);
                        fread(data, 1, sz, f);
                        stb_vorbis* v = stb_vorbis_open_memory(data, sz, NULL, NULL);
                        if (v) {
                            d = loadResource_newptr(t, g, p, pcrc);
                            if (opt.sound_decodewhole) {
                                d->sound.format = RC_SOUND_FRMT_WAV;
                                stb_vorbis_info info = stb_vorbis_get_info(v);
                                int len = stb_vorbis_stream_length_in_samples(v);
                                int ch = (info.channels > 1) + 1;
                                int size = len * ch * sizeof(int16_t);
                                d->sound.len = len;
                                d->sound.size = size;
                                d->sound.data = malloc(size);
                                d->sound.freq = info.sample_rate;
                                d->sound.channels = info.channels;
                                d->sound.is8bit = false;
                                d->sound.stereo = (info.channels > 1);
                                stb_vorbis_get_samples_short_interleaved(v, ch, (int16_t*)d->sound.data, len * ch);
                                stb_vorbis_close(v);
                                free(data);
                            } else {
                                d->sound.format = RC_SOUND_FRMT_VORBIS;
                                d->sound.size = sz;
                                d->sound.data = data;
                                d->sound.len = stb_vorbis_stream_length_in_samples(v);
                                stb_vorbis_info info = stb_vorbis_get_info(v);
                                d->sound.freq = info.sample_rate;
                                d->sound.channels = info.channels;
                                d->sound.stereo = (info.channels > 1);
                                stb_vorbis_close(v);
                            }
                        } else {
                            free(data);
                        }
                    }
                } else if (!strcmp(ext, ".mp3")) {
                    fseek(f, 0, SEEK_END);
                    long sz = ftell(f);
                    if (sz > 0) {
                        uint8_t* data = malloc(sz);
                        fseek(f, 0, SEEK_SET);
                        fread(data, 1, sz, f);
                        mp3dec_ex_t* m = malloc(sizeof(*m));
                        if (mp3dec_ex_open_buf(m, data, sz, MP3D_SEEK_TO_SAMPLE)) {
                            free(data);
                            free(m);
                        } else {
                            d = loadResource_newptr(t, g, p, pcrc);
                            if (opt.sound_decodewhole) {
                                d->sound.format = RC_SOUND_FRMT_WAV;
                                int len = m->samples / m->info.channels;
                                int size = m->samples * sizeof(mp3d_sample_t);
                                d->sound.len = len;
                                d->sound.size = size;
                                d->sound.data = malloc(size);
                                d->sound.freq = m->info.hz;
                                d->sound.channels = m->info.channels;
                                d->sound.is8bit = false;
                                d->sound.stereo = (m->info.channels > 1);
                                mp3dec_ex_read(m, (mp3d_sample_t*)d->sound.data, m->samples);
                                mp3dec_ex_close(m);
                                free(data);
                            } else {
                                d->sound.format = RC_SOUND_FRMT_MP3;
                                d->sound.size = sz;
                                d->sound.data = data;
                                d->sound.len = m->samples / m->info.channels;
                                d->sound.freq = m->info.hz;
                                d->sound.channels = m->info.channels;
                                d->sound.stereo = (m->info.channels > 1);
                                mp3dec_ex_close(m);
                            }
                        }
                        free(m);
                    }
                } else if (!strcmp(ext, ".wav")) {
                    SDL_RWops* rwops = SDL_RWFromFP(f, false);
                    if (rwops) {
                        SDL_AudioSpec spec;
                        uint8_t* data;
                        uint32_t sz;
                        if (SDL_LoadWAV_RW(rwops, false, &spec, &data, &sz)) {
                            SDL_AudioCVT cvt;
                            SDL_AudioFormat destfrmt;
                            if (SDL_AUDIO_BITSIZE(spec.format) == 8) {
                                destfrmt = AUDIO_U8;
                            } else {
                                destfrmt = AUDIO_S16SYS;
                            }
                            int ret = SDL_BuildAudioCVT(
                                &cvt,
                                spec.format, spec.channels, spec.freq,
                                destfrmt, (spec.channels > 1) + 1, spec.freq
                            );
                            if (ret >= 0) {
                                if (ret) {
                                    cvt.len = sz;
                                    data = SDL_realloc(data, cvt.len * cvt.len_mult);
                                    cvt.buf = data;
                                    if (SDL_ConvertAudio(&cvt)) {
                                        free(data);
                                    } else {
                                        data = SDL_realloc(data, cvt.len_cvt);
                                        sz = cvt.len_cvt;
                                        d = loadResource_newptr(t, g, p, pcrc);
                                        d->sound.format = RC_SOUND_FRMT_WAV;
                                        d->sound.size = sz;
                                        d->sound.data = data;
                                        d->sound.len = sz / ((spec.channels > 1) + 1) / ((destfrmt == AUDIO_S16SYS) + 1);
                                        d->sound.freq = spec.freq;
                                        d->sound.channels = spec.channels;
                                        d->sound.is8bit = (destfrmt == AUDIO_U8);
                                        d->sound.stereo = (spec.channels > 1);
                                    }
                                } else {
                                    d = loadResource_newptr(t, g, p, pcrc);
                                    d->sound.format = RC_SOUND_FRMT_WAV;
                                    d->sound.size = sz;
                                    d->sound.data = data;
                                    d->sound.len = sz / ((spec.channels > 1) + 1) / ((destfrmt == AUDIO_S16SYS) + 1);
                                    d->sound.freq = spec.freq;
                                    d->sound.channels = spec.channels;
                                    d->sound.is8bit = (destfrmt == AUDIO_U8);
                                    d->sound.stereo = (spec.channels > 1);
                                }
                            } else {
                                free(data);
                            }
                        }
                        SDL_RWclose(rwops);
                    }
                }
                fclose(f);
            }
        } break;
        case RC_TEXTURE: {
            int w, h, c;
            if (stbi_info(p, &w, &h, &c)) {
                if (o.texture->needsalpha) {
                    c = 4;
                } else {
                    if (c < 3) c += 2;
                }
                int c2;
                unsigned char* data = stbi_load(p, &w, &h, &c2, c);
                if (data) {
                    if (o.texture->quality != RCOPT_TEXTURE_QLT_HIGH) {
                        int w2 = w, h2 = h;
                        switch ((uint8_t)o.texture->quality) {
                            case RCOPT_TEXTURE_QLT_MED: {
                                w2 /= 2;
                                h2 /= 2;
                            } break;
                            case RCOPT_TEXTURE_QLT_LOW: {
                                w2 /= 4;
                                h2 /= 4;
                            } break;
                        }
                        if (w2 < 1) w2 = 1;
                        if (h2 < 1) h2 = 1;
                        unsigned char* data2 = malloc(w * h * c);
                        int status = stbir_resize_uint8_generic(
                            data, w, h, 0,
                            data2, w2, h2, 0,
                            c, -1, 0,
                            STBIR_EDGE_WRAP, STBIR_FILTER_BOX, STBIR_COLORSPACE_LINEAR,
                            NULL
                        );
                        if (status) {
                            free(data);
                            w = w2;
                            h = h2;
                            data = data2;
                        }
                    }
                    d = loadResource_newptr(t, g, p, pcrc);
                    d->texture.width = w;
                    d->texture.height = h;
                    d->texture.channels = c;
                    d->texture.data = data;
                    d->textureopt = *o.texture;
                }
            }
        } break;
    }
    free(p);
    if (!d) plog(LL_WARN, "Failed to load resource %s", uri);
    return d;
}

union resource loadResource(enum rctype t, const char* uri, union rcopt o) {
    lockMutex(&rclock);
    struct rcdata* r = loadResource_internal(t, uri, o);
    unlockMutex(&rclock);
    if (!r) return (union resource){.ptr = NULL};
    return (union resource){.ptr = (void*)r + sizeof(struct rcheader)};
}

static void freeResource_internal(struct rcdata*);

static void freeResource_union(union resource r) {
    if (r.ptr) {
        freeResource_internal(r.ptr - sizeof(struct rcheader));
    }
}
#define freeResource_union(r) freeResource_union((union resource){.ptr = (void*)(r)})

static void freeResource_internal(struct rcdata* _r) {
    enum rctype type = _r->header.type;
    int index = _r->header.index;
    struct rcdata* r = groups[type].data[index];
    --r->header.refs;
    if (!r->header.refs) {
        switch ((uint8_t)type) {
            case RC_CONFIG: {
                cfg_close(r->config.config);
            } break;
            case RC_MATERIAL: {
                freeResource_union(r->material.texture);
            } break;
            case RC_MODEL: {
                for (unsigned i = 0; i < r->model.parts; ++i) {
                    freeResource_union(r->model.partdata[i].material);
                }
            } break;
            case RC_SOUND: {
                free(r->sound.data);
            } break;
            case RC_TEXTURE: {
                free(r->texture.data);
            } break;
        }
        free(r->header.path);
        free(r);
        groups[type].data[index] = NULL;
    }
}

void freeResource(union resource r) {
    if (r.ptr) {
        lockMutex(&rclock);
        freeResource_internal(r.ptr - sizeof(struct rcheader));
        unlockMutex(&rclock);
    }
}

void grabResource(union resource _r) {
    if (_r.ptr) {
        lockMutex(&rclock);
        struct rcdata* r = _r.ptr - sizeof(struct rcheader);
        ++r->header.refs;
        unlockMutex(&rclock);
    }
}

static inline void loadMods_addpath(char* p) {
    ++modinfo.len;
    if (modinfo.len == modinfo.size) {
        modinfo.size *= 2;
        modinfo.paths = realloc(modinfo.paths, modinfo.size * sizeof(*modinfo.paths));
    }
    modinfo.paths[modinfo.len - 1] = p;
}

void loadMods(const char* const* modnames, int modcount) {
    lockMutex(&modinfo.lock);
    for (int i = 0; i < modinfo.len; ++i) {
        free(modinfo.paths[i]);
    }
    modinfo.len = 0;
    if (modcount > 0 && modnames && *modnames) {
        if (modinfo.size < 4) {
            modinfo.size = 4;
            modinfo.paths = realloc(modinfo.paths, modinfo.size * sizeof(*modinfo.paths));
        }
        #if DEBUG(1)
        {
            struct charbuf cb;
            cb_init(&cb, 256);
            cb_add(&cb, '{');
            if (modcount) {
                const char* tmp = modnames[0];
                char c;
                cb_add(&cb, '"');
                while ((c = *tmp)) {
                    if (c == '"') cb_add(&cb, '\\');
                    cb_add(&cb, c);
                    ++tmp;
                }
                cb_add(&cb, '"');
                for (int i = 1; i < modcount; ++i) {
                    cb_add(&cb, ',');
                    cb_add(&cb, ' ');
                    tmp = modnames[i];
                    cb_add(&cb, '"');
                    while ((c = *tmp)) {
                        if (c == '"' || c == '\\') cb_add(&cb, '\\');
                        cb_add(&cb, c);
                        ++tmp;
                    }
                    cb_add(&cb, '"');
                }
            }
            cb_add(&cb, '}');
            plog(LL_INFO | LF_DEBUG, "Requested mods: %s", cb_peek(&cb));
            cb_dump(&cb);
        }
        #endif
        for (int i = 0; i < modcount; ++i) {
            bool notfound = true;
            char* tmp = mkpath(userdir, "mods", modnames[i], NULL);
            if (isFile(tmp)) {
                free(tmp);
            } else {
                notfound = false;
                loadMods_addpath(tmp);
            }
            tmp = mkpath(maindir, "mods", modnames[i], NULL);
            if (isFile(tmp)) {
                free(tmp);
            } else {
                notfound = false;
                loadMods_addpath(tmp);
            }
            if (notfound) {
                plog(LL_WARN, "Unable to locate mod: %s", modnames[i]);
            }
        }
        #if DEBUG(1)
        {
            struct charbuf cb;
            cb_init(&cb, 256);
            cb_add(&cb, '{');
            if (modinfo.len) {
                const char* tmp = modinfo.paths[0];
                char c;
                cb_add(&cb, '"');
                while ((c = *tmp)) {
                    if (c == '"') cb_add(&cb, '\\');
                    cb_add(&cb, c);
                    ++tmp;
                }
                cb_add(&cb, '"');
                for (int i = 1; i < modinfo.len; ++i) {
                    cb_add(&cb, ',');
                    cb_add(&cb, ' ');
                    tmp = modinfo.paths[i];
                    cb_add(&cb, '"');
                    while ((c = *tmp)) {
                        if (c == '"' || c == '\\') cb_add(&cb, '\\');
                        cb_add(&cb, c);
                        ++tmp;
                    }
                    cb_add(&cb, '"');
                }
            }
            cb_add(&cb, '}');
            plog(LL_INFO | LF_DEBUG, "Found mods: %s", cb_peek(&cb));
            cb_dump(&cb);
        }
        #endif
    } else {
        modinfo.size = 0;
        free(modinfo.paths);
        modinfo.paths = NULL;
    }
    unlockMutex(&modinfo.lock);
}

char** queryModInfo(int* len) {
    lockMutex(&modinfo.lock);
    if (modinfo.len > 0) {
        if (len) *len = modinfo.len;
        char** data = malloc((modinfo.len + 1) * sizeof(*data));
        for (int i = 0; i < modinfo.len; ++i) {
            data[i] = strdup(modinfo.paths[i]);
        }
        data[modinfo.len] = NULL;
        unlockMutex(&modinfo.lock);
        return data;
    }
    unlockMutex(&modinfo.lock);
    return NULL;
}

bool initResource(void) {
    if (!createMutex(&rclock)) return false;
    if (!createMutex(&modinfo.lock)) return false;

    for (int i = 0; i < RC__COUNT; ++i) {
        groups[i].len = 0;
        groups[i].size = groupsizes[i];
        groups[i].data = malloc(groups[i].size * sizeof(*groups[i].data));
    }

    char* tmp = cfg_getvar(config, NULL, "mods");
    if (tmp) {
        int modcount;
        char** modnames = splitstrlist(tmp, ',', false, &modcount);
        free(tmp);
        loadMods((const char* const*)modnames, modcount);
        for (int i = 0; i < modcount; ++i) {
            free(modnames[i]);
        }
        free(modnames);
    } else {
        loadMods(NULL, 0);
    }
    tmp = cfg_getvar(config, "Sound", "decodewhole");
    opt.sound_decodewhole = strbool(tmp, false);
    free(tmp);

    return true;
}
