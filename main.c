#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mutex.h>

#include "cdev.h"
#include "glob_conf.h"

// BSD licensed
MODULE_LICENSE(SCC_LICENSE);
MODULE_AUTHOR(SCC_AUTHOR);
MODULE_DESCRIPTION(SCC_DESCRIPTION);
MODULE_VERSION(FULL_VERSION);

static DEFINE_MUTEX(scc_mutex);

static int __init __scc_init(void)
{
    printk(KERN_DEBUG "__scc_init\n");
    mutex_init(&scc_mutex);

    // register char device
    int rc = dev_init();
    if (rc < 0)
    {
        printk(KERN_ERR "Failed to initialize char device\n");
        return rc;
    }

    return 0;
}

static void __exit __scc_exit(void)
{
    printk(KERN_DEBUG "__scc_exit\n");
    mutex_destroy(&scc_mutex);
    dev_exit();
}

// register init and exit function
module_init(__scc_init);
module_exit(__scc_exit);