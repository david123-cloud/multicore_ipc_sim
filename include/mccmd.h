#ifndef MCCMD_H
#define MCCMD_H

#include <stdint.h>

/*
 * mccmd - control channel between the simulated GVM and PVM processes.
 *
 * Transport: AF_UNIX SOCK_STREAM.  Every message is a fixed 12-byte header
 * followed by `length` payload bytes, so a reader can never consume a partial
 * message silently: short reads are loops, and a header/payload length
 * mismatch is a protocol error rather than a guess.
 *
 * The socket carries control only.  Audio never travels here: the receiver
 * answers SET_HW_PARAMS with the name of the shared memory segment it created,
 * and both sides then touch PCM through that mapping.
 */

#define MCCMD_MAGIC 0x4D43434Du /* "MCCM" */
#define MCCMD_VERSION 1u
#define MCCMD_MAX_NAME 64u
#define MCCMD_MAX_PAYLOAD 256u

/* Status codes. */
#define MCCMD_OK 0
#define MCCMD_ERR_INVALID (-1)
#define MCCMD_ERR_IO (-2)
#define MCCMD_ERR_PROTOCOL (-3)
#define MCCMD_ERR_CLOSED (-4)
#define MCCMD_ERR_TIMEOUT (-5)
#define MCCMD_ERR_REFUSED (-6)

enum mccmd_type {
    MCCMD_MSG_SET_HW_PARAMS = 1, /* GVM -> PVM: geometry, PVM creates the segment */
    MCCMD_MSG_ACK = 2,           /* PVM -> GVM: status + segment name */
    MCCMD_MSG_START = 3,         /* GVM -> PVM: begin consuming */
    MCCMD_MSG_STOP = 4,          /* GVM -> PVM: stop consuming */
    MCCMD_MSG_DRAIN = 5,         /* GVM -> PVM: consume what is left, then ack */
    MCCMD_MSG_STATS = 6,         /* PVM -> GVM: counters */
    MCCMD_MSG_SHUTDOWN = 7,      /* GVM -> PVM: exit */
    MCCMD_MSG_PREPARE = 8,       /* GVM -> PVM: reset the ring, device stopped */
    MCCMD_MSG_PERIOD = 9,        /* device -> app: period elapsed, hw_ptr advanced */
    MCCMD_MSG_XRUN = 10,         /* device -> app: underrun detected */
    MCCMD_MSG_CONSUMED = 11      /* DSP -> PVM: DMA done, hw_ptr advanced (direct topology) */
};

struct mccmd_header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t seq;
    uint32_t length;
};

/* SET_HW_PARAMS payload: the ALSA-style geometry the application asks for. */
struct mccmd_hw_params {
    uint32_t channels;
    uint32_t rate;
    uint32_t period_frames;
    uint32_t period_count;
    uint32_t sample_bytes;
};

/* ACK payload: status plus the segment the receiver created. */
struct mccmd_ack {
    int32_t status;
    uint32_t ring_frames;
    uint32_t stride;
    char shm_name[MCCMD_MAX_NAME];
};

struct mccmd_stats {
    uint64_t frames_consumed;
    uint64_t periods_consumed;
    uint64_t empty_events;
    uint64_t short_periods;
};

/* Async device notification: one per elapsed period.  This is the simulated
 * period interrupt; the client uses it to wake from poll(). */
struct mccmd_period {
    uint64_t hw_ptr;       /* frames consumed by the device so far */
    uint64_t period_index; /* monotonic period counter */
};

/* Async device notification: the device needed a period and found nothing. */
struct mccmd_xrun {
    uint64_t hw_ptr;
    uint64_t period_index;
};

/*
 * Direct topology only: the DSP is the consumer, so it owns the device clock
 * and the DMA-complete interrupt.  This is that interrupt, travelling upstream
 * to the host, which relays it to the application as a PERIOD or XRUN.
 */
struct mccmd_progress {
    uint64_t hw_ptr;       /* frames the DSP has consumed */
    uint64_t period_index; /* periods the DSP has ticked */
    uint32_t underrun;     /* 1 when the tick found no data */
    uint32_t reserved;
};

/* Listener socket bound to `path`; a stale socket file is replaced. */
int mccmd_listen(const char *path);

/* Blocks until a client connects; returns a connected fd. */
int mccmd_accept(int listen_fd, int timeout_ms);

/* Connects to a listening peer. */
int mccmd_connect(const char *path, int timeout_ms);

int mccmd_send(int fd, uint16_t type, uint32_t seq, const void *payload, uint32_t length);

/*
 * Reads one message.
 *   returns 1  message received (header and payload filled)
 *   returns 0  timeout, nothing consumed
 *   returns negative MCCMD_ERR_* on failure; MCCMD_ERR_CLOSED means the peer
 *              closed the connection cleanly
 */
int mccmd_recv(int fd, struct mccmd_header *header, void *payload, uint32_t capacity,
               int timeout_ms);

void mccmd_close(int fd);

#endif /* MCCMD_H */
