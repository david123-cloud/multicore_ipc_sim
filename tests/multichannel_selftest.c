#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "agc.h"
#include "biquad.h"
#include "dsp_error.h"
#include "dsp_pipeline.h"
#include "ns.h"

#define CHANNELS 32u
#define FRAMES 960u
#define REJECT_CHANNELS 33u

static float g_in[REJECT_CHANNELS * FRAMES];
static float g_out[REJECT_CHANNELS * FRAMES];
static int g_failures;

static void check(int condition, const char *message)
{
    if (condition) {
        printf("  [ok]   %s\n", message);
    } else {
        printf("  [FAIL] %s\n", message);
        g_failures++;
    }
}

static int build_chain(struct dsp_pipeline *pipe, uint32_t channels)
{
    struct biquad_config filter;
    struct ns_config gate;
    struct dsp_module *module;

    memset(pipe, 0, sizeof(*pipe));
    if (pipeline_init(pipe, FRAMES, channels) != DSP_OK) {
        return -1;
    }
    filter.type = BIQUAD_HIGHPASS;
    filter.fc = 100.0f;
    filter.q = 0.70710678f;
    filter.gain = 1.0f;
    module = biquad_module_create(&filter, 48000);
    if (module == NULL || pipeline_add(pipe, module) != DSP_OK) {
        return -1;
    }
    ns_config_default(&gate);
    module = ns_module_create(&gate, 48000);
    if (module == NULL || pipeline_add(pipe, module) != DSP_OK) {
        return -1;
    }
    module = create_agc_module();
    if (module == NULL || pipeline_add(pipe, module) != DSP_OK) {
        return -1;
    }
    return 0;
}

int main(void)
{
    struct dsp_pipeline pipe;
    struct dsp_buffer in_view;
    struct dsp_buffer out_view;
    uint32_t channel;
    uint32_t frame;
    int all_finite = 1;
    double silent_energy = 0.0;
    double loud_energy = 0.0;

    memset(g_in, 0, sizeof(g_in));
    memset(g_out, 0, sizeof(g_out));

    check(build_chain(&pipe, CHANNELS) == 0, "32-channel pipeline with biquad + ns + agc");

    /* Only channel 5 carries a tone; every other channel stays silent. */
    for (frame = 0u; frame < FRAMES; ++frame) {
        g_in[(size_t)frame * CHANNELS + 5u] =
            0.25f * sinf(2.0f * 3.14159265358979323846f * 440.0f * (float)frame / 48000.0f);
    }

    in_view.data = g_in;
    in_view.frames = FRAMES;
    in_view.channels = CHANNELS;
    out_view.data = g_out;
    out_view.frames = FRAMES;
    out_view.channels = CHANNELS;

    check(pipeline_process(&pipe, &in_view, &out_view) == DSP_OK, "32-channel block processed");

    for (frame = 0u; frame < FRAMES; ++frame) {
        for (channel = 0u; channel < CHANNELS; ++channel) {
            float value = g_out[(size_t)frame * CHANNELS + channel];

            if (!isfinite(value)) {
                all_finite = 0;
            }
            if (channel == 5u) {
                loud_energy += (double)value * (double)value;
            } else {
                silent_energy += (double)value * (double)value;
            }
        }
    }
    check(all_finite, "all 32 output channels are finite");
    check(loud_energy > 1.0, "the tone survives on channel 5");
    check(silent_energy < 1e-6, "the other 31 channels stay silent (independent state)");

    pipeline_destroy(&pipe);

    /* One channel past the limit must be refused, not silently truncated. */
    {
        struct dsp_pipeline wide;
        struct dsp_buffer wide_in;
        struct dsp_buffer wide_out;

        check(build_chain(&wide, REJECT_CHANNELS) == 0, "pipeline accepts 33-channel geometry");
        wide_in.data = g_in;
        wide_in.frames = 8u;
        wide_in.channels = REJECT_CHANNELS;
        wide_out.data = g_out;
        wide_out.frames = 8u;
        wide_out.channels = REJECT_CHANNELS;
        check(pipeline_process(&wide, &wide_in, &wide_out) == DSP_ERR_UNSUPPORTED,
              "33 channels refused by the module");
        pipeline_destroy(&wide);
    }

    printf("%s\n", g_failures == 0 ? "multichannel selftest: ok" : "multichannel selftest: FAILED");
    return g_failures == 0 ? 0 : 1;
}
