#ifndef AWESOMEWM_SCREENLOCK_CAPTURE_H
#define AWESOMEWM_SCREENLOCK_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

struct screenlock_capture {
    uint8_t *pixels;
    int width;
    int height;
    int stride;
};

/* Capture and privacy-filter the X11 desktop before the lock surface appears. */
int awesomewm_screenlock_capture(
    const char *display,
    const char *resolution,
    struct screenlock_capture *capture
);

void awesomewm_screenlock_capture_free(struct screenlock_capture *capture);

#endif
