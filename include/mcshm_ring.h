#ifndef MCSHM_RING_H
#define MCSHM_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * mcshm_ring - single-producer / single-consumer shared memory ring buffer.
 *
 * One side creates a POSIX shared memory segment, initialises a header and a
 * data region, and both sides map the same segment.  The producer appends
 * frames, the consumer drains them; both advance a monotonic 64-bit counter
 * instead of a wrapping index, so wrap-around arithmetic cannot corrupt the
 * buffer.
 *
 * Layout
 *
 *   +---------------------------+ 0
 *   | struct mcshm_header       |  control block, cache-line aligned counters
 *   +---------------------------+ data_offset (page aligned)
 *   | ring_frames * stride      |  interleaved PCM payload
 *   +---------------------------+
 *
 * All shared-state updates use C11 atomics with release/acquire ordering: the
 * payload bytes are published by the release store of write_frames and read
 * after the acquire load, which keeps the copy correct on weakly ordered CPUs
 * as well.
 */

#define MCSHM_MAGIC 0x4D435348u /* "MCSH" */
#define MCSHM_VERSION 1u

#define MCSHM_MAX_CHANNELS 32u
#define MCSHM_MAX_PERIOD_FRAMES 4800u /* 100 ms at 48 kHz */
#define MCSHM_MAX_PERIODS 16u
#define MCSHM_MAX_NAME 64u
#define MCSHM_LINE 64u

/* Status codes; every fallible call returns MCSHM_OK (0) or one of these. */
#define MCSHM_OK 0
#define MCSHM_ERR_INVALID (-1)
#define MCSHM_ERR_RANGE (-2)
#define MCSHM_ERR_IO (-3)
#define MCSHM_ERR_MAPPED (-4)
#define MCSHM_ERR_MAGIC (-5)
#define MCSHM_ERR_EXISTS (-6)
#define MCSHM_ERR_NOT_FOUND (-7)
#define MCSHM_ERR_NOMEM (-8)

/* Geometry of one simulated PCM stream. */
struct mcshm_config {
    uint32_t channels;
    uint32_t rate;
    uint32_t period_frames;
    uint32_t period_count;
    uint32_t sample_bytes; /* 2 for S16_LE */
};

/* Control block.  Counters are monotonic and never reset while the segment
 * lives; indexes are derived modulo ring_frames on use. */
struct mcshm_header {
    uint32_t magic;
    uint32_t version;
    uint32_t channels;
    uint32_t rate;
    uint32_t period_frames;
    uint32_t period_count;
    uint32_t sample_bytes;
    uint32_t reserved;
    uint64_t ring_frames;
    uint64_t data_offset;

    /* Producer-owned counter; the release store publishes the payload. */
    _Alignas(MCSHM_LINE) _Atomic uint64_t write_frames;
    /* Consumer-owned counter; the acquire load orders the payload read. */
    _Atomic uint64_t read_frames;

    /* Diagnostics, updated by the side that detects the condition. */
    _Atomic uint64_t full_count;  /* producer could not fit the request */
    _Atomic uint64_t empty_count; /* consumer found nothing to read */
};

/* Process-local handle; the mapping belongs to this process only. */
struct mcshm_ring {
    struct mcshm_header *header;
    uint8_t *data;
    size_t map_size;
    uint64_t ring_frames;
    uint32_t stride; /* channels * sample_bytes */
    char name[MCSHM_MAX_NAME];
    int created; /* this handle created the segment */
};

/* Rejects impossible geometry (zero sizes, too many channels/periods,
 * sample_bytes other than 2 or 4, and byte-size overflow). */
int mcshm_config_validate(const struct mcshm_config *cfg);

/* Total mapping size in bytes, including the page-aligned data start. */
int mcshm_ring_size(const struct mcshm_config *cfg, size_t *out_size);

/*
 * Creates (or recreates) the segment.  A stale segment with the same name is
 * unlinked first, so a crashed run never blocks the next one.
 */
int mcshm_ring_create(struct mcshm_ring *ring, const char *name,
                      const struct mcshm_config *cfg);

/* Attaches to an existing segment and reports its geometry in *out_cfg
 * (may be NULL).  Verifies magic, version and that the mapping is big enough. */
int mcshm_ring_open(struct mcshm_ring *ring, const char *name,
                    struct mcshm_config *out_cfg);

/* Unmaps.  Never unlinks: the segment outlives the creating process on
 * purpose, so a consumer can attach after the producer restarts. */
void mcshm_ring_close(struct mcshm_ring *ring);

/* Removes the name so the segment disappears once every mapping is gone. */
int mcshm_ring_unlink(const char *name);

uint64_t mcshm_ring_avail_write(const struct mcshm_ring *ring);
uint64_t mcshm_ring_avail_read(const struct mcshm_ring *ring);

/* Appends up to `frames` frames and returns how many were accepted (0 means
 * the ring is full).  Partial writes are normal and must be handled. */
uint64_t mcshm_ring_write(struct mcshm_ring *ring, const void *src, uint64_t frames);

/* Drains up to `frames` frames and returns how many were available. */
uint64_t mcshm_ring_read(struct mcshm_ring *ring, void *dst, uint64_t frames);

/* Resets both counters and the diagnostic counters to zero.  Only valid while
 * both sides are quiesced (pcm prepared, device stopped); the caller owns that
 * guarantee. */
int mcshm_ring_reset(struct mcshm_ring *ring);

uint64_t mcshm_ring_written(const struct mcshm_ring *ring);
uint64_t mcshm_ring_consumed(const struct mcshm_ring *ring);
uint64_t mcshm_ring_full_count(const struct mcshm_ring *ring);
uint64_t mcshm_ring_empty_count(const struct mcshm_ring *ring);

#endif /* MCSHM_RING_H */
