"""Apply the selected patches to the OTA images and write the device ROM (images + boot.img)."""
import importlib.util
import json
import os
import shutil

from . import bootimg, erofs, sweetrom
from .util import REPO, step, info, warn, die, mkdirs, rmtree, human

PREBUILT = os.path.join(REPO, "prebuilt")
SUPER_PARTS = ["system", "vendor", "chip_prod", "sys_prod", "cust", "patch", "version", "preload"]
INIT_CFG = "etc/init/hmos2sweet.cfg"


class Context:
    """What a patch hook gets: the trees being edited plus helpers."""

    def __init__(self, build, patch):
        self._b = build
        self.patch = patch
        self.vendor = build.vendor
        self.work = build.work
        self.out = build.out
        self.images = build.images
        self.blobs = build.blobs_dir
        self.info = info
        self.warn = warn

    def system(self):
        return self._b.system_tree()

    def image_file(self, part, path):
        """Read one file from an untouched OTA partition image (bytes or None)."""
        if part not in self.images:
            return None
        return erofs.read_file(self.images[part], path, os.path.join(self.work, "tmp"))

    def list_image_dir(self, part, path):
        """[(name, is_dir)] of a directory in an untouched OTA partition image."""
        if part not in self.images:
            return []
        return erofs.list_dir(self.images[part], path)

    def cat_image_file(self, part, path):
        """Small file from an untouched OTA partition image, read without extracting (bytes or None)."""
        if part not in self.images:
            return None
        return erofs.cat_file(self.images[part], path)

    def save_removed(self, rel_name, data):
        p = os.path.join(self.out, "removed", rel_name)
        mkdirs(os.path.dirname(p))
        with open(p, "wb") as fh:
            fh.write(data)


class Build:
    def __init__(self, ota, sweet_rom, work, out, patches, device, kernel=None, stock_boot=None, dtb=None):
        self.ota = ota
        self.sweet_rom = sweet_rom          # donor-device ROM for patches that take blobs from it
        self.dev = device
        self.kernel = kernel
        self.stock_boot = stock_boot
        self.dtb = dtb
        self.work = mkdirs(work)
        self.out = mkdirs(out)
        self.patches = patches
        self.images = {}
        self.vendor = None
        self._system = None
        self.blobs_dir = os.path.join(self.work, "sweet_blobs")

    # ------------------------------------------------------------- inputs
    def extract_ota(self):
        step("Extracting the HarmonyOS OTA (%s)" % (self.ota.version or os.path.basename(self.ota.path)))
        names = [n for n in SUPER_PARTS + ["ramdisk"] if self.ota.has(n)]
        for need in ("system", "vendor", "ramdisk"):
            if need not in names:
                die("the OTA has no %s image: not a full HarmonyOS phone package?" % need)
        self.images = self.ota.extract(names, mkdirs(os.path.join(self.work, "ota")))

    def fetch_blobs(self):
        blobs = []
        for p in self.patches:
            blobs += p["sweet_blobs"] or []
        if not blobs:
            return
        step("Collecting firmware and audio configs from the sweet ROM")
        if not self.sweet_rom:
            die("the selected patches need files from a donor-device ROM: pass --sweet-rom")
        img = sweetrom.vendor_image(self.sweet_rom, self.work)
        sweetrom.check_vendor(img)
        rmtree(self.blobs_dir)
        got = sweetrom.fetch(img, blobs, mkdirs(self.blobs_dir))
        info("%d files" % len(got))

    # ------------------------------------------------------------ vendor
    def build_vendor(self):
        step("Building vendor.img")
        self.vendor = erofs.Tree(self.images["vendor"], os.path.join(self.work, "vendor_tree"), "vendor")
        self.vendor.extract()
        self.vendor.write("etc/fstab." + self.dev.hardware, self.dev.fstab())
        init = {"jobs": [], "services": []}
        ueventd = []
        for p in self.patches:
            info("[%s] %s" % (p.id, p.name))
            self._apply_patch(p, init, ueventd)
        self._write_init(init)
        self._write_ueventd(ueventd)
        return self._build_tree(self.vendor, "vendor")

    def _build_tree(self, tree, name):
        return tree.build(os.path.join(self.out, name + ".img"), fs=self.dev.fs,
                          headroom_pct=self.dev.headroom_pct, slack_mb=self.dev.slack_mb)

    def _apply_patch(self, p, init, ueventd):
        v = self.vendor
        for d in p["vendor_dirs"] or []:
            v.add_dir(d["dst"], d.get("mode", "0755"), label_from=d.get("label_from", "firmware"))
        for f in p["vendor_files"] or []:
            if f.get("only_if_exists") and not v.exists(f["dst"]):
                info("  skip %s (not in this OTA)" % f["dst"])
                continue
            v.add_file(p.file(f["src"]), f["dst"], f.get("mode", "0644"), label_from=f.get("label_from"),
                       uid=f.get("uid", 0), gid=f.get("gid", 2000))
        for l in p["vendor_symlinks"] or []:
            v.add_symlink(l["dst"], l["target"])
        for rel in p["vendor_remove"] or []:
            if v.remove(rel):
                info("  removed /vendor/%s" % rel)
        for b in p["sweet_blobs"] or []:
            src = os.path.join(self.blobs_dir, b["to"])
            if os.path.isdir(src):
                for root, dirs, files in os.walk(src):
                    dirs.sort()
                    rel = os.path.relpath(root, self.blobs_dir).replace(os.sep, "/")
                    for f in sorted(files):
                        v.add_file(os.path.join(root, f), rel + "/" + f, "0644")
            else:
                v.add_file(src, b["to"], "0644")
        for m in p["markers"] or []:
            v.write(m, b"")
        frag = p["init"] or {}
        for job in frag.get("jobs", []):
            init["jobs"].append(dict(job))
        init["services"] += frag.get("services", [])
        ueventd += p["ueventd"] or []
        if p["hook"]:
            spec = importlib.util.spec_from_file_location("hook_" + p.id.replace("-", "_"), p.file(p["hook"]))
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            mod.apply(Context(self, p))
        for rel in p["system_remove"] or []:
            self._remove_from(self.system_tree(), "system", rel)

    def _remove_from(self, tree, part, rel):
        """Delete a path from an extracted partition tree, keeping a copy in out/removed/<part>/."""
        data_dir = os.path.join(self.out, "removed", part, rel)
        src = tree.path(rel)
        if not os.path.lexists(src):
            warn("  /%s not in the %s image" % (rel, part))
            return
        if os.path.isdir(src) and not os.path.islink(src):
            shutil.copytree(src, data_dir, symlinks=True, dirs_exist_ok=True)
        else:
            mkdirs(os.path.dirname(data_dir))
            shutil.copy2(src, data_dir)
        tree.remove(rel)
        info("  removed /%s from %s (saved in removed/)" % (rel, part))

    def _write_init(self, init):
        jobs = []
        index = {}
        for j in init["jobs"]:
            if j["name"] in index:
                index[j["name"]]["cmds"] += j["cmds"]
            else:
                j = {"name": j["name"], **({"condition": j["condition"]} if "condition" in j else {}),
                     "cmds": list(j["cmds"])}
                index[j["name"]] = j
                jobs.append(j)
        cfg = {"jobs": jobs, "services": init["services"]}
        self.vendor.write(INIT_CFG, json.dumps(cfg, indent=4) + "\n")
        info("init: %d jobs, %d services in /vendor/%s" % (len(jobs), len(init["services"]), INIT_CFG))

    def _write_ueventd(self, lines):
        if not lines:
            return
        rel = "etc/ueventd.config"
        if not self.vendor.exists(rel):
            warn("no /vendor/etc/ueventd.config: device node permissions left to init")
            return
        cur = self.vendor.read(rel).decode(errors="replace")
        add = [l for l in lines if l.startswith("#") or l.split()[0] not in cur]
        if add:
            self.vendor.write(rel, (cur.rstrip("\n") + "\n\n" + "\n".join(add) + "\n").encode())

    # ------------------------------------------------------------ system
    def system_tree(self):
        if self._system is None:
            self._system = erofs.Tree(self.images["system"], os.path.join(self.work, "system_tree"), "system")
            self._system.extract()
        return self._system

    def build_system(self):
        if self._system is None and self.dev.fs != "erofs":
            self.system_tree()                  # the OTA's EROFS system has to be converted anyway
        if self._system is None:
            return None
        step("Building system.img")
        return self._build_tree(self._system, "system")

    # -------------------------------------------------------------- boot
    def build_boot(self):
        step("Building boot.img")
        preinit = os.path.join(PREBUILT, "ramdisk", "init")
        if not os.path.isfile(preinit):
            die("prebuilt/ramdisk/init (hmos-preinit) is missing: run build/build_preinit.sh first")
        if not self.kernel or not os.path.isfile(self.kernel):
            die("the device kernel is needed to build boot.img: pass --kernel (Image.gz-dtb for header v0/v1)")
        if not self.stock_boot or not os.path.isfile(self.stock_boot):
            die("pass --stock-boot: the phone's own boot.img, for header values and the kernel command line")
        tpl = bootimg.parse_header(self.stock_boot)
        dtb = self.dtb
        if tpl["dt_size"] and not dtb:
            dtb = os.path.join(self.work, "stock.dt")        # v0 header: reuse the stock device tree
            with open(dtb, "wb") as fh:
                fh.write(bootimg.extract_dt(self.stock_boot, tpl))
        cmdline = (tpl["cmdline"] + " " + self.dev.kernel_cmdline_extra()).strip()
        rd = bootimg.build_ramdisk(self.images["ramdisk"], preinit, conf=self.dev.kernel_cmdline_extra())
        bootimg.build_boot_from_template(tpl, self.kernel, rd, cmdline, os.path.join(self.out, "boot.img"),
                                         dtb=dtb, samsung_trailer=self.dev.boot.get("samsung_trailer", True))
        info("command line: " + cmdline)

    # ------------------------------------------------------------ others
    def copy_untouched(self):
        step("Copying the unchanged partitions")
        for n in SUPER_PARTS:
            if n not in self.images or os.path.exists(os.path.join(self.out, n + ".img")):
                continue
            dst = os.path.join(self.out, n + ".img")
            drop = [rel for p in self.patches for rel in (p[n + "_remove"] or [])]
            if not drop and (self.dev.fs == "erofs" or erofs.fs_type(self.images[n]) == self.dev.fs):
                try:             # already in the wanted filesystem (HarmonyOS 5 ships some partitions as ext4)
                    os.link(self.images[n], dst)
                except OSError:
                    shutil.copyfile(self.images[n], dst)
            else:                                # EROFS from the OTA -> ext4 for this kernel (or files to drop)
                t = erofs.Tree(self.images[n], os.path.join(self.work, "conv_" + n), n)
                t.extract()
                for rel in drop:
                    self._remove_from(t, n, rel)
                self._build_tree(t, n)
                rmtree(t.work)
            info("%-10s %s" % (n, human(os.path.getsize(dst))))
