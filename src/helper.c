#include "awesomewm_screenlock.h"

#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--control-socket PATH] [--integrated --wibar-window ID ...]\n", program);
}

int main(int argc, char **argv)
{
    const char *service = getenv("AWESOMEWM_SCREENLOCK_PAM_SERVICE");
    const char *user = getenv("USER");

    struct awesomewm_screenlock_options options = { .lockdown_enabled = 1 };
    uint32_t wibar_windows[16];
    int argument = 1;

    while (argument < argc) {
        if (strcmp(argv[argument], "--control-socket") == 0 && argument + 1 < argc) {
            options.control_socket = argv[argument + 1];
            argument += 2;
        } else if (strcmp(argv[argument], "--integrated") == 0) {
            options.lockdown_enabled = 0;
            argument++;
        } else if (strcmp(argv[argument], "--wibar-window") == 0
                   && argument + 1 < argc
                   && options.wibar_window_count < sizeof(wibar_windows) / sizeof(wibar_windows[0])) {
            char *end = NULL;
            unsigned long window = strtoul(argv[argument + 1], &end, 0);

            if (end == argv[argument + 1] || *end != '\0' || window > UINT32_MAX) {
                usage(argv[0]);
                return 64;
            }
            wibar_windows[options.wibar_window_count++] = (uint32_t)window;
            argument += 2;
        } else {
            usage(argv[0]);
            return 64;
        }
    }
    options.wibar_windows = wibar_windows;

    if (service == NULL || service[0] == '\0')
        service = "xlock";
    if (user == NULL || user[0] == '\0') {
        struct passwd *account = getpwuid(getuid());
        if (account != NULL)
            user = account->pw_name;
    }
    if (user == NULL || user[0] == '\0') {
        fprintf(stderr, "awesomewm-screenlock-helper: current user is unavailable\n");
        return 64;
    }

    return awesomewm_screenlock_run_with_options(service, user, &options);
}
