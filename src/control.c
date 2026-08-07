#include "awesomewm_screenlock_control.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    CONTROL_FRAME_HEADER_SIZE = 4,
    CONTROL_MAX_PAYLOAD = sizeof(((struct screenlock_control *)0)->frame)
};

struct message_cursor {
    const unsigned char *data;
    size_t length;
    size_t offset;
};

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0)
        return;
    strncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
}

static int set_nonblocking(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFL, 0);

    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int read_bytes(struct message_cursor *cursor, void *destination, size_t length)
{
    if (length > cursor->length - cursor->offset)
        return 0;
    memcpy(destination, cursor->data + cursor->offset, length);
    cursor->offset += length;
    return 1;
}

static int read_message_string(
    struct message_cursor *cursor,
    char *destination,
    size_t capacity
)
{
    unsigned char marker;
    uint16_t length;

    if (!read_bytes(cursor, &marker, sizeof(marker)))
        return 0;
    if ((marker & 0xe0) == 0xa0) {
        length = marker & 0x1f;
    } else if (marker == 0xd9) {
        unsigned char short_length;

        if (!read_bytes(cursor, &short_length, sizeof(short_length)))
            return 0;
        length = short_length;
    } else if (marker == 0xda) {
        uint16_t network_length;

        if (!read_bytes(cursor, &network_length, sizeof(network_length)))
            return 0;
        length = ntohs(network_length);
    } else {
        return 0;
    }
    if (length >= capacity || !read_bytes(cursor, destination, length))
        return 0;
    destination[length] = '\0';
    return 1;
}

static int read_message_map_length(struct message_cursor *cursor, unsigned *length)
{
    unsigned char marker;

    if (!read_bytes(cursor, &marker, sizeof(marker)))
        return 0;
    if ((marker & 0xf0) == 0x80) {
        *length = marker & 0x0f;
        return 1;
    }
    if (marker == 0xde) {
        uint16_t network_length;

        if (!read_bytes(cursor, &network_length, sizeof(network_length)))
            return 0;
        *length = ntohs(network_length);
        return 1;
    }
    return 0;
}

static int read_message_value(
    struct message_cursor *cursor,
    char *destination,
    size_t capacity
)
{
    return read_message_string(cursor, destination, capacity);
}

static int decode_notification(
    const unsigned char *payload,
    size_t payload_length,
    struct screenlock_control_notification *notification
)
{
    struct message_cursor cursor = {
        .data = payload,
        .length = payload_length,
    };
    char key[32];
    char value[1024];
    unsigned field_count;
    int type_valid = 0;

    memset(notification, 0, sizeof(*notification));
    if (!read_message_map_length(&cursor, &field_count))
        return 0;
    for (unsigned index = 0; index < field_count; index++) {
        if (!read_message_string(&cursor, key, sizeof(key))
            || !read_message_value(&cursor, value, sizeof(value))) {
            return 0;
        }
        if (strcmp(key, "type") == 0) {
            if (strcmp(value, "notification") != 0)
                return 0;
            type_valid = 1;
        } else if (strcmp(key, "level") == 0) {
            copy_text(notification->level, sizeof(notification->level), value);
        } else if (strcmp(key, "title") == 0) {
            copy_text(notification->title, sizeof(notification->title), value);
        } else if (strcmp(key, "text") == 0) {
            copy_text(notification->text, sizeof(notification->text), value);
        }
    }
    return type_valid && notification->text[0] != '\0';
}

static int consume_frame(
    struct screenlock_control *control,
    struct screenlock_control_notification *notification
)
{
    uint32_t network_length;
    size_t payload_length;

    if (control->frame_length < CONTROL_FRAME_HEADER_SIZE)
        return 0;
    memcpy(&network_length, control->frame, sizeof(network_length));
    payload_length = ntohl(network_length);
    if (payload_length < 1 || payload_length > CONTROL_MAX_PAYLOAD
        || control->frame_length < CONTROL_FRAME_HEADER_SIZE + payload_length) {
        if (payload_length < 1 || payload_length > CONTROL_MAX_PAYLOAD) {
            control->frame_length = 0;
            control->frame_expected = 0;
            return 1;
        }
        return 0;
    }
    if (control->frame[CONTROL_FRAME_HEADER_SIZE] == SCREENLOCK_CONTROL_NOTIFICATION)
        decode_notification(
            control->frame + CONTROL_FRAME_HEADER_SIZE + 1,
            payload_length - 1,
            notification
        );
    memmove(
        control->frame,
        control->frame + CONTROL_FRAME_HEADER_SIZE + payload_length,
        control->frame_length - CONTROL_FRAME_HEADER_SIZE - payload_length
    );
    control->frame_length -= CONTROL_FRAME_HEADER_SIZE + payload_length;
    control->frame_expected = 0;
    return 1;
}

int screenlock_control_open(struct screenlock_control *control, const char *path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };

    memset(control, 0, sizeof(*control));
    control->listener = -1;
    control->client = -1;
    if (path == NULL || strlen(path) >= sizeof(address.sun_path))
        return 1;
    snprintf(control->path, sizeof(control->path), "%s", path);
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    unlink(path);
    control->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (control->listener < 0 || bind(
            control->listener, (struct sockaddr *)&address,
            sizeof(address)
        ) < 0
        || chmod(path, S_IRUSR | S_IWUSR) < 0
        || listen(control->listener, 1) < 0
        || !set_nonblocking(control->listener)) {
        screenlock_control_close(control);
        return 0;
    }
    return 1;
}

void screenlock_control_poll(
    struct screenlock_control *control,
    struct screenlock_control_notification *notification
)
{
    ssize_t received;

    memset(notification, 0, sizeof(*notification));
    if (control->listener < 0)
        return;
    if (control->client < 0) {
        control->client = accept(control->listener, NULL, NULL);
        if (control->client >= 0 && !set_nonblocking(control->client)) {
            close(control->client);
            control->client = -1;
        }
    }
    if (control->client < 0)
        return;
    received = read(
        control->client,
        control->frame + control->frame_length,
        sizeof(control->frame) - control->frame_length
    );
    if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(control->client);
        control->client = -1;
        control->frame_length = 0;
        return;
    }
    if (received > 0)
        control->frame_length += (size_t)received;
    while (control->frame_length >= CONTROL_FRAME_HEADER_SIZE
           && consume_frame(control, notification)) {
    }
}

void screenlock_control_close(struct screenlock_control *control)
{
    if (control->client >= 0)
        close(control->client);
    if (control->listener >= 0)
        close(control->listener);
    if (control->path[0] != '\0')
        unlink(control->path);
    control->client = -1;
    control->listener = -1;
    control->path[0] = '\0';
}
