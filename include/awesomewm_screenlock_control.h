#ifndef AWESOMEWM_SCREENLOCK_CONTROL_H
#define AWESOMEWM_SCREENLOCK_CONTROL_H

#include <stddef.h>

enum screenlock_control_message {
    SCREENLOCK_CONTROL_NOTIFICATION = 1,
};

struct screenlock_control_notification {
    char level[16];
    char title[128];
    char text[1024];
};

struct screenlock_control {
    int listener;
    int client;
    char path[108];
    unsigned char frame[8192];
    size_t frame_length;
    size_t frame_expected;
};

int screenlock_control_open(struct screenlock_control *control, const char *path);

void screenlock_control_poll(
    struct screenlock_control *control,
    struct screenlock_control_notification *notification
);

void screenlock_control_close(struct screenlock_control *control);

#endif
