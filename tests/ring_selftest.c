#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mcshm_ring.h"

#define TEST_NAME "/mcshm_selftest"
#define CHANNELS 2u
#define RATE 48000u
#define PERIOD_FRAMES 960u
#define PERIOD_COUNT 4u
#define SAMPLE_BYTES 2u
#define RING_FRAMES (PERIOD_FRAMES * PERIOD_COUNT)
#define STRIDE (CHANNELS * SAMPLE_BYTES)
#define BLOCK_FRAMES 960u

static int g_failures;
static uint16_t g_tx[RING_FRAMES * CHANNELS];
static uint16_t g_rx[RING_FRAMES * CHANNELS];

static void check(int condition, const char *message)
{
    if (condition) {
        printf("  [ok]   %s\n", message);
    } else {
        printf("  [FAIL] %s\n", message);
        g_failures++;
    }
}

static uint16_t pattern_at(uint64_t frame, uint32_t channel)
{
    uint64_t mixed = frame * 131u + (uint64_t)channel * 7919u;

    return (uint16_t)((mixed ^ (mixed >> 7)) & 0xFFFFu);
}

static void fill_block(uint16_t *block, uint64_t start_frame, uint32_t frames, uint32_t channels)
{
    uint32_t frame;
    uint32_t channel;

    for (frame = 0u; frame < frames; ++frame) {
        for (channel = 0u; channel < channels; ++channel) {
            block[(size_t)frame * channels + channel] = pattern_at(start_frame + frame, channel);
        }
    }
}

static int verify_block(const uint16_t *block, uint64_t start_frame, uint32_t frames,
                        uint32_t channels)
{
    uint32_t frame;
    uint32_t channel;

    for (frame = 0u; frame < frames; ++frame) {
        for (channel = 0u; channel < channels; ++channel) {
            uint16_t expected = pattern_at(start_frame + frame, channel);

            if (block[(size_t)frame * channels + channel] != expected) {
                printf("  mismatch at frame %llu channel %u: got %u expected %u\n",
                       (unsigned long long)(start_frame + frame), channel,
                       (unsigned)block[(size_t)frame * channels + channel], (unsigned)expected);
                return 0;
            }
        }
    }
    return 1;
}

static void fill_config(struct mcshm_config *cfg)
{
    cfg->channels = CHANNELS;
    cfg->rate = RATE;
    cfg->period_frames = PERIOD_FRAMES;
    cfg->period_count = PERIOD_COUNT;
    cfg->sample_bytes = SAMPLE_BYTES;
}

static void test_config_validation(void)
{
    struct mcshm_config cfg;
    size_t size = 0u;

    printf("config validation\n");

    check(mcshm_config_validate(NULL) == MCSHM_ERR_INVALID, "NULL config rejected");

    fill_config(&cfg);
    cfg.channels = 0u;
    check(mcshm_config_validate(&cfg) == MCSHM_ERR_RANGE, "zero channels rejected");

    fill_config(&cfg);
    cfg.channels = MCSHM_MAX_CHANNELS + 1u;
    check(mcshm_config_validate(&cfg) == MCSHM_ERR_RANGE, "33 channels rejected");

    fill_config(&cfg);
    cfg.period_frames = 0u;
    check(mcshm_config_validate(&cfg) == MCSHM_ERR_RANGE, "zero period frames rejected");

    fill_config(&cfg);
    cfg.period_count = 0u;
    check(mcshm_config_validate(&cfg) == MCSHM_ERR_RANGE, "zero period count rejected");

    fill_config(&cfg);
    cfg.period_count = MCSHM_MAX_PERIODS + 1u;
    check(mcshm_config_validate(&cfg) == MCSHM_ERR_RANGE, "too many periods rejected");

    fill_config(&cfg);
    cfg.sample_bytes = 3u;
    check(mcshm_config_validate(&cfg) == MCSHM_ERR_RANGE, "sample_bytes 3 rejected");

    fill_config(&cfg);
    check(mcshm_config_validate(&cfg) == MCSHM_OK, "valid config accepted");
    check(mcshm_ring_size(&cfg, &size) == MCSHM_OK, "ring size computed");
    check(size > (size_t)(RING_FRAMES * STRIDE), "size covers header and payload");
}

static void test_basic_transfer(void)
{
    struct mcshm_ring ring;
    struct mcshm_config cfg;
    uint64_t written;

    printf("basic transfer\n");
    memset(&ring, 0, sizeof(ring));
    fill_config(&cfg);

    check(mcshm_ring_create(&ring, TEST_NAME, &cfg) == MCSHM_OK, "create");
    check(ring.ring_frames == RING_FRAMES, "ring_frames from geometry");
    check(ring.stride == STRIDE, "stride from geometry");
    check(mcshm_ring_avail_write(&ring) == RING_FRAMES, "initially empty");
    check(mcshm_ring_avail_read(&ring) == 0u, "nothing to read");

    fill_block(g_tx, 0u, BLOCK_FRAMES, CHANNELS);
    written = mcshm_ring_write(&ring, g_tx, BLOCK_FRAMES);
    check(written == BLOCK_FRAMES, "first block written");
    check(mcshm_ring_avail_read(&ring) == BLOCK_FRAMES, "bytes became available");

    memset(g_rx, 0, sizeof(g_rx));
    check(mcshm_ring_read(&ring, g_rx, BLOCK_FRAMES) == BLOCK_FRAMES, "first block read");
    check(verify_block(g_rx, 0u, BLOCK_FRAMES, CHANNELS), "first block intact");

    mcshm_ring_close(&ring);
    mcshm_ring_close(&ring); /* second close is a no-op */
    check(1, "double close is safe");
    check(mcshm_ring_unlink(TEST_NAME) == MCSHM_OK, "unlink");
    check(mcshm_ring_unlink(TEST_NAME) == MCSHM_ERR_NOT_FOUND, "second unlink reports missing");
}

static void test_full_and_empty(void)
{
    struct mcshm_ring ring;
    struct mcshm_config cfg;
    uint64_t total = 0u;
    uint32_t index;

    printf("full and empty behaviour\n");
    memset(&ring, 0, sizeof(ring));
    fill_config(&cfg);
    check(mcshm_ring_create(&ring, TEST_NAME, &cfg) == MCSHM_OK, "create");

    for (index = 0u; index < PERIOD_COUNT; ++index) {
        fill_block(g_tx, (uint64_t)index * BLOCK_FRAMES, BLOCK_FRAMES, CHANNELS);
        total += mcshm_ring_write(&ring, g_tx, BLOCK_FRAMES);
    }
    check(total == RING_FRAMES, "ring filled to capacity");
    check(mcshm_ring_avail_write(&ring) == 0u, "no write space left");

    fill_block(g_tx, 0u, BLOCK_FRAMES, CHANNELS);
    check(mcshm_ring_write(&ring, g_tx, BLOCK_FRAMES) == 0u, "write into a full ring returns 0");
    check(mcshm_ring_full_count(&ring) == 1u, "full event counted");

    check(mcshm_ring_read(&ring, g_rx, RING_FRAMES) == RING_FRAMES, "drained everything");
    check(mcshm_ring_read(&ring, g_rx, BLOCK_FRAMES) == 0u, "read from an empty ring returns 0");
    check(mcshm_ring_empty_count(&ring) == 1u, "empty event counted");

    mcshm_ring_close(&ring);
    (void)mcshm_ring_unlink(TEST_NAME);
}

static void test_wraparound(void)
{
    struct mcshm_ring ring;
    struct mcshm_config cfg;
    uint64_t frame_index = 0u;
    uint32_t iteration;
    int intact = 1;

    printf("wraparound over 1000 blocks\n");
    memset(&ring, 0, sizeof(ring));
    fill_config(&cfg);
    check(mcshm_ring_create(&ring, TEST_NAME, &cfg) == MCSHM_OK, "create");

    for (iteration = 0u; iteration < 1000u; ++iteration) {
        fill_block(g_tx, frame_index, BLOCK_FRAMES, CHANNELS);
        if (mcshm_ring_write(&ring, g_tx, BLOCK_FRAMES) != BLOCK_FRAMES) {
            intact = 0;
            break;
        }
        memset(g_rx, 0, sizeof(g_rx));
        if (mcshm_ring_read(&ring, g_rx, BLOCK_FRAMES) != BLOCK_FRAMES) {
            intact = 0;
            break;
        }
        if (!verify_block(g_rx, frame_index, BLOCK_FRAMES, CHANNELS)) {
            intact = 0;
            break;
        }
        frame_index += BLOCK_FRAMES;
    }
    check(intact, "data survives repeated wrap-around");
    check(mcshm_ring_written(&ring) == frame_index, "producer counter matches");
    check(mcshm_ring_consumed(&ring) == frame_index, "consumer counter matches");

    mcshm_ring_close(&ring);
    (void)mcshm_ring_unlink(TEST_NAME);
}

static void child_consumer(void)
{
    struct mcshm_ring ring;
    struct mcshm_config cfg;
    uint64_t frame_index = 0u;
    uint32_t blocks = 0u;
    int intact = 1;
    struct timespec pause = {0, 200000};

    memset(&ring, 0, sizeof(ring));
    if (mcshm_ring_open(&ring, TEST_NAME, &cfg) != MCSHM_OK) {
        _exit(2);
    }
    if (cfg.channels != CHANNELS || cfg.sample_bytes != SAMPLE_BYTES) {
        mcshm_ring_close(&ring);
        _exit(3);
    }

    while (blocks < 500u) {
        uint64_t got = mcshm_ring_read(&ring, g_rx, BLOCK_FRAMES);

        if (got == 0u) {
            nanosleep(&pause, NULL);
            continue;
        }
        if (got != BLOCK_FRAMES || !verify_block(g_rx, frame_index, BLOCK_FRAMES, CHANNELS)) {
            intact = 0;
            break;
        }
        frame_index += BLOCK_FRAMES;
        blocks++;
    }

    mcshm_ring_close(&ring);
    _exit(intact ? 0 : 4);
}

static void test_two_processes(void)
{
    struct mcshm_ring ring;
    struct mcshm_config cfg;
    pid_t child;
    int status = 0;
    uint64_t frame_index = 0u;
    uint32_t blocks = 0u;
    struct timespec pause = {0, 200000};

    printf("two processes, 500 blocks each\n");
    memset(&ring, 0, sizeof(ring));
    fill_config(&cfg);
    check(mcshm_ring_create(&ring, TEST_NAME, &cfg) == MCSHM_OK, "producer created segment");

    child = fork();
    if (child == 0) {
        child_consumer();
    }
    check(child > 0, "consumer forked");

    while (blocks < 500u) {
        fill_block(g_tx, frame_index, BLOCK_FRAMES, CHANNELS);
        if (mcshm_ring_write(&ring, g_tx, BLOCK_FRAMES) != BLOCK_FRAMES) {
            nanosleep(&pause, NULL);
            continue;
        }
        frame_index += BLOCK_FRAMES;
        blocks++;
    }

    if (child > 0) {
        (void)waitpid(child, &status, 0);
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "consumer verified every block");
    check(mcshm_ring_consumed(&ring) == frame_index, "consumer drained everything");

    mcshm_ring_close(&ring);
    (void)mcshm_ring_unlink(TEST_NAME);
}

static void test_lifecycle(void)
{
    struct mcshm_ring ring;
    struct mcshm_ring other;
    struct mcshm_config cfg;

    printf("lifecycle\n");
    memset(&ring, 0, sizeof(ring));
    fill_config(&cfg);

    check(mcshm_ring_create(&ring, TEST_NAME, &cfg) == MCSHM_OK, "create");
    check(mcshm_ring_create(&ring, TEST_NAME, &cfg) == MCSHM_ERR_MAPPED,
          "second create on the same handle rejected");

    memset(&other, 0, sizeof(other));
    check(mcshm_ring_open(&other, TEST_NAME, NULL) == MCSHM_OK, "second handle attaches");
    check(other.ring_frames == RING_FRAMES, "attached handle sees the geometry");
    mcshm_ring_close(&other);

    /* Recreating must clear a stale segment instead of failing. */
    check(mcshm_ring_create(&ring, TEST_NAME, &cfg) == MCSHM_ERR_MAPPED,
          "still mapped while open");
    mcshm_ring_close(&ring);
    check(mcshm_ring_create(&ring, TEST_NAME, &cfg) == MCSHM_OK, "stale segment recreated");
    check(mcshm_ring_written(&ring) == 0u, "recreated segment starts at zero");

    mcshm_ring_close(&ring);
    (void)mcshm_ring_unlink(TEST_NAME);
}

int main(void)
{
    test_config_validation();
    test_basic_transfer();
    test_full_and_empty();
    test_wraparound();
    test_two_processes();
    test_lifecycle();

    printf("%s\n", g_failures == 0 ? "ring selftest: ok" : "ring selftest: FAILED");
    return g_failures == 0 ? 0 : 1;
}
