#ifndef AWESOMEWM_SCREENLOCK_H
#define AWESOMEWM_SCREENLOCK_H

#include <stddef.h>
#include <stdint.h>

struct awesomewm_screenlock_options {
    /* Optional per-session socket for non-sensitive lock display state. */
    const char *control_socket;
    /* True keeps the helper's complete keyboard/pointer lockdown. */
    int lockdown_enabled;
    /* Real Awesome wibar XIDs used only when lockdown_enabled is false. */
    const uint32_t *wibar_windows;
    size_t wibar_window_count;
};

/* Run the lock surface until PAM authenticates the current user. */
int awesomewm_screenlock_run(const char *pam_service, const char *user);

/* Run the lock surface with optional AwesomeWM integration settings. */
int awesomewm_screenlock_run_with_options(
    const char *pam_service,
    const char *user,
    const struct awesomewm_screenlock_options *options
);

#endif
