/*
 * hmos-preinit: first process when booting the HarmonyOS 5.x userspace on a
 * phone that is not the Huawei original (sweet: Snapdragon, star2lte: Exynos).
 *
 *  1. mounts proc/sysfs/devtmpfs temporarily
 *  2. two ways to find the HarmonyOS partitions:
 *     - hmos.imgpart=<PARTNAME> on the kernel command line ("loop" layout, no
 *       super partition, e.g. Galaxy S9+): mounts that ext4 partition read-only,
 *       attaches <hmos.imgdir>/<name>.img as a loop device for every HarmonyOS
 *       partition, detaches the mount (the loop devices keep the files open)
 *       and writes /etc/fstab.required with hmos.imgfs (default ext4)
 *     - otherwise (sweet): finds the Android "super" partition, parses its LP
 *       metadata and creates one dm-linear device per logical partition
 *  3. (super layout) the devices become /dev/block/dm-N in the order listed in
 *     the metadata; the mapping is written to kmsg and to /hmos_dm.txt so
 *     fstab.required can be checked against it (loop layout: /hmos_dm.txt
 *     lists name -> loopN)
 *  4. forks a logger that copies /dev/kmsg to <cache>/hmos/kmsg-N.log so
 *     the boot log survives a crash/reboot (read it back from recovery)
 *  5. unmounts everything and execs the real HarmonyOS /bin/init_early as PID 1
 *
 * License: GPL-2.0
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/reboot.h>
#include <linux/input.h>
#include <sys/reboot.h>

#define REAL_INIT "/bin/init_early"
#define LP_RESERVED 4096
#define LP_GEOMETRY_MAGIC 0x616c4467u
#define LP_HEADER_MAGIC 0x414C5030u
#define LP_ATTR_READONLY 1u
#define MAX_PARTS 32

static int kfd = -1;
static FILE *mapf;
static FILE *fstab;

/* logical partition -> HarmonyOS mount point (first-stage, read-only) */
static const struct { const char *name, *mnt, *flags; } hm_mounts[] = {
	{ "system",    "/usr",       "wait,required" },
	{ "vendor",    "/vendor",    "wait,required" },
	{ "chip_prod", "/chip_prod", "wait,required,nofail" },
	{ "sys_prod",  "/sys_prod",  "wait,required,nofail" },
	{ "cust",      "/cust",      "wait,required,nofail" },
	{ "patch",     "/patch_hw",  "wait,required,nofail" },
	{ "version",   "/version",   "wait,required,nofail" },
	{ "preload",   "/preload",   "wait,required,nofail" },
};

static char img_fs[16] = "erofs";	/* filesystem of the partition images (hmos.imgfs) */
static char log_part[64] = "cache";	/* partition for the persistent log (hmos.logpart) */

static void fstab_add_dev(const char *name, const char *dev)
{
	for (size_t i = 0; i < sizeof(hm_mounts) / sizeof(hm_mounts[0]); i++)
		if (!strcmp(name, hm_mounts[i].name) && fstab)
			fprintf(fstab, "%s %s %s ro %s\n", dev, hm_mounts[i].mnt, img_fs,
				hm_mounts[i].flags);
}

static void fstab_add(const char *name, unsigned mn)
{
	char dev[48];
	snprintf(dev, sizeof(dev), "/dev/block/dm-%u", mn);
	fstab_add_dev(name, dev);
}

static void klog(const char *fmt, ...)
{
	char buf[512];
	int n = snprintf(buf, sizeof(buf), "<3>hmos-preinit: ");
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	if (kfd >= 0)
		write(kfd, buf, n);
}

#pragma pack(push, 1)
struct lp_geometry {
	uint32_t magic, struct_size;
	uint8_t checksum[32];
	uint32_t metadata_max_size, metadata_slot_count, logical_block_size;
};
struct lp_table_desc { uint32_t offset, num_entries, entry_size; };
struct lp_header {
	uint32_t magic;
	uint16_t major, minor;
	uint32_t header_size;
	uint8_t header_checksum[32];
	uint32_t tables_size;
	uint8_t tables_checksum[32];
	struct lp_table_desc partitions, extents, groups, block_devices;
};
struct lp_partition {
	char name[36];
	uint32_t attributes, first_extent_index, num_extents, group_index;
};
struct lp_extent {
	uint64_t num_sectors;
	uint32_t target_type;
	uint64_t target_data;
	uint32_t target_source;
};
#pragma pack(pop)

static int find_super(char *devpath, size_t len)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;
	char p[300], buf[1024];
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "/sys/class/block/%s/uevent", e->d_name);
		int fd = open(p, O_RDONLY);
		if (fd < 0)
			continue;
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = 0;
		if (strstr(buf, "PARTNAME=super\n")) {
			snprintf(devpath, len, "/dev/%s", e->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

static int dm_ctl = -1;

static void dm_init(struct dm_ioctl *io, size_t size, const char *name)
{
	memset(io, 0, size);
	io->version[0] = DM_VERSION_MAJOR;
	io->version[1] = 0;
	io->version[2] = 0;
	io->data_size = size;
	io->data_start = sizeof(*io);
	strncpy(io->name, name, sizeof(io->name) - 1);
}

static int dm_create_linear(const char *name, int ro, const char *super_mm,
			    struct lp_extent *ext, uint32_t n_ext, unsigned *minor_out)
{
	char buf[16384];
	struct dm_ioctl *io = (struct dm_ioctl *)buf;
	uint64_t start = 0;

	dm_init(io, sizeof(buf), name);
	if (ioctl(dm_ctl, DM_DEV_CREATE, io)) {
		klog("DM_DEV_CREATE %s: %s", name, strerror(errno));
		return -1;
	}
	*minor_out = minor(io->dev);

	dm_init(io, sizeof(buf), name);
	if (ro)
		io->flags |= DM_READONLY_FLAG;
	char *p = buf + sizeof(*io);
	char *end = buf + sizeof(buf);
	for (uint32_t i = 0; i < n_ext; i++) {
		struct dm_target_spec *ts = (struct dm_target_spec *)p;
		char *params = p + sizeof(*ts);
		memset(ts, 0, sizeof(*ts));
		ts->sector_start = start;
		ts->length = ext[i].num_sectors;
		if (ext[i].target_type == 0) {
			strcpy(ts->target_type, "linear");
			snprintf(params, end - params, "%s %llu", super_mm,
				 (unsigned long long)ext[i].target_data);
		} else {
			strcpy(ts->target_type, "zero");
			params[0] = 0;
		}
		size_t l = sizeof(*ts) + strlen(params) + 1;
		l = (l + 7) & ~(size_t)7;
		ts->next = l;
		p += l;
		start += ext[i].num_sectors;
		io->target_count++;
	}
	if (ioctl(dm_ctl, DM_TABLE_LOAD, io)) {
		klog("DM_TABLE_LOAD %s: %s", name, strerror(errno));
		return -1;
	}
	dm_init(io, sizeof(buf), name);
	if (ioctl(dm_ctl, DM_DEV_SUSPEND, io)) { /* resume */
		klog("DM resume %s: %s", name, strerror(errno));
		return -1;
	}
	return 0;
}

static int map_super(void)
{
	char super[64];
	struct stat st;
	if (find_super(super, sizeof(super))) {
		klog("super partition not found");
		return -1;
	}
	int fd = open(super, O_RDONLY);
	if (fd < 0 || fstat(fd, &st)) {
		klog("open %s: %s", super, strerror(errno));
		return -1;
	}
	char super_mm[32];
	snprintf(super_mm, sizeof(super_mm), "%u:%u", major(st.st_rdev), minor(st.st_rdev));

	struct lp_geometry g;
	if (pread(fd, &g, sizeof(g), LP_RESERVED) != sizeof(g) || g.magic != LP_GEOMETRY_MAGIC) {
		klog("bad LP geometry on %s", super);
		return -1;
	}
	/* primary metadata slot 0: reserved + geometry + backup geometry */
	off_t moff = LP_RESERVED + 2 * 4096;
	char *meta = malloc(g.metadata_max_size);
	if (!meta || pread(fd, meta, g.metadata_max_size, moff) != (ssize_t)g.metadata_max_size) {
		klog("cannot read LP metadata");
		return -1;
	}
	struct lp_header *h = (struct lp_header *)meta;
	if (h->magic != LP_HEADER_MAGIC) {
		klog("bad LP header magic");
		return -1;
	}
	char *tables = meta + h->header_size;
	klog("super=%s (%s) LP v%u.%u, %u partitions", super, super_mm, h->major,
	     h->minor, h->partitions.num_entries);

	dm_ctl = open("/dev/mapper/control", O_RDWR);
	if (dm_ctl < 0) {
		mknod("/dev/device-mapper", S_IFCHR | 0600, makedev(10, 236));
		dm_ctl = open("/dev/device-mapper", O_RDWR);
	}
	if (dm_ctl < 0) {
		klog("no device-mapper control: %s", strerror(errno));
		return -1;
	}
	mapf = fopen("/hmos_dm.txt", "w");
	mkdir("/etc", 0755);
	fstab = fopen("/etc/fstab.required", "w");
	if (fstab)
		fprintf(fstab, "# generated by hmos-preinit from the super LP metadata\n");
	for (uint32_t i = 0; i < h->partitions.num_entries; i++) {
		struct lp_partition *pt = (struct lp_partition *)(tables + h->partitions.offset +
								 i * h->partitions.entry_size);
		char name[37];
		memcpy(name, pt->name, 36);
		name[36] = 0;
		if (!pt->num_extents)
			continue;
		struct lp_extent ext[64];
		uint32_t n = pt->num_extents > 64 ? 64 : pt->num_extents;
		for (uint32_t k = 0; k < n; k++)
			memcpy(&ext[k], tables + h->extents.offset +
					    (pt->first_extent_index + k) * h->extents.entry_size,
			       sizeof(struct lp_extent));
		unsigned mn = 0;
		if (!dm_create_linear(name, pt->attributes & LP_ATTR_READONLY, super_mm, ext, n, &mn)) {
			klog("mapped %s -> dm-%u (%u extents)", name, mn, n);
			if (mapf)
				fprintf(mapf, "%s dm-%u\n", name, mn);
			fstab_add(name, mn);
		}
	}
	if (mapf)
		fclose(mapf);
	if (fstab)
		fclose(fstab);
	free(meta);
	close(fd);
	return 0;
}

/* ---- persistent kmsg logger on the cache partition ---- */
/* PARTNAME is compared case-insensitively: Samsung names are upper case (CACHE, SYSTEM), Qualcomm ones lower case */
static int find_part(const char *name, char *devpath, size_t len)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;
	char p[300], buf[1024], want[96];
	snprintf(want, sizeof(want), "PARTNAME=%s\n", name);
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		snprintf(p, sizeof(p), "/sys/class/block/%s/uevent", e->d_name);
		int fd = open(p, O_RDONLY);
		if (fd < 0)
			continue;
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = 0;
			if (strcasestr(buf, want)) {
				snprintf(devpath, len, "/dev/%s", e->d_name);
				closedir(d);
				return 0;
			}
		}
	}
	closedir(d);
	return -1;
}

/* value of "key=value" on the kernel command line, copied into out (NUL-terminated); 0 if missing */
static int cmdline_get(const char *cmdline, const char *key, char *out, size_t len)
{
	const char *p = cmdline;
	size_t kl = strlen(key);
	while ((p = strstr(p, key))) {
		if ((p == cmdline || p[-1] == ' ') && p[kl] == '=') {
			p += kl + 1;
			size_t n = strcspn(p, " \n");
			if (n >= len)
				n = len - 1;
			memcpy(out, p, n);
			out[n] = 0;
			return 1;
		}
		p += kl;
	}
	return 0;
}

/*
 * Loop layout: <imgpart> is an ext4 partition holding <imgdir>/<name>.img for every HarmonyOS partition.
 * Each image is attached to a free loop device; /etc/fstab.required then points at /dev/block/loopN.
 * The host mount is detached afterwards: the loop devices keep the files (and so the filesystem) alive.
 */
static int loop_attach(const char *file, char *devout, size_t len)
{
	int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0) {
		klog("no /dev/loop-control: %s", strerror(errno));
		return -1;
	}
	int n = ioctl(ctl, LOOP_CTL_GET_FREE);
	close(ctl);
	if (n < 0) {
		klog("LOOP_CTL_GET_FREE: %s", strerror(errno));
		return -1;
	}
	char path[32];
	snprintf(path, sizeof(path), "/dev/loop%d", n);
	int lfd = -1;
	for (int i = 0; i < 50 && lfd < 0; i++) {	/* devtmpfs creates the node a moment after the ioctl */
		lfd = open(path, O_RDWR | O_CLOEXEC);
		if (lfd < 0)
			usleep(20000);
	}
	int ffd = open(file, O_RDONLY | O_CLOEXEC);
	if (lfd < 0 || ffd < 0) {
		klog("open %s / %s: %s", path, file, strerror(errno));
		if (lfd >= 0)
			close(lfd);
		if (ffd >= 0)
			close(ffd);
		return -1;
	}
	struct loop_info64 li;
	memset(&li, 0, sizeof(li));
	li.lo_flags = LO_FLAGS_READ_ONLY;
	strncpy((char *)li.lo_file_name, file, LO_NAME_SIZE - 1);
	if (ioctl(lfd, LOOP_SET_FD, ffd) || ioctl(lfd, LOOP_SET_STATUS64, &li)) {
		klog("attach %s to %s: %s", file, path, strerror(errno));
		close(lfd);
		close(ffd);
		return -1;
	}
	close(lfd);
	close(ffd);
	snprintf(devout, len, "/dev/block/loop%d", n);
	return 0;
}

static int map_images(const char *imgpart, const char *imgdir, const char *hostfs)
{
	char dev[64];
	int found = -1;
	for (int i = 0; i < 100 && found; i++) {	/* up to 10 s for the block device */
		found = find_part(imgpart, dev, sizeof(dev));
		if (found)
			usleep(100000);
	}
	if (found) {
		klog("image partition %s not found", imgpart);
		return -1;
	}
	mkdir("/hmosroot", 0755);
	if (mount(dev, "/hmosroot", hostfs, MS_RDONLY | MS_NOATIME | MS_NOSUID | MS_NODEV, NULL)) {
		klog("mount %s (%s) failed: %s", dev, hostfs, strerror(errno));
		return -1;
	}
	klog("images from %s (%s) %s/", dev, hostfs, imgdir);
	mapf = fopen("/hmos_dm.txt", "w");
	mkdir("/etc", 0755);
	fstab = fopen("/etc/fstab.required", "w");
	if (fstab)
		fprintf(fstab, "# generated by hmos-preinit from loop-mounted image files\n");
	int ok = 0;
	for (size_t i = 0; i < sizeof(hm_mounts) / sizeof(hm_mounts[0]); i++) {
		char file[160], loop[32];
		snprintf(file, sizeof(file), "/hmosroot/%s/%s.img", imgdir, hm_mounts[i].name);
		if (access(file, R_OK))
			continue;
		if (loop_attach(file, loop, sizeof(loop)))
			continue;
		klog("mapped %s -> %s", hm_mounts[i].name, loop);
		if (mapf)
			fprintf(mapf, "%s %s\n", hm_mounts[i].name, loop);
		fstab_add_dev(hm_mounts[i].name, loop);
		ok++;
	}
	if (mapf)
		fclose(mapf);
	if (fstab)
		fclose(fstab);
	umount2("/hmosroot", MNT_DETACH);
	if (!ok)
		klog("no %s/*.img found: nothing to boot", imgdir);
	return ok ? 0 : -1;
}

/*
 * Diagnostics snapshots: run HarmonyOS's own tools inside its root
 * (/proc/1/root) and store their output next to the kernel log, so every
 * test boot also yields decoded hilog, process list, devices and dumpers.
 */
static int snap_dir = -1;
/* kept light on purpose: heavy dumpers (hidumper RenderService, bm/aa dump,
 * screenshots, kernel stacks of every thread) caused visible hitches and are
 * not needed once the system boots */
static const char *snap_cmds[][8] = {
	{ "ps.txt", "/system/bin/ps", "-ef", NULL },
	{ "threads.txt", "/system/bin/sh", "-c",
	  "top -H -b -n 1 -m 40; for p in $(pidof composer_host render_service riladapter_host audio_host "
	  "foundation); do echo fds $p $(cat /proc/$p/comm) $(ls /proc/$p/fd | wc -l); done", NULL },
	{ "dev.txt", "/system/bin/ls", "-lZ", "/dev/dri", "/dev/binder", "/dev/graphics", NULL },
	{ "param.txt", "/system/bin/param", "get", NULL },
	{ "sa.txt", "/system/bin/hidumper", "-ls", NULL },
	{ "mounts.txt", "/system/bin/cat", "/proc/mounts", NULL },
	{ "gpu.txt", "/system/bin/ls", "-lZ", "/dev/kgsl-3d0", "/dev/ion", "/vendor/firmware", NULL },
	{ "hw.txt", "/system/bin/sh", "-c",
	  "cat /proc/asound/cards; ls /sys/class/net; for s in /sys/bus/msm_subsys/devices/*; do "
	  "echo $s $(cat $s/name) $(cat $s/state); done; ls /vendor/firmware_mnt/image | head -80; "
	  "ls -R /vendor/bt_firmware | head -40; "
	  "ps -ef | grep -E 'qrtr|rmtfs|tqftp|pd-mapper|sweet-rproc|hexagonrpc' | grep -v grep", NULL },
};

/* kernel stack + wchan of the display path processes: shows where they are stuck. Every task would be too heavy
 * (thousands of threads), so only the interesting processes are dumped. */
static const char *const stack_comms[] = { "render_service", "composer_host", "display_engine_", "ohos.sceneboard" };

static bool stack_wanted(const char *pid)
{
	char path[96], comm[64];
	snprintf(path, sizeof(path), "/hmosproc/%s/comm", pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	ssize_t n = read(fd, comm, sizeof(comm) - 1);
	close(fd);
	if (n <= 0)
		return false;
	comm[n] = 0;
	for (size_t i = 0; i < sizeof(stack_comms) / sizeof(stack_comms[0]); i++) {
		if (strncmp(comm, stack_comms[i], strlen(stack_comms[i])) == 0)
			return true;
	}
	return false;
}

static void dump_stacks(int round)
{
	char name[64], path[96], buf[4096];
	snprintf(name, sizeof(name), "snap%d-stacks.txt", round);
	int out = openat(snap_dir, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (out < 0)
		return;
	DIR *d = opendir("/hmosproc");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		if (!stack_wanted(e->d_name))
			continue;
		char tdir[64];
		snprintf(tdir, sizeof(tdir), "/hmosproc/%s/task", e->d_name);
		DIR *td = opendir(tdir);
		struct dirent *t;
		while (td && (t = readdir(td))) {
			if (t->d_name[0] < '1' || t->d_name[0] > '9')
				continue;
			const char *files[] = { "comm", "wchan", "stack" };
			dprintf(out, "== pid %s tid %s\n", e->d_name, t->d_name);
			for (int i = 0; i < 3; i++) {
				snprintf(path, sizeof(path), "%s/%s/%s", tdir, t->d_name, files[i]);
				int fd = open(path, O_RDONLY | O_CLOEXEC);
				if (fd < 0)
					continue;
				ssize_t n = read(fd, buf, sizeof(buf) - 1);
				close(fd);
				if (n > 0) {
					buf[n] = 0;
					dprintf(out, "%s: %s%s", files[i], buf, buf[n - 1] == '\n' ? "" : "\n");
				}
			}
		}
		if (td)
			closedir(td);
	}
	if (d)
		closedir(d);
	close(out);
}

/* copy a small /proc file into the current boot's log folder */
static void snap_copy(const char *name, const char *src)
{
	char buf[8192];
	int in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return;
	int out = openat(snap_dir, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (out >= 0) {
		ssize_t n;
		while ((n = read(in, buf, sizeof(buf))) > 0)
			write(out, buf, n);
		fsync(out);
		close(out);
	}
	close(in);
}

static void run_snapshot(int round)
{
	static int proc_ok;
	if (!proc_ok) {
		mkdir("/hmosproc", 0700);
		proc_ok = (mount("proc", "/hmosproc", "proc", 0, NULL) == 0);
		if (!proc_ok)
			return;
	}
	for (size_t i = 0; i < sizeof(snap_cmds) / sizeof(snap_cmds[0]); i++) {
		char name[64];
		snprintf(name, sizeof(name), "snap%d-%s", round, snap_cmds[i][0]);
		int fd = openat(snap_dir, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (fd < 0)
			continue;
		pid_t pid = fork();
		if (pid == 0) {
			dup2(fd, 1);
			dup2(fd, 2);
			if (chroot("/hmosproc/1/root") || chdir("/"))
				_exit(126);
			char *const env[] = { "PATH=/system/bin:/vendor/bin", "LD_LIBRARY_PATH=", NULL };
			execve(snap_cmds[i][1], (char *const *)&snap_cmds[i][1], env);
			_exit(127);
		}
		/* give each tool at most 20 s */
		for (int t = 0; pid > 0 && t < 200; t++) {
			if (waitpid(pid, NULL, WNOHANG) == pid) {
				pid = 0;
				break;
			}
			usleep(100000);
		}
		if (pid > 0) {
			kill(pid, SIGKILL);
			waitpid(pid, NULL, 0);
		}
		fsync(fd);
		close(fd);
	}
	dump_stacks(round);
}

/* hmos.autorecovery=N: after N s reboot into recovery so the logs can be
 * pulled with adb without anyone pressing buttons (unattended bring-up) */
static int autorecovery_secs;

/* ---------------- boot verbose (hmos.verbose=1) ---------------- */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include "hmos_text.h"

static int verbose_on;
static int kms_fd = -1;

#define VLINES 160
#define VLINE_LEN 220
static char vring[VLINES][VLINE_LEN];
static int vhead, vcount, vdirty;

static void vpush(const char *s)
{
	snprintf(vring[vhead], VLINE_LEN, "%s", s);
	vhead = (vhead + 1) % VLINES;
	if (vcount < VLINES)
		vcount++;
	vdirty = 1;
}

/* kmsg record: "pri,seq,usec,flags;message\n KEY=VAL..." */
static void vparse_kmsg(const char *rec, ssize_t n)
{
	char buf[512];
	const char *semi = memchr(rec, ';', n);
	if (!semi)
		return;
	unsigned long long usec = 0;
	sscanf(rec, "%*d,%*d,%llu", &usec);
	const char *msg = semi + 1;
	/* boot finished: tell the composer to drop the overlay */
	if (memmem(rec, n, "bootevent.boot.completed", 24)) {
		int d = open("/hmosproc/1/root/dev/hmos_verbose_done", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (d >= 0) {
			fchmod(d, 0644);
			close(d);
		}
	}
	const char *end = memchr(msg, '\n', n - (msg - rec));
	int len = end ? (int)(end - msg) : (int)(n - (msg - rec));
	if (len > (int)sizeof(buf) - 32)
		len = sizeof(buf) - 32;
	/* skip the noisiest kernel chatter */
	if (memmem(msg, len, "avc:", 4) || memmem(msg, len, "GTP-INF", 7) || memmem(msg, len, "audit", 5) ||
	    memmem(msg, len, "QG-K", 4) || memmem(msg, len, "smblib", 6) || memmem(msg, len, "bq2597x", 7) ||
	    (len > 5 && !memcmp(msg, "[drm:", 5)) ||
	    memmem(msg, len, "CameraDaemon", 12) || memmem(msg, len, "starttime:", 10) ||
	    memmem(msg, len, "init_context_static", 19))
		return;
	snprintf(buf, sizeof(buf), "[%5llu.%03llu] %.*s", usec / 1000000, (usec / 1000) % 1000, len, msg);
	vpush(buf);
}

static size_t vtext(char *out, size_t cap)
{
	size_t o = 0;
	for (int i = 0; i < vcount && o + VLINE_LEN + 2 < cap; i++) {
		int idx = (vhead - vcount + i + VLINES) % VLINES;
		size_t l = strlen(vring[idx]);
		memcpy(out + o, vring[idx], l);
		o += l;
		out[o++] = '\n';
	}
	out[o] = 0;
	return o;
}

struct kms_state {
	int ok, gave_up, fails, modeset_done, front;
	uint32_t conn, crtc, pitch, w, h;
	uint32_t fb[2], handle[2];
	uint64_t size;
	uint8_t *map[2];
	struct drm_mode_modeinfo mode;
};
static struct kms_state kms;

static int kms_setup(void)
{
	struct drm_mode_card_res res = {0};
	if (ioctl(kms_fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		return -1;
	uint32_t conns[16] = {0}, crtcs[16] = {0}, encs[16] = {0};
	if (res.count_connectors > 16 || res.count_crtcs > 16 || res.count_encoders > 16)
		return -1;
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.count_fbs = 0;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		return -1;
	for (uint32_t i = 0; i < res.count_connectors; i++) {
		struct drm_mode_get_connector c = {0};
		c.connector_id = conns[i];
		if (ioctl(kms_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c))
			continue;
		if (c.connection != 1 || c.count_modes == 0)
			continue;
		struct drm_mode_modeinfo modes[32];
		uint32_t cenc[8];
		uint32_t nmodes = c.count_modes > 32 ? 32 : c.count_modes;
		struct drm_mode_get_connector c2 = {0};
		c2.connector_id = conns[i];
		c2.count_modes = nmodes;
		c2.modes_ptr = (uintptr_t)modes;
		c2.count_encoders = c.count_encoders > 8 ? 8 : c.count_encoders;
		c2.encoders_ptr = (uintptr_t)cenc;
		if (ioctl(kms_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c2))
			continue;
		kms.mode = modes[0];
		for (uint32_t m = 0; m < nmodes; m++)
			if (modes[m].type & DRM_MODE_TYPE_PREFERRED) {
				kms.mode = modes[m];
				break;
			}
		uint32_t enc_id = c2.encoder_id ? c2.encoder_id : (c2.count_encoders ? cenc[0] : 0);
		struct drm_mode_get_encoder e = {0};
		e.encoder_id = enc_id;
		if (enc_id && ioctl(kms_fd, DRM_IOCTL_MODE_GETENCODER, &e) == 0) {
			kms.crtc = e.crtc_id;
			if (!kms.crtc)
				for (uint32_t k = 0; k < res.count_crtcs; k++)
					if (e.possible_crtcs & (1u << k)) {
						kms.crtc = crtcs[k];
						break;
					}
		}
		if (!kms.crtc && res.count_crtcs)
			kms.crtc = crtcs[0];
		kms.conn = conns[i];
		break;
	}
	if (!kms.conn || !kms.crtc)
		return -1;
	kms.w = kms.mode.hdisplay;
	kms.h = kms.mode.vdisplay;
	/* two scanout buffers: draw into the hidden one, then flip */
	for (int b = 0; b < 2; b++) {
		struct drm_mode_create_dumb cd = {0};
		cd.width = kms.w;
		cd.height = kms.h;
		cd.bpp = 32;
		if (ioctl(kms_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
			return -1;
		kms.handle[b] = cd.handle;
		kms.pitch = cd.pitch;
		kms.size = cd.size;
		struct drm_mode_fb_cmd fb = {0};
		fb.width = kms.w;
		fb.height = kms.h;
		fb.pitch = kms.pitch;
		fb.bpp = 32;
		fb.depth = 24;
		fb.handle = cd.handle;
		if (ioctl(kms_fd, DRM_IOCTL_MODE_ADDFB, &fb))
			return -1;
		kms.fb[b] = fb.fb_id;
		struct drm_mode_map_dumb md = {0};
		md.handle = cd.handle;
		if (ioctl(kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
			return -1;
		kms.map[b] = mmap(NULL, kms.size, PROT_READ | PROT_WRITE, MAP_SHARED, kms_fd, md.offset);
		if (kms.map[b] == MAP_FAILED)
			return -1;
		memset(kms.map[b], 0, kms.size);
	}
	kms.front = 1;
	kms.ok = 1;
	klog("verbose: %ux%u on crtc %u connector %u", kms.w, kms.h, kms.crtc, kms.conn);
	return 0;
}

/*
 * For a short window after the composer takes over, turn on DRM atomic
 * debugging so a rejected commit says why in kmsg.
 */
static time_t drm_dbg_until;
static void drm_debug_set(const char *v)
{
	int fd = open("/hmosproc/1/root/sys/module/drm/parameters/debug", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		write(fd, v, strlen(v));
		close(fd);
	}
}
static void drm_debug_window(int secs)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	drm_dbg_until = t.tv_sec + secs;
	drm_debug_set("0x14"); /* KMS | ATOMIC */
}
static void drm_debug_tick(const struct timespec *now)
{
	if (drm_dbg_until && now->tv_sec >= drm_dbg_until) {
		drm_debug_set("0");
		drm_dbg_until = 0;
		klog("verbose: drm debug window closed");
	}
}

/* draw on the panel until the HarmonyOS composer owns the display */
static void kms_draw(const char *text)
{
	if (kms_fd < 0 || kms.gave_up)
		return;
	if (!kms.ok && kms_setup()) {
		kms.gave_up = 1;
		return;
	}
	/* while someone else holds master only probe once a second */
	if (kms.fails >= 5 && (++kms.fails % 10) != 0)
		return;
	if (ioctl(kms_fd, DRM_IOCTL_SET_MASTER, 0)) {
		if (kms.fails < 5 && ++kms.fails == 5) {
			klog("verbose: display master taken by the HarmonyOS composer (errno %d), handing over", errno);
			drm_debug_window(30);
		}
		return;
	}
	if (kms.fails >= 5) {
		klog("verbose: got the display back, redrawing");
		kms.modeset_done = 0; /* the composer changed the plane setup */
	}
	kms.fails = 0;
	int back = kms.front ^ 1;
	uint8_t *map = kms.map[back];
	memset(map, 0, kms.size);
	hmos_draw_screen(map, kms.pitch, kms.w, kms.h, text, 0);
	if (!kms.modeset_done) {
		/* first frame: one real mode set (replaces the bootloader splash) */
		struct drm_mode_crtc c = {0};
		c.crtc_id = kms.crtc;
		c.fb_id = kms.fb[back];
		c.set_connectors_ptr = (uintptr_t)&kms.conn;
		c.count_connectors = 1;
		c.mode = kms.mode;
		c.mode_valid = 1;
		if (ioctl(kms_fd, DRM_IOCTL_MODE_SETCRTC, &c) == 0)
			kms.modeset_done = 1;
	} else {
		/* afterwards just swap buffers: no blanking, no tearing */
		struct drm_mode_crtc_page_flip pf = {0};
		pf.crtc_id = kms.crtc;
		pf.fb_id = kms.fb[back];
		for (int t = 0; t < 20; t++) {
			if (ioctl(kms_fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0 || errno != EBUSY)
				break;
			usleep(5000);
		}
	}
	kms.front = back;
	ioctl(kms_fd, DRM_IOCTL_DROP_MASTER, 0);
}

static void verbose_flush(void)
{
	static char text[VLINES * (VLINE_LEN + 1) + 1];
	if (!vdirty)
		return;
	vdirty = 0;
	size_t n = vtext(text, sizeof(text));
	/* the composer reads this file and overlays it on every frame */
	int fd = open("/hmosproc/1/root/dev/hmos_verbose", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		write(fd, text, n);
		fchmod(fd, 0644);
		close(fd);
	}
	kms_draw(text);
}

/* hilog flow control drops most of SceneBoard's lines (window, display
 * power, lock screen): switch it off for the debug stream */
static void hilog_flowctrl_off(void)
{
	static const char *const modes[] = { "pidoff", "domainoff" };
	for (int i = 0; i < 2; i++) {
		pid_t pid = fork();
		if (pid == 0) {
			int nfd = open("/dev/null", O_RDWR);
			if (nfd >= 0) {
				dup2(nfd, 1);
				dup2(nfd, 2);
			}
			if (chroot("/hmosproc/1/root") || chdir("/"))
				_exit(126);
			char *const argv[] = { "/system/bin/hilog", "-Q", (char *)modes[i], NULL };
			char *const env[] = { "PATH=/system/bin:/vendor/bin", NULL };
			execve(argv[0], argv, env);
			_exit(127);
		}
		if (pid > 0)
			waitpid(pid, NULL, 0);
	}
}

/* continuous decoded hilog into the per-boot directory */
static pid_t hilog_pid = -1;
static void hilog_stream_tick(void)
{
	if (hilog_pid > 0) {
		if (waitpid(hilog_pid, NULL, WNOHANG) == 0)
			return;
		hilog_pid = -1;
	}
	if (snap_dir < 0)
		return;
	hilog_flowctrl_off();
	static int seq;
	char name[64];
	snprintf(name, sizeof(name), "hilog-stream-%d.txt", seq++);
	int fd = openat(snap_dir, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(fd, 1);
		dup2(fd, 2);
		if (chroot("/hmosproc/1/root") || chdir("/"))
			_exit(126);
		char *const argv[] = { "/system/bin/hilog", NULL };
		char *const env[] = { "PATH=/system/bin:/vendor/bin", NULL };
		execve(argv[0], argv, env);
		_exit(127);
	}
	close(fd);
	hilog_pid = pid;
}

/* Every boot needs a few MB of log space. Older layouts had a tiny cache partition, so all logs were deleted at
 * the start of each boot; that also destroyed the only evidence of a crash-reboot, because the next boot starts a
 * few seconds after the crash. Now the 20 slots are rotated instead (kmsg-0..19, pstore-N, boot-N) and space is
 * only reclaimed when the log partition gets full. */
static void rm_tree(const char *dir)
{
	DIR *d = opendir(dir);
	if (d) {
		struct dirent *e;
		char p[300];
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
			unlink(p);
		}
		closedir(d);
	}
	rmdir(dir);
}

static void purge_old_logs(void)
{
	struct statvfs vfs;
	char p[128];
	/* keep the previous boots' logs while there is room (cf. crash-reboot evidence above) */
	if (statvfs("/hmoslog", &vfs) == 0 &&
	    (uint64_t)vfs.f_bavail * vfs.f_frsize >= (64ULL << 20))
		return;
	klog("log partition almost full, deleting old logs");
	for (int j = 0; j < 20; j++) {
		snprintf(p, sizeof(p), "/hmoslog/hmos/kmsg-%d.log", j);
		unlink(p);
		snprintf(p, sizeof(p), "/hmoslog/hmos/boot-%d", j);
		rm_tree(p);
		snprintf(p, sizeof(p), "/hmoslog/hmos/pstore-%d", j);
		rm_tree(p);
	}
	sync();
}

static void start_logger(void)
{
	char dev[64], path[128];
	if (find_part(log_part, dev, sizeof(dev))) {
		klog("no cache partition, persistent log disabled");
		return;
	}
	mkdir("/hmoslog", 0700);
	if (mount(dev, "/hmoslog", "ext4", MS_NOATIME | MS_NOSUID | MS_NODEV, NULL) &&
	    mount(dev, "/hmoslog", "f2fs", MS_NOATIME | MS_NOSUID | MS_NODEV, NULL)) {
		klog("mount cache failed: %s", strerror(errno));
		return;
	}
	mkdir("/hmoslog/hmos", 0700);
	if (autorecovery_secs) {
		/* unattended mode: if the previous boot never reached its planned
		 * reboot (kernel panic, hang, watchdog), go straight to recovery so
		 * the logs can be collected instead of looping */
		if (access("/hmoslog/hmos/boot_inflight", F_OK) == 0) {
			int m = open("/hmoslog/hmos/PREVIOUS_BOOT_CRASHED", O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if (m >= 0)
				close(m);
			unlink("/hmoslog/hmos/boot_inflight");
			sync();
			klog("autorecovery: previous boot did not finish, rebooting to recovery");
			syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
				LINUX_REBOOT_CMD_RESTART2, "recovery");
		}
		int f = open("/hmoslog/hmos/boot_inflight", O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (f >= 0) {
			fsync(f);
			close(f);
		}
	}
	purge_old_logs();
	int idx = 0;
	for (idx = 0; idx < 20; idx++) {
		snprintf(path, sizeof(path), "/hmoslog/hmos/kmsg-%d.log", idx);
		if (access(path, F_OK))
			break;
	}
	if (idx == 20) { /* rotate: reuse slot 0, clear its old snapshots and pstore copy */
		idx = 0;
		snprintf(path, sizeof(path), "/hmoslog/hmos/pstore-%d", idx);
		rm_tree(path);
		snprintf(path, sizeof(path), "/hmoslog/hmos/boot-%d", idx);
		rm_tree(path);
		snprintf(path, sizeof(path), "/hmoslog/hmos/kmsg-0.log");
	}
	/* O_DSYNC: every write reaches the flash before write() returns, so the last lines before a crash survive */
	int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_DSYNC, 0600);
	int in = open("/dev/kmsg", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (out < 0 || in < 0) {
		klog("logger open failed");
		return;
	}
	klog("persistent log: cache:/hmos/kmsg-%d.log", idx);
	/* RAM console of the previous boot (kernel panics): ramoops sits in the old exynos_ss log area, see
	 * scripts/build_kernel.sh. Copy what survived next to this boot's logs. */
	{
		mkdir("/sys/fs/pstore", 0755);
		mount("pstore", "/sys/fs/pstore", "pstore", 0, NULL);
		DIR *pd = opendir("/sys/fs/pstore");
		if (pd) {
			char pdir[128];
			snprintf(pdir, sizeof(pdir), "/hmoslog/hmos/pstore-%d", idx);
			struct dirent *pe;
			int made = 0, nfiles = 0;
			while ((pe = readdir(pd))) {
				if (pe->d_name[0] == '.')
					continue;
				char src[300], dst[300], cp[4096];
				snprintf(src, sizeof(src), "/sys/fs/pstore/%s", pe->d_name);
				int sfd = open(src, O_RDONLY | O_CLOEXEC);
				if (sfd < 0)
					continue;
				if (!made) {
					mkdir(pdir, 0700);
					made = 1;
				}
				snprintf(dst, sizeof(dst), "%s/%s", pdir, pe->d_name);
				int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
				ssize_t rn;
				while (dfd >= 0 && (rn = read(sfd, cp, sizeof(cp))) > 0)
					write(dfd, cp, rn);
				if (dfd >= 0) {
					fsync(dfd);
					close(dfd);
				}
				close(sfd);
				nfiles++;
			}
			closedir(pd);
			klog("pstore: %d file(s) of the previous boot saved to cache:/hmos/pstore-%d", nfiles, idx);
		}
	}
	/* per-boot directory for the diagnostics snapshots */
	char sdir[128];
	snprintf(sdir, sizeof(sdir), "/hmoslog/hmos/boot-%d", idx);
	mkdir(sdir, 0700);
	snap_dir = open(sdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	/* /proc/cmdline carries sec_debug.reset_reason / rst_stat: how the *previous* boot ended (panic, watchdog,
	 * soft reboot, ...), which is the only record of a crash-reboot that leaves no log behind */
	if (snap_dir >= 0) {
		snap_copy("cmdline.txt", "/proc/cmdline");
	}
	pid_t pid = fork();
	if (pid == 0) {
		/* double fork so init_early never has to wait for us directly */
		if (fork())
			_exit(0);
		setsid();
		mkdir("/hmosproc", 0700);
		mount("proc", "/hmosproc", "proc", 0, NULL);
		char line[8192];
		unsigned lines = 0;
		int dirty = 0;
		struct timespec last_flush = {0}, last_hilog = {0};
		/* snapshot schedule (seconds after the logger started) */
		static const int snap_at[] = { 40, 60, 90, 150, 300, 600 };
		int next_snap = 0;
		struct timespec t0, now;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (;;) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (snap_dir >= 0 && next_snap < (int)(sizeof(snap_at) / sizeof(snap_at[0])) &&
			    now.tv_sec - t0.tv_sec >= snap_at[next_snap]) {
				run_snapshot(next_snap);
				next_snap++;
			}
			drm_debug_tick(&now);
			if (autorecovery_secs && now.tv_sec - t0.tv_sec >= autorecovery_secs) {
				klog("autorecovery: %d s elapsed, final snapshot then reboot to recovery", autorecovery_secs);
				if (snap_dir >= 0)
					run_snapshot(9);
				fdatasync(out);
				unlink("/hmoslog/hmos/boot_inflight");
				sync();
				syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
					LINUX_REBOOT_CMD_RESTART2, "recovery");
				autorecovery_secs = 0; /* refused: keep logging */
			}
			/* verbose overlay: up to 10 refreshes per second (page-flipped) */
			if (verbose_on) {
				long ms = (now.tv_sec - last_flush.tv_sec) * 1000 + (now.tv_nsec - last_flush.tv_nsec) / 1000000;
				if (ms >= 100) {
					verbose_flush();
					last_flush = now;
				}
			}
			/* decoded hilog stream, (re)started every 5 s once hilogd is up */
			if (now.tv_sec - t0.tv_sec >= 12 && now.tv_sec - last_hilog.tv_sec >= 5) {
				hilog_stream_tick();
				last_hilog = now;
			}
			struct pollfd pfd = { .fd = in, .events = POLLIN };
			if (poll(&pfd, 1, verbose_on ? 100 : 1000) == 0) {
				/* idle for 1s: make sure the tail of the log is on disk */
				if (dirty) {
					fdatasync(out);
					dirty = 0;
				}
				continue;
			}
			/* drain every pending record into one buffer, then a single synchronous write */
			static char big[65536];
			size_t used = 0;
			ssize_t n;
			while (used + sizeof(line) <= sizeof(big) && (n = read(in, big + used, sizeof(line))) != 0) {
				if (n < 0) {
					if (errno == EPIPE)
						continue;         /* records were overwritten: next read resumes */
					break;                    /* EAGAIN: caught up */
				}
				if (verbose_on)
					vparse_kmsg(big + used, n);
				used += n;
			}
			if (used)
				write(out, big, used);
			dirty = 0;
			(void)lines;
		}
	}
	waitpid(pid, NULL, 0);
	close(in);
	close(out);
	umount2("/hmoslog", MNT_DETACH);
}

/*
 * Off-mode charging: with a charger plugged in, "power off" makes the PMIC
 * start the phone again and the bootloader passes androidboot.mode=charger.
 * Android shows its charging screen then; booting HarmonyOS here would look
 * like the phone rebooted instead of switching off. Stay in a minimal loop
 * (screen off, charging continues in hardware): unplugging the charger powers
 * the phone off, holding the power key for 1.5 s boots HarmonyOS.
 */
static int read_int_file(const char *p)
{
	char b[16] = {0};
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int n = read(fd, b, sizeof(b) - 1);
	close(fd);
	return n > 0 ? atoi(b) : -1;
}

static int charger_online(void)
{
	static const char *nodes[] = {
		"/sys/class/power_supply/usb/online", "/sys/class/power_supply/usb/present",
		"/sys/class/power_supply/ac/online", "/sys/class/power_supply/dc/online",
	};
	int seen = 0;
	for (unsigned i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		int v = read_int_file(nodes[i]);
		if (v > 0)
			return 1;
		if (v == 0)
			seen = 1;
	}
	return seen ? 0 : 1; /* unknown: assume plugged in */
}

static void charger_mode(void)
{
	klog("androidboot.mode=charger: off-mode charging (unplug = power off, hold power = boot)");
	int fds[16], nfd = 0;
	for (int i = 0; i < 16 && nfd < 16; i++) {
		char p[32];
		snprintf(p, sizeof(p), "/dev/input/event%d", i);
		int fd = open(p, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (fd >= 0)
			fds[nfd++] = fd;
	}
	struct timespec down = {0};
	int pressed = 0, unplugged = 0;
	for (;;) {
		struct pollfd pf[16];
		for (int i = 0; i < nfd; i++) {
			pf[i].fd = fds[i];
			pf[i].events = POLLIN;
			pf[i].revents = 0;
		}
		poll(pf, nfd, 500);
		for (int i = 0; i < nfd; i++) {
			struct input_event ev;
			while (read(fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				if (ev.type == EV_KEY && ev.code == KEY_POWER) {
					pressed = ev.value != 0;
					if (pressed)
						clock_gettime(CLOCK_MONOTONIC, &down);
				}
			}
		}
		if (pressed) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			long ms = (now.tv_sec - down.tv_sec) * 1000 + (now.tv_nsec - down.tv_nsec) / 1000000;
			if (ms >= 1500) {
				klog("power key held: booting HarmonyOS");
				sync();
				syscall(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART, NULL);
			}
		}
		if (!charger_online()) {
			if (++unplugged >= 4) {
				klog("charger unplugged: powering off");
				sync();
				syscall(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, NULL);
			}
		} else {
			unplugged = 0;
		}
	}
}

/*
 * hmos.policytest=1: load the HarmonyOS SELinux policy from here, with the log being written synchronously, to see
 * whether the kernel survives it (the phone resets within a fraction of a second of init_early starting).
 */
/* hmos.fbtest=1: paint colour bars on the DECON framebuffer and keep /dev/fb0 open from a helper process.
 * Proves the panel/DECON/backlight path independent of HarmonyOS. The kernel powers the panel off on the LAST
 * close of fb0 (decon_release -> decon_disable), which is what the HarmonyOS composer's open/ioctl/close does
 * to the screen, so the held fd also keeps the panel lit until the real composer is fixed. */
#include <linux/fb.h>
/* Samsung DECON window-config ABI (exynos9810 dpu, decon.h without CONFIG_SUPPORT_DSU): same layout as the kernel */
struct sdecon_rect { int x, y; uint32_t w, h; };
struct sdecon_frame { int x, y; uint32_t w, h, f_w, f_h; };
struct sdecon_dpp { uint64_t addr[3]; int rot, eq_mode, comp_src, hdr_std; uint32_t min_lum, max_lum; };
struct sdecon_win {
	int state;                      /* 0 disabled, 1 color, 2 buffer, 3 update, 4 cursor */
	union {
		uint32_t color;
		struct {
			int fd_idma[3];
			int acq_fence, rel_fence, plane_alpha, blending, idma_type, format;
			struct sdecon_dpp dpp;
			struct sdecon_rect block, transparent, opaque;
			struct sdecon_frame src;
		};
	};
	struct sdecon_frame dst;
	_Bool protection, compression;
};
struct sdecon_data {
	int retire_fence, fd_odma;
	struct sdecon_win config[7];
	struct { int remained_frames; uint32_t reserved[7]; } extra;
};
#define S3CFB_WIN_CONFIG _IOW('F', 209, struct sdecon_data)

static void decon_commit(int fd, struct sdecon_data *d, const char *what)
{
	d->retire_fence = -1;
	d->fd_odma = -1;
	int r = ioctl(fd, S3CFB_WIN_CONFIG, d);
	klog("fbtest: WIN_CONFIG %s -> %d errno %d retire_fence %d", what, r, r ? errno : 0, d->retire_fence);
	if (r == 0 && d->retire_fence > 0) {
		struct pollfd pf = { .fd = d->retire_fence, .events = POLLIN };
		klog("fbtest: retire fence wait -> %d", poll(&pf, 1, 1000));
		close(d->retire_fence);
	}
}

static void fb_test(void)
{
	const char *nodes[] = { "/dev/fb0", "/dev/graphics/fb0", NULL };
	int fd = -1, i;
	for (i = 0; nodes[i] && fd < 0; i++)
		fd = open(nodes[i], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		klog("fbtest: no fb0 (%s)", strerror(errno));
		return;
	}
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
		klog("fbtest: FBIOGET failed: %s", strerror(errno));
		close(fd);
		return;
	}
	klog("fbtest: %ux%u virt %ux%u bpp %u stride %u smem %u r%u/%u g%u/%u b%u/%u", v.xres, v.yres, v.xres_virtual,
	     v.yres_virtual, v.bits_per_pixel, f.line_length, f.smem_len, v.red.offset, v.red.length, v.green.offset,
	     v.green.length, v.blue.offset, v.blue.length);
	if (v.bits_per_pixel != 32) {
		klog("fbtest: only 32bpp handled");
		close(fd);
		return;
	}
	size_t len = f.smem_len;
	uint8_t *fb = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		klog("fbtest: mmap failed: %s", strerror(errno));
		close(fd);
		return;
	}
	static const uint32_t bars[8][3] = { {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
					     {255,0,255}, {255,0,0}, {0,0,255}, {40,40,40} };
	for (uint32_t y = 0; y < v.yres; y++) {
		for (uint32_t x = 0; x < v.xres; x++) {
			const uint32_t *c = bars[x * 8 / v.xres];
			uint32_t px = (c[0] >> (8 - v.red.length) << v.red.offset) |
				      (c[1] >> (8 - v.green.length) << v.green.offset) |
				      (c[2] >> (8 - v.blue.length) << v.blue.offset);
			if (v.transp.length)
				px |= ((1u << v.transp.length) - 1) << v.transp.offset;
			if ((size_t)y * f.line_length + x * 4 + 4 <= len)
				*(uint32_t *)(fb + (size_t)y * f.line_length + x * 4) = px;
		}
	}
	v.yoffset = 0;
	v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
	klog("fbtest: put_vscreeninfo %d, pan %d, unblank %d", ioctl(fd, FBIOPUT_VSCREENINFO, &v),
	     ioctl(fd, FBIOPAN_DISPLAY, &v), ioctl(fd, FBIOBLANK, 0));
	{	/* DECON in this kernel stays INIT until a window config arrives: fill a solid colour first */
		static struct sdecon_data d;
		int w = v.xres, h = v.yres;
		memset(&d, 0, sizeof(d));
		for (i = 0; i < 7; i++)
			d.config[i].state = 0;
		d.config[0].state = 1;
		d.config[0].color = 0x00ff0000;
		d.config[0].dst = (struct sdecon_frame){ 0, 0, w, h, w, h };
		decon_commit(fd, &d, "solid red");
		sleep(2);
		/* then the bars from the framebuffer memory itself, via an ION dma-buf of the same size */
		memset(&d, 0, sizeof(d));
		int ion = open("/dev/ion", O_RDWR | O_CLOEXEC);
		struct { size_t len, align; unsigned int heap_id_mask, flags; int handle; } al = {
			(size_t)f.line_length * v.yres, 4096, 1u << 0, 0, 0 };
		struct { int handle, fd; } sh = { 0, -1 };
		if (ion >= 0 && ioctl(ion, _IOWR('I', 0, al), &al) == 0 && (sh.handle = al.handle, 1) &&
		    ioctl(ion, _IOWR('I', 4, sh), &sh) == 0 && sh.fd >= 0) {
			uint8_t *buf = mmap(NULL, al.len, PROT_READ | PROT_WRITE, MAP_SHARED, sh.fd, 0);
			if (buf != MAP_FAILED) {
				memcpy(buf, fb, al.len);
				struct sdecon_win *c = &d.config[0];
				c->state = 2;
				c->fd_idma[0] = sh.fd;
				c->fd_idma[1] = c->fd_idma[2] = -1;
				c->acq_fence = c->rel_fence = -1;
				c->plane_alpha = 255;
				c->blending = 0;
				c->idma_type = 0;
				c->format = 4;          /* DECON_PIXEL_FORMAT_XRGB_8888 */
				c->src = (struct sdecon_frame){ 0, 0, w, h, w, h };
				c->dst = c->src;
				decon_commit(fd, &d, "ion buffer bars");
			} else
				klog("fbtest: ion mmap failed: %s", strerror(errno));
		} else
			klog("fbtest: ion alloc/share failed (ion %d): %s", ion, strerror(errno));
	}
	const char *bl[] = { "/sys/class/backlight/panel/", "/sys/class/backlight/backlight/", NULL };
	for (i = 0; bl[i]; i++) {
		char p1[128], b[16] = "";
		int m;
		snprintf(p1, sizeof(p1), "%smax_brightness", bl[i]);
		m = open(p1, O_RDONLY);
		if (m < 0)
			continue;
		read(m, b, sizeof(b) - 1);
		close(m);
		snprintf(p1, sizeof(p1), "%sbrightness", bl[i]);
		m = open(p1, O_WRONLY);
		if (m >= 0) {
			klog("fbtest: backlight %s max %d -> %zd", bl[i], atoi(b), write(m, b, strlen(b)));
			close(m);
		}
	}
	if (fork() == 0) {            /* hold fb0 open so the last-close panel-off never happens */
		for (;;)
			pause();
	}
	klog("fbtest: bars drawn, fb0 held open by helper");
}

static void policy_test(void)
{
	char dev[64];
	int found = 0;
	mkdir("/hmospt", 0755);
	for (int i = 0; i < 8 && !found; i++) {
		snprintf(dev, sizeof(dev), "/dev/block/loop%d", i);
		if (mount(dev, "/hmospt", "ext4", MS_RDONLY, NULL))
			continue;
		if (access("/hmospt/etc/selinux/prebuild_sepolicy/policy.31", R_OK) == 0)
			found = 1;
		else
			umount2("/hmospt", 0);
	}
	if (!found) {
		klog("policytest: policy.31 not found on loop0..7");
		return;
	}
	int pf = open("/hmospt/etc/selinux/prebuild_sepolicy/policy.31", O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (pf < 0 || fstat(pf, &st) || st.st_size <= 0) {
		klog("policytest: cannot read policy.31");
		return;
	}
	char *buf = malloc(st.st_size);
	size_t got = 0;
	while (buf && got < (size_t)st.st_size) {
		ssize_t r = read(pf, buf + got, st.st_size - got);
		if (r <= 0)
			break;
		got += r;
	}
	close(pf);
	klog("policytest: policy.31 is %zu bytes (read %zu)", (size_t)st.st_size, got);
	mkdir("/sys/fs/selinux", 0755);
	mount("selinuxfs", "/sys/fs/selinux", "selinuxfs", 0, NULL);
	int lf = open("/sys/fs/selinux/load", O_WRONLY | O_CLOEXEC);
	if (lf < 0) {
		klog("policytest: open /sys/fs/selinux/load: %s", strerror(errno));
		return;
	}
	sleep(3);                       /* let the logger flush everything up to here */
	klog("policytest: writing the policy to the kernel NOW");
	sleep(1);
	ssize_t w = write(lf, buf, got);
	klog("policytest: write returned %zd (%s)", w, w < 0 ? strerror(errno) : "policy loaded");
	close(lf);
	sleep(3);
	klog("policytest: survived the policy load");
	umount2("/sys/fs/selinux", MNT_DETACH);
}

int main(int argc, char **argv)
{
	(void)argc;
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mkdir("/dev", 0755);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, "mode=0755");
	kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	klog("starting (HarmonyOS userspace, stage 1)");

	char cmdline[4096] = {0};
	int cfd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
	if (cfd >= 0) {
		read(cfd, cmdline, 2048);
		close(cfd);
	}
	/* The bootloader/kernel may drop or truncate our extra parameters (Samsung adds a long command line of its
	 * own): the same hmos.* options are also stored in /hmos.conf inside this ramdisk and appended here. */
	size_t cl = strlen(cmdline);
	klog("kernel cmdline: %zu bytes, tail: %s", cl, cl > 200 ? cmdline + cl - 200 : cmdline);
	int hfd = open("/hmos.conf", O_RDONLY | O_CLOEXEC);
	if (hfd >= 0) {
		char conf[1024] = {0};
		ssize_t hn = read(hfd, conf, sizeof(conf) - 1);
		close(hfd);
		if (hn > 0 && cl + 1 + (size_t)hn < sizeof(cmdline)) {
			cmdline[cl] = ' ';
			memcpy(cmdline + cl + 1, conf, hn);
			klog("options from /hmos.conf: %s", conf);
		}
	}
	char imgpart[64] = "", imgdir[64] = "hmosimg", hostfs[16] = "ext4";
	cmdline_get(cmdline, "hmos.imgfs", img_fs, sizeof(img_fs));
	cmdline_get(cmdline, "hmos.logpart", log_part, sizeof(log_part));
	cmdline_get(cmdline, "hmos.imgdir", imgdir, sizeof(imgdir));
	cmdline_get(cmdline, "hmos.imghostfs", hostfs, sizeof(hostfs));

	if (cmdline_get(cmdline, "hmos.imgpart", imgpart, sizeof(imgpart))) {
		/* loop layout (no super partition) */
		map_images(imgpart, imgdir, hostfs);
	} else {
		/* devtmpfs needs a moment for UFS partitions on some boots */
		char tmp[64];
		for (int i = 0; i < 50 && find_super(tmp, sizeof(tmp)); i++)
			usleep(100000);
		map_super();
	}

	/* boot verbose: hmos.verbose=1 on the kernel command line */
	verbose_on = strstr(cmdline, "hmos.verbose=1") != NULL;
	const char *ar = strstr(cmdline, "hmos.autorecovery=");
	if (ar)
		autorecovery_secs = atoi(ar + strlen("hmos.autorecovery="));
	if (verbose_on) {
		kms_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
		if (kms_fd >= 0)
			ioctl(kms_fd, DRM_IOCTL_DROP_MASTER, 0); /* first opener becomes master: give it back */
		klog("boot verbose on (kms fd %d)", kms_fd);
	}
	start_logger();

	if (strstr(cmdline, "androidboot.mode=charger") && !strstr(cmdline, "hmos.charger=boot"))
		charger_mode();

	if (strstr(cmdline, "hmos.policytest=1"))
		policy_test();
	if (strstr(cmdline, "hmos.fbtest=1"))
		fb_test();
	usleep(1500000);   /* let the logger write out the boot log so far before init_early starts */
	klog("handing over to " REAL_INIT);
	if (kfd >= 0)
		close(kfd);
	umount2("/dev", MNT_DETACH);
	umount2("/sys", MNT_DETACH);
	umount2("/proc", MNT_DETACH);

	argv[0] = "/init";
	execv(REAL_INIT, argv);
	/* should never get here */
	kfd = open("/dev/kmsg", O_WRONLY);
	klog("exec " REAL_INIT " failed: %s", strerror(errno));
	for (;;)
		pause();
}
