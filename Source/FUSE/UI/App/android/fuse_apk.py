#!/usr/bin/env python3
"""Packs a NativeActivity APK without Gradle or the SDK build-tools (fuse_app Android backend).

Writes the binary AndroidManifest.xml, stores the native libraries uncompressed and page aligned
(extractNativeLibs=false), deflates the assets (with an asset list the app extracts at start) and signs the
result with APK Signature Scheme v2 using a debug key generated next to the APK on first use.

  fuse_apk.py --out app.apk --package org.fuse.demo --label "Demo" --lib-name demo
              --lib arm64-v8a=libdemo.so [--lib x86_64=...] [--assets dir] [--min-sdk 26 --target-sdk 35]
              [--key debug.pem]

Needs Python 3.9+ and the `cryptography` package (signing).
"""
import argparse
import hashlib
import io
import os
import struct
import sys
import zipfile

# ---- binary XML (AXML) -------------------------------------------------------------------------------------------
ANDROID_NS = "http://schemas.android.com/apk/res/android"
ATTR_IDS = {
    "label": 0x01010001, "name": 0x01010003, "hasCode": 0x0101000C, "debuggable": 0x0101000F,
    "exported": 0x01010010, "launchMode": 0x0101001D, "screenOrientation": 0x0101001E,
    "configChanges": 0x0101001F, "value": 0x01010024, "minSdkVersion": 0x0101020C,
    "versionCode": 0x0101021B, "versionName": 0x0101021C, "targetSdkVersion": 0x01010270,
    "extractNativeLibs": 0x010104EA,
}
T_STRING, T_INT_DEC, T_INT_HEX, T_BOOL = 0x03, 0x10, 0x11, 0x12


class Elem:
    def __init__(self, tag, attrs=None, children=None):
        self.tag = tag
        self.attrs = attrs or []  # (android_ns: bool, name, value) value: str | int | bool | ('hex', int)
        self.children = children or []


def axml(root):
    # string pool: android attribute names first (in resource-id order, matched by the resource map)
    attr_names, others = [], []

    def walk(e):
        for ns, name, _ in e.attrs:
            if ns and name not in attr_names:
                attr_names.append(name)
        for c in e.children:
            walk(c)

    walk(root)
    attr_names.sort(key=lambda n: ATTR_IDS[n])
    strings = list(attr_names)

    def idx(s):
        if s not in strings:
            strings.append(s)
        return strings.index(s)

    body = io.BytesIO()
    idx("android")
    idx(ANDROID_NS)

    def chunk(kind, header, payload):
        return struct.pack("<HHI", kind, header, 8 + len(payload)) + payload

    def start(e, line):
        attrs = []
        for ns, name, v in e.attrs:
            raw = 0xFFFFFFFF
            if isinstance(v, bool):
                t, d = T_BOOL, 0xFFFFFFFF if v else 0
            elif isinstance(v, int):
                t, d = T_INT_DEC, v
            elif isinstance(v, tuple):
                t, d = T_INT_HEX, v[1]
            else:
                raw = idx(v)
                t, d = T_STRING, raw
            nsi = idx(ANDROID_NS) if ns else 0xFFFFFFFF
            attrs.append((ATTR_IDS.get(name, 0) if ns else 0xFFFFFFFF, nsi, idx(name), raw, t, d))
        attrs.sort(key=lambda a: a[0])  # android:* by resource id, plain attributes last
        p = struct.pack("<II", line, 0xFFFFFFFF) + struct.pack("<IIHHHHHH", 0xFFFFFFFF, idx(e.tag), 20, 20,
                                                                len(attrs), 0, 0, 0)
        for _, nsi, ni, raw, t, d in attrs:
            p += struct.pack("<IIIHBBI", nsi, ni, raw, 8, 0, t, d)
        body.write(chunk(0x0102, 16, p))
        for c in e.children:
            start(c, line + 1)
        body.write(chunk(0x0103, 16, struct.pack("<IIII", line, 0xFFFFFFFF, 0xFFFFFFFF, idx(e.tag))))

    ns_payload = struct.pack("<IIII", 1, 0xFFFFFFFF, idx("android"), idx(ANDROID_NS))
    body.write(chunk(0x0100, 16, ns_payload))
    start(root, 2)
    body.write(chunk(0x0101, 16, ns_payload))

    # UTF-8 string pool
    data = io.BytesIO()
    offsets = []
    for s in strings:
        offsets.append(data.tell())
        b = s.encode("utf-8")
        for n in (len(s), len(b)):
            data.write(bytes([n]) if n < 0x80 else bytes([0x80 | (n >> 8), n & 0xFF]))
        data.write(b + b"\0")
    while data.tell() % 4:
        data.write(b"\0")
    hdr = 28
    pool = struct.pack("<IIIII", len(strings), 0, 0x100, hdr + 4 * len(strings), 0)
    pool += b"".join(struct.pack("<I", o) for o in offsets) + data.getvalue()
    pool_chunk = struct.pack("<HHI", 0x0001, hdr, 8 + len(pool)) + pool
    resmap = b"".join(struct.pack("<I", ATTR_IDS[n]) for n in attr_names)
    resmap_chunk = struct.pack("<HHI", 0x0180, 8, 8 + len(resmap)) + resmap
    content = pool_chunk + resmap_chunk + body.getvalue()
    return struct.pack("<HHI", 0x0003, 8, 8 + len(content)) + content


def manifest(a):
    A = True
    config = 0x80 | 0x20 | 0x10 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000  # rotation, keyboard, size, density
    activity = Elem("activity", [(A, "name", "android.app.NativeActivity"), (A, "exported", True),
                                 (A, "configChanges", ("hex", config)), (A, "label", a.label)], [
        Elem("meta-data", [(A, "name", "android.app.lib_name"), (A, "value", a.lib_name)]),
        Elem("intent-filter", [], [
            Elem("action", [(A, "name", "android.intent.action.MAIN")]),
            Elem("category", [(A, "name", "android.intent.category.LAUNCHER")]),
        ]),
    ])
    app = Elem("application", [(A, "label", a.label), (A, "hasCode", False), (A, "debuggable", a.debuggable),
                               (A, "extractNativeLibs", False)], [activity])
    root = Elem("manifest", [(False, "package", a.package), (A, "versionCode", a.version_code),
                             (A, "versionName", a.version_name)], [
        Elem("uses-sdk", [(A, "minSdkVersion", a.min_sdk), (A, "targetSdkVersion", a.target_sdk)]),
        app,
    ])
    return axml(root)


# ---- zip with aligned stored entries -----------------------------------------------------------------------------
def add(zf, name, data, compress, align=4):
    info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    if not compress:
        # pad the local header's extra field (zipalign's 0xD935 record) so the data starts aligned
        offset = zf.fp.tell() + 30 + len(name.encode("utf-8"))
        pad = (-(offset + 6)) % align
        info.extra = struct.pack("<HHH", 0xD935, 2 + pad, align) + b"\0" * pad
    zf.writestr(info, data)


# ---- APK Signature Scheme v2 -------------------------------------------------------------------------------------
def lp(b):
    return struct.pack("<I", len(b)) + b


def chunk_digest(sections):
    digests = []
    for sec in sections:
        for i in range(0, len(sec), 1 << 20):
            c = sec[i:i + (1 << 20)]
            digests.append(hashlib.sha256(b"\xa5" + struct.pack("<I", len(c)) + c).digest())
    return hashlib.sha256(b"\x5a" + struct.pack("<I", len(digests)) + b"".join(digests)).digest()


def load_key(path):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    import datetime
    if os.path.exists(path):
        pem = open(path, "rb").read()
        key = serialization.load_pem_private_key(pem, None)
        cert = x509.load_pem_x509_certificate(pem[pem.index(b"-----BEGIN CERTIFICATE-----"):])
        return key, cert
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "FUSE Android Debug")])
    now = datetime.datetime(2020, 1, 1, tzinfo=datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(1).not_valid_before(now).not_valid_after(now + datetime.timedelta(days=365 * 30))
            .sign(key, hashes.SHA256()))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    return key, cert


def sign_v2(apk, key, cert):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    eocd = apk.rfind(b"PK\x05\x06")
    cd_offset, = struct.unpack_from("<I", apk, eocd + 16)
    entries, cd, end = apk[:cd_offset], apk[cd_offset:eocd], bytearray(apk[eocd:])
    struct.pack_into("<I", end, 16, cd_offset)  # digested with the CD offset = signing block offset
    alg = 0x0103  # RSASSA-PKCS1-v1_5 + SHA2-256
    digest = chunk_digest([entries, cd, bytes(end)])
    cert_der = cert.public_bytes(serialization.Encoding.DER)
    signed = lp(lp(struct.pack("<I", alg) + lp(digest))) + lp(lp(cert_der)) + lp(b"")
    sig = key.sign(signed, padding.PKCS1v15(), hashes.SHA256())
    pub = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    signer = lp(signed) + lp(lp(struct.pack("<I", alg) + lp(sig))) + lp(pub)
    value = lp(lp(signer))
    pair = struct.pack("<Q", 4 + len(value)) + struct.pack("<I", 0x7109871A) + value
    size = len(pair) + 8 + 16
    block = struct.pack("<Q", size) + pair + struct.pack("<Q", size) + b"APK Sig Block 42"
    end = bytearray(apk[eocd:])
    struct.pack_into("<I", end, 16, cd_offset + len(block))
    return entries + block + cd + bytes(end)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("--label", default="FUSE")
    ap.add_argument("--lib-name", required=True, help="android.app.lib_name (lib<name>.so)")
    ap.add_argument("--lib", action="append", default=[], help="abi=path/to/lib<name>.so (repeatable)")
    ap.add_argument("--assets", action="append", default=[], help="directory copied to assets/ (repeatable, prefix=dir allowed)")
    ap.add_argument("--min-sdk", type=int, default=26)
    ap.add_argument("--target-sdk", type=int, default=35)
    ap.add_argument("--version-code", type=int, default=1)
    ap.add_argument("--version-name", default="1.0")
    ap.add_argument("--release", dest="debuggable", action="store_false", help="not debuggable (no run-as)")
    ap.add_argument("--key", help="PEM with key + certificate (created when missing); default next to --out")
    a = ap.parse_args()

    files = []  # (apk path, bytes, compress)
    for spec in a.lib:
        abi, path = spec.split("=", 1)
        files.append((f"lib/{abi}/lib{a.lib_name}.so", open(path, "rb").read(), False))
    assets = []
    for spec in a.assets:
        prefix, d = spec.split("=", 1) if "=" in spec and not os.path.isdir(spec) else ("", spec)
        for root, _, names in os.walk(d):
            for n in sorted(names):
                full = os.path.join(root, n)
                rel = os.path.relpath(full, d).replace(os.sep, "/")
                assets.append(((prefix + "/" if prefix else "") + rel, open(full, "rb").read()))
    assets.sort()
    build_id = hashlib.sha256(b"".join(p.encode() + b"\0" + data for p, data in assets)).hexdigest()[:16]
    listing = "build " + build_id + "\n" + "".join(p + "\n" for p, _ in assets)

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        add(zf, "AndroidManifest.xml", manifest(a), True)
        for name, data, compress in files:
            add(zf, name, data, compress, 16384)  # page aligned: loaded straight from the APK (4K and 16K pages)
        add(zf, "assets/fuse_assets.txt", listing.encode(), True)
        for p, data in assets:
            add(zf, "assets/" + p, data, True)
    key, cert = load_key(a.key or os.path.join(os.path.dirname(os.path.abspath(a.out)), "fuse_debug_key.pem"))
    apk = sign_v2(buf.getvalue(), key, cert)
    tmp = a.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(apk)
    os.replace(tmp, a.out)
    print(f"fuse_apk: {a.out} ({len(apk)} bytes, {len(files)} libs, {len(assets)} assets)")


if __name__ == "__main__":
    sys.exit(main())
