#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agc.h"
#include "biquad.h"
#include "dsp_error.h"
#include "dsp_pipeline.h"
#include "fft.h"
#include "mccmd.h"
#include "mcshm_ring.h"
#include "mcs16.h"
#include "ns.h"

#define DEFAULT_SOCKET "/tmp/mc_ipc_m2.sock"
#define ANALYSIS_SIZE 1024u /* 960-frame period zero-padded */
#define ANALYSIS_EVERY 10u  /* analyse one period in ten */

struct dsp_engine {
    int fd;
    struct mcshm_ring ring;
    uint32_t channels;
    uint32_t period_frames;

    struct dsp_pipeline pipe;

    int16_t *s16;
    float *interleaved;
    float *processed;

    struct fft_plan plan;
    float *analysis;

    uint64_t frames;
    uint64_t periods;
    uint64_t empty_events;
    double energy_in;
    double energy_out;
    uint32_t peak_bin;
    double peak_magnitude;
};

static void engine_free(struct dsp_engine *engine)
{
    pipeline_destroy(&engine->pipe);
    fft_plan_destroy(&engine->plan);
    free(engine->analysis);
    free(engine->s16);
    free(engine->interleaved);
    free(engine->processed);
    if (engine->ring.header != NULL) {
        mcshm_ring_close(&engine->ring);
    }
    if (engine->fd >= 0) {
        mccmd_close(engine->fd);
    }
}

static int engine_setup_chain(struct dsp_engine *engine)
{
    struct biquad_config filter;
    struct ns_config gate;
    struct dsp_module *module;

    memset(&engine->pipe, 0, sizeof(engine->pipe));
    if (pipeline_init(&engine->pipe, engine->period_frames, engine->channels) != DSP_OK) {
        return -1;
    }

    filter.type = BIQUAD_HIGHPASS;
    filter.fc = 100.0f;
    filter.q = 0.70710678f;
    filter.gain = 1.0f;
    module = biquad_module_create(&filter, 48000);
    if (module == NULL || pipeline_add(&engine->pipe, module) != DSP_OK) {
        return -1;
    }

    ns_config_default(&gate);
    module = ns_module_create(&gate, 48000);
    if (module == NULL || pipeline_add(&engine->pipe, module) != DSP_OK) {
        return -1;
    }

    module = create_agc_module();
    if (module == NULL || pipeline_add(&engine->pipe, module) != DSP_OK) {
        return -1;
    }
    return 0;
}

/* Peak bin of the zero-padded spectrum of channel 0.  The resolution here is
 * 46.875 Hz, so this reports a bin centre rather than a precise pitch. */
static void engine_analyse(struct dsp_engine *engine)
{
    uint32_t index;
    uint32_t bin;
    uint32_t best = 0u;
    float best_magnitude = 0.0f;

    for (index = 0u; index < ANALYSIS_SIZE; ++index) {
        engine->analysis[index] = (index < engine->period_frames)
                                      ? engine->interleaved[(size_t)index * engine->channels]
                                      : 0.0f;
    }
    if (fft_plan_forward(&engine->plan, engine->analysis, ANALYSIS_SIZE) != DSP_OK) {
        return;
    }
    for (bin = 1u; bin < ANALYSIS_SIZE / 2u; ++bin) {
        float magnitude = hypotf(engine->plan.real[bin], engine->plan.imag[bin]);

        if (magnitude > best_magnitude) {
            best_magnitude = magnitude;
            best = bin;
        }
    }
    if ((double)best_magnitude > engine->peak_magnitude) {
        engine->peak_magnitude = (double)best_magnitude;
        engine->peak_bin = best;
    }
}

static int engine_process_period(struct dsp_engine *engine, uint32_t frames)
{
    size_t count = (size_t)frames * engine->channels;
    struct dsp_buffer in_view;
    struct dsp_buffer out_view;
    size_t index;

    mcs16_to_float(engine->s16, engine->interleaved, count);

    in_view.data = engine->interleaved;
    in_view.frames = frames;
    in_view.channels = engine->channels;
    out_view.data = engine->processed;
    out_view.frames = frames;
    out_view.channels = engine->channels;

    if (pipeline_process(&engine->pipe, &in_view, &out_view) != DSP_OK) {
        return -1;
    }

    for (index = 0u; index < count; ++index) {
        double in = (double)in_view.data[index];
        double out = (double)out_view.data[index];

        engine->energy_in += in * in;
        engine->energy_out += out * out;
    }

    if (engine->periods % ANALYSIS_EVERY == 0u) {
        engine_analyse(engine);
    }
    return 0;
}

static int engine_drain_one_period(struct dsp_engine *engine)
{
    uint64_t got = mcshm_ring_read(&engine->ring, engine->s16, engine->period_frames);

    if (got == 0u) {
        engine->empty_events += 1u;
        return 0;
    }
    if (engine_process_period(engine, (uint32_t)got) != 0) {
        return -1;
    }
    engine->frames += got;
    engine->periods += 1u;
    return 0;
}

int main(int argc, char **argv)
{
    const char *socket_path = DEFAULT_SOCKET;
    uint32_t requested_channels = 2u;
    int socket_set = 0;
    int argi;
    struct dsp_engine engine;
    struct mccmd_hw_params params;
    struct mccmd_header header;
    unsigned char payload[MCCMD_MAX_PAYLOAD];
    struct mccmd_ack ack;
    struct mcshm_config actual;
    char dsp_path[MCCMD_MAX_NAME];
    size_t count;
    int result;
    int running = 1;

    memset(&engine, 0, sizeof(engine));
    engine.fd = -1;

    for (argi = 1; argi < argc; ++argi) {
        if (strcmp(argv[argi], "--channels") == 0 && argi + 1 < argc) {
            requested_channels = (uint32_t)strtoul(argv[++argi], NULL, 10);
        } else if (!socket_set) {
            socket_path = argv[argi];
            socket_set = 1;
        }
    }
    if (requested_channels == 0u || requested_channels > 32u) {
        fprintf(stderr, "dsp: channel count must be 1..32\n");
        return 1;
    }

    if ((size_t)snprintf(dsp_path, sizeof(dsp_path), "%s.dsp", socket_path) >= sizeof(dsp_path)) {
        fprintf(stderr, "dsp: socket path too long\n");
        return 1;
    }

    engine.fd = mccmd_connect(dsp_path, 500);
    if (engine.fd < 0) {
        fprintf(stderr, "dsp: cannot connect to %s\n", dsp_path);
        return 1;
    }

    params.channels = requested_channels;
    params.rate = 48000u;
    params.period_frames = 960u;
    params.period_count = 4u;
    params.sample_bytes = 2u;

    if (mccmd_send(engine.fd, MCCMD_MSG_SET_HW_PARAMS, 0u, &params, (uint32_t)sizeof(params)) !=
        MCCMD_OK) {
        fprintf(stderr, "dsp: cannot send hw_params\n");
        engine_free(&engine);
        return 1;
    }
    result = mccmd_recv(engine.fd, &header, payload, (uint32_t)sizeof(payload), 2000);
    if (result != 1 || header.type != MCCMD_MSG_ACK || header.length != sizeof(ack)) {
        fprintf(stderr, "dsp: no hw_params ack\n");
        engine_free(&engine);
        return 1;
    }
    memcpy(&ack, payload, sizeof(ack));
    if (ack.status != MCCMD_OK) {
        fprintf(stderr, "dsp: hw_params refused (%d)\n", ack.status);
        engine_free(&engine);
        return 1;
    }

    if (mcshm_ring_open(&engine.ring, ack.shm_name, &actual) != MCSHM_OK) {
        fprintf(stderr, "dsp: cannot attach to %s\n", ack.shm_name);
        engine_free(&engine);
        return 1;
    }
    engine.channels = actual.channels;
    engine.period_frames = actual.period_frames;
    if (engine.channels > NS_MAX_CHANNELS || engine.period_frames == 0u) {
        fprintf(stderr, "dsp: unsupported geometry\n");
        engine_free(&engine);
        return 1;
    }

    count = (size_t)engine.period_frames * engine.channels;
    engine.s16 = (int16_t *)malloc(count * sizeof(int16_t));
    engine.interleaved = (float *)malloc(count * sizeof(float));
    engine.processed = (float *)malloc(count * sizeof(float));
    engine.analysis = (float *)calloc(ANALYSIS_SIZE, sizeof(float));
    if (engine.s16 == NULL || engine.interleaved == NULL || engine.processed == NULL ||
        engine.analysis == NULL) {
        fprintf(stderr, "dsp: out of memory\n");
        engine_free(&engine);
        return 1;
    }
    if (fft_plan_init(&engine.plan, ANALYSIS_SIZE) != DSP_OK) {
        fprintf(stderr, "dsp: fft plan failed\n");
        engine_free(&engine);
        return 1;
    }
    if (engine_setup_chain(&engine) != 0) {
        fprintf(stderr, "dsp: pipeline setup failed\n");
        engine_free(&engine);
        return 1;
    }

    printf("DSP: attached %s, %u ch, %u frames/period, %u module(s)\n", ack.shm_name,
           (unsigned)engine.channels, (unsigned)engine.period_frames,
           (unsigned)engine.pipe.module_count);
    fflush(stdout);

    while (running) {
        result = mccmd_recv(engine.fd, &header, payload, (uint32_t)sizeof(payload), 500);
        if (result == 0) {
            continue;
        }
        if (result < 0) {
            break;
        }
        if (header.type == MCCMD_MSG_PERIOD) {
            if (engine_drain_one_period(&engine) != 0) {
                fprintf(stderr, "dsp: processing failed\n");
                break;
            }
        } else if (header.type == MCCMD_MSG_SHUTDOWN) {
            while (mcshm_ring_avail_read(&engine.ring) > 0u) {
                if (engine_drain_one_period(&engine) != 0) {
                    break;
                }
            }
            running = 0;
        }
    }

    printf("DSP_STATS frames=%llu periods=%llu empty=%llu channels=%u\n",
           (unsigned long long)engine.frames, (unsigned long long)engine.periods,
           (unsigned long long)engine.empty_events, (unsigned)engine.channels);
    if (engine.frames > 0u) {
        printf("DSP: rms in %.6f out %.6f over %llu frames\n",
               sqrt(engine.energy_in / (double)(engine.frames * engine.channels)),
               sqrt(engine.energy_out / (double)(engine.frames * engine.channels)),
               (unsigned long long)engine.frames);
        printf("DSP: peak bin %u -> %.1f Hz (resolution %.1f Hz)\n", (unsigned)engine.peak_bin,
               (double)engine.peak_bin * 48000.0 / (double)ANALYSIS_SIZE,
               48000.0 / (double)ANALYSIS_SIZE);
    }
    fflush(stdout);

    engine_free(&engine);
    return 0;
}
