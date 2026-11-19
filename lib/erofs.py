"""EROFS partition images: extract to a tree, edit, rebuild with the same metadata.

Uses the erofs-utils build with Android extensions (extract.erofs, mkfs.erofs
with --fs-config-file / --file-contexts, dump.erofs), see tools/fetch_tools.sh.

Paths inside a partition are written like on the device without the mount
point: "lib64/libfoo.so" for /vendor/lib64/libfoo.so, "system/app/X" for
/system/app/X on the system partition (system-as-root layout).
"""
import os
import re
import shutil
import subprocess

from .util import die, info, tool, run, mkdirs, rmtree

DEFAULT_LABELS = [  # used when no existing entry can be copied
    ("bin/", "u:object_r:{p}_bin_file:s0"),
    ("lib", "u:object_r:{p}_lib_file:s0"),
    ("etc/", "u:object_r:{p}_etc_file:s0"),
    ("firmware", "u:object_r:{p}_file:s0"),
]


def fs_type(image):
    """"erofs", "ext4" or None, from the superblock magic."""
    with open(image, "rb") as fh:
        fh.seek(1024)
        b = fh.read(64)
    if b[:4] == b"\xe2\xe1\xf5\xe0":
        return "erofs"
    if len(b) >= 58 and b[56:58] == b"\x53\xef":
        return "ext4"
    return None


def _dbg(image, cmd):
    """One debugfs -R command, stdout as bytes."""
    p = subprocess.run([tool("debugfs"), "-R", cmd, image], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return p.stdout


def _dbg_batch(image, cmds, workdir):
    """Many debugfs commands in one process: [(command, [output lines])] (debugfs echoes 'debugfs: <cmd>')."""
    script = os.path.join(workdir, "_batch.cmd")
    with open(script, "wb") as fh:
        fh.write(b"\n".join(c.encode() for c in cmds) + b"\n")
    out = []
    cur = None
    with subprocess.Popen([tool("debugfs"), "-f", script, image], stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL) as pr:
        for raw in pr.stdout:
            line = raw.decode(errors="replace").rstrip("\n")
            if line.startswith("debugfs: "):
                cur = (line[9:], [])
                out.append(cur)
            elif cur is not None:
                cur[1].append(line)
    os.remove(script)
    return out


def _q(path):
    return '"%s"' % path.replace("\\", "\\\\").replace('"', '\\"')


class Tree:
    def __init__(self, image, workdir, name):
        self.image = image
        self.name = name
        self.work = workdir
        self.root = os.path.join(workdir, name)          # mkfs source directory
        self.cfg_dir = os.path.join(workdir, "config")
        self.fs_config = os.path.join(self.cfg_dir, name + "_fs_config")
        self.contexts = os.path.join(self.cfg_dir, name + "_file_contexts")
        self.options = os.path.join(self.cfg_dir, name + "_fs_options")

    # ------------------------------------------------------------ extract
    def extract(self):
        rmtree(self.work)
        mkdirs(self.work)
        if fs_type(self.image) == "ext4":
            return self._extract_ext4()
        info("extracting %s" % os.path.basename(self.image))
        p = subprocess.run([tool("extract.erofs"), "-i", self.image, "-x", "-o", self.work, "-s", "-f", "-T4"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        # extract.erofs names the tree after the image file; normalise it
        produced = [d for d in os.listdir(self.work) if d != "config" and os.path.isdir(os.path.join(self.work, d))]
        if len(produced) != 1:
            die("extract.erofs failed for %s\n%s" % (self.image, p.stdout.decode(errors="replace")[-2000:]))
        if produced[0] != self.name:
            os.rename(os.path.join(self.work, produced[0]), self.root)
            for f in os.listdir(self.cfg_dir):
                if f.startswith(produced[0] + "_"):
                    os.rename(os.path.join(self.cfg_dir, f),
                              os.path.join(self.cfg_dir, self.name + f[len(produced[0]):]))
            for path in (self.fs_config, self.contexts):
                with open(path) as fh:
                    lines = fh.read().splitlines()
                pre_cfg, pre_ctx = produced[0] + "/", "/" + produced[0] + "/"
                out = []
                for l in lines:
                    if path == self.fs_config and (l.startswith(pre_cfg) or l.split(" ")[0] == produced[0]):
                        l = self.name + l[len(produced[0]):]
                    elif path == self.contexts and (l.startswith(pre_ctx) or l.split(" ")[0] == "/" + produced[0]):
                        l = "/" + self.name + l[len(produced[0]) + 1:]
                    out.append(l)
                with open(path, "w") as fh:
                    fh.write("\n".join(out) + "\n")
        self._repair()
        self._read_options()

    def _extract_ext4(self):
        """ext4 partition image (HarmonyOS 5 system): files with rdump, owners/modes/SELinux labels with debugfs,
        written as the same fs_config / file_contexts the EROFS path produces."""
        info("extracting %s (ext4)" % os.path.basename(self.image))
        mkdirs(self.cfg_dir)
        mkdirs(self.root)
        run([tool("debugfs"), "-R", "rdump / " + self.root, self.image], check=False)
        lost = os.path.join(self.root, "lost+found")
        if os.path.isdir(lost):
            rmtree(lost)
        dirs, entries = ["/"], []
        for r, ds, fs in os.walk(self.root):
            rel = os.path.relpath(r, self.root)
            rel = "/" if rel == "." else "/" + rel.replace(os.sep, "/")
            if rel != "/":
                dirs.append(rel)
            os.chmod(r, 0o755)
            for d in ds:
                if not os.path.islink(os.path.join(r, d)):
                    os.chmod(os.path.join(r, d), 0o755)
            for f in fs:
                fp = os.path.join(r, f)
                if not os.path.islink(fp):
                    os.chmod(fp, os.lstat(fp).st_mode | 0o600)
            entries += [(rel.rstrip("/") + "/" + n) for n in ds + fs]
        info("reading owners and modes of %d entries" % (len(entries) + 1))
        meta = {}
        for cmd, lines in _dbg_batch(self.image, ["ls -p " + _q(d) for d in dirs], self.work):
            for l in lines:
                pr = l.split("/")
                if len(pr) >= 7 and pr[5] not in (".", ".."):
                    base = cmd[len("ls -p "):].strip('"')
                    key = (base.rstrip("/") + "/" + pr[5]).replace("\\\\", "\\").replace('\\"', '"')
                    meta[key] = (int(pr[3]), int(pr[4]), int(pr[2], 8) & 0o7777)
        rootmeta = _dbg(self.image, "ls -p /").decode(errors="replace").splitlines()
        rootmode = (0, 0, 0o755)
        for l in rootmeta:
            pr = l.split("/")
            if len(pr) >= 6 and pr[5] == ".":
                rootmode = (int(pr[3]), int(pr[4]), int(pr[2], 8) & 0o7777)
        info("reading SELinux labels")
        labels = {}
        for cmd, lines in _dbg_batch(self.image, ["ea_list " + _q(p) for p in ["/"] + entries], self.work):
            for l in lines:
                m = re.match(r'\s+security\.selinux \(\d+\) = "(.*)"\s*$', l)
                if m:
                    labels[cmd[len("ea_list "):].strip('"').replace("\\\\", "\\").replace('\\"', '"')] = \
                        m.group(1).rstrip("\0")
        odd = 0
        with open(self.fs_config, "w") as fc, open(self.contexts, "w") as fx:
            for rel in ["/"] + sorted(entries):
                if re.search(r"\s", rel):          # fs_config / file_contexts cannot hold names with blanks
                    odd += 1
                    continue
                uid, gid, mode = rootmode if rel == "/" else meta.get(rel, (0, 0, 0o644))
                key = self.name if rel == "/" else self.name + rel
                fc.write("%s %d %d %04o\n" % (key, uid, gid, mode))
                lab = labels.get(rel)
                if lab:
                    ck = "/" + self.name + ("" if rel == "/" else rel)
                    fx.write("%s %s\n" % (re.sub(r"([.+*?\[\]()])", r"\\\1", ck), lab))
        if odd:
            info("note: %d entries with blanks in the name keep default owner/mode" % odd)
        self.uuid = None
        self.mount_point = "/" + self.name
        self.compression = "lz4hc"
        if not labels:
            info("note: the image carries no SELinux labels; defaults will be used for new files")

    def _repair(self):
        """extract.erofs cannot fill directories that are read-only (mode 0500)
        when not running as root: make them writable and pull the missing
        entries one by one with dump.erofs."""
        for r, dirs, _ in os.walk(self.root):
            for d in dirs:
                p = os.path.join(r, d)
                if not os.path.islink(p):
                    os.chmod(p, 0o755)
        missing = []
        with open(self.fs_config) as fh:
            for line in fh:
                key = line.split(" ")[0]
                rel = key[len(self.name):]
                if rel and not os.path.lexists(self.root + rel):
                    missing.append(rel)
        if not missing:
            return
        info("recovering %d entries from read-only directories" % len(missing))
        dump = tool("dump.erofs")
        for rel in missing:
            out = subprocess.run([dump, "--path=" + rel, self.image], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE).stdout.decode(errors="replace").splitlines()
            kind = out[1] if len(out) > 1 else ""
            dst = self.root + rel
            if "directory" in kind:
                os.makedirs(dst, exist_ok=True)
            elif "symlink" in kind:
                tgt = subprocess.run([dump, "--cat", "--path=" + rel, self.image], stdout=subprocess.PIPE).stdout
                mkdirs(os.path.dirname(dst))
                os.symlink(tgt.decode(), dst)
            elif "regular" in kind:
                mkdirs(os.path.dirname(dst))
                with open(dst, "wb") as o:
                    o.write(subprocess.run([dump, "--cat", "--path=" + rel, self.image],
                                           stdout=subprocess.PIPE).stdout)
            else:  # listed in fs_config but not in the image (lost+found): drop it
                self._drop_config(rel)

    def _read_options(self):
        self.uuid = None
        self.mount_point = "/" + self.name
        self.compression = "lz4hc"
        if os.path.isfile(self.options):
            txt = open(self.options).read()
            m = re.search(r"-U\s+([0-9a-f-]{36})", txt)
            if m:
                self.uuid = m.group(1)
            m = re.search(r"--mount-point=(\S+)", txt)
            if m:
                self.mount_point = m.group(1)
            m = re.search(r"-z(\S+)", txt)
            if m:
                self.compression = m.group(1)

    # ------------------------------------------------------------- labels
    def _ctx_key(self, rel):
        return "/" + self.name + "/" + rel.strip("/")

    def label_of(self, rel):
        key = self._ctx_key(rel) if rel.strip("/") else "/" + self.name
        with open(self.contexts) as fh:
            for line in fh:
                parts = line.split()
                if len(parts) >= 2 and parts[0].replace("\\", "") == key:
                    return parts[1]
        return None

    def _default_label(self, rel):
        for prefix, lab in DEFAULT_LABELS:
            if rel.startswith(prefix):
                return lab.format(p=self.name)
        return "u:object_r:%s_file:s0" % self.name

    def _has_config(self, rel):
        key = self.name + "/" + rel.strip("/")
        with open(self.fs_config) as fh:
            return any(l.split(" ")[0] == key for l in fh)

    def _add_config(self, rel, mode, uid=0, gid=2000, label=None, label_from=None):
        rel = rel.strip("/")
        if not self._has_config(rel):
            with open(self.fs_config, "a") as fh:
                fh.write("%s/%s %d %d %s\n" % (self.name, rel, uid, gid, mode))
        if self.label_of(rel) is None:
            lab = label or (label_from and self.label_of(label_from)) or self._nearest_label(rel)
            with open(self.contexts, "a") as fh:
                fh.write("%s %s\n" % (re.sub(r"([.+*?\[\]()])", r"\\\1", self._ctx_key(rel)), lab))

    def _nearest_label(self, rel):
        """Label of a sibling file of the same kind, else a sensible default."""
        d = os.path.dirname(rel)
        if os.path.isdir(self.path(rel)) and not os.path.islink(self.path(rel)):
            return self.label_of(d) or self._default_label(rel)
        if os.path.isdir(self.root + "/" + d):
            for f in sorted(os.listdir(self.root + "/" + d)):
                cand = (d + "/" + f).strip("/")
                if cand != rel and os.path.isfile(self.root + "/" + cand) and not os.path.islink(self.root + "/" + cand):
                    lab = self.label_of(cand)
                    if lab:
                        return lab
        return self._default_label(rel)

    def _drop_config(self, rel):
        rel = rel.strip("/")
        key = self.name + "/" + rel
        ctx = self._ctx_key(rel)
        for path, test in ((self.fs_config, lambda k: k == key or k.startswith(key + "/")),
                           (self.contexts, lambda k: k.replace("\\", "") == ctx or k.replace("\\", "").startswith(ctx + "/"))):
            with open(path) as fh:
                lines = fh.read().splitlines()
            with open(path, "w") as fh:
                fh.write("\n".join(l for l in lines if not (l.split() and test(l.split()[0]))) + "\n")

    # -------------------------------------------------------------- edits
    def path(self, rel):
        return self.root + "/" + rel.strip("/")

    def exists(self, rel):
        return os.path.lexists(self.path(rel))

    def add_dir(self, rel, mode="0755", label_from=None):
        for i in range(1, len(rel.strip("/").split("/")) + 1):
            part = "/".join(rel.strip("/").split("/")[:i])
            if not os.path.isdir(self.path(part)):
                os.makedirs(self.path(part), exist_ok=True)
            self._add_config(part, mode, label_from=label_from)

    def add_file(self, src, rel, mode="0644", label_from=None, data=None, uid=0, gid=2000):
        """Copy a file in (replacing a file or symlink with the same name)."""
        dst = self.path(rel)
        parent = os.path.dirname(rel.strip("/"))
        if parent and not os.path.isdir(self.path(parent)):
            self.add_dir(parent)
        if os.path.lexists(dst):
            os.remove(dst)
        if data is not None:
            with open(dst, "wb") as o:
                o.write(data)
        else:
            shutil.copyfile(src, dst)
        os.chmod(dst, int(mode, 8))
        self._add_config(rel, mode, uid=uid, gid=gid, label_from=label_from)

    def add_symlink(self, rel, target):
        dst = self.path(rel)
        parent = os.path.dirname(rel.strip("/"))
        if parent and not os.path.isdir(self.path(parent)):
            self.add_dir(parent)
        if os.path.lexists(dst):
            if os.path.isdir(dst) and not os.path.islink(dst):
                rmtree(dst)
            else:
                os.remove(dst)
        os.symlink(target, dst)
        self._add_config(rel, "0644", gid=0)

    def remove(self, rel):
        p = self.path(rel)
        if not os.path.lexists(p):
            return False
        if os.path.isdir(p) and not os.path.islink(p):
            rmtree(p)
        else:
            os.remove(p)
        self._drop_config(rel)
        return True

    def read(self, rel):
        with open(self.path(rel), "rb") as fh:
            return fh.read()

    def write(self, rel, data, mode="0644"):
        self.add_file(None, rel, mode, data=data if isinstance(data, bytes) else data.encode())

    # -------------------------------------------------------------- build
    def build(self, out_image, fs="erofs", headroom_pct=8, slack_mb=64):
        if fs == "ext4":
            return self._build_ext4(out_image, headroom_pct, slack_mb)
        info("building %s" % os.path.basename(out_image))
        cmd = [tool("mkfs.erofs"), "-z" + self.compression, "-T", "0"]
        if self.uuid:
            cmd += ["-U", self.uuid]
        cmd += ["--mount-point=" + self.mount_point, "--fs-config-file=" + self.fs_config,
                "--file-contexts=" + self.contexts, out_image, self.root]
        if os.path.exists(out_image):
            os.remove(out_image)
        run(cmd)
        run([tool("fsck.erofs"), out_image])
        return out_image

    # --------------------------------------------------------------- ext4
    def _estimate_size(self, headroom_pct, slack_mb):
        """Bytes needed for the tree as ext4: data blocks + a margin for inodes, directories and extents."""
        blk = 4096
        total = 0
        inodes = 0
        for r, dirs, files in os.walk(self.root):
            inodes += len(dirs) + len(files) + 1
            total += blk * (1 + len(dirs) // 64)
            for f in files:
                p = os.path.join(r, f)
                st = os.lstat(p)
                total += ((st.st_size + blk - 1) // blk) * blk if not os.path.islink(p) else 0
        total += inodes * 256                      # inode table (-I 256)
        total = int(total * (1 + headroom_pct / 100.0)) + slack_mb * 1024 * 1024
        return ((total + blk - 1) // blk) * blk

    def _meta_script(self):
        """debugfs commands that give every entry the uid/gid/mode of fs_config and the SELinux label of
        file_contexts (mke2fs -d copies the content as the current user, without labels)."""
        cfg = {}
        with open(self.fs_config) as fh:
            for line in fh:
                p = line.split()
                if len(p) >= 4:
                    cfg[p[0]] = (int(p[1]), int(p[2]), int(p[3], 8))
        ctx = {}
        with open(self.contexts) as fh:
            for line in fh:
                p = line.split()
                if len(p) >= 2:
                    ctx[p[0].replace("\\", "")] = p[1]
        cmds = []
        for key, (uid, gid, mode) in sorted(cfg.items()):
            rel = key[len(self.name):]                # "" for the root, "/lib64/x" otherwise
            src = self.root + rel
            if not os.path.lexists(src):
                continue
            st = os.lstat(src)
            full = (st.st_mode & 0o170000) | (mode & 0o7777)
            path = rel if rel else "/"
            q = '"%s"' % path.replace('"', '\\"')
            cmds.append("sif %s uid %d" % (q, uid))
            cmds.append("sif %s gid %d" % (q, gid))
            cmds.append("sif %s mode 0%o" % (q, full))
            lab = ctx.get("/" + self.name + rel) or (ctx.get("/" + self.name) if not rel else None)
            if lab:
                cmds.append('ea_set %s security.selinux "%s"' % (q, lab))
        return "\n".join(cmds) + "\n"

    def _build_ext4(self, out_image, headroom_pct, slack_mb):
        info("building %s (ext4)" % os.path.basename(out_image))
        size = self._estimate_size(headroom_pct, slack_mb)
        if os.path.exists(out_image):
            os.remove(out_image)
        with open(out_image, "wb") as fh:
            fh.truncate(size)
        run([tool("mke2fs"), "-q", "-F", "-t", "ext4", "-b", "4096", "-I", "256", "-m", "0",
             "-O", "^has_journal,^resize_inode,extent,huge_file,dir_nlink,extra_isize,uninit_bg",
             "-L", self.name, "-d", self.root, out_image])
        script = os.path.join(self.cfg_dir, self.name + "_debugfs.cmd")
        with open(script, "w") as fh:
            fh.write(self._meta_script())
        info("setting owners, modes and SELinux labels")
        run([tool("debugfs"), "-w", "-f", script, out_image])
        run([tool("e2fsck"), "-fn", out_image])
        return out_image


def read_file(image, path, workdir):
    """Extract one file from an EROFS image (path as on the device without mount point)."""
    if fs_type(image) == "ext4":
        data = _dbg(image, "cat " + _q("/" + path.strip("/")))
        return data or None
    tmp = mkdirs(os.path.join(workdir, "_one"))
    run([tool("extract.erofs"), "-i", image, "-X", "/" + path.strip("/"), "-o", tmp, "-s", "-f"], check=False)
    for r, _, files in os.walk(tmp):
        if os.path.basename(path) in files and r.replace("\\", "/").endswith(os.path.dirname("/" + path.strip("/"))):
            with open(os.path.join(r, os.path.basename(path)), "rb") as fh:
                data = fh.read()
            rmtree(tmp)
            return data
    rmtree(tmp)
    return None


def list_dir(image, path):
    """Names in one directory of an EROFS image: [(name, is_dir)], [] if missing."""
    if fs_type(image) == "ext4":
        out = []
        for l in _dbg(image, "ls -p " + _q("/" + path.strip("/"))).decode(errors="replace").splitlines():
            pr = l.split("/")
            if len(pr) >= 6 and pr[5] not in (".", ".."):
                out.append((pr[5], pr[2].startswith("04")))
        return out
    p = subprocess.run([tool("dump.erofs"), "--ls", "--path=/" + path.strip("/"), image],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, universal_newlines=True)
    out = []
    for line in p.stdout.splitlines():
        m = re.match(r"\s*(\d+)\s+(\d+)\s+(.+)$", line)
        if m and m.group(3) not in (".", ".."):
            out.append((m.group(3), m.group(2) == "2"))
    return out


def cat_file(image, path):
    """Contents of one (small) file of an EROFS image, None if missing."""
    if fs_type(image) == "ext4":
        return _dbg(image, "cat " + _q("/" + path.strip("/"))) or None
    p = subprocess.run([tool("dump.erofs"), "--cat", "--path=/" + path.strip("/"), image],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return p.stdout if p.returncode == 0 and p.stdout else None
