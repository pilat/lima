#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/fsnotify.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#define MODULE_NAME "fake_inotify"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vladimir Urushev");
MODULE_DESCRIPTION("Kernel Module for Fake Inotify Events");
MODULE_VERSION("0.9");

/* -------------------------------------------------------------------------- */
/* Optional parameter to enable/disable log messages (default: false).        */
/*                                                                            */
/* To enable logging, load the module with enable_logging=1 parameter:        */
/*    `sudo insmod fake_inotify.ko enable_logging=1`                          */
/*                                                                            */
/* At runtime, you can enable/disable logging by writing to the parameter:    */
/*    `echo 1 > /sys/module/fake_inotify/parameters/enable_logging`           */
/* -------------------------------------------------------------------------- */
static bool enable_logging = false;
module_param(enable_logging, bool, 0644);
MODULE_PARM_DESC(enable_logging, "Enable or disable debug logs");

static struct proc_dir_entry *proc_entry;

/* Lightweight logging wrappers */
#define log_info(fmt, ...) \
	do { \
		if (enable_logging) \
			pr_info("%s: " fmt, MODULE_NAME, ##__VA_ARGS__); \
	} while (0)

#define log_warn(fmt, ...) \
	do { \
		if (enable_logging) \
			pr_warn("%s: " fmt, MODULE_NAME, ##__VA_ARGS__); \
	} while (0)

/* -------------------------------------------------------------------------- */
/* Helper: Safely copy a path string from user space.                         */
/* -------------------------------------------------------------------------- */
static char *copy_path_from_user(const char __user *ubuf, size_t count)
{
	char *buf;

	if (count > PATH_MAX)
		return ERR_PTR(-EFAULT);

	buf = kmalloc(count + 1, GFP_USER);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	if (copy_from_user(buf, ubuf, count)) {
		kfree(buf);
		return ERR_PTR(-EFAULT);
	}
	buf[count] = '\0';

	return buf;
}

/* -------------------------------------------------------------------------- */
/* Helper: Generic function to trigger inotify events. Logs the event name.   */
/* -------------------------------------------------------------------------- */
static int trigger_event(const char __user *ubuf, size_t count, loff_t *ppos,
			 void (*event_handler)(struct inode *, struct dentry *, struct path *),
			 const char *event_name)
{
	char *buf;
	struct path child_path;
	struct dentry *parent_dentry;
	struct inode *parent_inode;
	int ret;

	buf = copy_path_from_user(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	log_info("Triggering %s event for path: %s\n", event_name, buf);

	ret = kern_path(buf, LOOKUP_FOLLOW, &child_path);
	if (ret) {
		kfree(buf);
		log_warn("kern_path failed in %s event, ret=%d\n", event_name, ret);
		return ret;
	}

	parent_dentry = dget_parent(child_path.dentry);
	parent_inode = parent_dentry->d_inode;

	event_handler(parent_inode, child_path.dentry, &child_path);

	dput(parent_dentry);
	path_put(&child_path);
	kfree(buf);

	*ppos = count;
	return count;
}

/* -------------------------------------------------------------------------- */
/* Event handler functions                                                    */
/* -------------------------------------------------------------------------- */
static void handle_modify(struct inode *inode, struct dentry *dentry, struct path *path)
{
	fsnotify_parent(dentry, FS_MODIFY, path, FSNOTIFY_EVENT_PATH);
}

static void handle_attrib(struct inode *inode, struct dentry *dentry, struct path *path)
{
	fsnotify_change(dentry, FS_ATTRIB);
}

static void handle_create(struct inode *inode, struct dentry *dentry, struct path *path)
{
	fsnotify_create(inode, dentry);
}

/* -------------------------------------------------------------------------- */
/* Write handler wrappers for "modify", "attrib", "create"                    */
/* -------------------------------------------------------------------------- */
static ssize_t modify_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	return trigger_event(ubuf, count, ppos, handle_modify, "modify");
}

static ssize_t attrib_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	return trigger_event(ubuf, count, ppos, handle_attrib, "attrib");
}

static ssize_t create_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	return trigger_event(ubuf, count, ppos, handle_create, "create");
}

/* -------------------------------------------------------------------------- */
/* Common logic for simulating file deletion.                                 */
/* -------------------------------------------------------------------------- */
static ssize_t handle_deletion(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	int ret;
	char *buf;
	char *parent_path;
	char *filename;
	struct path parent_dir_path;
	struct dentry *parent_dentry;
	struct inode *parent_inode, *fake_inode;
	struct qstr fake_dname;

	buf = copy_path_from_user(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	log_info("Requested file deletion: %s\n", buf);

	filename = strrchr(buf, '/');
	if (!filename) {
		log_warn("Invalid path: no '/' separator found\n");
		kfree(buf);
		return -EINVAL;
	}

	*filename = '\0'; /* Separate parent path from filename */
	filename++;
	parent_path = buf;

	log_info("Parent path: %s, Filename: %s\n", parent_path, filename);

	ret = kern_path(parent_path, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent_dir_path);
	if (ret) {
		log_warn("kern_path failed for parent directory: %d\n", ret);
		kfree(buf);
		return ret;
	}

	parent_dentry = parent_dir_path.dentry;
	parent_inode  = parent_dentry->d_inode;

	fake_inode = new_inode(parent_inode->i_sb);
	if (!fake_inode) {
		log_warn("Failed to allocate fake inode\n");
		path_put(&parent_dir_path);
		kfree(buf);
		return -ENOMEM;
	}

	fake_inode->i_ino  = get_next_ino();
	fake_inode->i_mode = S_IFREG; /* Simulate file deletion */

	fake_dname.name = filename;
	fake_dname.len  = strlen(filename);
	fake_dname.hash = full_name_hash(parent_dentry, fake_dname.name, fake_dname.len);

	log_info("Simulating FS_DELETE for %s in %s\n", filename, parent_path);
	fsnotify_name(FS_DELETE, fake_inode, FSNOTIFY_EVENT_INODE,
		      parent_inode, &fake_dname, 0);

	path_put(&parent_dir_path);
	kfree(buf);

	*ppos = count;
	return count;
}

/* -------------------------------------------------------------------------- */
/* File deletion (unlink)                                                     */
/* -------------------------------------------------------------------------- */
static ssize_t unlink_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	return handle_deletion(file, ubuf, count, ppos);
}

/* -------------------------------------------------------------------------- */
/* proc_ops definitions for each operation                                    */
/* -------------------------------------------------------------------------- */
static const struct proc_ops modify_ops = {
	.proc_write = modify_write,
};

static const struct proc_ops attrib_ops = {
	.proc_write = attrib_write,
};

static const struct proc_ops create_ops = {
	.proc_write = create_write,
};

static const struct proc_ops unlink_ops = {
	.proc_write = unlink_write,
};

/* -------------------------------------------------------------------------- */
/* Module initialization                                                      */
/* -------------------------------------------------------------------------- */
static int __init fake_inotify_init(void)
{
	proc_entry = proc_mkdir("fake_inotify", NULL);
	if (!proc_entry)
		return -ENOMEM;

	proc_create("modify", 0666, proc_entry, &modify_ops);
	proc_create("attrib", 0666, proc_entry, &attrib_ops);
	proc_create("create", 0666, proc_entry, &create_ops);
	proc_create("unlink", 0666, proc_entry, &unlink_ops);

	log_info("Fake inotify module loaded. Logging = %s\n",
		 enable_logging ? "enabled" : "disabled");
	return 0;
}

/* -------------------------------------------------------------------------- */
/* Module cleanup                                                             */
/* -------------------------------------------------------------------------- */
static void __exit fake_inotify_exit(void)
{
	proc_remove(proc_entry);
	log_info("Fake inotify module unloaded\n");
}

module_init(fake_inotify_init);
module_exit(fake_inotify_exit);
