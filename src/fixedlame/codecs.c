/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 Björn Stenberg
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <timefuncs.h>
#include <ctype.h>
#include <stdarg.h>
#include "string-extra.h"
#include "debug.h"
#include "button.h"
#include "dir.h"
#include "file.h"
#include "kernel.h"
#include "screens.h"
#include "misc.h"
#include "mas.h"
#include "codecs.h"
#include "lang.h"
#include "keyboard.h"
#include "buffering.h"
#include "mp3_playback.h"
#include "backlight.h"
#include "storage.h"
#include "talk.h"
#include "mp3data.h"
#include "powermgmt.h"
#include "system.h"
#include "sound.h"
#include "splash.h"
#include "general.h"

#define LOGF_ENABLE
#include "logf.h"

#ifdef SIMULATOR
#define PREFIX(_x_) sim_ ## _x_
#else
#define PREFIX
#endif

#ifdef SIMULATOR
#if CONFIG_CODEC == SWCODEC
unsigned char codecbuf[CODEC_SIZE];
#endif
void *sim_codec_load_ram(char* codecptr, int size, void **pd);
void sim_codec_close(void *pd);
#else
#define sim_codec_close(x)
#endif

size_t codec_size;

extern void* plugin_get_audio_buffer(size_t *buffer_size);

#undef open
static int open(const char* pathname, int flags, ...)
{
#ifdef SIMULATOR
    int fd;
    if (flags & O_CREAT)
    {
        va_list ap;
        va_start(ap, flags);
        fd = sim_open(pathname, flags, va_arg(ap, unsigned int));
        va_end(ap);
    }
    else
        fd = sim_open(pathname, flags);

    return fd;
#else
    return file_open(pathname, flags);
#endif
}
struct codec_api ci = {

    0, /* filesize */
    0, /* curpos */
    NULL, /* id3 */
    NULL, /* taginfo_ready */
    false, /* stop_codec */
    0, /* new_track */
    0, /* seek_time */
    NULL, /* struct dsp_config *dsp */
    NULL, /* codec_get_buffer */
    NULL, /* pcmbuf_insert */
    NULL, /* set_elapsed */
    NULL, /* read_filebuf */
    NULL, /* request_buffer */
    NULL, /* advance_buffer */
    NULL, /* advance_buffer_loc */
    NULL, /* seek_buffer */
    NULL, /* seek_complete */
    NULL, /* request_next_track */
    NULL, /* discard_codec */
    NULL, /* set_offset */
    NULL, /* configure */
    
    /* kernel/ system */
#ifdef CPU_ARM
    __div0,
#endif
    PREFIX(sleep),
    yield,

#if NUM_CORES > 1
    create_thread,
    thread_thaw,
    thread_wait,
    semaphore_init,
    semaphore_wait,
    semaphore_release,
#endif

#if NUM_CORES > 1
    cpucache_flush,
    cpucache_invalidate,
#endif

    /* strings and memory */
    strcpy,
    strlen,
    strcmp,
    strcat,
    memset,
    memcpy,
    memmove,
    memcmp,
    memchr,
    strcasestr,
#if defined(DEBUG) || defined(SIMULATOR)
    debugf,
#endif
#ifdef ROCKBOX_HAS_LOGF
    logf,
#endif

    (qsort_func)qsort,
    &global_settings,

#ifdef RB_PROFILE
    profile_thread,
    profstop,
    __cyg_profile_func_enter,
    __cyg_profile_func_exit,
#endif

#if defined(HAVE_RECORDING) && !defined(SIMULATOR)
    false,  /* stop_encoder */
    0,      /* enc_codec_loaded */
    enc_get_inputs,
    enc_set_parameters,
    enc_get_chunk,
    enc_finish_chunk,
    enc_get_pcm_data,
    enc_unget_pcm_data,

    /* file */
    (open_func)PREFIX(open),
    close,
    (read_func)read,
    PREFIX(lseek),
    (write_func)write,
    round_value_to_list32,

#endif

    /* new stuff at the end, sort into place next time
       the API gets incompatible */
};

void codec_get_full_path(char *path, const char *codec_root_fn)
{
    snprintf(path, MAX_PATH-1, CODECS_DIR "/%s." CODEC_EXTENSION,
             codec_root_fn);
}

static int codec_load_ram(int size, struct codec_api *api)
{
    struct codec_header *hdr;
    int status;
#ifndef SIMULATOR
    hdr = (struct codec_header *)codecbuf;
        
    if (size <= (signed)sizeof(struct codec_header)
        || (hdr->magic != CODEC_MAGIC
#ifdef HAVE_RECORDING
             && hdr->magic != CODEC_ENC_MAGIC
#endif
            )
        || hdr->target_id != TARGET_ID
        || hdr->load_addr != codecbuf
        || hdr->end_addr > codecbuf + CODEC_SIZE)
    {
        logf("codec header error");
        return CODEC_ERROR;
    }

    codec_size = hdr->end_addr - codecbuf;

#else /* SIMULATOR */
    void *pd;
    
    hdr = sim_codec_load_ram(codecbuf, size, &pd);

    if (pd == NULL)
        return CODEC_ERROR;

    if (hdr == NULL
        || (hdr->magic != CODEC_MAGIC
#ifdef HAVE_RECORDING
             && hdr->magic != CODEC_ENC_MAGIC
#endif
           )
        || hdr->target_id != TARGET_ID) {
        sim_codec_close(pd);
        return CODEC_ERROR;
    }

    codec_size = codecbuf - codecbuf;

#endif /* SIMULATOR */
    if (hdr->api_version > CODEC_API_VERSION
        || hdr->api_version < CODEC_MIN_API_VERSION) {
        sim_codec_close(pd);
        return CODEC_ERROR;
    }

    *(hdr->api) = api;
    cpucache_invalidate();
    status = hdr->entry_point();

    sim_codec_close(pd);

    return status;
}

int codec_load_buf(unsigned int hid, struct codec_api *api)
{
    int rc;
    rc = bufread(hid, CODEC_SIZE, codecbuf);
    if (rc < 0) {
        logf("error loading codec");
        return CODEC_ERROR;
    }
    api->discard_codec();
    return codec_load_ram(rc, api);
}

int codec_load_file(const char *plugin, struct codec_api *api)
{
    char path[MAX_PATH];
    int fd;
    int rc;

    codec_get_full_path(path, plugin);
    
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        logf("Codec load error:%d", fd);
        splashf(HZ*2, "Couldn't load codec: %s", path);
        return fd;
    }
    
    rc = read(fd, &codecbuf[0], CODEC_SIZE);
    close(fd);
    if (rc <= 0) {
        logf("Codec read error");
        return CODEC_ERROR;
    }

    return codec_load_ram((size_t)rc, api);
}
