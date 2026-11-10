#define _POSIX_C_SOURCE 200809L

#include "mcpcm.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static uint32_t mcpcm_next_seq(struct mcpcm *pcm)
{
    return pcm->seq++;
}

static int64_t mcpcm_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

const char *mcpcm_state_name(enum mcpcm_state state)
{
    switch (state) {
    case MCPCM_STATE_CLOSED:
        return "closed";
    case MCPCM_STATE_SETUP:
        return "setup";
    case MCPCM_STATE_PREPARED:
        return "prepared";
    case MCPCM_STATE_RUNNING:
        return "running";
    case MCPCM_STATE_DRAINING:
        return "draining";
    default:
        return "unknown";
    }
}

/* Consumes async device notifications until the matching ACK arrives. */
static int mcpcm_wait_ack(struct mcpcm *pcm, struct mccmd_ack *ack, int timeout_ms)
{
    int64_t deadline = mcpcm_now_ms() + (int64_t)timeout_ms;

    for (;;) {
        struct mccmd_header header;
        unsigned char payload[MCCMD_MAX_PAYLOAD];
        int64_t remaining = deadline - mcpcm_now_ms();
        int result;

        if (remaining <= 0) {
            return MCPCM_ERR_TIMEOUT;
        }
        result = mccmd_recv(pcm->fd, &header, payload, (uint32_t)sizeof(payload),
                            (int)remaining);
        if (result == 0) {
            return MCPCM_ERR_TIMEOUT;
        }
        if (result < 0) {
            return (result == MCCMD_ERR_CLOSED) ? MCPCM_ERR_IO : MCPCM_ERR_IO;
        }

        switch (header.type) {
        case MCCMD_MSG_ACK: {
            if (header.length != sizeof(*ack)) {
                return MCPCM_ERR_IO;
            }
            memcpy(ack, payload, sizeof(*ack));
            return MCPCM_OK;
        }
        case MCCMD_MSG_PERIOD: {
            struct mccmd_period period;

            if (header.length == sizeof(period)) {
                memcpy(&period, payload, sizeof(period));
                pcm->periods += 1u;
                pcm->last_hw_ptr = period.hw_ptr;
            }
            break;
        }
        case MCCMD_MSG_XRUN: {
            struct mccmd_xrun xrun;

            if (header.length == sizeof(xrun)) {
                memcpy(&xrun, payload, sizeof(xrun));
                pcm->xruns += 1u;
                pcm->last_hw_ptr = xrun.hw_ptr;
            }
            break;
        }
        default:
            break;
        }
    }
}

static int mcpcm_call(struct mcpcm *pcm, uint16_t type, const void *payload, uint32_t length,
                      struct mccmd_ack *ack)
{
    int status = mccmd_send(pcm->fd, type, mcpcm_next_seq(pcm), payload, length);

    if (status != MCCMD_OK) {
        return MCPCM_ERR_IO;
    }
    status = mcpcm_wait_ack(pcm, ack, 2000);
    if (status != MCPCM_OK) {
        return status;
    }
    return (ack->status == MCCMD_OK) ? MCPCM_OK : MCPCM_ERR_STATE;
}

int mcpcm_open(struct mcpcm *pcm, const char *socket_path)
{
    if (pcm == NULL || socket_path == NULL) {
        return MCPCM_ERR_INVALID;
    }
    memset(pcm, 0, sizeof(*pcm));
    pcm->fd = mccmd_connect(socket_path, 500);
    if (pcm->fd < 0) {
        return MCPCM_ERR_IO;
    }
    pcm->state = MCPCM_STATE_SETUP;
    return MCPCM_OK;
}

int mcpcm_set_hw_params(struct mcpcm *pcm, const struct mcshm_config *config)
{
    struct mccmd_hw_params params;
    struct mccmd_ack ack;
    struct mcshm_config actual;
    int status;

    if (pcm == NULL || config == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (pcm->state != MCPCM_STATE_SETUP) {
        return MCPCM_ERR_STATE;
    }

    params.channels = config->channels;
    params.rate = config->rate;
    params.period_frames = config->period_frames;
    params.period_count = config->period_count;
    params.sample_bytes = config->sample_bytes;

    memset(&ack, 0, sizeof(ack));
    status = mcpcm_call(pcm, MCCMD_MSG_SET_HW_PARAMS, &params, (uint32_t)sizeof(params), &ack);
    if (status != MCPCM_OK) {
        return status;
    }

    memset(&pcm->ring, 0, sizeof(pcm->ring));
    if (mcshm_ring_open(&pcm->ring, ack.shm_name, &actual) != MCSHM_OK) {
        return MCPCM_ERR_IO;
    }
    if (actual.channels != config->channels || actual.period_frames != config->period_frames ||
        actual.period_count != config->period_count) {
        mcshm_ring_close(&pcm->ring);
        return MCPCM_ERR_IO;
    }
    pcm->config = actual;
    return MCPCM_OK;
}

int mcpcm_prepare(struct mcpcm *pcm)
{
    struct mccmd_ack ack;
    int status;

    if (pcm == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (pcm->state != MCPCM_STATE_SETUP && pcm->state != MCPCM_STATE_PREPARED) {
        return MCPCM_ERR_STATE;
    }
    memset(&ack, 0, sizeof(ack));
    status = mcpcm_call(pcm, MCCMD_MSG_PREPARE, NULL, 0u, &ack);
    if (status != MCPCM_OK) {
        return status;
    }
    pcm->periods = 0u;
    pcm->xruns = 0u;
    pcm->last_hw_ptr = 0u;
    pcm->state = MCPCM_STATE_PREPARED;
    return MCPCM_OK;
}

int mcpcm_start(struct mcpcm *pcm)
{
    struct mccmd_ack ack;
    int status;

    if (pcm == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (pcm->state != MCPCM_STATE_PREPARED) {
        return MCPCM_ERR_STATE;
    }
    memset(&ack, 0, sizeof(ack));
    status = mcpcm_call(pcm, MCCMD_MSG_START, NULL, 0u, &ack);
    if (status != MCPCM_OK) {
        return status;
    }
    pcm->state = MCPCM_STATE_RUNNING;
    return MCPCM_OK;
}

int mcpcm_wait_writable(struct mcpcm *pcm, int timeout_ms)
{
    int64_t deadline;
    uint64_t period_frames;

    if (pcm == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (pcm->state != MCPCM_STATE_PREPARED && pcm->state != MCPCM_STATE_RUNNING) {
        return MCPCM_ERR_STATE;
    }
    period_frames = pcm->config.period_frames;
    if (period_frames == 0u) {
        return MCPCM_ERR_STATE;
    }

    deadline = mcpcm_now_ms() + (int64_t)timeout_ms;
    for (;;) {
        struct mccmd_header header;
        unsigned char payload[MCCMD_MAX_PAYLOAD];
        int64_t remaining;
        int result;

        if (mcshm_ring_avail_write(&pcm->ring) >= period_frames) {
            return 1;
        }
        remaining = deadline - mcpcm_now_ms();
        if (remaining <= 0) {
            return 0;
        }
        result = mccmd_recv(pcm->fd, &header, payload, (uint32_t)sizeof(payload), (int)remaining);
        if (result == 0) {
            return 0;
        }
        if (result < 0) {
            return MCPCM_ERR_IO;
        }
        if (header.type == MCCMD_MSG_PERIOD && header.length == sizeof(struct mccmd_period)) {
            struct mccmd_period period;

            memcpy(&period, payload, sizeof(period));
            pcm->periods += 1u;
            pcm->last_hw_ptr = period.hw_ptr;
        } else if (header.type == MCCMD_MSG_XRUN && header.length == sizeof(struct mccmd_xrun)) {
            struct mccmd_xrun xrun;

            memcpy(&xrun, payload, sizeof(xrun));
            pcm->xruns += 1u;
            pcm->last_hw_ptr = xrun.hw_ptr;
        }
    }
}

long mcpcm_write(struct mcpcm *pcm, const void *frames, uint32_t frame_count)
{
    const uint8_t *source = (const uint8_t *)frames;
    uint32_t written = 0u;

    if (pcm == NULL || (frames == NULL && frame_count > 0u)) {
        return MCPCM_ERR_INVALID;
    }
    if (pcm->state != MCPCM_STATE_PREPARED && pcm->state != MCPCM_STATE_RUNNING) {
        return MCPCM_ERR_STATE;
    }
    if (frame_count == 0u) {
        return 0;
    }

    while (written < frame_count) {
        uint64_t accepted = mcshm_ring_write(&pcm->ring, source + (size_t)written * pcm->ring.stride,
                                             frame_count - written);

        if (accepted > 0u) {
            written += (uint32_t)accepted;
            continue;
        }
        /* Ring full: this is the backpressure path.  Wait for the device to
         * advance hw_ptr instead of spinning. */
        {
            int ready;

            pcm->waits += 1u;
            ready = mcpcm_wait_writable(pcm, 1000);

            if (ready == 0) {
                return MCPCM_ERR_TIMEOUT;
            }
            if (ready < 0) {
                return ready;
            }
        }
    }
    return (long)written;
}

int mcpcm_drain(struct mcpcm *pcm)
{
    struct mccmd_ack ack;
    int status;

    if (pcm == NULL) {
        return MCPCM_ERR_INVALID;
    }
    if (pcm->state != MCPCM_STATE_RUNNING) {
        return MCPCM_ERR_STATE;
    }
    pcm->state = MCPCM_STATE_DRAINING;
    memset(&ack, 0, sizeof(ack));
    status = mcpcm_call(pcm, MCCMD_MSG_DRAIN, NULL, 0u, &ack);
    if (status != MCPCM_OK) {
        return status;
    }
    pcm->state = MCPCM_STATE_SETUP;
    return MCPCM_OK;
}

void mcpcm_close(struct mcpcm *pcm)
{
    struct mccmd_ack ack;

    if (pcm == NULL) {
        return;
    }
    if (pcm->fd >= 0) {
        memset(&ack, 0, sizeof(ack));
        (void)mcpcm_call(pcm, MCCMD_MSG_SHUTDOWN, NULL, 0u, &ack);
    }
    if (pcm->ring.header != NULL) {
        mcshm_ring_close(&pcm->ring);
    }
    if (pcm->fd >= 0) {
        mccmd_close(pcm->fd);
    }
    pcm->fd = -1;
    pcm->state = MCPCM_STATE_CLOSED;
}

uint64_t mcpcm_hw_ptr(const struct mcpcm *pcm)
{
    if (pcm == NULL || pcm->ring.header == NULL) {
        return 0u;
    }
    return mcshm_ring_consumed(&pcm->ring);
}

uint64_t mcpcm_appl_ptr(const struct mcpcm *pcm)
{
    if (pcm == NULL || pcm->ring.header == NULL) {
        return 0u;
    }
    return mcshm_ring_written(&pcm->ring);
}

uint64_t mcpcm_xruns(const struct mcpcm *pcm)
{
    return (pcm != NULL) ? pcm->xruns : 0u;
}

uint64_t mcpcm_waits(const struct mcpcm *pcm)
{
    return (pcm != NULL) ? pcm->waits : 0u;
}
