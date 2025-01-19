#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/fsnotify.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/user_namespace.h>

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

/* /proc entry for writing fake events */
static struct proc_dir_entry *proc_file;

/* Serialize writers to /proc/fake_inotify */
static DEFINE_MUTEX(fake_inotify_mutex);

/* Lightweight logging macros */
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
/* Pseudo-FS definitions                                                     */
/* -------------------------------------------------------------------------- */
/*
 * We'll create a minimal "fakefs" pseudo-filesystem, from which we allocate
 * inodes (via new_inode(fakefs_sb)) to avoid polluting a FUSE superblock.
 */
static struct vfsmount *fakefs_mnt;
static struct super_block *fakefs_sb;

/* Pick a unique magic number: e.g. "fake" in hex is 0x66616b65 */
#define FAKEFS_MAGIC 0x66616b65

/*
 * Provide minimal super_operations. We only use .statfs and .drop_inode here.
 */
static const struct super_operations fakefs_super_ops = {
	.statfs         = simple_statfs,
	.drop_inode     = generic_delete_inode,
};

/* -------------------------------------------------------------------------- */
/* fill_super callback for our pseudo-fs                                      */
/* -------------------------------------------------------------------------- */
static int fakefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root_inode;
	struct dentry *root_dentry;

	sb->s_magic = FAKEFS_MAGIC;
	sb->s_op = &fakefs_super_ops;
	sb->s_time_gran = 1;  /* 1ns granularity is typical in modern kernels */

	/*
	 * Create a root inode (directory) for the pseudo-fs. We won't really
	 * use this directory, but the VFS expects a root dentry.
	 */
	root_inode = new_inode(sb);
	if (!root_inode)
		return -ENOMEM;

	root_inode->i_ino  = 1; /* Usually 1 for the root inode */
	root_inode->i_sb   = sb;
	root_inode->i_mode = S_IFDIR | 0755;  /* a directory with 0755 perms */

	/*
	 * Initialize owner. init_user_ns is declared in <linux/user_namespace.h>
	 * If your kernel lacks it, see "If init_user_ns is missing" note below.
	 */
	// inode_init_owner(&init_user_ns, root_inode, NULL, S_IFDIR);

    /* Just set the permissions and ownership directly: */
    root_inode->i_uid = KUIDT_INIT(0);  /* UID 0 (root) */
    root_inode->i_gid = KGIDT_INIT(0);  /* GID 0 (root) */

	root_dentry = d_make_root(root_inode);
	if (!root_dentry) {
		iput(root_inode);
		return -ENOMEM;
	}
	sb->s_root = root_dentry;

	return 0;
}

/* -------------------------------------------------------------------------- */
/* .mount callback that calls mount_nodev() with our fill_super function.     */
/* -------------------------------------------------------------------------- */
static struct dentry *fakefs_mount(struct file_system_type *fs_type,
				   int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, fakefs_fill_super);
}

/* -------------------------------------------------------------------------- */
/* Define the pseudo-fs type                                                 */
/* -------------------------------------------------------------------------- */
static struct file_system_type fakefs_type = {
	.owner   = THIS_MODULE,
	.name    = "fakefs",
	.mount   = fakefs_mount,
	.kill_sb = kill_anon_super,
};

/* -------------------------------------------------------------------------- */
/* Helper: Safely copy a path string from user space.                         */
/* -------------------------------------------------------------------------- */
static char *copy_user_buffer(const char __user *ubuf, size_t count)
{
	char *buf;

	if (count > 4096)
		return ERR_PTR(-EFAULT);

	buf = kmalloc(count + 1, GFP_KERNEL);
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
/* Handler functions for each event type.                                     */
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
/* "unlink" / file deletion logic: create a fake inode on our pseudo-fs.      */
/* -------------------------------------------------------------------------- */
static int handle_deletion(const char *full_path)
{
	int ret;
	char *buf;
	char *parent_path;
	char *filename;
	struct path parent_dir_path;
	struct dentry *parent_dentry;
	struct inode *parent_inode;
	struct inode *fake_inode;
	struct qstr fake_dname;

	buf = kstrdup(full_path, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	filename = strrchr(buf, '/');
	if (!filename) {
		log_warn("Invalid path for unlink: no '/' separator found\n");
		kfree(buf);
		return -EINVAL;
	}

	*filename = '\0';
	filename++;
	parent_path = buf;

	log_info("Unlink requested -> parent path: %s, filename: %s\n",
		 parent_path, filename);

	/* Look up the parent directory path (could be FUSE or anything). */
	ret = kern_path(parent_path, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent_dir_path);
	if (ret) {
		log_warn("kern_path failed for parent directory: %d\n", ret);
		kfree(buf);
		return ret;
	}

	parent_dentry = parent_dir_path.dentry;
	parent_inode  = d_inode(parent_dentry);

	/*
	 * Use our pseudo-fs superblock to allocate an inode, so we never
	 * call fuse_evict_inode().
	 */
	fake_inode = new_inode(fakefs_sb);
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

	iput(fake_inode);  /* release reference */
	path_put(&parent_dir_path);
	kfree(buf);

	return 0;
}

/* -------------------------------------------------------------------------- */
/* Write handler: parse "EVENT_TYPE,PATH", dispatch the event.               */
/* -------------------------------------------------------------------------- */
static ssize_t fake_inotify_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	ssize_t ret = 0;
	char *input = NULL;
	char *event_type = NULL;
	char *path_str = NULL;
	char *comma_pos = NULL;

	mutex_lock(&fake_inotify_mutex);

	input = copy_user_buffer(ubuf, count);
	if (IS_ERR(input)) {
		ret = PTR_ERR(input);
		goto out_unlock;
	}

	/* Expect: EVENT_TYPE,PATH */
	comma_pos = strchr(input, ',');
	if (!comma_pos) {
		log_warn("Bad input format. Expected 'EVENT_TYPE,PATH'\n");
		ret = -EINVAL;
		goto out_free;
	}

	*comma_pos = '\0';
	event_type = input;
	path_str   = comma_pos + 1;

	log_info("Parsed event_type='%s', path='%s'\n", event_type, path_str);

	if (!strcmp(event_type, "MODIFY")) {
		struct path child_path;
		struct dentry *parent_dentry;
		struct inode *parent_inode;
		int kret;

		kret = kern_path(path_str, LOOKUP_FOLLOW, &child_path);
		if (kret) {
			log_warn("kern_path failed in MODIFY event, ret=%d\n", kret);
			ret = kret;
			goto out_free;
		}

		parent_dentry = dget_parent(child_path.dentry);
		parent_inode  = d_inode(parent_dentry);

		log_info("Triggering MODIFY for path: %s\n", path_str);
		handle_modify(parent_inode, child_path.dentry, &child_path);

		dput(parent_dentry);
		path_put(&child_path);
	} else if (!strcmp(event_type, "ATTRIB")) {
		struct path child_path;
		struct dentry *parent_dentry;
		struct inode *parent_inode;
		int kret;

		kret = kern_path(path_str, LOOKUP_FOLLOW, &child_path);
		if (kret) {
			log_warn("kern_path failed in ATTRIB event, ret=%d\n", kret);
			ret = kret;
			goto out_free;
		}

		parent_dentry = dget_parent(child_path.dentry);
		parent_inode  = d_inode(parent_dentry);

		log_info("Triggering ATTRIB for path: %s\n", path_str);
		handle_attrib(parent_inode, child_path.dentry, &child_path);

		dput(parent_dentry);
		path_put(&child_path);
	} else if (!strcmp(event_type, "CREATE")) {
		struct path child_path;
		struct dentry *parent_dentry;
		struct inode *parent_inode;
		int kret;

		kret = kern_path(path_str, LOOKUP_FOLLOW, &child_path);
		if (kret) {
			log_warn("kern_path failed in CREATE event, ret=%d\n", kret);
			ret = kret;
			goto out_free;
		}

		parent_dentry = dget_parent(child_path.dentry);
		parent_inode  = d_inode(parent_dentry);

		log_info("Triggering CREATE for path: %s\n", path_str);
		handle_create(parent_inode, child_path.dentry, &child_path);

		dput(parent_dentry);
		path_put(&child_path);
	} else if (!strcmp(event_type, "UNLINK")) {
		log_info("Triggering UNLINK for path: %s\n", path_str);
		ret = handle_deletion(path_str);
	} else {
		log_warn("Unknown event type '%s'\n", event_type);
		ret = -EINVAL;
		goto out_free;
	}

	if (ret >= 0) {
		/* Consumed all 'count' bytes if we succeeded */
		*ppos += count;
		ret = count;
	}

out_free:
	kfree(input);
out_unlock:
	mutex_unlock(&fake_inotify_mutex);
	return ret;
}

/* -------------------------------------------------------------------------- */
/* Proc ops for /proc/fake_inotify                                           */
/* -------------------------------------------------------------------------- */
static const struct proc_ops fake_inotify_fops = {
	.proc_write = fake_inotify_write,
};

/* -------------------------------------------------------------------------- */
/* Module initialization:                                                     */
/* 1) Create /proc/fake_inotify                                               */
/* 2) Register pseudo-fs & mount in memory (kern_mount)                       */
/* -------------------------------------------------------------------------- */
static int __init fake_inotify_init(void)
{
	int err;

	proc_file = proc_create("fake_inotify", 0666, NULL, &fake_inotify_fops);
	if (!proc_file) {
		pr_err("%s: Failed to create /proc/fake_inotify\n", MODULE_NAME);
		return -ENOMEM;
	}

	err = register_filesystem(&fakefs_type);
	if (err) {
		pr_err("%s: Could not register fakefs.\n", MODULE_NAME);
		goto out_remove_proc;
	}

	fakefs_mnt = kern_mount(&fakefs_type);
	if (IS_ERR(fakefs_mnt)) {
		pr_err("%s: kern_mount() failed.\n", MODULE_NAME);
		err = PTR_ERR(fakefs_mnt);
		goto out_unregister_fs;
	}
	fakefs_sb = fakefs_mnt->mnt_sb;

	pr_info("%s: module loaded. Logging = %s\n",
		MODULE_NAME, enable_logging ? "enabled" : "disabled");
	return 0;

out_unregister_fs:
	unregister_filesystem(&fakefs_type);
out_remove_proc:
	proc_remove(proc_file);
	return err;
}

/* -------------------------------------------------------------------------- */
/* Module cleanup:                                                            */
/* 1) Unmount pseudo-fs (kern_unmount)                                        */
/* 2) Unregister filesystem type                                              */
/* 3) Remove /proc/fake_inotify                                              */
/* -------------------------------------------------------------------------- */
static void __exit fake_inotify_exit(void)
{
	if (!IS_ERR_OR_NULL(fakefs_mnt)) {
		kern_unmount(fakefs_mnt);
		fakefs_mnt = NULL;
		fakefs_sb = NULL;
	}
	unregister_filesystem(&fakefs_type);

	proc_remove(proc_file);
	pr_info("%s: module unloaded\n", MODULE_NAME);
}

module_init(fake_inotify_init);
module_exit(fake_inotify_exit);
