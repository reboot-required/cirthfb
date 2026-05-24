#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>

float metrics_cpu_percent(void);
float metrics_cpu_temp(void);
int   metrics_ram_percent(void);
void  metrics_uptime_str(char *buf, size_t n);

#endif /* METRICS_H */
