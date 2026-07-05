#include <linux/anon_inodes.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/susfs.h>
#include <linux/version.h>
#include <linux/utsname.h> // utsname() and uts_sem

#include "uapi/supercall.h"
#include "supercall/internal.h"
#include "arch.h"
#include "klog.h" // IWYU pragma: keep
#include "manager/manager_identity.h"

#include "tiny_sulog.h"

uint32_t ksuver_override = 0;
static char toolkit_orig_release[65] = {0};
static char toolkit_orig_version[65] = {0};

void ksu_toolkit_uname_reset(void)
{
	if (toolkit_orig_release[0] != '\0') {
		struct new_utsname *u = utsname();

		pr_info("ksu: resetting toolkit uname memory to stock\n");
		down_write(&uts_sem);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
		strscpy(u->release, toolkit_orig_release, sizeof(u->release));
		strscpy(u->version, toolkit_orig_version, sizeof(u->version));
#else
		strlcpy(u->release, toolkit_orig_release, sizeof(u->release));
		strlcpy(u->version, toolkit_orig_version, sizeof(u->version));
#endif
		up_write(&uts_sem);
	}
}

static int ksu_handle_toolkit_reboot(int magic2, unsigned int cmd, void __user *arg)
{
	u64 reply = (u64)arg;

	if (magic2 == CHANGE_MANAGER_UID) {
		if (current_uid().val != 0)
			return -EPERM;

		pr_info("sys_reboot: ksu_set_manager_appid to: %d\n", cmd);
		ksu_set_manager_appid(cmd);

		if (cmd == ksu_get_manager_appid()) {
			if (copy_to_user(arg, &reply, sizeof(reply)))
				return -EFAULT;
		}
		return 0;
	}

	if (magic2 == GET_SULOG_DUMP_V2) {
		int ret;

		if (current_uid().val != 0)
			return -EPERM;

		ret = send_sulog_dump(arg);
		if (ret)
			return ret;

		if (copy_to_user(arg, &reply, sizeof(reply)))
			return -EFAULT;
		return 0;
	}

	if (magic2 == CHANGE_KSUVER) {
		if (current_uid().val != 0)
			return -EPERM;

		pr_info("sys_reboot: ksu_change_ksuver to: %d\n", cmd);
		ksuver_override = cmd;

		if (copy_to_user(arg, &reply, sizeof(reply)))
			return -EFAULT;
		return 0;
	}

	if (magic2 == CHANGE_SPOOF_UNAME) {
		char release_buf[65];
		char version_buf[65];
		void __user **ppptr = (void __user **)arg;
		u64 u_pptr = 0;
		u64 u_ptr = 0;

		if (current_uid().val != 0)
			return -EPERM;

		if (copy_from_user(&u_pptr, ppptr, sizeof(u_pptr)))
			return -EFAULT;

		if (copy_from_user(&u_ptr, (void __user *)(uintptr_t)u_pptr, sizeof(u_ptr)))
			return -EFAULT;

		if (strncpy_from_user(release_buf, (char __user *)(uintptr_t)u_ptr,
					      sizeof(release_buf)) < 0)
			return -EFAULT;
		release_buf[sizeof(release_buf) - 1] = '\0';

		if (strncpy_from_user(version_buf,
				      (char __user *)(uintptr_t)(u_ptr + strlen(release_buf) + 1),
				      sizeof(version_buf)) < 0)
			return -EFAULT;
		version_buf[sizeof(version_buf) - 1] = '\0';

		if (toolkit_orig_release[0] == '\0') {
			struct new_utsname *u_curr = utsname();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
			strscpy(toolkit_orig_release, u_curr->release,
				sizeof(toolkit_orig_release));
			strscpy(toolkit_orig_version, u_curr->version,
				sizeof(toolkit_orig_version));
#else
			strlcpy(toolkit_orig_release, u_curr->release,
				sizeof(toolkit_orig_release));
			strlcpy(toolkit_orig_version, u_curr->version,
				sizeof(toolkit_orig_version));
#endif
		}

		if (!strcmp(release_buf, "default") || !strcmp(version_buf, "default")) {
			memcpy(release_buf, toolkit_orig_release, sizeof(release_buf));
			memcpy(version_buf, toolkit_orig_version, sizeof(version_buf));
		}

		{
			struct new_utsname *u = utsname();

			down_write(&uts_sem);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
			strscpy(u->release, release_buf, sizeof(u->release));
			strscpy(u->version, version_buf, sizeof(u->version));
#else
			strlcpy(u->release, release_buf, sizeof(u->release));
			strlcpy(u->version, version_buf, sizeof(u->version));
#endif
			up_write(&uts_sem);
		}

		if (copy_to_user(arg, &reply, sizeof(reply)))
			return -EFAULT;
		return 0;
	}

	return -EINVAL;
}

static int anon_ksu_release(struct inode *inode, struct file *filp)
{
	pr_info("ksu fd released\n");
	return 0;
}

static long anon_ksu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    return ksu_supercall_handle_ioctl(cmd, (void __user *)arg);
}

static const struct file_operations anon_ksu_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = anon_ksu_ioctl,
	.compat_ioctl = anon_ksu_ioctl,
	.release = anon_ksu_release,
};

int ksu_install_fd(void)
{
	struct file *filp;
	int fd;

	// Get unused fd
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		pr_err("ksu_install_fd: failed to get unused fd\n");
		return fd;
	}

	// Create anonymous inode file
	filp = anon_inode_getfile("[ksu_driver]", &anon_ksu_fops, NULL, O_RDWR | O_CLOEXEC);
	if (IS_ERR(filp)) {
		pr_err("ksu_install_fd: failed to create anon inode file\n");
		put_unused_fd(fd);
		return PTR_ERR(filp);
	}

	// Install fd
	fd_install(fd, filp);

	pr_info("ksu fd installed: %d for pid %d\n", fd, current->pid);

	return fd;
}

int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd,
			  void __user **arg)
{
	if (magic1 != KSU_INSTALL_MAGIC1)
		return 0;

#ifdef CONFIG_KSU_DEBUG
	pr_info("sys_reboot: intercepted call! magic: 0x%x id: %d\n", magic1,
		magic2);
#endif

	// Check if this is a request to install KSU fd
	if (magic2 == KSU_INSTALL_MAGIC2) {
		int fd = ksu_install_fd();
		// downstream: dereference all arg usage!
		if (copy_to_user((void __user *)*arg, &fd, sizeof(fd))) {
			pr_err("install ksu fd reply err\n");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		close_fd(fd);
#else
		__close_fd(current->files, fd);
#endif
		}
		return 0;
	}

#ifdef CONFIG_KSU_SUSFS
	if (magic2 == SUSFS_MAGIC && current_uid().val == 0) {
		switch (cmd) {
		case CMD_SUSFS_ADD_SUS_PATH:
			susfs_add_sus_path(arg);
			return 0;
		case CMD_SUSFS_ADD_SUS_PATH_LOOP:
			susfs_add_sus_path_loop(arg);
			return 0;
		case CMD_SUSFS_HIDE_SUS_MNTS_FOR_NON_SU_PROCS:
			susfs_set_hide_sus_mnts_for_non_su_procs(arg);
			return 0;
		case CMD_SUSFS_ADD_SUS_KSTAT:
		case CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY:
			susfs_add_sus_kstat(arg);
			return 0;
		case CMD_SUSFS_UPDATE_SUS_KSTAT:
			susfs_update_sus_kstat(arg);
			return 0;
		case CMD_SUSFS_SET_UNAME: {
			struct st_susfs_uname info = {0};
			if (!copy_from_user(&info, (struct st_susfs_uname __user *)*arg,
					    sizeof(info))) {
				if (strcmp(info.release, "default") ||
				    strcmp(info.version, "default")) {
					ksu_toolkit_uname_reset();
				}
			}
			susfs_set_uname(arg);
			return 0;
		}
		case CMD_SUSFS_ENABLE_LOG:
			susfs_enable_log(arg);
			return 0;
		case CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG:
			susfs_set_cmdline_or_bootconfig(arg);
			return 0;
		case CMD_SUSFS_ADD_OPEN_REDIRECT:
			susfs_add_open_redirect(arg);
			return 0;
		case CMD_SUSFS_ADD_SUS_MAP:
			susfs_add_sus_map(arg);
			return 0;
		case CMD_SUSFS_ENABLE_AVC_LOG_SPOOFING: {
			struct st_susfs_avc_log_spoofing info = {0};
			if (!copy_from_user(&info,
					    (struct st_susfs_avc_log_spoofing __user *)*arg,
					    sizeof(info))) {
				extern bool ksu_avc_spoof_enabled;
				extern void ksu_avc_spoof_disable(void);

				if (info.enabled && ksu_avc_spoof_enabled)
					ksu_avc_spoof_disable();
			}
			susfs_set_avc_log_spoofing(arg);
			return 0;
		}
		case CMD_SUSFS_SHOW_ENABLED_FEATURES:
			susfs_get_enabled_features(arg);
			return 0;
		case CMD_SUSFS_SHOW_VARIANT:
			susfs_show_variant(arg);
			return 0;
		case CMD_SUSFS_SHOW_VERSION:
			susfs_show_version(arg);
			return 0;
		default:
			return 0;
		}
	}
#endif

	return ksu_handle_toolkit_reboot(magic2, cmd, *arg);
}

#ifdef KSU_KPROBES_HOOK
static int reboot_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = PT_REAL_REGS(regs);
	int magic1 = (int)PT_REGS_PARM1(real_regs);
	int magic2 = (int)PT_REGS_PARM2(real_regs);
	unsigned int cmd = (unsigned int)PT_REGS_PARM3(real_regs);
	unsigned long arg4 = (unsigned long)PT_REGS_SYSCALL_PARM4(real_regs);
	unsigned long reply = (unsigned long)arg4;

	return ksu_handle_sys_reboot(magic1, magic2, cmd, (void __user **)&arg4);
}

static struct kprobe reboot_kp = {
	.symbol_name = REBOOT_SYMBOL,
	.pre_handler = reboot_handler_pre,
};
#endif

void __init ksu_supercalls_init(void)
{
	int i;

	ksu_supercall_dump_commands();

#ifdef KSU_KPROBES_HOOK
	int rc = register_kprobe(&reboot_kp);
	if (rc) {
		pr_err("reboot kprobe failed: %d\n", rc);
	} else {
		pr_info("reboot kprobe registered successfully\n");
	}
#endif

	sulog_init_heap(); // grab heap memory
}

void __exit ksu_supercalls_exit(void){
	struct mount_entry *entry, *tmp;

#ifdef KSU_KPROBES_HOOK
	unregister_kprobe(&reboot_kp);
#endif

	ksu_supercall_cleanup_state();
}
