/*
 * Audacious ALSA Plugin (-ng)
 * Copyright (c) 2009 William Pitcock <nenolod@dereferenced.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#define ALSA_DEBUG
#include "alsa-stdinc.h"

static snd_pcm_t *pcm_handle = NULL;
static alsaplug_ringbuf_t pcm_ringbuf;
static gboolean pcm_going = FALSE;
static GThread *audio_thread = NULL;
static gint bps;
static GMutex *pcm_mutex;
static GCond *pcm_cond;

static gsize wr_total = 0;
static gsize wr_hwframes = 0;

static gint flush_request;

static void
alsaplug_write_buffer(gpointer data, gint length)
{
    snd_pcm_sframes_t wr_frames;

    while (length > 0)
    {
        gint frames = snd_pcm_bytes_to_frames(pcm_handle, length);
        wr_frames = snd_pcm_writei(pcm_handle, data, frames);

        if (wr_frames > 0)
        {
            gint written = snd_pcm_frames_to_bytes(pcm_handle, wr_frames);
            length -= written;
            data += written;
        }
        else
        {
            gint err = snd_pcm_recover(pcm_handle, wr_frames, 1);
            if (err < 0)
                _ERROR("(write) snd_pcm_recover: %s", snd_strerror(err));

            return;
        }
    }
}

static gpointer
alsaplug_loop(gpointer unused)
{
    gchar buf[2048];

    while (pcm_going)
    {
        if (flush_request != -1)
        {
            snd_pcm_drop(pcm_handle);
            snd_pcm_prepare(pcm_handle);
            wr_total = flush_request * (bps / 1000);
            flush_request = -1;
        }

        if (alsaplug_ringbuffer_read(&pcm_ringbuf, buf, 2048) == -1)
        {
            GTimeVal pcm_abs_time;

            g_get_current_time(&pcm_abs_time);
            g_time_val_add(&pcm_abs_time, 10000);

            g_mutex_lock(pcm_mutex);
            g_cond_timed_wait(pcm_cond, pcm_mutex, &pcm_abs_time);
            g_mutex_unlock(pcm_mutex);

            continue;
        }

        alsaplug_write_buffer(buf, 2048);
    }

    snd_pcm_close(pcm_handle);
    pcm_handle = NULL;

    return NULL;
}

/********************************************************************************
 * Output Plugin API implementation.                                            *
 ********************************************************************************/

static OutputPluginInitStatus
alsaplug_init(void)
{
    gint card = -1;

    pcm_mutex = g_mutex_new();
    pcm_cond = g_cond_new();

    if (snd_card_next(&card) != 0)
        return OUTPUT_PLUGIN_INIT_NO_DEVICES;

    return OUTPUT_PLUGIN_INIT_FOUND_DEVICES;
}

static gint
alsaplug_open_audio(AFormat fmt, gint rate, gint nch)
{
    gint err, bitwidth, ringbuf_size;
    snd_pcm_format_t afmt;
    snd_pcm_hw_params_t *hwparams = NULL;

    afmt = alsaplug_format_convert(fmt);

    if ((err = snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0)
    {
        _ERROR("snd_pcm_open: %s", snd_strerror(err));
        pcm_handle = NULL;
        return -1;
    }

    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_hw_params_any(pcm_handle, hwparams);
    snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_handle, hwparams, afmt);
    snd_pcm_hw_params_set_channels(pcm_handle, hwparams, nch);
    snd_pcm_hw_params_set_rate(pcm_handle, hwparams, rate, 0);

    err = snd_pcm_hw_params(pcm_handle, hwparams);
    if (err < 0)
    {
        _ERROR("snd_pcm_hw_params failed: %s", snd_strerror(err));
        return -1;
    }

    bitwidth = snd_pcm_format_physical_width(afmt);
    bps = (rate * bitwidth * nch) >> 3;
    ringbuf_size = aud_cfg->output_buffer_size * bps / 1000;
    alsaplug_ringbuffer_init(&pcm_ringbuf, ringbuf_size);
    pcm_going = TRUE;
    flush_request = -1;

    audio_thread = g_thread_create(alsaplug_loop, NULL, TRUE, NULL);
    return 1;
}

static void
alsaplug_close_audio(void)
{
    pcm_going = FALSE;

    g_thread_join(audio_thread);

    wr_total = 0;
    wr_hwframes = 0;
    bps = 0;
    alsaplug_ringbuffer_destroy(&pcm_ringbuf);
}

static void
alsaplug_write_audio(gpointer data, gint length)
{
    wr_total += length;
    alsaplug_ringbuffer_write(&pcm_ringbuf, data, length);
}

static gint
alsaplug_output_time(void)
{
    snd_pcm_sframes_t delay;
    gsize bytes = wr_total;

    if (pcm_going && pcm_handle != NULL)
    {
        if (!snd_pcm_delay(pcm_handle, &delay))
        {
            guint d = snd_pcm_frames_to_bytes(pcm_handle, delay);
            if (bytes < d)
                bytes = 0;
            else
                bytes -= d;
        }

        return (bytes * 1000) / bps;
    }

    return 0;
}

static gint
alsaplug_written_time(void)
{
    if (pcm_going)
        return (wr_total * 1000) / bps;

    return 0;
}

static gint
alsaplug_buffer_free(void)
{
    return alsaplug_ringbuffer_free(&pcm_ringbuf);
}

static void
alsaplug_flush(gint time)
{
    flush_request = time;
    while (flush_request != -1 && pcm_going)
        g_usleep(10000);
}

static gint
alsaplug_buffer_playing(void)
{
    return pcm_going;
}

/********************************************************************************
 * Plugin glue.                                                                 *
 ********************************************************************************/

static OutputPlugin alsa_op = {
    .description = "ALSA Output Plugin (-ng)",
    .probe_priority = 1,
    .init = alsaplug_init,
    .open_audio = alsaplug_open_audio,
    .close_audio = alsaplug_close_audio,
    .write_audio = alsaplug_write_audio,
    .output_time = alsaplug_output_time,
    .written_time = alsaplug_written_time,
    .buffer_free = alsaplug_buffer_free,
    .buffer_playing = alsaplug_buffer_playing,
    .flush = alsaplug_flush,
};

OutputPlugin *alsa_oplist[] = { &alsa_op, NULL };
SIMPLE_OUTPUT_PLUGIN(alsa, alsa_oplist);
