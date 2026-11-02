#ifndef SHIYING_AUDIO_H
#define SHIYING_AUDIO_H

int         audio_available(void);
const char *audio_error(void);
int         audio_start(int mode, const char *device);
void        audio_set_fd(int fd, double origin);
void        audio_end_fd(void);
void        audio_stop(void);
double      audio_level(int i);

#endif
