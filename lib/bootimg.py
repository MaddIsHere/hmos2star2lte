"""boot.img for sweet: LineageOS kernel + HarmonyOS ramdisk with hmos-preinit as /init.

The OTA's ramdisk.img is a "HARMONY!" container around a gzip'd newc cpio.
HarmonyOS keeps its real first-stage init at /bin/init_early; hmos-preinit is
added as /init (it maps the super partition, starts the kernel log keeper and
then execs /bin/init_early).
"""
import gzip
import hashlib
import io
import os
import struct
import zlib

from .util import die, info, warn

CPIO_MAGIC = b"070701"


def unpack_ramdisk(data):
    """Return the cpio archive inside a HarmonyOS ramdisk.img (or a plain gzip/cpio)."""
    if data.startswith(CPIO_MAGIC):
        return data
    start = data.find(b"\x1f\x8b\x08")
    if start < 0 or start > 1 << 20:
        die("ramdisk.img: no gzip payload found (unknown format)")
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    cpio = d.decompress(data[start:])
    if not cpio.startswith(CPIO_MAGIC):
        die("ramdisk.img: payload is not a newc cpio archive")
    return cpio


def read_cpio(cpio):
    """Parse a newc archive into a list of [name, mode, ino-fields..., data]."""
    entries = []
    p = 0
    while True:
        if cpio[p:p + 6] != CPIO_MAGIC:
            die("corrupt cpio at %d" % p)
        f = [int(cpio[p + 6 + 8 * i:p + 14 + 8 * i], 16) for i in range(13)]
        namesize, filesize = f[11], f[6]
        name = cpio[p + 110:p + 110 + namesize - 1].decode()
        p = (p + 110 + namesize + 3) & ~3
        body = cpio[p:p + filesize]
        p = (p + filesize + 3) & ~3
        if name == "TRAILER!!!":
            break
        entries.append({"name": name, "mode": f[1], "uid": f[2], "gid": f[3], "nlink": f[4],
                        "mtime": f[5], "rdevmajor": f[9], "rdevminor": f[10], "data": body})
    return entries


def write_cpio(entries):
    out = io.BytesIO()
    ino = 300000
    for e in sorted(entries, key=lambda e: e["name"]) + [{"name": "TRAILER!!!", "mode": 0, "uid": 0, "gid": 0,
                                                          "nlink": 1, "mtime": 0, "rdevmajor": 0,
                                                          "rdevminor": 0, "data": b""}]:
        name = e["name"].encode() + b"\0"
        ino += 1
        hdr = CPIO_MAGIC + b"".join(b"%08X" % v for v in (
            ino, e["mode"], 0, 0, e.get("nlink", 1), 0, len(e["data"]), 0, 0,
            e.get("rdevmajor", 0), e.get("rdevminor", 0), len(name), 0))
        out.write(hdr + name)
        out.write(b"\0" * ((-out.tell()) % 4))
        out.write(e["data"])
        out.write(b"\0" * ((-out.tell()) % 4))
    return out.getvalue()


def build_ramdisk(ota_ramdisk, preinit, conf=None):
    entries = read_cpio(unpack_ramdisk(open(ota_ramdisk, "rb").read()))
    names = {e["name"] for e in entries}
    if "bin/init_early" not in names:
        die("the OTA ramdisk has no /bin/init_early: unsupported HarmonyOS ramdisk layout")
    entries = [e for e in entries if e["name"] not in ("init", "hmoslog")]
    entries.append({"name": "init", "mode": 0o100755, "uid": 0, "gid": 0, "nlink": 1, "mtime": 0,
                    "rdevmajor": 0, "rdevminor": 0, "data": open(preinit, "rb").read()})
    if conf:
        entries = [e for e in entries if e["name"] != "hmos.conf"]
        entries.append({"name": "hmos.conf", "mode": 0o100644, "uid": 0, "gid": 0, "nlink": 1, "mtime": 0,
                        "rdevmajor": 0, "rdevminor": 0, "data": conf.encode() + b"\n"})
    entries.append({"name": "hmoslog", "mode": 0o040755, "uid": 0, "gid": 0, "nlink": 2, "mtime": 0,
                    "rdevmajor": 0, "rdevminor": 0, "data": b""})
    info("ramdisk: %d entries, /init = hmos-preinit" % len(entries))
    return gzip.compress(write_cpio(entries), 9, mtime=0)


def build_boot(kernel, ramdisk, dtb, cmdline, out, pagesize=4096, os_version=0x220001aa):
    """Android boot image header v2, the layout the Xiaomi ABL on sweet boots."""
    k = open(kernel, "rb").read()
    d = open(dtb, "rb").read()
    r = ramdisk
    cmd = cmdline.encode()
    main, extra = cmd[:511], cmd[511:]
    if len(extra) >= 1024:
        die("kernel command line too long")
    sha = hashlib.sha1()
    for blob in (k, r, b"", b"", d):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    hdr = struct.pack("<8s10I16s512s32s1024sIQIIQ", b"ANDROID!", len(k), 0x8000, len(r), 0x01000000, 0, 0,
                      0x100, pagesize, 2, os_version, b"", main, sha.digest(), extra, 0, 0, 1660, len(d),
                      0x01f00000)

    def pad(b):
        return b + b"\0" * ((-len(b)) % pagesize)

    with open(out, "wb") as o:
        o.write(pad(hdr) + pad(k) + pad(r) + pad(d))
    info("boot.img: kernel %d, ramdisk %d, dtb %d bytes" % (len(k), len(r), len(d)))


# --------------------------------------------------------------------------------------------------
# Boot image built from a stock template (Samsung/Exynos: header v0/v1, DTB appended to the kernel or
# header v2). The header fields (page size, load addresses, OS version, command line) come from the
# phone's own boot.img, so nothing about the bootloader is guessed.
# --------------------------------------------------------------------------------------------------
def parse_header(path):
    """Fields of an Android boot image header (v0..v2) from a stock boot.img."""
    with open(path, "rb") as fh:
        raw = fh.read(2048)
    if raw[:8] != b"ANDROID!":
        die("%s is not an Android boot image (no ANDROID! magic)" % path)
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr, tags_addr, page_size,
     header_version, os_version) = struct.unpack_from("<10I", raw, 8)
    dt_size = 0
    if header_version > 2:      # v0 header: this word is dt_size (the S9+ has a 300 KB device tree here)
        dt_size, header_version = header_version, 0
    name = raw[48:64].split(b"\0")[0]
    cmd = raw[64:576].split(b"\0")[0] + raw[608:1632].split(b"\0")[0]
    return {"kernel_addr": kernel_addr, "ramdisk_addr": ramdisk_addr, "second_addr": second_addr,
            "tags_addr": tags_addr, "page_size": page_size, "header_version": header_version,
            "os_version": os_version, "name": name, "cmdline": cmd.decode(errors="replace"),
            "kernel_size": kernel_size, "ramdisk_size": ramdisk_size, "second_size": second_size,
            "dt_size": dt_size}


def extract_dt(path, tpl):
    """The device-tree blob of a v0 boot image (stock boot.img, the whole file)."""
    pg = tpl["page_size"]
    n = lambda x: (x + pg - 1) // pg
    off = pg * (1 + n(tpl["kernel_size"]) + n(tpl["ramdisk_size"]) + n(tpl["second_size"]))
    with open(path, "rb") as fh:
        fh.seek(off)
        dt = fh.read(tpl["dt_size"])
    if len(dt) != tpl["dt_size"] or dt[:4] not in (b"\xd0\x0d\xfe\xed", b"QCDT", b"DTBH", b"\x00\x00\x00\x00"):
        warn("the device tree read from %s does not look like a DTB/DTBH table (size %d, magic %r)" % (path, len(dt), dt[:4]))
    return dt


def build_boot_from_template(tpl, kernel, ramdisk, cmdline, out, dtb=None, samsung_trailer=True):
    """boot.img with the header values of `tpl` (parse_header). Header v0/v1: the DTB must already be
    appended to the kernel (Image.gz-dtb). Header v2: pass the DTB separately."""
    k = open(kernel, "rb").read()
    r = ramdisk
    ver = tpl["header_version"]
    d = open(dtb, "rb").read() if dtb else b""
    dt_v0 = tpl.get("dt_size", 0) if ver == 0 else 0
    if dt_v0 and not d:
        die("the stock boot image carries a %d byte device tree (v0 header): pass --dtb, or the full --stock-boot" % dt_v0)
    if ver == 2 and not d:
        die("the stock boot image uses header v2: pass the DTB with --dtb")
    if ver in (1,) and dtb:
        info("header v1: --dtb ignored, append the DTB to the kernel (Image.gz-dtb)")
        d = b""
    if ver == 0 and not dt_v0:
        d = b""
    pagesize = tpl["page_size"]
    cmd = cmdline.encode()
    main, extra = cmd[:511], cmd[511:]
    if len(extra) >= 1024:
        die("kernel command line too long (%d bytes)" % len(cmd))
    blobs = [k, r, b""] + ([b""] if ver >= 1 else []) + ([d] if ver >= 2 or dt_v0 else [])
    sha = hashlib.sha1()
    for blob in blobs:
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    word9 = len(d) if dt_v0 else ver            # v0: dt_size, v1/v2: header version
    hdr = struct.pack("<8s10I16s512s32s1024s", b"ANDROID!", len(k), tpl["kernel_addr"], len(r),
                      tpl["ramdisk_addr"], 0, tpl["second_addr"], tpl["tags_addr"], pagesize, word9,
                      tpl["os_version"], tpl["name"], main, sha.digest().ljust(32, b"\0"), extra)
    if ver >= 1:
        hdr += struct.pack("<IQI", 0, 0, 1648 if ver == 1 else 1660)
    if ver >= 2:
        hdr += struct.pack("<IQ", len(d), 0x01f00000)

    def pad(b):
        return b + b"\0" * ((-len(b)) % pagesize)

    body = pad(hdr) + pad(k) + pad(r) + (pad(d) if (ver >= 2 or dt_v0) else b"")
    with open(out, "wb") as o:
        o.write(body)
        if samsung_trailer:
            o.write(b"SEANDROIDENFORCE")
    info("boot.img (header v%d, page %d): kernel %d, ramdisk %d bytes" % (ver, pagesize, len(k), len(r)))
