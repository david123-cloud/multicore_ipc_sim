#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mcpcm_ops.h"

#define DEFAULT_SOCKET "/tmp/mc_ipc_m2.sock"
#define DEFAULT_DEVICE "pulse"
#define DEFAULT_PERIODS 100u

#define CHANNELS 2u
#define RATE 48000u
#define PERIOD_FRAMES 960u
#define PERIOD_COUNT 4u
#define SAMPLE_BYTES 2u
#define TONE_HZ 440.0
#define PI 3.14159265358979323846

static double now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int check_states(struct mcpcm_device *device, const void *silence, uint32_t frames)
{
    int failures = 0;

    if (mcpcm_dev_write(device, silence, frames) == MCPCM_ERR_STATE) {
        printf("  [ok]   write in setup refused\n");
    } else {
        printf("  [FAIL] write in setup accepted\n");
        failures++;
    }
    if (mcpcm_dev_start(device) == MCPCM_ERR_STATE) {
        printf("  [ok]   start in setup refused\n");
    } else {
        printf("  [FAIL] start in setup accepted\n");
        failures++;
    }
    return failures;
}

int main(int argc, char **argv)
{
    const char *socket_path = DEFAULT_SOCKET;
    const char *device_name = DEFAULT_DEVICE;
    const char *backend = "shm";
    uint32_t periods = DEFAULT_PERIODS;
    int stall = 0;
    int state_checks = 0;
    int socket_set = 0;
    struct mcpcm_device dev;
    struct mcshm_config config;
    int16_t *buffer;
    double phase = 0.0;
    double increment = 2.0 * PI * TONE_HZ / (double)RATE;
    double started;
    double elapsed;
    uint64_t frame_index = 0u;
    uint32_t period;
    int failures = 0;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        if (strcmp(argv[argi], "--stall") == 0) {
            stall = 1;
        } else if (strcmp(argv[argi], "--check-states") == 0) {
            state_checks = 1;
        } else if (strcmp(argv[argi], "--backend") == 0 && argi + 1 < argc) {
            backend = argv[++argi];
        } else if (strcmp(argv[argi], "--device") == 0 && argi + 1 < argc) {
            device_name = argv[++argi];
        } else if (!socket_set && strspn(argv[argi], "0123456789") != strlen(argv[argi])) {
            socket_path = argv[argi];
            socket_set = 1;
        } else {
            periods = (uint32_t)strtoul(argv[argi], NULL, 10);
        }
    }
    if (periods == 0u) {
        fprintf(stderr, "gvm: period count must be positive\n");
        return 1;
    }

    buffer = (int16_t *)malloc((size_t)PERIOD_FRAMES * CHANNELS * sizeof(int16_t));
    if (buffer == NULL) {
        fprintf(stderr, "gvm: out of memory\n");
        return 1;
    }

    if (strcmp(backend, "alsa") == 0) {
        if (mcpcm_alsa_create(&dev, device_name) != MCPCM_OK) {
            fprintf(stderr, "gvm: cannot open alsa device %s\n", device_name);
            free(buffer);
            return 1;
        }
    } else if (strcmp(backend, "shm") == 0) {
        if (mcpcm_shm_create(&dev, socket_path) != MCPCM_OK) {
            fprintf(stderr, "gvm: cannot connect to %s\n", socket_path);
            free(buffer);
            return 1;
        }
    } else {
        fprintf(stderr, "gvm: unknown backend %s\n", backend);
        free(buffer);
        return 1;
    }
    printf("GVM: backend=%s device=%s state=%s\n", dev.ops->name,
           (strcmp(backend, "alsa") == 0) ? device_name : socket_path,
           mcpcm_state_name(dev.state));

    config.channels = CHANNELS;
    config.rate = RATE;
    config.period_frames = PERIOD_FRAMES;
    config.period_count = PERIOD_COUNT;
    config.sample_bytes = SAMPLE_BYTES;

    if (mcpcm_dev_configure(&dev, &config) != MCPCM_OK) {
        fprintf(stderr, "gvm: configure refused\n");
        mcpcm_dev_close(&dev);
        free(buffer);
        return 1;
    }
    printf("GVM: configured, state=%s\n", mcpcm_state_name(dev.state));

    if (state_checks) {
        failures += check_states(&dev, buffer, PERIOD_FRAMES);
    }

    if (mcpcm_dev_prepare(&dev) != MCPCM_OK) {
        fprintf(stderr, "gvm: prepare failed\n");
        mcpcm_dev_close(&dev);
        free(buffer);
        return 1;
    }

    if (mcpcm_dev_start(&dev) != MCPCM_OK) {
        fprintf(stderr, "gvm: start failed\n");
        mcpcm_dev_close(&dev);
        free(buffer);
        return 1;
    }
    printf("GVM: running, state=%s\n", mcpcm_state_name(dev.state));

    if (state_checks) {
        /* ALSA allows prepare() in SETUP/PREPARED, but never while running. */
        if (mcpcm_dev_prepare(&dev) == MCPCM_OK) {
            printf("  [FAIL] prepare while running accepted\n");
            failures++;
        } else {
            printf("  [ok]   prepare while running refused\n");
        }
    }

    started = now_seconds();

    for (period = 0u; period < periods; ++period) {
        uint32_t frame;
        long written;

        for (frame = 0u; frame < PERIOD_FRAMES; ++frame) {
            float value = (float)(0.25 * sin(phase));

            phase += increment;
            if (phase > 2.0 * PI) {
                phase -= 2.0 * PI;
            }
            buffer[(size_t)frame * CHANNELS] = (int16_t)(value * 32767.0f);
            buffer[(size_t)frame * CHANNELS + 1u] = (int16_t)(value * 16384.0f);
        }

        /* A deliberately slow producer: with the simulated backend this shows
         * up as an underrun, with ALSA as a recovered -EPIPE. */
        if (stall && period == 25u) {
            struct timespec pause = {0, 250000000};

            printf("GVM: stalling 250 ms to provoke an underrun\n");
            fflush(stdout);
            nanosleep(&pause, NULL);
        }

        written = mcpcm_dev_write(&dev, buffer, PERIOD_FRAMES);
        if (written != (long)PERIOD_FRAMES) {
            fprintf(stderr, "gvm: write failed (%ld)\n", written);
            failures++;
            break;
        }
        frame_index += PERIOD_FRAMES;
    }

    elapsed = now_seconds() - started;

    if (mcpcm_dev_drain(&dev) != MCPCM_OK) {
        fprintf(stderr, "gvm: drain failed\n");
        failures++;
    }

    printf("GVM_STATS frames=%llu periods=%u full_events=%llu xruns=%llu elapsed_ms=%.1f\n",
           (unsigned long long)frame_index, (unsigned)periods,
           (unsigned long long)mcpcm_dev_waits(&dev), (unsigned long long)mcpcm_dev_xruns(&dev),
           elapsed * 1000.0);
    printf("GVM: audio %.3f s, wall %.3f s, hw_ptr=%llu\n",
           (double)frame_index / (double)RATE, elapsed,
           (unsigned long long)mcpcm_dev_hw_ptr(&dev));
    fflush(stdout);

    mcpcm_dev_close(&dev);
    free(buffer);

    if (failures != 0) {
        printf("gvm: FAILED (%d)\n", failures);
        return 1;
    }
    return 0;
}
