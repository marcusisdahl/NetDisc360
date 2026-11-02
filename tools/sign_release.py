"""Signs a release's NetDisc360.exe, so the in-app update can tell it is
really from this project even if the GitHub account or a release were taken
over (see NETDISC_UPDATE_KEY in server/version.h). Uses the openssl command
(Git for Windows has one: C:\\Program Files\\Git\\usr\\bin\\openssl.exe).

    python tools/sign_release.py new-key <key.pem>
        Makes a new ECDSA P-256 key in key.pem and prints the public key to
        put in server/version.h as NETDISC_UPDATE_KEY. Keep key.pem safe and
        out of the repository: whoever has it can sign updates. Lose it and
        the builds that have its public key can't be updated in the app any
        more (only by downloading the new exe by hand).

    python tools/sign_release.py sign <key.pem> build/NetDisc360.exe
        Writes build/NetDisc360.exe.sig: attach it to the release next to
        NetDisc360.exe.

    python tools/sign_release.py verify <public key hex> build/NetDisc360.exe
        Checks NetDisc360.exe.sig as the app would.
"""
import os
import shutil
import subprocess
import sys
import tempfile

# A P-256 public key in DER (SubjectPublicKeyInfo) is this header, then 04, X and Y.
SPKI_HEADER = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")


def openssl(*args, data=None):
    tool = shutil.which("openssl") or r"C:\Program Files\Git\usr\bin\openssl.exe"
    result = subprocess.run([tool] + list(args), input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        sys.exit("openssl %s failed: %s" % (args[0], (result.stderr or result.stdout).decode(errors="replace").strip()))
    return result.stdout


def public_hex(key):
    der = openssl("ec", "-in", key, "-pubout", "-outform", "DER")
    if not der.startswith(SPKI_HEADER) or len(der) != len(SPKI_HEADER) + 65 or der[len(SPKI_HEADER)] != 4:
        sys.exit("%s isn't a P-256 key." % key)
    return der[len(SPKI_HEADER) + 1:].hex()


def der_to_raw(der):
    """An ECDSA signature in DER (SEQUENCE of two INTEGERs) as r and s, 32 bytes each."""
    def integer(at):
        if der[at] != 2:
            raise ValueError("not an INTEGER")
        length = der[at + 1]
        value = der[at + 2:at + 2 + length].lstrip(b"\0")
        if len(value) > 32:
            raise ValueError("too long")
        return value.rjust(32, b"\0"), at + 2 + length
    if der[0] != 0x30:
        raise ValueError("not a SEQUENCE")
    at = 3 if der[1] & 0x80 else 2
    r, at = integer(at)
    s, _ = integer(at)
    return r + s


def raw_to_der(raw):
    def integer(value):
        value = value.lstrip(b"\0") or b"\0"
        if value[0] & 0x80:
            value = b"\0" + value
        return bytes([2, len(value)]) + value
    body = integer(raw[:32]) + integer(raw[32:])
    return bytes([0x30, len(body)]) + body


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "new-key":
        if os.path.exists(args[1]):
            sys.exit("%s is there already; not overwriting a key." % args[1])
        openssl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", args[1])
        print("Key written to %s. Keep it safe, and out of git." % args[1])
        print('In server/version.h:  #define NETDISC_UPDATE_KEY "%s"' % public_hex(args[1]))
    elif len(args) == 3 and args[0] == "sign":
        der = openssl("dgst", "-sha256", "-sign", args[1], args[2])
        with open(args[2] + ".sig", "w", newline="\n") as f:
            f.write(der_to_raw(der).hex() + "\n")
        print("Wrote %s.sig; attach it to the release next to %s." % (args[2], os.path.basename(args[2])))
        print("Signed with the key whose public part is %s." % public_hex(args[1]))
    elif len(args) == 3 and args[0] == "verify":
        key = bytes.fromhex(args[1])
        with open(args[2] + ".sig") as f:
            raw = bytes.fromhex(f.read().strip())
        if len(key) != 64 or len(raw) != 64:
            sys.exit("The key and the signature are 64 bytes each (128 hex digits).")
        with tempfile.TemporaryDirectory() as folder:
            pem = os.path.join(folder, "public.der")
            sig = os.path.join(folder, "sig.der")
            with open(pem, "wb") as f:
                f.write(SPKI_HEADER + b"\4" + key)
            with open(sig, "wb") as f:
                f.write(raw_to_der(raw))
            openssl("dgst", "-sha256", "-verify", pem, "-keyform", "DER", "-signature", sig, args[2])
        print("%s's signature matches." % args[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
