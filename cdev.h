#ifndef __SCC_CDEV_H__
#define __SCC_CDEV_H__

#include <linux/cdev.h>

#include "glob_conf.h"

#ifdef CDEV_FUNC
#undef CDEV_FUNC
#endif

#define CDEV_FUNC(x) SCC_cdev_##x

int CDEV_FUNC(open)(struct inode *, struct file *);
int CDEV_FUNC(release)(struct inode *, struct file *);
ssize_t CDEV_FUNC(read)(struct file *, char __user *, size_t, loff_t *);
ssize_t CDEV_FUNC(write)(struct file *, const char __user *, size_t, loff_t *);

#ifdef CDEV_NAME
#undef CDEV_NAME
#endif

#define CDEV_NAME DEVICE_NAME

int dev_init(void);
void dev_exit(void);

#endif // __SCC_CDEV_H__