#ifndef FFVFW_LOG_H
#define FFVFW_LOG_H
/* Logging: OutputDebugStringA always (level-gated), plus an optional log file.
 * Configuration (env wins over registry HKCU\Software\FFmpegVfW):
 *   FFVFW_LOG       path of a log file (appended)
 *   FFVFW_LOGLEVEL  0=off 1=errors/lifecycle (default) 2=per-call 3=per-frame
 */
enum { LOG_ERR = 1, LOG_INFO = 2, LOG_TRACE = 3 };
void ffvfw_log_init(void);
void ffvfw_log_shutdown(void);
void ffvfw_log(int level, const char *fmt, ...);
int  ffvfw_log_level(void);
#endif
