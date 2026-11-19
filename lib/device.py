"""Device description (devices/<name>/device.json): partition names, image storage, boot image and kernel
command line. Everything that used to be hard-coded for sweet lives here."""
import json
import os

from .util import REPO, die

DEVICE_DIR = os.path.join(REPO, "devices")


class Device:
    def __init__(self, name):
        path = os.path.join(DEVICE_DIR, name, "device.json")
        if not os.path.isfile(path):
            known = sorted(d for d in os.listdir(DEVICE_DIR) if os.path.isfile(os.path.join(DEVICE_DIR, d, "device.json")))
            die("unknown device '%s' (known: %s)" % (name, ", ".join(known) or "none"))
        with open(path) as fh:
            self.data = json.load(fh)
        d = self.data
        self.name = d["name"]
        self.title = d.get("title", self.name)
        self.hardware = d["hardware"]
        st = d["storage"]
        self.storage_mode = st.get("mode", "loop")        # "loop": images as files in a host partition
        self.fs = st.get("fs", "ext4")                    # filesystem of the HarmonyOS partition images
        self.host_partition = st.get("host_partition")
        self.host_fs = st.get("host_fs", "ext4")
        self.image_dir = st.get("dir", "hmosimg")
        self.headroom_pct = st.get("headroom_pct", 8)
        self.slack_mb = st.get("slack_mb", 64)
        if self.storage_mode not in ("loop",):
            die("device %s: storage.mode '%s' is not implemented (only 'loop')" % (self.name, self.storage_mode))
        if self.fs not in ("ext4", "erofs"):
            die("device %s: storage.fs must be ext4 or erofs" % self.name)
        if self.fs == "erofs":
            self.erofs_note = "the device kernel must have EROFS (4.9 needs a backport)"
        self.parts = d.get("partitions", {})
        self.byname_dir = d.get("by_name_dir", "/dev/block/by-name")
        self.boot = d.get("boot", {})
        self.cmdline_extra = d.get("cmdline_extra", "")

    def part_name(self, key):
        v = self.parts.get(key)
        return v["name"] if isinstance(v, dict) else v

    def kernel_cmdline_extra(self):
        """hmos-preinit parameters that describe where the images are, then the generic options."""
        extra = ["hardware=" + self.hardware,
                 "hmos.imgpart=" + self.host_partition,
                 "hmos.imgdir=" + self.image_dir,
                 "hmos.imgfs=" + self.fs,
                 "hmos.imghostfs=" + self.host_fs,
                 "hmos.logpart=" + (self.part_name("cache") or "cache")]
        return " ".join(extra + [self.cmdline_extra]).strip()

    def fstab(self):
        """Second-stage fstab (/vendor/etc/fstab.<hardware>)."""
        lines = ["# fstab for HarmonyOS userspace on %s - second stage" % self.title,
                 "# system/vendor/sys_prod/chip_prod/cust/patch come from the first stage (/etc/fstab.required),",
                 "# written by hmos-preinit from the loop-mounted image files.",
                 "# <src>  <mnt_point>  <type>  <mnt_flags and options>  <fs_mgr_flags>"]
        ud = self.parts.get("userdata")
        if ud:
            lines.append("%s/%s /data %s noatime,nosuid,nodev,discard wait,check,nofail"
                         % (self.byname_dir, ud["name"], ud.get("fs", "ext4")))
        c = self.parts.get("cache")
        if c:
            lines.append("%s/%s %s %s rw,nosuid,nodev,noatime wait,nofail"
                         % (self.byname_dir, c["name"], c.get("mount", "/log"), c.get("fs", "ext4")))
        return "\n".join(lines) + "\n"
