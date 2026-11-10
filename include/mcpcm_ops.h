#ifndef MCPCM_OPS_H
#define MCPCM_OPS_H

#include <stdint.h>

#include "mcpcm.h"
#include "mcshm_ring.h"

/*
 * Backend abstraction for the PCM client.
 *
 * Two implementations exist behind this interface:
 *
 *   mcpcm_shm  - the simulated device: control channel + shared memory ring,
 *                hw_ptr advanced by the PVM process (this is what M2..M5 test)
 *   mcpcm_alsa - the real ALSA stack: snd_pcm_* on a named PCM such as
 *                "pulse", "null" or "hw:0,0"
 *
 * The state machine lives in the facade, so both backends reject the same
 * illegal transitions and the application code cannot tell them apart.
 */

struct mcpcm_ops {
    const char *name;
    int (*configure)(void *ctx, const struct mcshm_config *config);
    int (*prepare)(void *ctx);
    int (*start)(void *ctx);
    long (*write)(void *ctx, const void *frames, uint32_t frame_count);
    int (*drain)(void *ctx);
    void (*close)(void *ctx);
    uint64_t (*hw_ptr)(const void *ctx);
    uint64_t (*xruns)(const void *ctx);
    uint64_t (*waits)(const void *ctx);
};

struct mcpcm_device {
    const struct mcpcm_ops *ops;
    void *ctx;
    enum mcpcm_state state;
    struct mcshm_config config;
};

/* Factories.  Both return MCPCM_OK or a negative error. */
int mcpcm_shm_create(struct mcpcm_device *device, const char *socket_path);
int mcpcm_alsa_create(struct mcpcm_device *device, const char *pcm_name);

/* Facade: same contract for every backend. */
int mcpcm_dev_configure(struct mcpcm_device *device, const struct mcshm_config *config);
int mcpcm_dev_prepare(struct mcpcm_device *device);
int mcpcm_dev_start(struct mcpcm_device *device);
long mcpcm_dev_write(struct mcpcm_device *device, const void *frames, uint32_t frame_count);
int mcpcm_dev_drain(struct mcpcm_device *device);
void mcpcm_dev_close(struct mcpcm_device *device);

uint64_t mcpcm_dev_hw_ptr(const struct mcpcm_device *device);
uint64_t mcpcm_dev_xruns(const struct mcpcm_device *device);
uint64_t mcpcm_dev_waits(const struct mcpcm_device *device);

#endif /* MCPCM_OPS_H */
