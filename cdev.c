#include <linux/version.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "cdev.h"

// the char device for this module interacts with user space
// via file operations
const static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = CDEV_FUNC(open),
    .release = CDEV_FUNC(release),
    .read = CDEV_FUNC(read),
    .write = CDEV_FUNC(write),
};

static int major = 0, minor = 0;
static dev_t scc_dev;
static struct class *scc_class;

int dev_init(void)
{
    int rc = 0;

    rc = major = register_chrdev(major, CDEV_NAME, &fops);
    if (rc < 0)
    {
        printk(KERN_ERR "Failed to register char device\n");
        rc = -2;
        goto failed_cdev;
    }

    scc_dev = MKDEV(major, minor);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    scc_class = class_create(CDEV_NAME);
#else
    scc_class = class_create(THIS_MODULE, CDEV_NAME);
#endif
    if (!scc_class)
    {
        printk(KERN_ERR "Failed to create class\n");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(scc_class, NULL, scc_dev, NULL, CDEV_NAME))
    {
        printk(KERN_ERR "Failed to create device\n");
        rc = -4;
        goto failed_device_create;
    }

    return rc;

failed_device_create:
    class_destroy(scc_class);

failed_class_create:
failed_cdev:
    unregister_chrdev(major, CDEV_NAME);
    return rc;
}

void dev_exit(void)
{
    device_destroy(scc_class, scc_dev);
    class_destroy(scc_class);
    unregister_chrdev(major, CDEV_NAME);
}

// open 
int CDEV_FUNC(open)(struct inode *inode, struct file *filp)
{
    printk(KERN_DEBUG "CDEV_FUNC(open)\n");
    return 0;
}

// release
int CDEV_FUNC(release)(struct inode *inode, struct file *filp)
{
    printk(KERN_DEBUG "CDEV_FUNC(release)\n");
    return 0;
}

// read
ssize_t CDEV_FUNC(read)(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    printk(KERN_DEBUG "CDEV_FUNC(read)\n");
    return 0;
}

// write 
ssize_t CDEV_FUNC(write)(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    printk(KERN_DEBUG "CDEV_FUNC(write)\n");
    return 0;
}