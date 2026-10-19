#ifndef ANYPS5_KYTY_GPU_KERNEL_PTHREAD_H
#define ANYPS5_KYTY_GPU_KERNEL_PTHREAD_H

#include <pthread.h>
#undef pthread_attr_getguardsize
#undef pthread_attr_setguardsize
#include_next "kernel/pthread.h"

#endif
