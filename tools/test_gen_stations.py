#!/usr/bin/env python3
"""Smoke test for tools/gen_stations.py.

Runs the generator against a local MegaNet checkout into a temporary file and checks the
invariants the C decoder relies on. Point it at a checkout with $MEGANET_DIR; it falls
back to /home/user/MegaNet.

    python3 tools/test_gen_stations.py
"""

import os
import re
import bisect
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GEN = os.path.join(HERE, "gen_stations.py")
FILTER = os.path.join(ROOT, "stations.filter")
MEGANET = os.environ.get("MEGANET_DIR") or "/home/user/MegaNet"
MAX_BYTES = 6400

# A site from the default filter (Mt Kanigan covers the Burnett). 728 is a non-base
# offset, so this also exercises the 3-bits-per-slot unpacking.
KNOWN_ID = 728
KNOWN_NAME = "MONTO"
KNOWN_KIND = 2  # LEVEL

ROW = re.compile(r"^\t\{\s*(\d+),\s*0x([0-9a-f]{4}),\s*(\d+)\s*\},\s*//\s*(.*?)\s{2}\(")
POOL = re.compile(r'^\t"(.*)\\0"$')
SUMMARY = re.compile(r"^// Sites: (\d+)\s+Addresses covered: (\d+)\s+Table bytes: (\d+)", re.M)


def generate():
    fd, path = tempfile.mkstemp(suffix=".h", prefix="alert_stations_")
    os.close(fd)
    proc = subprocess.run(
        [sys.executable, GEN, "--meganet-dir", MEGANET, "--filter", FILTER,
         "--out", path, "--max-bytes", str(MAX_BYTES)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    out = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0:
        os.unlink(path)
        raise AssertionError("generator failed (%d):\n%s" % (proc.returncode, out))
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    os.unlink(path)
    return text


class GenStationsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.isdir(MEGANET):
            raise unittest.SkipTest("no MegaNet checkout at %s" % MEGANET)
        cls.text = generate()
        cls.sites = []       # (base_id, kinds, name_off, comment name)
        cls.names = []
        for line in cls.text.splitlines():
            m = ROW.match(line)
            if m:
                cls.sites.append((int(m.group(1)), int(m.group(2), 16),
                                  int(m.group(3)), m.group(4)))
                continue
            m = POOL.match(line)
            if m:
                cls.names.append(m.group(1))
        cls.pool = b"".join(n.encode("ascii") + b"\0" for n in cls.names)
        m = SUMMARY.search(cls.text)
        assert m, "no summary comment in header"
        cls.declared = (int(m.group(1)), int(m.group(2)), int(m.group(3)))

    def test_parsed_something(self):
        self.assertTrue(self.sites, "no site rows parsed")
        self.assertTrue(self.names, "no name pool parsed")
        self.assertEqual(len(self.sites), self.declared[0])
        self.assertIn("#define ALERT_STATIONS_COUNT %d" % len(self.sites), self.text)

    def test_base_ids_sorted_and_unique(self):
        bases = [s[0] for s in self.sites]
        self.assertEqual(bases, sorted(bases), "gAlertSites is not sorted by base_id")
        self.assertEqual(len(bases), len(set(bases)), "duplicate base_id")
        for b in bases:
            self.assertTrue(0 < b <= 0x1FFF, "base_id %d outside the 13-bit range" % b)

    def test_name_offsets_are_valid(self):
        for base, _kinds, off, comment in self.sites:
            self.assertTrue(0 <= off < len(self.pool),
                            "site %d: name_off %d outside the pool" % (base, off))
            self.assertTrue(off == 0 or self.pool[off - 1] == 0,
                            "site %d: name_off %d is not at a string start" % (base, off))
            end = self.pool.find(b"\0", off)
            self.assertNotEqual(end, -1,
                                "site %d: name at %d is not NUL terminated" % (base, off))
            name = self.pool[off:end].decode("ascii")
            self.assertEqual(name, comment,
                             "site %d: pool says %r, comment says %r" % (base, name, comment))
            self.assertLessEqual(len(name), 13, "site %d: name longer than 13" % base)
            self.assertEqual(name, name.rstrip(), "site %d: trailing space" % base)

    def test_kind_codes_in_range(self):
        for base, kinds, _off, _c in self.sites:
            self.assertEqual(kinds >> 15, 0, "site %d: bit 15 set in kinds" % base)
            for slot in range(5):
                k = (kinds >> (3 * slot)) & 7
                self.assertLessEqual(k, 6, "site %d: kind %d at +%d" % (base, k, slot))
            self.assertNotEqual(kinds & 7, 0, "site %d: base address has no kind" % base)

    def lookup(self, aid):
        """Mirror of the C lookup: binary-search the greatest base_id <= aid."""
        bases = [s[0] for s in self.sites]
        i = bisect.bisect_right(bases, aid) - 1
        if i < 0:
            return None
        base, kinds, off, _c = self.sites[i]
        slot = aid - base
        if slot >= 5:
            return None
        kind = (kinds >> (3 * slot)) & 7
        if kind == 0:
            return None
        end = self.pool.find(b"\0", off)
        return self.pool[off:end].decode("ascii"), kind

    def test_known_address(self):
        hit = self.lookup(KNOWN_ID)
        self.assertIsNotNone(hit, "%d not resolvable in the table" % KNOWN_ID)
        self.assertEqual(hit, (KNOWN_NAME, KNOWN_KIND))

    def test_no_site_shadows_another(self):
        """Every covered address must resolve back to the site that declared it."""
        for base, kinds, off, comment in self.sites:
            for slot in range(5):
                if not (kinds >> (3 * slot)) & 7:
                    continue
                hit = self.lookup(base + slot)
                self.assertIsNotNone(hit, "address %d is unreachable" % (base + slot))
                self.assertEqual(hit[0], comment,
                                 "address %d resolves to %r, not %r"
                                 % (base + slot, hit[0], comment))

    def test_al_marker_stripped(self):
        """The "AL"/"ALERT" station marker must not survive into the name pool."""
        for name in self.names:
            self.assertFalse(name.endswith(" AL") or name.endswith(" ALERT"),
                             "name %r still carries the AL marker" % name)

    def test_size_within_budget(self):
        total = 6 * len(self.sites) + len(self.pool)
        self.assertEqual(total, self.declared[2],
                         "declared table bytes %d != computed %d" % (self.declared[2], total))
        self.assertLessEqual(total, MAX_BYTES,
                             "table is %d bytes, over the %d budget" % (total, MAX_BYTES))

    def test_addresses_covered_matches(self):
        covered = 0
        for _base, kinds, _off, _c in self.sites:
            covered += sum(1 for slot in range(5) if (kinds >> (3 * slot)) & 7)
        self.assertEqual(covered, self.declared[1])

    def test_deterministic(self):
        self.assertEqual(self.text, generate(), "two runs produced different output")


    def test_header_carries_no_meganet_commit(self):
        """The header must not stamp MegaNet's HEAD commit.

        Anything in this file is compiled into the firmware image, so a commit id
        here would rewrite both binaries on every unrelated push to MegaNet even
        though the station table is identical. The provenance lives in
        docs/firmware/manifest.json instead; the header carries a fingerprint of
        the table's own contents.
        """
        text = self.text
        self.assertIsNotNone(re.search(r"^// Sites: .*Data: [0-9a-f]{7}$", text, re.M),
                             "header should carry a data fingerprint")
        m = re.search(r'#define ALERT_STATIONS_SOURCE "MegaNet:([0-9a-f]{7})"', text)
        self.assertIsNotNone(m, "ALERT_STATIONS_SOURCE should be MegaNet:<fingerprint>")

        # the fingerprint in the summary comment and the define must agree
        summary = re.search(r"Data: ([0-9a-f]{7})", text).group(1)
        self.assertEqual(summary, m.group(1))

        # and nothing that looks like a git short sha should be attributed to MegaNet
        self.assertNotRegex(text, r"MegaNet\s*@\s*[0-9a-f]{7}",
                            "the MegaNet commit must not appear in the compiled header")

if __name__ == "__main__":
    unittest.main(verbosity=2)
