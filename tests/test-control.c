#define _POSIX_C_SOURCE 200809L

#include "awesomewm_screenlock_control.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_client(const char *path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int client = socket(AF_UNIX, SOCK_STREAM, 0);

    assert(client >= 0);
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    assert(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    return client;
}

static void send_frame(int client, const unsigned char *payload, size_t length)
{
    uint32_t header = htonl((uint32_t)length + 1);
    unsigned char type = SCREENLOCK_CONTROL_NOTIFICATION;

    assert(write(client, &header, sizeof(header)) == sizeof(header));
    assert(write(client, &type, 1) == 1);
    assert(write(client, payload, length) == (ssize_t)length);
}

int main(void)
{
    struct screenlock_control control;
    struct screenlock_control_notification notification;
    char directory[] = "/tmp/screenlock-control-XXXXXX";
    char path[108];
    int client;
    const unsigned char valid[] = "\x82\xa4type\xacnotification\xa4text\xa5hello";
    const unsigned char malformed[] = "\x82\xa4text\xa6unsafe\xa4type\xa5wrong";
    uint32_t header = htonl(8192);

    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/socket", directory);
    assert(screenlock_control_open(&control, path));
    client = connect_client(path);

    /* Partial headers survive nonblocking reads, then decode once complete. */
    header = htonl(sizeof(valid));
    assert(write(client, &header, 2) == 2);
    screenlock_control_poll(&control, &notification);
    assert(notification.text[0] == '\0');
    assert(write(client, (unsigned char *)&header + 2, 2) == 2);
    {
        unsigned char type = SCREENLOCK_CONTROL_NOTIFICATION;
        assert(write(client, &type, 1) == 1);
    }
    assert(write(client, valid, sizeof(valid) - 1) == sizeof(valid) - 1);
    screenlock_control_poll(&control, &notification);
    assert(strcmp(notification.text, "hello") == 0);

    /* A rejected map must never expose partially decoded text. */
    send_frame(client, malformed, sizeof(malformed) - 1);
    screenlock_control_poll(&control, &notification);
    assert(notification.text[0] == '\0');

    /* Lengths exceeding buffer space must reset without wedging the client. */
    header = htonl(8192);
    assert(write(client, &header, sizeof(header)) == sizeof(header));
    screenlock_control_poll(&control, &notification);
    assert(control.frame_length == 0);
    send_frame(client, valid, sizeof(valid) - 1);
    screenlock_control_poll(&control, &notification);
    assert(strcmp(notification.text, "hello") == 0);

    close(client);
    screenlock_control_poll(&control, &notification);
    assert(control.client == -1);
    client = connect_client(path);
    send_frame(client, valid, sizeof(valid) - 1);
    screenlock_control_poll(&control, &notification);
    assert(strcmp(notification.text, "hello") == 0);
    close(client);
    screenlock_control_close(&control);
    assert(access(path, F_OK) != 0);
    assert(rmdir(directory) == 0);
    {
        char too_long[109];
        memset(too_long, 'a', sizeof(too_long) - 1);
        too_long[sizeof(too_long) - 1] = '\0';
        assert(!screenlock_control_open(&control, too_long));
    }
    return 0;
}
