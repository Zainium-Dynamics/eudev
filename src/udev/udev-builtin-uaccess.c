/*
 * uaccess — grant the active seat user ACL access to a device node.
 *
 * No systemd here, so this doesn't use sd-login. Instead it asks
 * quantra-logind (the local session/seat manager) two questions over its
 * control socket: "who's the active session on this seat" and "what's
 * that session's uid". Both already exist as quantra-logind control verbs
 * (get_seat, get_session) — this file just speaks that protocol.
 *
 * Wire format, same as quantra-ctl uses against /run/quantra/control:
 * 4-byte little-endian length prefix, then a JSON payload, both directions.
 * The two request/response shapes used here are fixed and tiny, so this
 * hand-rolls the encode/decode instead of pulling in a JSON library —
 * see json_extract_* below.
 *
 * If the socket isn't there, or nobody's logged in yet, that's not an
 * error — this can run during boot before any session exists, or in a
 * build/chroot sandbox with no quantra-logind at all. Just skip.
 *
 * Not handled here (see docs/UDEV_VS_SYSTEMD.md): ACLs aren't revoked when
 * a session ends or the seat switches to a different user. That needs
 * quantra-logind to drive a re-scan on session change, which is a bigger,
 * separate piece of work.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/acl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "udev.h"

#define LOGIND_SOCKET_PATH "/run/quantra-logind/control"

/* Connect with a short timeout — this runs inline in udev rule
 * processing, it must not hang device add events if logind is wedged. */
static int logind_connect(void) {
        int fd;
        struct sockaddr_un addr;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
                return -errno;

        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, LOGIND_SOCKET_PATH, sizeof(addr.sun_path) - 1);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                int err = -errno;
                close(fd);
                return err;
        }

        return fd;
}

/* Send a request, get the raw response payload back. Returns the number
 * of bytes read into `out` (caller-owned buffer), or a negative errno. */
static ssize_t logind_roundtrip(int fd, const char *request, char *out, size_t out_size) {
        uint32_t len_le;
        uint32_t reply_len;
        size_t req_len = strlen(request);

        len_le = (uint32_t)req_len; /* host is little-endian (x86_64) */
        if (write(fd, &len_le, sizeof(len_le)) != sizeof(len_le))
                return -EIO;
        if (write(fd, request, req_len) != (ssize_t)req_len)
                return -EIO;

        if (read(fd, &reply_len, sizeof(reply_len)) != sizeof(reply_len))
                return -EIO;
        if (reply_len == 0 || reply_len >= out_size)
                return -EMSGSIZE;

        {
                size_t got = 0;
                while (got < reply_len) {
                        ssize_t n = read(fd, out + got, reply_len - got);
                        if (n <= 0)
                                return -EIO;
                        got += (size_t)n;
                }
        }
        out[reply_len] = '\0';
        return (ssize_t)reply_len;
}

/* Pull a JSON integer or string value for "key" out of a flat response
 * object. Only handles what quantra-logind's Response shape actually
 * contains here — not a general parser. Returns 0 on success. */
static int json_extract_int(const char *json, const char *key, long *out) {
        char needle[64];
        const char *p;

        snprintf(needle, sizeof(needle), "\"%s\":", key);
        p = strstr(json, needle);
        if (!p)
                return -1;
        p += strlen(needle);
        while (*p == ' ')
                p++;
        if (strncmp(p, "null", 4) == 0)
                return -1;
        errno = 0;
        *out = strtol(p, NULL, 10);
        if (errno != 0)
                return -1;
        return 0;
}

static int json_response_ok(const char *json) {
        return strstr(json, "\"ok\":true") != NULL;
}

/* seat_id -> active session's uid, via two round-trips to quantra-logind.
 * Returns 0 and fills *uid on success, negative on any failure (including
 * "nobody logged in", which the caller treats as a normal skip). */
static int lookup_active_uid(const char *seat_id, uid_t *uid) {
        int fd;
        char req[128];
        char resp[8192]; /* get_seat's reply includes a devices[] array, can get long */
        ssize_t n;
        long session_id;
        long uid_val;

        fd = logind_connect();
        if (fd < 0)
                return fd;

        snprintf(req, sizeof(req), "{\"cmd\":\"get_seat\",\"seat_id\":\"%s\"}", seat_id);
        n = logind_roundtrip(fd, req, resp, sizeof(resp));
        close(fd);
        if (n < 0 || !json_response_ok(resp))
                return -1;
        if (json_extract_int(resp, "active_session", &session_id) < 0)
                return -1; /* no active session on this seat right now */

        fd = logind_connect();
        if (fd < 0)
                return fd;

        snprintf(req, sizeof(req), "{\"cmd\":\"get_session\",\"session_id\":%ld}", session_id);
        n = logind_roundtrip(fd, req, resp, sizeof(resp));
        close(fd);
        if (n < 0 || !json_response_ok(resp))
                return -1;
        if (json_extract_int(resp, "uid", &uid_val) < 0)
                return -1;

        *uid = (uid_t)uid_val;
        return 0;
}

static int grant_acl(const char *devnode, uid_t uid) {
        acl_t acl;
        acl_entry_t entry;
        acl_permset_t permset;
        int r = -1;

        /* On any ACL-capable Linux filesystem this always returns a valid
         * minimal ACL synthesized from the mode bits, even if no extended
         * ACL has ever been set — it only fails on a real error (ENOENT,
         * or the filesystem/kernel not supporting ACLs at all, e.g. some
         * devtmpfs configs). Don't try to synthesize a blank one on
         * failure: acl_init() gives an ACL with none of the required base
         * entries (ACL_USER_OBJ/ACL_GROUP_OBJ/ACL_OTHER), which acl_valid()
         * below would then correctly reject anyway. */
        acl = acl_get_file(devnode, ACL_TYPE_ACCESS);
        if (!acl)
                return -1;

        if (acl_create_entry(&acl, &entry) != 0)
                goto out;
        if (acl_set_tag_type(entry, ACL_USER) != 0)
                goto out;
        if (acl_set_qualifier(entry, &uid) != 0)
                goto out;
        if (acl_get_permset(entry, &permset) != 0)
                goto out;
        acl_clear_perms(permset);
        acl_add_perm(permset, ACL_READ);
        acl_add_perm(permset, ACL_WRITE);
        if (acl_set_permset(entry, permset) != 0)
                goto out;

        if (acl_calc_mask(&acl) != 0)
                goto out;
        if (acl_valid(acl) != 0)
                goto out;

        r = acl_set_file(devnode, ACL_TYPE_ACCESS, acl);

out:
        acl_free(acl);
        return r;
}

static int builtin_uaccess(struct udev_device *dev, int argc, char *argv[], bool test) {
        const char *action = udev_device_get_action(dev);
        const char *devnode;
        const char *seat_id;
        uid_t uid;

        /* Device is gone, nothing to grant an ACL on. Revoking ACLs on
         * *existing* devices when a session ends is a separate,
         * logind-driven concern — not handled here, see the file header. */
        if (action && streq(action, "remove"))
                return EXIT_SUCCESS;

        devnode = udev_device_get_devnode(dev);
        if (!devnode)
                return EXIT_SUCCESS;

        seat_id = udev_device_get_property_value(dev, "ID_SEAT");
        if (!seat_id || seat_id[0] == '\0')
                seat_id = "seat0";

        if (lookup_active_uid(seat_id, &uid) < 0) {
                /* Not an error: logind unreachable (sandbox/chroot/boot-time
                 * race) or nobody's logged in on this seat yet. */
                if (test)
                        printf("uaccess: no active session on '%s', skipping %s\n", seat_id, devnode);
                return EXIT_SUCCESS;
        }

        if (grant_acl(devnode, uid) < 0) {
                log_warning_errno(errno, "uaccess: failed to grant ACL on '%s' to uid %u: %m", devnode, uid);
                return EXIT_SUCCESS; /* non-fatal — don't fail the whole rule chain over this */
        }

        if (test)
                printf("uaccess: granted uid %u access to %s (seat '%s')\n", uid, devnode, seat_id);

        return EXIT_SUCCESS;
}

const struct udev_builtin udev_builtin_uaccess = {
        .name = "uaccess",
        .cmd = builtin_uaccess,
        .help = "Grant the active seat user ACL access to this device node",
};
