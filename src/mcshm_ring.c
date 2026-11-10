#define _POSIX_C_SOURCE 200809L
#include "mcshm_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int mcshm_status_from_errno(int error)
{
    switch (error) {
    case EEXIST:
        return MCSHM_ERR_EXISTS;
    case ENOENT:
        return MCSHM_ERR_NOT_FOUND;
    case ENOMEM:
        return MCSHM_ERR_NOMEM;
    default:
        return MCSHM_ERR_IO;
    }
}

static size_t mcshm_round_up(size_t value, size_t alignment)
{
    return (value + alignment - 1u) / alignment * alignment;
}

static size_t mcshm_page_size(void)
{
    long page = sysconf(_SC_PAGESIZE);

    return (page > 0) ? (size_t)page : 4096u;
}

/* POSIX shared memory names start with '/'; accept both spellings. */
static int mcshm_normalize_name(const char *name, char *out)
{
    size_t length;
    int needs_slash;

    if (name == NULL || out == NULL || name[0] == '\0') {
        return MCSHM_ERR_INVALID;
    }
    length = strlen(name);
    needs_slash = (name[0] != '/') ? 1 : 0;
    if (length + (size_t)needs_slash + 1u > MCSHM_MAX_NAME) {
        return MCSHM_ERR_RANGE;
    }
    if (needs_slash) {
        out[0] = '/';
        memcpy(out + 1, name, length + 1u);
    } else {
        memcpy(out, name, length + 1u);
    }
    return MCSHM_OK;
}

static uint64_t mcshm_ring_frame_bytes(const struct mcshm_config *cfg)
{
    return (uint64_t)cfg->period_frames * (uint64_t)cfg->period_count *
           (uint64_t)cfg->channels * (uint64_t)cfg->sample_bytes;
}

int mcshm_config_validate(const struct mcshm_config *cfg)
{
    if (cfg == NULL) {
        return MCSHM_ERR_INVALID;
    }
    if (cfg->channels == 0u || cfg->channels > MCSHM_MAX_CHANNELS) {
        return MCSHM_ERR_RANGE;
    }
    if (cfg->rate == 0u) {
        return MCSHM_ERR_RANGE;
    }
    if (cfg->period_frames == 0u || cfg->period_frames > MCSHM_MAX_PERIOD_FRAMES) {
        return MCSHM_ERR_RANGE;
    }
    if (cfg->period_count == 0u || cfg->period_count > MCSHM_MAX_PERIODS) {
        return MCSHM_ERR_RANGE;
    }
    if (cfg->sample_bytes != 2u && cfg->sample_bytes != 4u) {
        return MCSHM_ERR_RANGE;
    }
    /* ring_frames * stride must stay inside size_t on this host. */
    if (mcshm_ring_frame_bytes(cfg) > (uint64_t)(SIZE_MAX / 2u)) {
        return MCSHM_ERR_RANGE;
    }
    return MCSHM_OK;
}

int mcshm_ring_size(const struct mcshm_config *cfg, size_t *out_size)
{
    uint64_t payload;
    size_t data_offset;

    if (out_size == NULL) {
        return MCSHM_ERR_INVALID;
    }
    if (mcshm_config_validate(cfg) != MCSHM_OK) {
        return MCSHM_ERR_RANGE;
    }
    data_offset = mcshm_round_up(sizeof(struct mcshm_header), mcshm_page_size());
    payload = mcshm_ring_frame_bytes(cfg);
    *out_size = data_offset + (size_t)payload;
    return MCSHM_OK;
}

int mcshm_ring_create(struct mcshm_ring *ring, const char *name, const struct mcshm_config *cfg)
{
    char shm_name[MCSHM_MAX_NAME];
    size_t map_size = 0u;
    size_t data_offset;
    uint64_t ring_frames;
    struct mcshm_header *header;
    void *base;
    int fd;
    int status;

    if (ring == NULL) {
        return MCSHM_ERR_INVALID;
    }
    if (ring->header != NULL) {
        return MCSHM_ERR_MAPPED;
    }
    status = mcshm_config_validate(cfg);
    if (status != MCSHM_OK) {
        return status;
    }
    status = mcshm_normalize_name(name, shm_name);
    if (status != MCSHM_OK) {
        return status;
    }
    status = mcshm_ring_size(cfg, &map_size);
    if (status != MCSHM_OK) {
        return status;
    }

    fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0660);
    if (fd < 0 && errno == EEXIST) {
        /* A stale segment from a previous run: drop it and retry once. */
        (void)shm_unlink(shm_name);
        fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0660);
    }
    if (fd < 0) {
        return mcshm_status_from_errno(errno);
    }

    if (ftruncate(fd, (off_t)map_size) != 0) {
        status = mcshm_status_from_errno(errno);
        (void)close(fd);
        (void)shm_unlink(shm_name);
        return status;
    }

    base = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        status = mcshm_status_from_errno(errno);
        (void)close(fd);
        (void)shm_unlink(shm_name);
        return status;
    }
    (void)close(fd);

    data_offset = mcshm_round_up(sizeof(struct mcshm_header), mcshm_page_size());
    ring_frames = (uint64_t)cfg->period_frames * (uint64_t)cfg->period_count;

    header = (struct mcshm_header *)base;
    memset(header, 0, sizeof(*header));
    header->magic = MCSHM_MAGIC;
    header->version = MCSHM_VERSION;
    header->channels = cfg->channels;
    header->rate = cfg->rate;
    header->period_frames = cfg->period_frames;
    header->period_count = cfg->period_count;
    header->sample_bytes = cfg->sample_bytes;
    header->ring_frames = ring_frames;
    header->data_offset = (uint64_t)data_offset;
    atomic_init(&header->write_frames, 0u);
    atomic_init(&header->read_frames, 0u);
    atomic_init(&header->full_count, 0u);
    atomic_init(&header->empty_count, 0u);

    ring->header = header;
    ring->data = (uint8_t *)base + data_offset;
    ring->map_size = map_size;
    ring->ring_frames = ring_frames;
    ring->stride = cfg->channels * cfg->sample_bytes;
    memcpy(ring->name, shm_name, strlen(shm_name) + 1u);
    ring->created = 1;
    return MCSHM_OK;
}

int mcshm_ring_open(struct mcshm_ring *ring, const char *name, struct mcshm_config *out_cfg)
{
    char shm_name[MCSHM_MAX_NAME];
    struct stat info;
    struct mcshm_header *header;
    void *base;
    int fd;
    int status;

    if (ring == NULL) {
        return MCSHM_ERR_INVALID;
    }
    if (ring->header != NULL) {
        return MCSHM_ERR_MAPPED;
    }
    status = mcshm_normalize_name(name, shm_name);
    if (status != MCSHM_OK) {
        return status;
    }

    fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) {
        return mcshm_status_from_errno(errno);
    }
    if (fstat(fd, &info) != 0) {
        status = mcshm_status_from_errno(errno);
        (void)close(fd);
        return status;
    }
    if (info.st_size < (off_t)sizeof(struct mcshm_header)) {
        (void)close(fd);
        return MCSHM_ERR_MAGIC;
    }

    base = mmap(NULL, (size_t)info.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        status = mcshm_status_from_errno(errno);
        (void)close(fd);
        return status;
    }
    (void)close(fd);

    header = (struct mcshm_header *)base;
    if (header->magic != MCSHM_MAGIC || header->version != MCSHM_VERSION) {
        (void)munmap(base, (size_t)info.st_size);
        return MCSHM_ERR_MAGIC;
    }
    if (header->data_offset < sizeof(struct mcshm_header) ||
        header->data_offset > (uint64_t)info.st_size) {
        (void)munmap(base, (size_t)info.st_size);
        return MCSHM_ERR_MAGIC;
    }
    if (header->channels == 0u || header->sample_bytes == 0u ||
        header->ring_frames == 0u) {
        (void)munmap(base, (size_t)info.st_size);
        return MCSHM_ERR_MAGIC;
    }
    if (header->data_offset + header->ring_frames * header->channels *
                                         header->sample_bytes >
        (uint64_t)info.st_size) {
        (void)munmap(base, (size_t)info.st_size);
        return MCSHM_ERR_MAGIC;
    }

    ring->header = header;
    ring->data = (uint8_t *)base + header->data_offset;
    ring->map_size = (size_t)info.st_size;
    ring->ring_frames = header->ring_frames;
    ring->stride = header->channels * header->sample_bytes;
    memcpy(ring->name, shm_name, strlen(shm_name) + 1u);
    ring->created = 0;

    if (out_cfg != NULL) {
        out_cfg->channels = header->channels;
        out_cfg->rate = header->rate;
        out_cfg->period_frames = header->period_frames;
        out_cfg->period_count = header->period_count;
        out_cfg->sample_bytes = header->sample_bytes;
    }
    return MCSHM_OK;
}

void mcshm_ring_close(struct mcshm_ring *ring)
{
    if (ring == NULL || ring->header == NULL) {
        return;
    }
    (void)munmap(ring->header, ring->map_size);
    memset(ring, 0, sizeof(*ring));
}

int mcshm_ring_unlink(const char *name)
{
    char shm_name[MCSHM_MAX_NAME];
    int status;

    if (name == NULL) {
        return MCSHM_ERR_INVALID;
    }
    status = mcshm_normalize_name(name, shm_name);
    if (status != MCSHM_OK) {
        return status;
    }
    if (shm_unlink(shm_name) != 0) {
        return (errno == ENOENT) ? MCSHM_ERR_NOT_FOUND : mcshm_status_from_errno(errno);
    }
    return MCSHM_OK;
}

uint64_t mcshm_ring_avail_write(const struct mcshm_ring *ring)
{
    uint64_t head;
    uint64_t tail;

    if (ring == NULL || ring->header == NULL) {
        return 0u;
    }
    head = atomic_load_explicit(&ring->header->write_frames, memory_order_relaxed);
    tail = atomic_load_explicit(&ring->header->read_frames, memory_order_acquire);
    if (head - tail >= ring->ring_frames) {
        return 0u;
    }
    return ring->ring_frames - (head - tail);
}

uint64_t mcshm_ring_avail_read(const struct mcshm_ring *ring)
{
    uint64_t head;
    uint64_t tail;

    if (ring == NULL || ring->header == NULL) {
        return 0u;
    }
    head = atomic_load_explicit(&ring->header->write_frames, memory_order_acquire);
    tail = atomic_load_explicit(&ring->header->read_frames, memory_order_relaxed);
    return head - tail;
}

uint64_t mcshm_ring_write(struct mcshm_ring *ring, const void *src, uint64_t frames)
{
    struct mcshm_header *header;
    uint64_t head;
    uint64_t tail;
    uint64_t used;
    uint64_t space;
    uint64_t offset;
    uint64_t first;

    if (ring == NULL || ring->header == NULL || (src == NULL && frames > 0u)) {
        return 0u;
    }
    if (frames == 0u) {
        return 0u;
    }
    header = ring->header;
    head = atomic_load_explicit(&header->write_frames, memory_order_relaxed);
    tail = atomic_load_explicit(&header->read_frames, memory_order_acquire);

    used = head - tail;
    space = (used >= ring->ring_frames) ? 0u : (ring->ring_frames - used);
    if (frames > space) {
        frames = space;
        (void)atomic_fetch_add_explicit(&header->full_count, 1u, memory_order_relaxed);
    }
    if (frames == 0u) {
        return 0u;
    }

    offset = head % ring->ring_frames;
    first = ring->ring_frames - offset;
    if (first > frames) {
        first = frames;
    }
    memcpy(ring->data + (size_t)(offset * ring->stride), src, (size_t)(first * ring->stride));
    if (frames > first) {
        memcpy(ring->data, (const uint8_t *)src + (size_t)(first * ring->stride),
               (size_t)((frames - first) * ring->stride));
    }

    atomic_store_explicit(&header->write_frames, head + frames, memory_order_release);
    return frames;
}

uint64_t mcshm_ring_read(struct mcshm_ring *ring, void *dst, uint64_t frames)
{
    struct mcshm_header *header;
    uint64_t head;
    uint64_t tail;
    uint64_t available;
    uint64_t offset;
    uint64_t first;

    if (ring == NULL || ring->header == NULL || (dst == NULL && frames > 0u)) {
        return 0u;
    }
    if (frames == 0u) {
        return 0u;
    }
    header = ring->header;
    head = atomic_load_explicit(&header->write_frames, memory_order_acquire);
    tail = atomic_load_explicit(&header->read_frames, memory_order_relaxed);

    available = head - tail;
    if (available == 0u) {
        (void)atomic_fetch_add_explicit(&header->empty_count, 1u, memory_order_relaxed);
        return 0u;
    }
    if (frames > available) {
        frames = available;
    }

    offset = tail % ring->ring_frames;
    first = ring->ring_frames - offset;
    if (first > frames) {
        first = frames;
    }
    memcpy(dst, ring->data + (size_t)(offset * ring->stride), (size_t)(first * ring->stride));
    if (frames > first) {
        memcpy((uint8_t *)dst + (size_t)(first * ring->stride), ring->data,
               (size_t)((frames - first) * ring->stride));
    }

    atomic_store_explicit(&header->read_frames, tail + frames, memory_order_release);
    return frames;
}

int mcshm_ring_reset(struct mcshm_ring *ring)
{
    if (ring == NULL || ring->header == NULL) {
        return MCSHM_ERR_INVALID;
    }
    /* Consumer first, then producer: no observer may see write < read. */
    atomic_store_explicit(&ring->header->read_frames, 0u, memory_order_release);
    atomic_store_explicit(&ring->header->write_frames, 0u, memory_order_release);
    atomic_store_explicit(&ring->header->full_count, 0u, memory_order_relaxed);
    atomic_store_explicit(&ring->header->empty_count, 0u, memory_order_relaxed);
    return MCSHM_OK;
}

uint64_t mcshm_ring_written(const struct mcshm_ring *ring)
{
    if (ring == NULL || ring->header == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&ring->header->write_frames, memory_order_relaxed);
}

uint64_t mcshm_ring_consumed(const struct mcshm_ring *ring)
{
    if (ring == NULL || ring->header == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&ring->header->read_frames, memory_order_relaxed);
}

uint64_t mcshm_ring_full_count(const struct mcshm_ring *ring)
{
    if (ring == NULL || ring->header == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&ring->header->full_count, memory_order_relaxed);
}

uint64_t mcshm_ring_empty_count(const struct mcshm_ring *ring)
{
    if (ring == NULL || ring->header == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&ring->header->empty_count, memory_order_relaxed);
}
