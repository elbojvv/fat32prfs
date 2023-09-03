#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>   
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#define BUFSIZE  100

int proc_prfs_mode;


MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("E.J. van Veldhuizen");
// Based on: https://devarea.com/linux-kernel-development-creating-a-proc-file-and-interfacing-with-user-space/
//MODULE_AUTHOR("Liran B.H");

static struct proc_dir_entry *ent;

int get_proc_prfs_mode(void)
{
	printk(KERN_INFO "get_proc_prfs_mode (proc_handler.c): %i\n", proc_prfs_mode);
	return proc_prfs_mode;
}
EXPORT_SYMBOL_GPL(get_proc_prfs_mode);

static ssize_t prfsproc_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos) 
{
	int num, mode, c;
	char buf[BUFSIZE];
	printk(KERN_INFO "prfsproc_write\n");
	if(*ppos > 0 || count > BUFSIZE)
		return -EFAULT;
	if(copy_from_user(buf, ubuf, count))
		return -EFAULT;
	//printk(KERN_INFO "prfsproc_write: buf: %s\n", buf);
	num = sscanf(buf,"%d", &mode);
	if (num < 1)
		return -EFAULT;
	printk(KERN_INFO "prfsproc_write: num: %i mode: %i\n", num, mode);
	proc_prfs_mode = mode;

	c = strlen(buf);
	*ppos = c;
	return c;
}

static ssize_t prfsproc_read(struct file *file, char __user *ubuf, size_t count, loff_t *ppos) 
{
	char buf[BUFSIZE];
	int len=0;
	if(*ppos > 0 || count < BUFSIZE)
		return 0;
	len += sprintf(buf, "%d\n",proc_prfs_mode);
	
	if(copy_to_user(ubuf, buf, len))
		return -EFAULT;
	*ppos = len;
	return len;
}

static struct proc_ops prfsproc_ops = 
{
//	.proc_owner = THIS_MODULE,
	.proc_read = prfsproc_read,
	.proc_write = prfsproc_write,
};

static int prfsproc_init(void)
{
	proc_prfs_mode = 1; // read only to start with
	ent=proc_create("prfs_mode", 0770, NULL, &prfsproc_ops);
	printk(KERN_INFO  "PRFS_mode proc started.\n");
	return 0;
}

static void prfsproc_cleanup(void)
{
	proc_remove(ent);
	printk(KERN_INFO  "PRFS_mode proc ended.\n");
}

module_init(prfsproc_init);
module_exit(prfsproc_cleanup);