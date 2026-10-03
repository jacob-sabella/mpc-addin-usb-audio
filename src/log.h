/* log.h: append-only log file. Never call from MPC's audio thread (it does file I/O); the audio
 * path only bumps atomic counters, which the forwarder reports. */
#ifndef MPCUA_LOG_H
#define MPCUA_LOG_H

void mpcua_log_open(const char *path);   /* "" or NULL disables logging */
void mpcua_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
