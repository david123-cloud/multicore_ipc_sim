#define _POSIX_C_SOURCE 200809L

#include <alloca.h> /* snd_pcm_hw_params_alloca expands to alloca() */
#include <alsa/asoundlib.h>

#include <stdlib.h>
#include <string.h>

#include "mcpcm_ops.h"

/*
 * Real ALSA backend.
 *
 * The device name is whatever ALSA accepts: "pulse" routes through the
 * PulseAudio server (WSLg on this machine), "hw:0,0" is a physical card,
 * "null" and "file" are test plugins.  Nothing above this file knows which
 * one is in use.
 */

struct alsa_backend {
    snd_pcm_t *pcm;
    snd_pcm_uframes_t period;
    unsigned int periods;
    uint32_t channels;
    uint64_t frames_written;
    uint64_t xruns;
    uint64_t waits;
};

static int alsa_configure(void *ctx, const struct mcshm_config *config)
{
    struct alsa_backend *backend = (struct alsa_backend *)ctx;
    snd_pcm_hw_params_t *params;
    snd_pcm_format_t format;
    unsigned int rate;
    snd_pcm_uframes_t period;
    unsigned int periods;
    int rc;

    if (backend == NULL || backend->pcm == NULL || config == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (config->sample_bytes == 2u) {
        format = SND_PCM_FORMAT_S16_LE;
    } else if (config->sample_bytes == 4u) {
        format = SND_PCM_FORMAT_S32_LE;
    } else {
        return MCPCM_ERR_INVALID;
    }

    rate = config->rate;
    period = config->period_frames;
    periods = config->period_count;

    snd_pcm_hw_params_alloca(&params);
    if ((rc = snd_pcm_hw_params_any(backend->pcm, params)) < 0 ||
        (rc = snd_pcm_hw_params_set_access(backend->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) <
            0 ||
        (rc = snd_pcm_hw_params_set_format(backend->pcm, params, format)) < 0 ||
        (rc = snd_pcm_hw_params_set_channels(backend->pcm, params, config->channels)) < 0 ||
        (rc = snd_pcm_hw_params_set_rate_near(backend->pcm, params, &rate, 0)) < 0 ||
        (rc = snd_pcm_hw_params_set_period_size_near(backend->pcm, params, &period, 0)) < 0 ||
        (rc = snd_pcm_hw_params_set_periods_near(backend->pcm, params, &periods, 0)) < 0 ||
        (rc = snd_pcm_hw_params(backend->pcm, params)) < 0) {
        return MCPCM_ERR_IO;
    }

    backend->period = period;
    backend->periods = periods;
    backend->channels = config->channels;
    return MCPCM_OK;
}

static int alsa_prepare(void *ctx)
{
    struct alsa_backend *backend = (struct alsa_backend *)ctx;

    if (backend == NULL || backend->pcm == NULL) {
        return MCPCM_ERR_INVALID;
    }
    return (snd_pcm_prepare(backend->pcm) < 0) ? MCPCM_ERR_IO : MCPCM_OK;
}

static int alsa_start(void *ctx)
{
    struct alsa_backend *backend = (struct alsa_backend *)ctx;

    if (backend == NULL || backend->pcm == NULL) {
        return MCPCM_ERR_INVALID;
    }
    return (snd_pcm_start(backend->pcm) < 0) ? MCPCM_ERR_IO : MCPCM_OK;
}

static long alsa_write(void *ctx, const void *frames, uint32_t frame_count)
{
    struct alsa_backend *backend = (struct alsa_backend *)ctx;
    snd_pcm_sframes_t written;

    if (backend == NULL || backend->pcm == NULL) {
        return MCPCM_ERR_STATE;
    }
    written = snd_pcm_writei(backend->pcm, frames, frame_count);
    if (written == -EPIPE || written == -ESTRPIPE) {
        /* A real underrun on a hardware device; recover and retry once. */
        backend->xruns += 1u;
        written = snd_pcm_recover(backend->pcm, (int)written, 1);
        if (written < 0) {
            return MCPCM_ERR_IO;
        }
        written = snd_pcm_writei(backend->pcm, frames, frame_count);
    }
    if (written < 0) {
        return MCPCM_ERR_IO;
    }
    if (written < (snd_pcm_sframes_t)frame_count) {
        backend->waits += 1u;
    }
    backend->frames_written += (uint64_t)written;
    return (long)written;
}

static int alsa_drain(void *ctx)
{
    struct alsa_backend *backend = (struct alsa_backend *)ctx;

    if (backend == NULL || backend->pcm == NULL) {
        return MCPCM_ERR_INVALID;
    }
    return (snd_pcm_drain(backend->pcm) < 0) ? MCPCM_ERR_IO : MCPCM_OK;
}

static void alsa_close(void *ctx)
{
    struct alsa_backend *backend = (struct alsa_backend *)ctx;

    if (backend == NULL) {
        return;
    }
    if (backend->pcm != NULL) {
        (void)snd_pcm_close(backend->pcm);
    }
    free(backend);
}

static uint64_t alsa_hw_ptr(const void *ctx)
{
    const struct alsa_backend *backend = (const struct alsa_backend *)ctx;
    snd_pcm_sframes_t avail;

    if (backend == NULL || backend->pcm == NULL) {
        return 0u;
    }
    avail = snd_pcm_avail_update(backend->pcm);
    if (avail < 0 || (uint64_t)avail > backend->frames_written) {
        return backend->frames_written;
    }
    return backend->frames_written - (uint64_t)avail;
}

static uint64_t alsa_xruns(const void *ctx)
{
    return ((const struct alsa_backend *)ctx)->xruns;
}

static uint64_t alsa_waits(const void *ctx)
{
    return ((const struct alsa_backend *)ctx)->waits;
}

static const struct mcpcm_ops g_alsa_ops = {
    "alsa", alsa_configure, alsa_prepare, alsa_start, alsa_write,
    alsa_drain, alsa_close, alsa_hw_ptr, alsa_xruns, alsa_waits,
};

int mcpcm_alsa_create(struct mcpcm_device *device, const char *pcm_name)
{
    struct alsa_backend *backend;
    int rc;

    if (device == NULL || pcm_name == NULL) {
        return MCPCM_ERR_INVALID;
    }
    memset(device, 0, sizeof(*device));
    backend = (struct alsa_backend *)calloc(1u, sizeof(*backend));
    if (backend == NULL) {
        return MCPCM_ERR_INVALID;
    }

    rc = snd_pcm_open(&backend->pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        free(backend);
        return MCPCM_ERR_IO;
    }

    device->ops = &g_alsa_ops;
    device->ctx = backend;
    device->state = MCPCM_STATE_SETUP;
    return MCPCM_OK;
}
