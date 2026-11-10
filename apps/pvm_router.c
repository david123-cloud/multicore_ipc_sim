#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mccmd.h"
#include "mcshm_ring.h"

#define DEFAULT_SOCKET "/tmp/mc_ipc_m2.sock"
#define GVM_SHM "/mcshm_pvm_gvm"
#define DSP_SHM "/mcshm_pvm_dsp"

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

/* One end of the bridge: the application side (GVM) or the DSP side. */
struct pvm_end {
    const char *label;
    const char *shm_name;
    int fd;
    int have_ring;
    struct mcshm_ring ring;
    uint32_t seq;
    uint32_t period_frames;
    uint32_t channels;
    uint32_t sample_bytes;
    uint64_t frames;
    uint64_t periods;
    uint64_t empty_events;
    uint64_t underruns;
    uint64_t drops;
    int16_t *buffer;
};

struct pvm_state {
    struct pvm_end gvm;
    struct pvm_end dsp;
    int running;

    /* Observation: a CSV row every stats_every periods. */
    int stats_on;
    uint32_t stats_every;
    uint64_t start_ns;
    uint64_t nominal_ns;
    int64_t max_jitter_us;

    /* Fault injection: all of it lives here because this process *is* the
     * simulated device. */
    int64_t jitter_us;   /* random +/- around the ideal period */
    uint32_t drop_pct;   /* percentage of forwarded frames to discard */
    uint32_t stall_at;   /* period index that stalls, 0 = never */
    uint32_t stall_ms;
    uint64_t rng;
};

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* xorshift64* — deterministic, so a failing run can be reproduced. */
static uint64_t next_rand(struct pvm_state *state)
{
    uint64_t x = state->rng;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state->rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static void pace_to(struct timespec *deadline)
{
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL) == EINTR) {
        ;
    }
}

static int end_announce(struct pvm_state *state, struct pvm_end *end, uint32_t seq,
                        struct mcshm_config *cfg)
{
    struct mccmd_ack ack;
    int status;

    memset(&ack, 0, sizeof(ack));
    status = mcshm_config_validate(cfg);
    if (status != MCSHM_OK) {
        ack.status = status;
        return mccmd_send(end->fd, MCCMD_MSG_ACK, seq, &ack, (uint32_t)sizeof(ack));
    }

    status = mcshm_ring_create(&end->ring, end->shm_name, cfg);
    if (status != MCSHM_OK) {
        ack.status = status;
        return mccmd_send(end->fd, MCCMD_MSG_ACK, seq, &ack, (uint32_t)sizeof(ack));
    }
    end->buffer = (int16_t *)malloc((size_t)cfg->period_frames * cfg->channels * sizeof(int16_t));
    if (end->buffer == NULL) {
        mcshm_ring_close(&end->ring);
        (void)mcshm_ring_unlink(end->shm_name);
        ack.status = MCSHM_ERR_NOMEM;
        return mccmd_send(end->fd, MCCMD_MSG_ACK, seq, &ack, (uint32_t)sizeof(ack));
    }

    end->have_ring = 1;
    end->period_frames = cfg->period_frames;
    end->channels = cfg->channels;
    end->sample_bytes = cfg->sample_bytes;

    ack.status = MCCMD_OK;
    ack.ring_frames = (uint32_t)end->ring.ring_frames;
    ack.stride = end->ring.stride;
    (void)snprintf(ack.shm_name, sizeof(ack.shm_name), "%s", end->shm_name);

    printf("PVM: %s end created %s (%uch, %u frames/period x %u)\n", end->label, end->shm_name,
           (unsigned)cfg->channels, (unsigned)cfg->period_frames, (unsigned)cfg->period_count);
    fflush(stdout);
    (void)state;
    return mccmd_send(end->fd, MCCMD_MSG_ACK, seq, &ack, (uint32_t)sizeof(ack));
}

static int end_handle(struct pvm_state *state, struct pvm_end *end,
                      const struct mccmd_header *header, const void *payload)
{
    (void)state;

    switch (header->type) {
    case MCCMD_MSG_SET_HW_PARAMS: {
        const struct mccmd_hw_params *params = (const struct mccmd_hw_params *)payload;
        struct mcshm_config cfg;

        if (header->length != sizeof(*params) || end->have_ring) {
            struct mccmd_ack ack;

            memset(&ack, 0, sizeof(ack));
            ack.status = MCCMD_ERR_PROTOCOL;
            return mccmd_send(end->fd, MCCMD_MSG_ACK, header->seq, &ack, (uint32_t)sizeof(ack));
        }
        cfg.channels = params->channels;
        cfg.rate = params->rate;
        cfg.period_frames = params->period_frames;
        cfg.period_count = params->period_count;
        cfg.sample_bytes = params->sample_bytes;
        return end_announce(state, end, header->seq, &cfg);
    }

    case MCCMD_MSG_PREPARE: {
        struct mccmd_ack ack;

        memset(&ack, 0, sizeof(ack));
        if (!end->have_ring || state->running) {
            ack.status = MCCMD_ERR_PROTOCOL;
        } else {
            (void)mcshm_ring_reset(&end->ring);
            end->frames = 0u;
            end->periods = 0u;
            end->empty_events = 0u;
            end->underruns = 0u;
            end->drops = 0u;
            ack.status = MCCMD_OK;
        }
        printf("PVM: %s prepare %s\n", end->label, (ack.status == MCCMD_OK) ? "ok" : "refused");
        fflush(stdout);
        return mccmd_send(end->fd, MCCMD_MSG_ACK, header->seq, &ack, (uint32_t)sizeof(ack));
    }

    case MCCMD_MSG_START: {
        struct mccmd_ack ack;

        memset(&ack, 0, sizeof(ack));
        ack.status = end->have_ring ? MCCMD_OK : MCCMD_ERR_INVALID;
        state->running = end->have_ring;
        if (state->running) {
            state->start_ns = now_ns();
            state->nominal_ns = 0u;
            state->max_jitter_us = 0;
        }
        printf("PVM: %s start\n", end->label);
        fflush(stdout);
        return mccmd_send(end->fd, MCCMD_MSG_ACK, header->seq, &ack, (uint32_t)sizeof(ack));
    }

    case MCCMD_MSG_DRAIN: {
        struct mccmd_ack ack;
        uint64_t drained = 0u;

        while (end->have_ring) {
            uint64_t got = mcshm_ring_read(&end->ring, end->buffer, end->period_frames);

            if (got == 0u) {
                break;
            }
            if (state->dsp.have_ring && state->dsp.fd >= 0) {
                uint64_t put = mcshm_ring_write(&state->dsp.ring, end->buffer, got);

                /* The bridge's own counter must include what the drain path
                 * forwarded, otherwise the two ends disagree by the tail. */
                state->dsp.frames += put;
                state->dsp.periods += 1u;
                if (put < got) {
                    state->dsp.drops += got - put;
                }
            }
            end->frames += got;
            end->periods += 1u;
            drained += got;
        }
        state->running = 0;

        memset(&ack, 0, sizeof(ack));
        ack.status = MCCMD_OK;
        ack.ring_frames = (uint32_t)drained;
        printf("PVM: %s drained %llu frames\n", end->label, (unsigned long long)drained);
        fflush(stdout);
        return mccmd_send(end->fd, MCCMD_MSG_ACK, header->seq, &ack, (uint32_t)sizeof(ack));
    }

    case MCCMD_MSG_SHUTDOWN: {
        struct mccmd_ack ack;

        memset(&ack, 0, sizeof(ack));
        ack.status = MCCMD_OK;
        (void)mccmd_send(end->fd, MCCMD_MSG_ACK, header->seq, &ack, (uint32_t)sizeof(ack));
        return MCCMD_ERR_CLOSED;
    }

    default: {
        struct mccmd_ack ack;

        memset(&ack, 0, sizeof(ack));
        ack.status = MCCMD_ERR_PROTOCOL;
        return mccmd_send(end->fd, MCCMD_MSG_ACK, header->seq, &ack, (uint32_t)sizeof(ack));
    }
    }
}

static void end_print(const struct pvm_end *end)
{
    printf("PVM_%s_STATS frames=%llu periods=%llu empty=%llu underruns=%llu drops=%llu\n",
           end->label, (unsigned long long)end->frames, (unsigned long long)end->periods,
           (unsigned long long)end->empty_events, (unsigned long long)end->underruns,
           (unsigned long long)end->drops);
}

int main(int argc, char **argv)
{
    const char *socket_path = DEFAULT_SOCKET;
    char dsp_path[MCCMD_MAX_NAME];
    struct pvm_state state;
    struct mccmd_header header;
    unsigned char payload[MCCMD_MAX_PAYLOAD];
    struct timespec deadline;
    uint64_t period_ns = 0u;
    int gvm_listen;
    int dsp_listen;
    int running = 1;
    int socket_set = 0;
    int argi;

    memset(&state, 0, sizeof(state));
    state.gvm.label = "gvm";
    state.gvm.shm_name = GVM_SHM;
    state.gvm.fd = -1;
    state.dsp.label = "dsp";
    state.dsp.shm_name = DSP_SHM;
    state.dsp.fd = -1;
    state.stats_every = 10u;
    state.rng = 0x9E3779B97F4A7C15ull;

    for (argi = 1; argi < argc; ++argi) {
        if (strcmp(argv[argi], "--stats") == 0) {
            state.stats_on = 1;
        } else if (strcmp(argv[argi], "--jitter-us") == 0 && argi + 1 < argc) {
            state.jitter_us = (int64_t)strtol(argv[++argi], NULL, 10);
        } else if (strcmp(argv[argi], "--drop-pct") == 0 && argi + 1 < argc) {
            state.drop_pct = (uint32_t)strtoul(argv[++argi], NULL, 10);
        } else if (strcmp(argv[argi], "--stall-at") == 0 && argi + 2 < argc) {
            state.stall_at = (uint32_t)strtoul(argv[++argi], NULL, 10);
            state.stall_ms = (uint32_t)strtoul(argv[++argi], NULL, 10);
        } else if (strcmp(argv[argi], "--stats-every") == 0 && argi + 1 < argc) {
            uint32_t value = (uint32_t)strtoul(argv[++argi], NULL, 10);

            if (value > 0u) {
                state.stats_every = value;
            }
        } else if (!socket_set) {
            socket_path = argv[argi];
            socket_set = 1;
        } else {
            fprintf(stderr, "pvm: unknown argument %s\n", argv[argi]);
            return 1;
        }
    }
    if (state.drop_pct > 100u) {
        fprintf(stderr, "pvm: --drop-pct must be 0..100\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if ((size_t)snprintf(dsp_path, sizeof(dsp_path), "%s.dsp", socket_path) >= sizeof(dsp_path)) {
        fprintf(stderr, "pvm: socket path too long\n");
        return 1;
    }

    gvm_listen = mccmd_listen(socket_path);
    if (gvm_listen < 0) {
        fprintf(stderr, "pvm: cannot listen on %s\n", socket_path);
        return 1;
    }
    dsp_listen = mccmd_listen(dsp_path);
    if (dsp_listen < 0) {
        fprintf(stderr, "pvm: cannot listen on %s\n", dsp_path);
        (void)unlink(socket_path);
        return 1;
    }
    printf("PVM: listening on %s (+ %s for the DSP end)\n", socket_path, dsp_path);
    fflush(stdout);

    state.gvm.fd = mccmd_accept(gvm_listen, 5000);
    if (state.gvm.fd < 0) {
        fprintf(stderr, "pvm: no application connected\n");
        (void)unlink(socket_path);
        (void)unlink(dsp_path);
        return 1;
    }
    printf("PVM: application connected\n");
    fflush(stdout);

    clock_gettime(CLOCK_MONOTONIC, &deadline);

    while (running && !g_stop) {
        int result;

        /* The DSP end may attach at any time; the application must not wait
         * for it, so the listener is polled without blocking. */
        if (state.dsp.fd < 0) {
            int accepted = mccmd_accept(dsp_listen, 0);

            if (accepted >= 0) {
                state.dsp.fd = accepted;
                printf("PVM: dsp connected\n");
                fflush(stdout);
            }
        }

        if (state.running) {
            uint64_t got;

            if (period_ns == 0u && state.gvm.period_frames > 0u) {
                period_ns = (uint64_t)state.gvm.period_frames * 1000000000ull / 48000ull;
                clock_gettime(CLOCK_MONOTONIC, &deadline);
            }
            /* Pacing on a nominal timeline: the ideal instant is start +
             * k*period, and injected jitter offsets that instant instead of
             * accumulating into the next one. */
            state.nominal_ns += period_ns;
            if (state.jitter_us != 0) {
                int64_t span = state.jitter_us * 2 + 1;
                int64_t offset = (int64_t)(next_rand(&state) % (uint64_t)span) - state.jitter_us;
                int64_t target = (int64_t)(state.start_ns + state.nominal_ns) + offset * 1000;

                deadline.tv_sec = (time_t)(target / 1000000000);
                deadline.tv_nsec = (long)(target % 1000000000);
            } else {
                uint64_t target = state.start_ns + state.nominal_ns;

                deadline.tv_sec = (time_t)(target / 1000000000);
                deadline.tv_nsec = (long)(target % 1000000000);
            }
            pace_to(&deadline);

            /* Fault injection: freeze the device for a while. */
            if (state.stall_at != 0u && state.gvm.periods == state.stall_at) {
                struct timespec pause;

                pause.tv_sec = (time_t)(state.stall_ms / 1000u);
                pause.tv_nsec = (long)(state.stall_ms % 1000u) * 1000000L;
                printf("PVM: injecting a %u ms device stall at period %u\n",
                       (unsigned)state.stall_ms, (unsigned)state.stall_at);
                fflush(stdout);
                (void)nanosleep(&pause, NULL);
            }

            got = mcshm_ring_read(&state.gvm.ring, state.gvm.buffer, state.gvm.period_frames);
            state.gvm.periods += 1u;
            if (got > 0u) {
                struct mccmd_period notice;

                state.gvm.frames += got;
                notice.hw_ptr = state.gvm.frames;
                notice.period_index = state.gvm.periods;
                (void)mccmd_send(state.gvm.fd, MCCMD_MSG_PERIOD, state.gvm.seq++, &notice,
                                 (uint32_t)sizeof(notice));

                /* Fault injection: pretend the transport lost some frames. */
                if (state.drop_pct > 0u && (next_rand(&state) % 100u) < (uint64_t)state.drop_pct) {
                    state.dsp.drops += got;
                    got = 0u;
                }

                /* Second hop: forward to the DSP ring, counting what does not fit
                 * instead of blocking the device clock. */
                if (got > 0u && state.dsp.have_ring && state.dsp.fd >= 0) {
                    uint64_t put = mcshm_ring_write(&state.dsp.ring, state.gvm.buffer, got);

                    state.dsp.frames += put;
                    state.dsp.periods += 1u;
                    if (put < got) {
                        state.dsp.drops += got - put;
                    }
                }
                if (state.dsp.have_ring && state.dsp.fd >= 0) {
                    struct mccmd_period dsp_notice;

                    dsp_notice.hw_ptr = state.dsp.frames;
                    dsp_notice.period_index = state.dsp.periods;
                    (void)mccmd_send(state.dsp.fd, MCCMD_MSG_PERIOD, state.dsp.seq++, &dsp_notice,
                                     (uint32_t)sizeof(dsp_notice));
                }
            } else {
                struct mccmd_xrun notice;

                state.gvm.empty_events += 1u;
                state.gvm.underruns += 1u;
                notice.hw_ptr = state.gvm.frames;
                notice.period_index = state.gvm.periods;
                (void)mccmd_send(state.gvm.fd, MCCMD_MSG_XRUN, state.gvm.seq++, &notice,
                                 (uint32_t)sizeof(notice));
            }

            /* Observation panel: one CSV row every stats_every periods. */
            if (state.stats_on && (state.gvm.periods % state.stats_every) == 0u) {
                uint64_t appl = mcshm_ring_written(&state.gvm.ring);
                uint64_t now = now_ns();
                int64_t jitter_us = (int64_t)(now - (state.start_ns + state.nominal_ns)) / 1000;
                double latency_ms = (double)(appl - state.gvm.frames) * 1000.0 / 48000.0;

                if (jitter_us > state.max_jitter_us) {
                    state.max_jitter_us = jitter_us;
                }
                printf("PVM_STAT t_ms=%llu appl=%llu hw=%llu fill=%llu xruns=%llu drops=%llu "
                       "lat_ms=%.1f jitter_us=%lld\n",
                       (unsigned long long)((now - state.start_ns) / 1000000ull),
                       (unsigned long long)appl, (unsigned long long)state.gvm.frames,
                       (unsigned long long)mcshm_ring_avail_read(&state.gvm.ring),
                       (unsigned long long)state.gvm.underruns,
                       (unsigned long long)state.dsp.drops, latency_ms, (long long)jitter_us);
                fflush(stdout);
            }

            result = mccmd_recv(state.gvm.fd, &header, payload, (uint32_t)sizeof(payload), 0);
        } else {
            result = mccmd_recv(state.gvm.fd, &header, payload, (uint32_t)sizeof(payload), 100);
            if (result == 0) {
                continue;
            }
        }

        if (result == 1) {
            if (end_handle(&state, &state.gvm, &header, payload) == MCCMD_ERR_CLOSED) {
                if (state.dsp.fd >= 0) {
                    (void)mccmd_send(state.dsp.fd, MCCMD_MSG_SHUTDOWN, state.dsp.seq++, NULL, 0u);
                }
                running = 0;
            }
        } else if (result != 0) {
            running = 0;
        }

        if (state.dsp.fd >= 0) {
            int dsp_result = mccmd_recv(state.dsp.fd, &header, payload, (uint32_t)sizeof(payload), 0);

            if (dsp_result == 1) {
                if (end_handle(&state, &state.dsp, &header, payload) == MCCMD_ERR_CLOSED) {
                    state.dsp.fd = -1;
                }
            } else if (dsp_result != 0) {
                state.dsp.fd = -1;
            }
        }
    }

    end_print(&state.gvm);
    end_print(&state.dsp);

    if (state.gvm.fd >= 0) {
        mccmd_close(state.gvm.fd);
    }
    if (state.dsp.fd >= 0) {
        mccmd_close(state.dsp.fd);
    }
    (void)close(gvm_listen);
    (void)close(dsp_listen);
    (void)unlink(socket_path);
    (void)unlink(dsp_path);

    if (state.gvm.have_ring) {
        (void)mcshm_ring_unlink(GVM_SHM);
        mcshm_ring_close(&state.gvm.ring);
    }
    if (state.dsp.have_ring) {
        (void)mcshm_ring_unlink(DSP_SHM);
        mcshm_ring_close(&state.dsp.ring);
    }
    free(state.gvm.buffer);
    free(state.dsp.buffer);
    return 0;
}
