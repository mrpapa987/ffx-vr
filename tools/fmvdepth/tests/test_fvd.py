import os
import struct
import tempfile
import unittest

import numpy as np

import helpers  # noqa: F401
import fvd


def _frames(n, w=320, h=180, seed=1):
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w]
    out = []
    for i in range(n):
        if i == 3:
            out.append(rng.integers(0, 256, (h, w)).astype(np.uint8))      # noise: will be stored, not compressed
        else:
            out.append(((xx * 255 // (w - 1) + yy + i * 7) % 256).astype(np.uint8))
    return out


class FvdTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, frames, codec=fvd.CODEC_ZLIB, thumbs=True, **kw):
        p = os.path.join(self.tmp.name, 'x.fvd')
        h, w = frames[0].shape
        wr = fvd.FvdWriter(p, w, h, 30000, 1001, codec=codec, model_id=fvd.MODEL_VDA_SMALL, input_w=924, input_h=518,
                           settings_hash=0x1122334455667788, src_name='OPN_us.webm', src_size=156893828,
                           src_offset=4230515860, thumb_w=16 if thumbs else 0, thumb_h=9 if thumbs else 0, **kw)
        for i, q in enumerate(frames):
            wr.add_frame(q, fvd.FF_CUT if i in (0, 4) else 0, p2=i + 0.25, p98=i + 2.5, lo=i, hi=i + 3,
                         thumb=np.full((9, 16), i, np.uint8) if thumbs else None)
        wr.close()
        return p

    def test_round_trip_zlib(self):
        fr = _frames(8)
        p = self._write(fr)
        with fvd.FvdReader(p) as r:
            h = r.h
            self.assertEqual((h.width, h.height, h.frame_count, h.fps_num, h.fps_den), (320, 180, 8, 30000, 1001))
            self.assertEqual((h.codec, h.predictor, h.model_id, h.input_w, h.input_h),
                             (fvd.CODEC_ZLIB, fvd.PRED_LEFT, fvd.MODEL_VDA_SMALL, 924, 518))
            self.assertEqual(h.settings_hash, 0x1122334455667788)
            self.assertEqual((h.src_name, h.src_size, h.src_offset), ('OPN_us.webm', 156893828, 4230515860))
            self.assertTrue(h.flags & fvd.HF_COMPLETE and h.flags & fvd.HF_THUMBS)
            self.assertAlmostEqual(h.fps, 29.97, places=2)
            for i in reversed(range(8)):                        # random access, any order
                np.testing.assert_array_equal(r.frame(i), fr[i])
                e = r.entry(i)
                self.assertEqual(bool(e.flags & fvd.FF_CUT), i in (0, 4))
                self.assertEqual((e.p2, e.p98, e.lo, e.hi), (i + 0.25, i + 2.5, i, i + 3))
                np.testing.assert_array_equal(r.thumb(i), np.full((9, 16), i, np.uint8))
            self.assertTrue(r.entry(3).flags & fvd.FF_STORED)   # noise did not compress
            self.assertFalse(r.entry(0).flags & fvd.FF_STORED)
            self.assertLess(r.entry(0).size, 320 * 180 // 4)
        # layout: header, payloads, index, thumbs, nothing else
        size = os.path.getsize(p)
        self.assertEqual(size, h.thumbs_offset + 8 * 144)
        self.assertEqual(h.thumbs_offset, h.index_offset + 8 * fvd.INDEX_ENTRY)

    def test_round_trip_stored_no_thumbs_4x3(self):
        fr = _frames(3, w=240, h=180)
        p = self._write(fr, codec=fvd.CODEC_STORED, thumbs=False)
        with fvd.FvdReader(p) as r:
            self.assertEqual((r.h.width, r.h.thumbs_offset, r.h.flags & fvd.HF_THUMBS), (240, 0, 0))
            self.assertIsNone(r.thumb(0))
            for i in range(3):
                np.testing.assert_array_equal(r.frame(i), fr[i])

    def test_header_bytes(self):
        """The fixed offsets FORMAT.md documents (what a C++ reader uses)."""
        p = self._write(_frames(2))
        with open(p, 'rb') as f:
            b = f.read(128)
        self.assertEqual(b[0:4], b'FVD1')
        self.assertEqual(struct.unpack_from('<HHHH', b, 4), (1, 128, 320, 180))
        self.assertEqual(struct.unpack_from('<III', b, 12), (2, 30000, 1001))
        self.assertEqual(b[24], fvd.CODEC_ZLIB)
        self.assertEqual(b[27] & 3, 3)
        self.assertEqual(struct.unpack_from('<Q', b, 32)[0], 0x1122334455667788)
        idx_off, th_off = struct.unpack_from('<QQ', b, 40)
        self.assertEqual(struct.unpack_from('<HH', b, 56), (16, 9))
        self.assertEqual(struct.unpack_from('<QQ', b, 64), (156893828, 4230515860))
        self.assertEqual(b[80:92], b'OPN_us.webm\0')
        with open(p, 'rb') as f:
            f.seek(idx_off)
            off, size, flags, _r, p2, p98, lo, hi = struct.unpack('<QIHHffff', f.read(32))
        self.assertEqual((off, flags, p2, p98, lo, hi), (128, fvd.FF_CUT, 0.25, 2.5, 0.0, 3.0))
        self.assertEqual(th_off, idx_off + 2 * 32)

    def test_incomplete_and_corrupt(self):
        p = os.path.join(self.tmp.name, 'part.fvd')
        wr = fvd.FvdWriter(p, 320, 180, 30, 1)
        wr.add_frame(_frames(1)[0])
        wr.f.flush()
        with self.assertRaises(fvd.FvdError):
            fvd.FvdReader(p)                            # no HF_COMPLETE yet
        self.assertFalse(fvd.read_header(p).flags & fvd.HF_COMPLETE)
        wr.abort()
        self.assertFalse(os.path.exists(p))
        p = self._write(_frames(2))
        with open(p, 'r+b') as f:
            f.truncate(os.path.getsize(p) - 300)       # cut into the index / thumbs
        with self.assertRaises(fvd.FvdError):
            fvd.FvdReader(p)
        with open(p, 'wb') as f:
            f.write(b'nope' * 40)
        self.assertIsNone(fvd.read_header(p))

    def test_bad_frame_shape(self):
        p = os.path.join(self.tmp.name, 'y.fvd')
        wr = fvd.FvdWriter(p, 320, 180, 30, 1)
        with self.assertRaises(fvd.FvdError):
            wr.add_frame(np.zeros((180, 321), np.uint8))
        with self.assertRaises(fvd.FvdError):
            wr.add_frame(np.zeros((180, 320), np.float32))
        wr.abort()

    def test_predictor(self):
        q = np.arange(256, dtype=np.uint8).reshape(16, 16)
        r = np.frombuffer(fvd.left_predict(q), np.uint8).reshape(16, 16)
        self.assertEqual(r[0, 0], 0)
        self.assertTrue((r[:, 1:] == 1).all())          # left differences
        self.assertTrue((r[1:, 0] == 16).all())         # column 0: difference to the row above
        np.testing.assert_array_equal(fvd.left_unpredict(r.tobytes(), 16, 16), q)

    @unittest.skipUnless(os.name == 'nt', 'XPRESS_HUFF needs Windows')
    def test_round_trip_xpress(self):
        fr = _frames(6)
        p = self._write(fr, codec=fvd.CODEC_XPRESS_HUFF)
        with fvd.FvdReader(p) as r:
            for i in range(6):
                np.testing.assert_array_equal(r.frame(i), fr[i])


if __name__ == '__main__':
    unittest.main()
