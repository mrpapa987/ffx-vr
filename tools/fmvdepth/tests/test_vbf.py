import io
import os
import random
import tempfile
import unittest

import helpers  # noqa: F401  (sys.path)
import vbf

VD = vbf.VIDEO_DIR


def _files():
    rnd = random.Random(7)
    noise = bytes(rnd.getrandbits(8) for _ in range(3 * 65536 + 1234))          # incompressible: raw blocks
    text = (b'abcdefgh' * 40000)[:5 * 65536]                                    # compressible, exact block multiple
    lst = (b'0:,OPN_us.webm,OPN_us.webm\r\n2:,IWK_us.webm,IWK_us.webm\r\n'
           b'114:,1_480_new.webm,1_480_new.webm\r\n\r\njunk line\r\n')
    return {
        VD + 'opn_us.webm': noise,
        VD + 'iwk_us.webm': text,
        VD + '1_480_new.webm': b'x' * 10,
        VD + 'us/ffx_videolist.txt': lst,
        VD + '32.dat': b'\0' * 16,
        'ffx_data/other/file.bin': text[:70000],
    }


class VbfTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.path = os.path.join(cls.tmp.name, 'T.vbf')
        cls.files = _files()
        helpers.write_vbf(cls.path, cls.files)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_index_and_read(self):
        a = vbf.VbfArchive(self.path)
        self.assertEqual(sorted(a.entries), sorted(self.files))
        for k, d in self.files.items():
            e = a.get(k.upper())                        # case-insensitive lookup
            self.assertEqual(e.size, len(d))
            self.assertEqual(a.read(e), d, k)
            buf = io.BytesIO()
            self.assertEqual(a.stream_to(e, buf), len(d))
            self.assertEqual(buf.getvalue(), d)

    def test_mixed_blocks_present(self):
        a = vbf.VbfArchive(self.path)
        e = a.get(VD + 'iwk_us.webm')
        sizes = list(a.blocks[e.first_block:e.first_block + 5])
        self.assertIn(0, sizes)                         # a raw full block
        self.assertTrue(any(0 < s < 65536 for s in sizes))   # zlib blocks
        self.assertLess(a.stored_size(e), e.size)

    def test_video_list(self):
        a = vbf.VbfArchive(self.path)
        self.assertEqual(vbf.read_video_list(a, 'us'), [(0, 'OPN_us.webm'), (2, 'IWK_us.webm'), (114, '1_480_new.webm')])
        self.assertEqual(vbf.all_videos(a), [VD + '1_480_new.webm', VD + 'iwk_us.webm', VD + 'opn_us.webm'])
        with self.assertRaises(vbf.VbfError):
            vbf.read_video_list(a, 'jp')               # not in this archive
        with self.assertRaises(vbf.VbfError):
            vbf.read_video_list(a, 'xx')

    def test_errors(self):
        bad = os.path.join(self.tmp.name, 'bad.vbf')
        with open(bad, 'wb') as f:
            f.write(b'NOPE' + b'\0' * 60)
        with self.assertRaises(vbf.VbfError):
            vbf.VbfArchive(bad)
        a = vbf.VbfArchive(self.path)
        with self.assertRaises(vbf.VbfError):
            a.get('no/such/file')
        with self.assertRaises(vbf.VbfError):
            a.read(a.get(VD + 'opn_us.webm'), limit=1000)

    def test_truncated_archive(self):
        cut = os.path.join(self.tmp.name, 'cut.vbf')
        with open(self.path, 'rb') as f:
            d = f.read()
        with open(cut, 'wb') as f:
            f.write(d[:-5000])
        a = vbf.VbfArchive(cut)
        last = max(a.entries.values(), key=lambda e: e.offset)
        with self.assertRaises(vbf.VbfError):
            a.read(last)


if __name__ == '__main__':
    unittest.main()
