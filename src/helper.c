#include "awesomewm_screenlock.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s\n", program);
}

int main(int argc, char **argv)
{
    const char *service = getenv("AWESOMEWM_SCREENLOCK_PAM_SERVICE");
    const char *user = getenv("USER");

    if (argc != 1) {
        usage(argv[0]);
        return 64;
    }

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

    return awesomewm_screenlock_run(service, user);
}
