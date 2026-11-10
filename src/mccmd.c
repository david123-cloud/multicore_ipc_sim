#define _POSIX_C_SOURCE 200809L

#include "mccmd.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <stdio.h>

static int mc_fill_address(const char *path, struct sockaddr_un *address)
{
    size_t length;

    if (path == NULL || address == NULL || path[0] == '\0') {
        return MCCMD_ERR_INVALID;
    }
    length = strlen(path);
    if (length >= sizeof(address->sun_path)) {
        return MCCMD_ERR_INVALID;
    }
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, length + 1u);
    return MCCMD_OK;
}

static int mc_read_exact(int fd, void *buffer, size_t length)
{
    uint8_t *cursor = (uint8_t *)buffer;
    size_t done = 0u;

    while (done < length) {
        ssize_t got = read(fd, cursor + done, length - done);

        if (got == 0) {
            return (done == 0u) ? 0 : -1; /* clean EOF only at a message boundary */
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)got;
    }
    return 1;
}

static int mc_write_exact(int fd, const void *buffer, size_t length)
{
    const uint8_t *cursor = (const uint8_t *)buffer;
    size_t done = 0u;

    while (done < length) {
        /* MSG_NOSIGNAL: a peer that closed must surface as an error code, not
         * as SIGPIPE killing the process. */
        ssize_t put = send(fd, cursor + done, length - done, MSG_NOSIGNAL);

        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return MCCMD_ERR_IO;
        }
        done += (size_t)put;
    }
    return MCCMD_OK;
}

int mccmd_listen(const char *path)
{
    struct sockaddr_un address;
    struct stat info;
    int fd;

    if (mc_fill_address(path, &address) != MCCMD_OK) {
        return MCCMD_ERR_INVALID;
    }

    /* Remove a stale socket file left behind by a crashed run. */
    if (lstat(path, &info) == 0 && S_ISSOCK(info.st_mode)) {
        (void)unlink(path);
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return MCCMD_ERR_IO;
    }
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd);
        return MCCMD_ERR_IO;
    }
    if (listen(fd, 4) != 0) {
        (void)close(fd);
        return MCCMD_ERR_IO;
    }
    return fd;
}

int mccmd_accept(int listen_fd, int timeout_ms)
{
    struct pollfd pfd;
    int rc;

    if (listen_fd < 0) {
        return MCCMD_ERR_INVALID;
    }
    pfd.fd = listen_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) {
        return MCCMD_ERR_TIMEOUT;
    }
    if (rc < 0) {
        return MCCMD_ERR_IO;
    }
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);

        if (fd >= 0) {
            return fd;
        }
        if (errno != EINTR) {
            return MCCMD_ERR_IO;
        }
    }
}

int mccmd_connect(const char *path, int timeout_ms)
{
    struct sockaddr_un address;
    int fd;

    (void)timeout_ms; /* a blocking connect to a Unix socket is effectively instant */
    if (mc_fill_address(path, &address) != MCCMD_OK) {
        return MCCMD_ERR_INVALID;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return MCCMD_ERR_IO;
    }
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd);
        return (errno == ENOENT || errno == ECONNREFUSED) ? MCCMD_ERR_REFUSED : MCCMD_ERR_IO;
    }
    return fd;
}

int mccmd_send(int fd, uint16_t type, uint32_t seq, const void *payload, uint32_t length)
{
    struct mccmd_header header;
    int status;

    if (fd < 0 || length > MCCMD_MAX_PAYLOAD) {
        return MCCMD_ERR_INVALID;
    }
    if (length > 0u && payload == NULL) {
        return MCCMD_ERR_INVALID;
    }

    header.magic = MCCMD_MAGIC;
    header.version = MCCMD_VERSION;
    header.type = type;
    header.seq = seq;
    header.length = length;

    status = mc_write_exact(fd, &header, sizeof(header));
    if (status != MCCMD_OK) {
        return status;
    }
    if (length > 0u) {
        status = mc_write_exact(fd, payload, (size_t)length);
        if (status != MCCMD_OK) {
            return status;
        }
    }
    return MCCMD_OK;
}

int mccmd_recv(int fd, struct mccmd_header *header, void *payload, uint32_t capacity,
               int timeout_ms)
{
    struct pollfd pfd;
    int rc;
    int status;

    if (fd < 0 || header == NULL || (capacity > 0u && payload == NULL)) {
        return MCCMD_ERR_INVALID;
    }
    if (timeout_ms >= 0) {
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        do {
            rc = poll(&pfd, 1, timeout_ms);
        } while (rc < 0 && errno == EINTR);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return MCCMD_ERR_IO;
        }
    }

    status = mc_read_exact(fd, header, sizeof(*header));
    if (status == 0) {
        return MCCMD_ERR_CLOSED;
    }
    if (status < 0) {
        return MCCMD_ERR_IO;
    }
    if (header->magic != MCCMD_MAGIC || header->version != MCCMD_VERSION) {
        return MCCMD_ERR_PROTOCOL;
    }
    if (header->length > capacity) {
        /* Cannot resynchronise a stream once the framing is wrong. */
        return MCCMD_ERR_PROTOCOL;
    }
    if (header->length > 0u) {
        status = mc_read_exact(fd, payload, (size_t)header->length);
        if (status == 0) {
            return MCCMD_ERR_CLOSED;
        }
        if (status < 0) {
            return MCCMD_ERR_IO;
        }
    }
    return 1;
}

void mccmd_close(int fd)
{
    if (fd >= 0) {
        (void)close(fd);
    }
}
