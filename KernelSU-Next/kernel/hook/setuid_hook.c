#include <linux/compiler.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <linux/sched/signal.h>
#endif
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/thread_info.h>
#include <linux/seccomp.h>
#include <linux/bpf.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/version.h>
#include <linux/susfs_def.h>

#include "policy/allowlist.h"
#include "setuid_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "manager/manager_identity.h"
#include "selinux/selinux.h"
#include "infra/seccomp_cache.h"
#include "supercall/supercall.h"
#include "hook_manager.h"
#include "feature/kernel_umount.h"
#include "compat/kernel_compat.h"

extern void disable_seccomp(void);

static void ksu_install_manager_fd_tw_func(struct callback_head *cb)
{
    ksu_install_fd();
    kfree(cb);
}

int ksu_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    // we rely on the fact that zygote always call setresuid(3) with same uids
    uid_t new_uid = ruid;

    pr_debug("handle_setresuid target uid %d\n", new_uid);

#ifdef CONFIG_KSU_SUSFS
    if (!susfs_is_sid_equal(current_cred(), susfs_zygote_sid))
        return 0;
#endif

    if (is_isolated_process(new_uid))
        goto do_umount;

    if (unlikely(is_uid_manager(new_uid))) {
        disable_seccomp();

#ifdef KSU_KPROBES_HOOK
        ksu_set_task_tracepoint_flag(current);
#endif

        pr_info("install fd for manager: %d\n", new_uid);
        struct callback_head *cb = kzalloc(sizeof(*cb), GFP_ATOMIC);
        if (!cb)
            return 0;
        cb->func = ksu_install_manager_fd_tw_func;
        if (task_work_add(current, cb, TWA_RESUME)) {
            kfree(cb);
            pr_warn("install manager fd add task_work failed\n");
        }
        return 0;
    }

    if (unlikely(new_uid == WEBVIEW_ZYGOTE_UID))
        return 0;

    if (likely(is_appuid(new_uid) && ksu_uid_should_umount(new_uid))) {
        goto do_umount;
    }

    if (ksu_is_allow_uid_for_current(new_uid)) {
        disable_seccomp();

#ifdef KSU_KPROBES_HOOK
        ksu_set_task_tracepoint_flag(current);
#endif
    } else {
#ifdef KSU_KPROBES_HOOK
        ksu_clear_task_tracepoint_flag_if_needed(current);
#endif
        return 0;
    }

do_umount:
    ksu_handle_umount(current_uid().val, new_uid);

#ifdef CONFIG_KSU_SUSFS
    susfs_set_current_proc_umounted();
#endif

    return 0;
}

extern void ksu_lsm_hook_init(void);
void __init ksu_setuid_hook_init(void)
{
	ksu_kernel_umount_init();
}

void __exit ksu_setuid_hook_exit(void)
{
	pr_info("ksu_core_exit\n");
	ksu_kernel_umount_exit();
}
