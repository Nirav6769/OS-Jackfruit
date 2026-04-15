/*
 * monitor.c — Kernel-space memory monitor LKM
 *
 * Registers a /dev/container_monitor character device.
 * Accepts PID registrations via ioctl, tracks RSS usage,
 * enforces soft (warn) and hard (kill) memory limits.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/pid.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Project");
MODULE_DESCRIPTION("Container memory monitor");

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_MS 2000   /* poll every 2 seconds */

/* ─── per-container tracking entry ─────────────────────────────────────── */
struct container_entry {
    struct list_head list;
    pid_t            pid;
    char             name[64];
    unsigned long    soft_limit;   /* bytes */
    unsigned long    hard_limit;   /* bytes */
    bool             soft_warned;  /* have we already logged the soft warning? */
};

static LIST_HEAD(container_list);
static DEFINE_MUTEX(list_mutex);

/* ─── character device bookkeeping ─────────────────────────────────────── */
static dev_t        dev_num;
static struct cdev  monitor_cdev;
static struct class *monitor_class;

/* ─── timer ─────────────────────────────────────────────────────────────── */
static struct timer_list check_timer;

/* ─── helpers ───────────────────────────────────────────────────────────── */

/* Return RSS in bytes for a given pid, or 0 if the process is gone. */
static unsigned long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    unsigned long rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm)
        rss = get_mm_rss(task->mm) << PAGE_SHIFT;
    rcu_read_unlock();
    return rss;
}

/* Send SIGKILL to a pid. */
static void kill_container(pid_t pid, const char *name)
{
    struct task_struct *task;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        send_sig(SIGKILL, task, 0);
        printk(KERN_WARNING "container_monitor: [%s] pid %d exceeded hard limit — SIGKILL sent\n",
               name, pid);
    }
    rcu_read_unlock();
}

/* Called periodically by the timer to check all registered containers. */
static void check_all_containers(struct timer_list *t)
{
    struct container_entry *entry, *tmp;
    unsigned long rss;

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        /* Check if process still exists */
        rcu_read_lock();
        if (!pid_task(find_vpid(entry->pid), PIDTYPE_PID)) {
            rcu_read_unlock();
            printk(KERN_INFO "container_monitor: [%s] pid %d no longer exists, removing\n",
                   entry->name, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }
        rcu_read_unlock();

        rss = get_rss_bytes(entry->pid);

        if (entry->hard_limit && rss > entry->hard_limit) {
            kill_container(entry->pid, entry->name);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (entry->soft_limit && rss > entry->soft_limit && !entry->soft_warned) {
            printk(KERN_WARNING "container_monitor: [%s] pid %d soft-limit WARNING — RSS %lu KB > limit %lu KB\n",
                   entry->name, entry->pid,
                   rss / 1024, entry->soft_limit / 1024);
            entry->soft_warned = true;
        }
    }
    mutex_unlock(&list_mutex);

    /* Re-arm the timer */
    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

/* ─── file operations ───────────────────────────────────────────────────── */

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct container_limits limits;
    struct container_entry *entry, *tmp;
    pid_t unreg_pid;

    switch (cmd) {
    case MONITOR_REGISTER:
        if (copy_from_user(&limits, (void __user *)arg, sizeof(limits)))
            return -EFAULT;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid        = limits.pid;
        entry->soft_limit = limits.soft_limit_bytes;
        entry->hard_limit = limits.hard_limit_bytes;
        entry->soft_warned = false;
        strncpy(entry->name, limits.name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';

        mutex_lock(&list_mutex);
        list_add_tail(&entry->list, &container_list);
        mutex_unlock(&list_mutex);

        printk(KERN_INFO "container_monitor: registered [%s] pid=%d soft=%lu hard=%lu bytes\n",
               entry->name, entry->pid, entry->soft_limit, entry->hard_limit);
        break;

    case MONITOR_UNREGISTER:
        if (copy_from_user(&unreg_pid, (void __user *)arg, sizeof(pid_t)))
            return -EFAULT;

        mutex_lock(&list_mutex);
        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->pid == unreg_pid) {
                printk(KERN_INFO "container_monitor: unregistered [%s] pid=%d\n",
                       entry->name, entry->pid);
                list_del(&entry->list);
                kfree(entry);
                break;
            }
        }
        mutex_unlock(&list_mutex);
        break;

    default:
        return -ENOTTY;
    }
    return 0;
}

static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ─── module init / exit ────────────────────────────────────────────────── */

static int __init monitor_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "container_monitor: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&monitor_cdev, &monitor_fops);
    monitor_cdev.owner = THIS_MODULE;

    ret = cdev_add(&monitor_cdev, dev_num, 1);
    if (ret < 0) {
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    monitor_class = class_create(DEVICE_NAME);
    if (IS_ERR(monitor_class)) {
        cdev_del(&monitor_cdev);
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(monitor_class);
    }

    device_create(monitor_class, NULL, dev_num, NULL, DEVICE_NAME);

    /* Start periodic check timer */
    timer_setup(&check_timer, check_all_containers, 0);
    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));

    printk(KERN_INFO "container_monitor: loaded, device /dev/%s created\n", DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct container_entry *entry, *tmp;

    timer_delete_sync(&check_timer);

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_mutex);

    device_destroy(monitor_class, dev_num);
    class_destroy(monitor_class);
    cdev_del(&monitor_cdev);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "container_monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
