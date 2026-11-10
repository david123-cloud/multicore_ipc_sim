#ifndef MCPCM_H
#define MCPCM_H

#include <stdint.h>

#include "mccmd.h"
#include "mcshm_ring.h"

/*
 * mcpcm - client side of the simulated PCM device.
 *
 * Models the subset of ALSA playback that this project needs to demonstrate:
 *
 *   CLOSED -> SETUP -> PREPARED -> RUNNING -> DRAINING -> SETUP
 *                          ^          |
 *                          +--- xrun -+
 *
 *   - hw_ptr is the device position.  It advances only when the device
 *     consumes a period, never because the application wrote something.
 *   - appl_ptr is the application position, advanced by mcpcm_write().
 *   - every device period arrives as an async mccmd PERIOD notification,
 *     which is what wakes a blocked write: the poll model, not a busy loop.
 *
 * Calling an operation in the wrong state fails with MCPCM_ERR_STATE instead
 * of silently doing something reasonable.
 */

#define MCPCM_OK 0
#define MCPCM_ERR_INVALID (-1)
#define MCPCM_ERR_IO (-2)
#define MCPCM_ERR_STATE (-3)
#define MCPCM_ERR_TIMEOUT (-4)

enum mcpcm_state {
    MCPCM_STATE_CLOSED = 0,
    MCPCM_STATE_SETUP,
    MCPCM_STATE_PREPARED,
    MCPCM_STATE_RUNNING,
    MCPCM_STATE_DRAINING
};

struct mcpcm {
    int fd;
    uint32_t seq;
    enum mcpcm_state state;
    struct mcshm_ring ring;
    struct mcshm_config config;
    uint64_t periods;    /* period notifications received */
    uint64_t xruns;      /* underrun notifications received */
    uint64_t waits;      /* write() had to wait for space (backpressure) */
    uint64_t last_hw_ptr;
};

const char *mcpcm_state_name(enum mcpcm_state state);

int mcpcm_open(struct mcpcm *pcm, const char *socket_path);
int mcpcm_set_hw_params(struct mcpcm *pcm, const struct mcshm_config *config);
int mcpcm_prepare(struct mcpcm *pcm);
int mcpcm_start(struct mcpcm *pcm);
int mcpcm_drain(struct mcpcm *pcm);
void mcpcm_close(struct mcpcm *pcm);

/* Waits until at least one period of space is available.
 *   1 = writable, 0 = timeout, negative = error. */
int mcpcm_wait_writable(struct mcpcm *pcm, int timeout_ms);

/* Blocks until every frame is accepted (like a blocking ALSA write).
 * Returns the number of frames written, or a negative MCPCM_ERR_*. */
long mcpcm_write(struct mcpcm *pcm, const void *frames, uint32_t frame_count);

uint64_t mcpcm_hw_ptr(const struct mcpcm *pcm);
uint64_t mcpcm_appl_ptr(const struct mcpcm *pcm);
uint64_t mcpcm_xruns(const struct mcpcm *pcm);
uint64_t mcpcm_waits(const struct mcpcm *pcm);

#endif /* MCPCM_H */
