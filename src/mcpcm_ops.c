#define _POSIX_C_SOURCE 200809L

#include "mcpcm_ops.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- facade -- */

static int facade_expect(const struct mcpcm_device *device, enum mcpcm_state a,
                         enum mcpcm_state b)
{
    return (device->state == a || device->state == b) ? 1 : 0;
}

int mcpcm_dev_configure(struct mcpcm_device *device, const struct mcshm_config *config)
{
    int status;

    if (device == NULL || config == NULL || device->ops == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (device->state != MCPCM_STATE_SETUP) {
        return MCPCM_ERR_STATE;
    }
    status = device->ops->configure(device->ctx, config);
    if (status == MCPCM_OK) {
        device->config = *config;
    }
    return status;
}

int mcpcm_dev_prepare(struct mcpcm_device *device)
{
    int status;

    if (device == NULL || device->ops == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (!facade_expect(device, MCPCM_STATE_SETUP, MCPCM_STATE_PREPARED)) {
        return MCPCM_ERR_STATE;
    }
    status = device->ops->prepare(device->ctx);
    if (status == MCPCM_OK) {
        device->state = MCPCM_STATE_PREPARED;
    }
    return status;
}

int mcpcm_dev_start(struct mcpcm_device *device)
{
    int status;

    if (device == NULL || device->ops == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (device->state != MCPCM_STATE_PREPARED) {
        return MCPCM_ERR_STATE;
    }
    status = device->ops->start(device->ctx);
    if (status == MCPCM_OK) {
        device->state = MCPCM_STATE_RUNNING;
    }
    return status;
}

long mcpcm_dev_write(struct mcpcm_device *device, const void *frames, uint32_t frame_count)
{
    if (device == NULL || device->ops == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (!facade_expect(device, MCPCM_STATE_PREPARED, MCPCM_STATE_RUNNING)) {
        return MCPCM_ERR_STATE;
    }
    return device->ops->write(device->ctx, frames, frame_count);
}

int mcpcm_dev_drain(struct mcpcm_device *device)
{
    int status;

    if (device == NULL || device->ops == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (device->state != MCPCM_STATE_RUNNING) {
        return MCPCM_ERR_STATE;
    }
    device->state = MCPCM_STATE_DRAINING;
    status = device->ops->drain(device->ctx);
    device->state = (status == MCPCM_OK) ? MCPCM_STATE_SETUP : MCPCM_STATE_RUNNING;
    return status;
}

void mcpcm_dev_close(struct mcpcm_device *device)
{
    if (device == NULL || device->ops == NULL) {
        return;
    }
    if (device->ops->close != NULL) {
        device->ops->close(device->ctx);
    }
    device->ctx = NULL;
    device->ops = NULL;
    device->state = MCPCM_STATE_CLOSED;
}

uint64_t mcpcm_dev_hw_ptr(const struct mcpcm_device *device)
{
    return (device != NULL && device->ops != NULL) ? device->ops->hw_ptr(device->ctx) : 0u;
}

uint64_t mcpcm_dev_xruns(const struct mcpcm_device *device)
{
    return (device != NULL && device->ops != NULL) ? device->ops->xruns(device->ctx) : 0u;
}

uint64_t mcpcm_dev_waits(const struct mcpcm_device *device)
{
    return (device != NULL && device->ops != NULL) ? device->ops->waits(device->ctx) : 0u;
}

/* ------------------------------------------------------- shm backend ----- */

struct shm_backend {
    struct mcpcm pcm;
};

static int shm_configure(void *ctx, const struct mcshm_config *config)
{
    return mcpcm_set_hw_params(&((struct shm_backend *)ctx)->pcm, config);
}

static int shm_prepare(void *ctx)
{
    return mcpcm_prepare(&((struct shm_backend *)ctx)->pcm);
}

static int shm_start(void *ctx)
{
    return mcpcm_start(&((struct shm_backend *)ctx)->pcm);
}

static long shm_write(void *ctx, const void *frames, uint32_t frame_count)
{
    return mcpcm_write(&((struct shm_backend *)ctx)->pcm, frames, frame_count);
}

static int shm_drain(void *ctx)
{
    return mcpcm_drain(&((struct shm_backend *)ctx)->pcm);
}

static void shm_close(void *ctx)
{
    struct shm_backend *backend = (struct shm_backend *)ctx;

    if (backend == NULL) {
        return;
    }
    mcpcm_close(&backend->pcm);
    free(backend);
}

static uint64_t shm_hw_ptr(const void *ctx)
{
    return mcpcm_hw_ptr(&((const struct shm_backend *)ctx)->pcm);
}

static uint64_t shm_xruns(const void *ctx)
{
    return mcpcm_xruns(&((const struct shm_backend *)ctx)->pcm);
}

static uint64_t shm_waits(const void *ctx)
{
    return mcpcm_waits(&((const struct shm_backend *)ctx)->pcm);
}

static const struct mcpcm_ops g_shm_ops = {
    "shm", shm_configure, shm_prepare, shm_start, shm_write,
    shm_drain, shm_close, shm_hw_ptr, shm_xruns, shm_waits,
};

int mcpcm_shm_create(struct mcpcm_device *device, const char *socket_path)
{
    struct shm_backend *backend;

    if (device == NULL || socket_path == NULL) {
        return MCPCM_ERR_INVALID;
    }
    memset(device, 0, sizeof(*device));
    backend = (struct shm_backend *)calloc(1u, sizeof(*backend));
    if (backend == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (mcpcm_open(&backend->pcm, socket_path) != MCPCM_OK) {
        free(backend);
        return MCPCM_ERR_IO;
    }
    device->ops = &g_shm_ops;
    device->ctx = backend;
    device->state = MCPCM_STATE_SETUP;
    return MCPCM_OK;
}
