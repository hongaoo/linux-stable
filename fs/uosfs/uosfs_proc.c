#include <linux/proc_fs.h>

int uos_path_filiter_enable_flag = 0;

static ssize_t uosfs_path_filiter_enable_write(struct file *file, const char __user *buffer,
                                         size_t count, loff_t *ppos)
{
    int ret;
    char buf[256];
    size_t len;
    int tmp;

    len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, buffer, len))
		return -EFAULT;
    buf[len] = '\0';

    ret = sscanf(buf, "%d", &tmp);
    if (ret != 1) {
        printk(KERN_ERR "Invalid input format\n");
        return -EINVAL;
    }
    if (tmp !=1 && tmp !=0){
        printk(KERN_ERR "Invalid input format\n");
        return -EINVAL;
    }
    uos_path_filiter_enable_flag = tmp;
    return count;
}

static ssize_t uosfs_path_filiter_enable_read(struct file *file, char __user *buffer,
                                        size_t count, loff_t *ppos)
{
    char buf[256];
    int len;

    len = snprintf(buf, sizeof(buf), "%d\n", uos_path_filiter_enable_flag);

    if (*ppos > 0 || count < len)
        return 0;

    if (copy_to_user(buffer, buf, len))
        return -EFAULT;

    *ppos = len;
    return len;
}

const struct proc_ops uosfs_path_filiter_enable_fops = {
    .proc_write = uosfs_path_filiter_enable_write,
    .proc_read = uosfs_path_filiter_enable_read,
};


int uos_path_filiter_pid;
extern char  *uos_path_filiter_path_name;

static ssize_t uosfs_path_filiter_write(struct file *file, const char __user *buffer,
                                         size_t count, loff_t *ppos)
{
    int ret;
    char buf[256];
    size_t len;

    len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, buffer, len))
		return -EFAULT;
    buf[len] = '\0';

    ret = sscanf(buf, "%d,%s", &uos_path_filiter_pid, uos_path_filiter_path_name);
    if (ret != 2) {
        printk(KERN_ERR "Invalid input format\n");
        return -EINVAL;
    }
    return count;
}

static ssize_t uosfs_path_filiter_read(struct file *file, char __user *buffer,
                                        size_t count, loff_t *ppos)
{
    char buf[256];
    int len;

    len = snprintf(buf, sizeof(buf), "%d,%s\n", uos_path_filiter_pid, uos_path_filiter_path_name);

    if (*ppos > 0 || count < len)
        return 0;

    if (copy_to_user(buffer, buf, len))
        return -EFAULT;

    *ppos = len;
    return len;
}

const struct proc_ops uosfs_path_filiter_fops = {
    .proc_write = uosfs_path_filiter_write,
    .proc_read = uosfs_path_filiter_read,
};