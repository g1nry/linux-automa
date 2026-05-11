#ifndef FILEWARD_LOG_H
#define FILEWARD_LOG_H

void log_init(const char *program_name);

void log_info(const char *format, ...);
void log_warn(const char *format, ...);
void log_error(const char *format, ...);
void log_debug(const char *format, ...);

#endif