#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_MAGIC 'M'

struct container_limits {
    pid_t pid;
    unsigned long soft_limit_bytes;   /* warn when RSS exceeds this */
    unsigned long hard_limit_bytes;   /* kill when RSS exceeds this */
    char name[64];
};

#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct container_limits)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, pid_t)

#endif /* MONITOR_IOCTL_H */
