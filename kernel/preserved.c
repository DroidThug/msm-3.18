/*
 * Copyright (C) 2010 The Chromium OS Authors <chromium-os-dev@chromium.org>
 *
 * This file is released under the GPLv2: see the file COPYING for details.
 */

#include <linux/chromeos_platform.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/nvram.h>
#include <linux/preserved.h>
#include <linux/reboot.h>
#include <linux/rtc.h>
#include <linux/smp.h>
#include <linux/sysctl.h>
#include <linux/uaccess.h>

/*
 * Much of the complexity here comes from a particular feature of the ChromeOS
 * boot firmware: although it reserves an area of RAM for our use, and that
 * area has been seen to be preserved across ordinary reboot, that can only
 * be guaranteed if we approach reboot from the S3 suspend-to-RAM state.
 *
 * In /sys/devices/platform/chromeos_acpi/CHNV, the ChromeOS ACPI driver
 * reports an offset in /dev/nvram at which a flag can be set before entering
 * S3: to tell the firmware to reboot instead of resume when awakened.
 *
 * The ifdefs below allow this file to be built without all the dependencies
 * which that feature adds.  And even when it is built in, by default we go
 * to a simple reboot, unless the required nvram offset has been written into
 * /sys/kernel/debug/preserved/chnv here.
 */
#if defined(CONFIG_PROC_SYSCTL)	/* for proc_dointvec_minmax() */	&& \
    defined(CONFIG_NVRAM)	/* for nvram_read/write_byte () */	&& \
    defined(CONFIG_RTC_CLASS)	/* for rtc_read/write_time() */		&& \
    defined(CONFIG_ACPI_SLEEP)	/* for acpi_S3_reboot() */		&& \
    defined(CONFIG_SUSPEND)	/* for acpi_S3_reboot() */

#define CHROMEOS_S3_REBOOT

#define NVRAM_BYTES (128 - NVRAM_FIRST_BYTE)	/* from drivers/char/nvram.c */
#define CHNV_DEBUG_RESET_FLAG	0x40		/* magic flag for S3 reboot */
#define AWAKEN_AFTER_SECONDS	2		/* 1 might fire too early?? */

/*
 * ACPI reports offset in NVRAM of CHromeos NVram byte used to program BIOS:
 * that offset is expected to be 94 (0x5e) when it is supported.  We shall
 * rely upon userspace to pass it here from the chromeos_acpi driver;
 * or leave it at -1, in which case a simple reboot works for now.
 */
static int chromeos_nvram_index = -1;

#else /* !CHROMEOS_S3_REBOOT */

#define chromeos_nvram_index	-1

#endif /* CHROMEOS_S3_REBOOT */

#define CHROMEOS_PRESERVED_RAM_ADDR 0x00f00000	/* 15MB */
#define CHROMEOS_PRESERVED_RAM_SIZE 0x00100000	/*  1MB */
#define CHROMEOS_PRESERVED_BUF_SIZE (CHROMEOS_PRESERVED_RAM_SIZE-4*sizeof(int))

struct preserved {
	unsigned int	magic;
	unsigned int	cursor;
	char		buf[CHROMEOS_PRESERVED_BUF_SIZE];
	unsigned int	ksize;
	unsigned int	usize;		/* up here to verify end of area */
};
static struct preserved *preserved = __va(CHROMEOS_PRESERVED_RAM_ADDR);

/* If a crash occurs very early, just assume that area was reserved */
static bool assume_preserved_area_safe = true;
static DEFINE_MUTEX(preserved_mutex);

/*
 * We avoid writing or reading the preserved area until we have to,
 * so that a kernel with this configured in can be run even on boxes
 * where writing to or reading from that area might cause trouble.
 */
static bool preserved_is_valid(void)
{
	BUILD_BUG_ON(sizeof(*preserved) != CHROMEOS_PRESERVED_RAM_SIZE);

	if (assume_preserved_area_safe &&
	    preserved->magic == DEBUGFS_MAGIC &&
	    preserved->cursor < sizeof(preserved->buf) &&
	    preserved->ksize <= sizeof(preserved->buf) &&
	    preserved->usize <= sizeof(preserved->buf) &&
	    preserved->ksize + preserved->usize >= preserved->cursor &&
	    preserved->ksize + preserved->usize <= sizeof(preserved->buf))
		return true;

	return false;
}

static bool preserved_make_valid(void)
{
	if (!assume_preserved_area_safe)
		return false;

	preserved->magic = DEBUGFS_MAGIC;
	preserved->cursor = 0;
	preserved->ksize = 0;
	preserved->usize = 0;

	return true;
}

/*
 * For runtime: reading and writing /sys/kernel/debug/preserved files.
 */

static ssize_t kcrash_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	unsigned int offset, limit, residue;
	int error = 0;

	mutex_lock(&preserved_mutex);
	if (!preserved_is_valid())
		goto out;
	if (pos < 0 || pos >= preserved->ksize)
		goto out;
	if (count > preserved->ksize - pos)
		count = preserved->ksize - pos;

	offset = preserved->cursor - preserved->ksize;
	if ((int)offset < 0)
		offset += sizeof(preserved->buf);
	offset += pos;
	if (offset > sizeof(preserved->buf))
		offset -= sizeof(preserved->buf);

	limit = sizeof(preserved->buf) - offset;
	residue = count;
	error = -EFAULT;

	if (residue > limit) {
		if (copy_to_user(buf, preserved->buf + offset, limit))
			goto out;
		offset = 0;
		residue -= limit;
		buf += limit;
	}

	if (copy_to_user(buf, preserved->buf + offset, residue))
		goto out;

	pos += count;
	*ppos = pos;
	error = count;
out:
	mutex_unlock(&preserved_mutex);
	return error;
}

static ssize_t kcrash_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	/*
	 * A write to kcrash does nothing but reset both kcrash and utrace.
	 */
	mutex_lock(&preserved_mutex);
	if (preserved_is_valid()) {
		preserved->cursor = 0;
		preserved->ksize = 0;
		preserved->usize = 0;
	}
	mutex_unlock(&preserved_mutex);
	return count;
}

static const struct file_operations kcrash_operations = {
	.read	= kcrash_read,
	.write	= kcrash_write,
};

static ssize_t utrace_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	unsigned int offset, limit, residue;
	unsigned int supersize, origin;
	int error = 0;

	/*
	 * Try to handle the case when utrace entries are being added
	 * in between our sequential reads; but if they're being added
	 * faster than we're reading them, this won't work very well.
	 */
	mutex_lock(&preserved_mutex);
	if (!preserved_is_valid())
		goto out;
	supersize = preserved->usize;

	if (pos == 0 || preserved->ksize != 0) {
		origin = 0;
		if (supersize == sizeof(preserved->buf) - preserved->ksize)
			origin = preserved->cursor;
		file->private_data = (void *)origin;
	} else {	/* cursor may have moved since we started reading */
		origin = (unsigned int)file->private_data;
		if (supersize == sizeof(preserved->buf)) {
			int advance = preserved->cursor - origin;
			if (advance < 0)
				advance += sizeof(preserved->buf);
			supersize += advance;
		}
	}

	if (pos < 0 || pos >= supersize)
		goto out;
	if (count > supersize - pos)
		count = supersize - pos;

	offset = origin + pos;
	if (offset > sizeof(preserved->buf))
		offset -= sizeof(preserved->buf);
	limit = sizeof(preserved->buf) - offset;
	residue = count;
	error = -EFAULT;

	if (residue > limit) {
		if (copy_to_user(buf, preserved->buf + offset, limit))
			goto out;
		offset = 0;
		residue -= limit;
		buf += limit;
	}

	if (copy_to_user(buf, preserved->buf + offset, residue))
		goto out;

	pos += count;
	*ppos = pos;
	error = count;
out:
	mutex_unlock(&preserved_mutex);
	return error;
}

static ssize_t utrace_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	unsigned int offset, limit, residue;
	unsigned int usize = 0;
	int error;

	/*
	 * Originally, writing to the preserved area was implemented
	 * just for testing that it is all preserved.  But it might be
	 * useful for debugging a kernel crash if we allow userspace
	 * to write trace records to that area as a circular buffer.
	 * But don't allow any utrace writes once a kcrash is present.
	 */
	mutex_lock(&preserved_mutex);
	if (!preserved_is_valid() && !preserved_make_valid()) {
		error = -ENXIO;
		goto out;
	}
	if (preserved->ksize != 0) {
		error = -ENOSPC;
		goto out;
	}

	if (count > sizeof(preserved->buf)) {
		buf += count - sizeof(preserved->buf);
		count = sizeof(preserved->buf);
	}

	offset = preserved->cursor;
	limit = sizeof(preserved->buf) - offset;
	residue = count;
	error = -EFAULT;

	if (residue > limit) {
		if (copy_from_user(preserved->buf + offset, buf, limit))
			goto out;
		usize = sizeof(preserved->buf);
		offset = 0;
		residue -= limit;
		buf += limit;
	}

	if (copy_from_user(preserved->buf + offset, buf, residue))
		goto out;

	offset += residue;
	if (usize < offset)
		usize = offset;
	if (preserved->usize < usize)
		preserved->usize = usize;
	if (offset == sizeof(preserved->buf))
		offset = 0;
	preserved->cursor = offset;

	/*
	 * We always append, ignoring ppos: don't even pretend to maintain it.
	 */
	error = count;
out:
	mutex_unlock(&preserved_mutex);
	return error;
}

static const struct file_operations utrace_operations = {
	.read	= utrace_read,
	.write	= utrace_write,
};

#ifdef CHROMEOS_S3_REBOOT
/*
 * chnv read and write chromeos_nvram_index like a /proc/sys sysctl value
 * (debugfs builtins are designed for unsigned values without rangechecking).
 */
static int minus_one = -1;
static int nvram_max = NVRAM_BYTES - 1;
static struct ctl_table chnv_ctl = {
	.procname	= "chnv",
	.data		= &chromeos_nvram_index,
	.maxlen		= sizeof(int),
	.mode		= 0644,
	.proc_handler	= &proc_dointvec_minmax,
	.extra1		= &minus_one,
	.extra2		= &nvram_max,
};

static ssize_t chnv_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	return proc_dointvec_minmax(&chnv_ctl, 0,
		(void __user *)buf, &count, ppos) ? : count;
}

static ssize_t chnv_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	return proc_dointvec_minmax(&chnv_ctl, 1,
		(void __user *)buf, &count, ppos) ? : count;
}

static const struct file_operations chnv_operations = {
	.read	= chnv_read,
	.write	= chnv_write,
};

/*
 * For emergency_restart: at the time of a bug, oops or panic.
 */

static int rtc_may_wakeup(struct device *dev, void *data)
{
	struct rtc_device *rtc = to_rtc_device(dev);
	return rtc->ops->set_alarm && device_may_wakeup(rtc->dev.parent);
}

static int set_rtc_alarm(int seconds)
{
	struct device *dev;
	struct rtc_device *rtc;
	struct rtc_wkalrm alarm;
	unsigned long now;
	int error;

	dev = class_find_device(rtc_class, NULL, NULL, rtc_may_wakeup);
	if (!dev)
		return -ENODEV;

	rtc = to_rtc_device(dev);
	error = rtc_read_time(rtc, &alarm.time);
	if (error)
		return error;

	rtc_tm_to_time(&alarm.time, &now);
	rtc_time_to_tm(now + seconds, &alarm.time);
	alarm.enabled = 1;

	return rtc_set_alarm(rtc, &alarm);
}

static void chromeos_S3_reboot(void)
{
	unsigned char chromeos_nvram_flags;
	int error;

	/*
	 * Overly paranoid, but just reboot if chnv has been corrupted.
	 */
	if (chromeos_nvram_index < 0 ||
	    chromeos_nvram_index >= NVRAM_BYTES) {
		printk(KERN_ERR "S3 reboot: chromeos_nvram_index=%d\n",
					    chromeos_nvram_index);
		return;
	}

	/*
	 * Tell the ChromeOS BIOS to use S3 to preserve RAM,
	 * but then to reboot instead of resuming.
	 */
	chromeos_nvram_flags = nvram_read_byte(chromeos_nvram_index);
	if (chromeos_nvram_flags & CHNV_DEBUG_RESET_FLAG) {
		printk(KERN_ERR "S3 reboot: chromeos_nvram_flags=0x%08x\n",
					    chromeos_nvram_flags);
		return;
	}
	chromeos_nvram_flags |= CHNV_DEBUG_RESET_FLAG;
	nvram_write_byte(chromeos_nvram_flags, chromeos_nvram_index);

	/*
	 * Must set an alarm to awaken from S3 to reboot.
	 */
	error = set_rtc_alarm(AWAKEN_AFTER_SECONDS);
	if (error) {
		printk(KERN_ERR "S3 reboot: set_rtc_alarm()=%d\n", error);
		return;
	}

	acpi_S3_reboot();
}
#else /* !CHROMEOS_S3_REBOOT */

static inline void chromeos_S3_reboot(void)
{
}
#endif /* CHROMEOS_S3_REBOOT */

static void kcrash_append(unsigned int log_size)
{
	int excess = preserved->usize + preserved->ksize +
			log_size - sizeof(preserved->buf);

	if (excess <= 0) {
		/* kcrash fits without losing any utrace */
		preserved->ksize += log_size;
	} else if (excess <= preserved->usize) {
		/* some of utrace was overwritten by kcrash */
		preserved->usize -= excess;
		preserved->ksize += log_size;
	} else {
		/* no utrace left and kcrash is full */
		preserved->usize = 0;
		preserved->ksize = sizeof(preserved->buf);
	}

	preserved->cursor += log_size;
	if (preserved->cursor >= sizeof(preserved->buf))
		preserved->cursor -= sizeof(preserved->buf);
}

static void kcrash_preserve(bool first_time)
{
	static unsigned int save_cursor;
	static unsigned int save_ksize;
	static unsigned int save_usize;

	if (first_time) {
		save_cursor = preserved->cursor;
		save_ksize  = preserved->ksize;
		save_usize  = preserved->usize;
	} else {
		/*
		 * Restore original cursor etc. so that we can take a fresh
		 * snapshot of the log_buf, including our own error messages,
		 * if something goes wrong in emergency_restart().  This does
		 * assume, reasonably, that log_size will not shrink.
		 */
		preserved->cursor = save_cursor;
		preserved->ksize  = save_ksize;
		preserved->usize  = save_usize;
	}

	kcrash_append(copy_log_buf(preserved->buf,
			sizeof(preserved->buf), preserved->cursor));
}

/*
 * HACK ALERT:
 * We are currently relying on undefined behavior of how reboot works in order
 * to preserve a crash in RAM. On a panic (see panic.c) we use
 * smp_call_function_single to trap to CPU 0 and reboot from there. Otherwise,
 * the crash does not appear to be preserved. This is a short-term hack fix.
 * Long term, we plan on using crash_kexec.
 */
void preserved_ram_panic_handler(void *info)
{
	static char buf[1024];

	/*
	 * Note smp_send_stop is the usual smp shutdown function, which
	 * unfortunately means it may not be hardened to work in a panic
	 * situation.
	 */
	smp_send_stop();

	atomic_notifier_call_chain(&panic_notifier_list, 0, buf);

	bust_spinlocks(0);

	/*
	 * Initialize a good header if that's not already been done.
	 */
	if (preserved_is_valid() || preserved_make_valid()) {
		printk(KERN_INFO "Preserving kcrash across %sreboot\n",
			(chromeos_nvram_index == -1) ? "" : "S3 ");

		/*
		 * Copy printk's log_buf (kmsg or dmesg) into our preserved buf,
		 * perhaps appending to a kcrash from the previous boot.
		 */
		kcrash_preserve(true);

		if (chromeos_nvram_index != -1) {
			chromeos_S3_reboot();
			/*
			 * It's an error if we reach here, so rewrite the log.
			 */
			kcrash_preserve(false);
		}
	}
	machine_emergency_restart();
}

/*
 * Initialization: when booting, we first assume that it will be safe to
 * write panics into the preserved area.  But as soon as we can, check that
 * it is indeed reserved.  Then once debugfs, chromeos_acpi and chromeos
 * drivers are ready, give the user interface to it - though it should be
 * safe to let a crashing kernel write there, we cannot allow utrace_write
 * without being sure that it is on a ChromeOS platform.  If S3 reboot is to
 * be used, userspace can enable that later by giving chnv the right value.
 */

static int __init preserved_early_init(void)
{
	unsigned int pfn = CHROMEOS_PRESERVED_RAM_ADDR >> PAGE_SHIFT;
	unsigned int efn = pfn + (CHROMEOS_PRESERVED_RAM_SIZE >> PAGE_SHIFT);

	while (pfn < efn) {
		if (!pfn_valid(pfn))
			break;
		if (!PageReserved(pfn_to_page(pfn)))
			break;
		pfn++;
	}

	assume_preserved_area_safe = (pfn >= efn);
	return 0;
}
early_initcall(preserved_early_init);

static int __init preserved_init(void)
{
	struct dentry *dir;

	/*
	 * Check that the RAM we expect to use has indeed been reserved
	 * for us: this kernel might be running on a machine without it.
	 * But to be even safer, we don't access that memory until asked,
	 * and don't give a user interface to it without ChromeOS firmware.
	 */
	if (assume_preserved_area_safe && chromeos_initialized()) {
		/*
		 * If error occurs in setting up /sys/kernel/debug/preserved/,
		 * we cannot do better than ignore it.
		 */
		dir = debugfs_create_dir("preserved", NULL);
		if (dir && !IS_ERR(dir)) {
#ifdef CHROMEOS_S3_REBOOT
			debugfs_create_file("chnv", S_IFREG|S_IRUGO|S_IWUSR,
						dir, NULL, &chnv_operations);
#endif
			debugfs_create_file("kcrash", S_IFREG|S_IRUSR|S_IWUSR,
						dir, NULL, &kcrash_operations);
			debugfs_create_file("utrace", S_IFREG|S_IRUSR|S_IWUGO,
						dir, NULL, &utrace_operations);
		}
	}

	return 0;
}
device_initcall(preserved_init);
