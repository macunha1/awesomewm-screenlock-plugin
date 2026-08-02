#ifndef AWESOMEWM_SCREENLOCK_H
#define AWESOMEWM_SCREENLOCK_H

/* Run the lock surface until PAM authenticates the current user. */
int awesomewm_screenlock_run(const char *pam_service, const char *user);

#endif
