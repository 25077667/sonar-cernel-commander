#include <linux/version.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include "cdev.h"
#include "syscall_hook.h"
#include "event_logger.h"
#include "event_schema.h"

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
static DEFINE_MUTEX(io_mutex);

static ssize_t detail_event_to_user(struct event *event, size_t count, char __user *buf);

// typedef dispatcher_fn
typedef ssize_t (*dispatcher_fn)(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static ssize_t do_hook(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static ssize_t do_unhook(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static ssize_t do_enable(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static ssize_t do_disable(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);

struct operation_dispatcher
{
    const char *name;
    const dispatcher_fn functor;
};

static struct operation_dispatcher dispatch_table[] = {
    {"hook", do_hook},
    {"unhook", do_unhook},
    {"enable", do_enable},
    {"disable", do_disable},
};

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

int CDEV_FUNC(open)(struct inode *inode, struct file *filp)
{
    if (!mutex_trylock(&io_mutex))
    {
        printk(KERN_ERR "Failed to lock scc mutex\n");
        return -EBUSY;
    }
    return 0;
}

int CDEV_FUNC(release)(struct inode *inode, struct file *filp)
{
    mutex_unlock(&io_mutex);
    return 0;
}

ssize_t CDEV_FUNC(read)(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    // infinite reading values from get_events(...), keep reading until signal is received
    // or the buffer is full
#define CAPACITY 10
    struct event events[CAPACITY];
    int size = 0;
    int rc = get_events(events, &size, CAPACITY);
    if (rc < 0)
    {
        printk(KERN_ERR "Failed to get events\n");
        return -ENODATA;
    }

    if (size == 0)
    {
        printk(KERN_INFO "No events\n");
        return 0;
    }

    return detail_event_to_user(events, size, buf);
#undef CAPACITY
}

ssize_t CDEV_FUNC(write)(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    static char buf_local[256] = {0};
    if (count >= sizeof(buf_local))
    {
        printk(KERN_ERR "Invalid count %ld\n", count);
        return -EINVAL;
    }
    if (copy_from_user(buf_local, buf, count))
    {
        printk(KERN_ERR "Failed to copy from user space\n");
        return -EINVAL;
    }

    for (int i = 0; i < sizeof(dispatch_table) / sizeof(dispatch_table[0]); ++i)
    {
        const size_t dispatcher_name_len = strlen(dispatch_table[i].name);
        const int len = min(dispatcher_name_len, count - 1);
        if (strncmp(buf_local, dispatch_table[i].name, len) == 0)
        {
            return dispatch_table[i].functor(filp, buf, count, f_pos);
        }
    }

    // not found
    printk(KERN_ERR "Invalid operation %s\n", buf);
    return -EINVAL;
}

static ssize_t detail_event_to_user(struct event *event, size_t count, char __user *buf)
{
    struct event_schema *schema = kmalloc(count * sizeof(struct event_schema), GFP_KERNEL);
    if (!schema)
    {
        printk(KERN_ERR "Failed to allocate memory\n");
        return -ENOMEM;
    }

    for (int i = 0; i < count; ++i)
    {
        event_to_schema(event + i, schema + i);
    }

    unsigned long _ = copy_to_user(buf, schema, count * sizeof(struct event_schema));
    if (_ != 0)
    {
        printk(KERN_ERR "Failed to copy to user space\n");
        kfree(schema);
        return -EINVAL;
    }

    kfree(schema);

    return count * sizeof(struct event_schema);
}

static ssize_t do_hook(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    int rc = hook_syscall();
    if (rc < 0)
    {
        printk(KERN_ERR "Failed to hook syscall\n");
        return -EINVAL;
    }
    return count;
}

static ssize_t do_unhook(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    int rc = unhook_syscall();
    if (rc < 0)
    {
        printk(KERN_ERR "Failed to unhook syscall\n");
        return -EINVAL;
    }
    printk(KERN_INFO "Unhooked syscall success\n");

    return count;
}

static ssize_t do_enable(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    enable_event_logger(1);
    printk(KERN_INFO "Enabled syscall event logger\n");

    return count;
}

static ssize_t do_disable(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    enable_event_logger(0);
    printk(KERN_INFO "Disabled syscall event logger\n");

    return count;
}
