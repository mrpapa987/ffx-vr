"""End to end without torch: synthetic videos -> synthetic VBF -> convert.py (fake model, zlib) -> .fvd -> previews.
Needs ffmpeg and ffprobe on PATH (skipped otherwise). Everything is generated here: no game data."""
import json
import os
import shutil
import subprocess
import tempfile
import unittest

import numpy as np

import helpers
import convert
import depthproc
import fvd
import preview
import vbf
import vda_model
from window import windowed_depth

FF = shutil.which('ffmpeg')
FP = shutil.which('ffprobe')


def _encoders():
    try:
        return subprocess.run([FF, '-hide_banner', '-encoders'], capture_output=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired, TypeError):
        return b''


def make_clip(path, w, h, seconds_a, seconds_b):
    """Two scenes (a test pattern, then colour bars): one cut. VP8 + Vorbis in WebM when the build has them."""
    enc = _encoders()
    v = ['-c:v', 'libvpx', '-b:v', '800k', '-f', 'webm'] if b' libvpx ' in enc else ['-c:v', 'mpeg4', '-f', 'matroska']
    au = ['-c:a', 'libvorbis'] if b' libvorbis ' in enc else ['-an']
    r = '30000/1001'
    cmd = [FF, '-hide_banner', '-loglevel', 'error', '-y',
           '-f', 'lavfi', '-i', f'testsrc2=s={w}x{h}:r={r}:d={seconds_a}',
           '-f', 'lavfi', '-i', f'smptebars=s={w}x{h}:r={r}:d={seconds_b}',
           '-f', 'lavfi', '-i', f'sine=f=440:d={seconds_a + seconds_b}',
           '-filter_complex', '[0:v][1:v]concat=n=2:v=1[v]', '-map', '[v]'] + \
          (['-map', '2:a'] if au[0] != '-an' else []) + v + au + [path]
    subprocess.run(cmd, check=True, timeout=120)
    with open(path, 'rb') as f:
        return f.read()


def count_frames(path):
    out = subprocess.run([FP, '-v', 'error', '-count_frames', '-select_streams', 'v:0', '-show_entries',
                          'stream=nb_read_frames', '-of', 'csv=p=0', path], capture_output=True, timeout=120)
    return int(out.stdout.decode().strip())


@unittest.skipUnless(FF and FP, 'ffmpeg / ffprobe not on PATH')
class PipelineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        t = cls.tmp.name
        a = make_clip(os.path.join(t, 'a.webm'), 320, 180, 1.5, 1.2)
        b = make_clip(os.path.join(t, 'b.webm'), 160, 120, 0.6, 0.5)
        cls.frames_a = count_frames(os.path.join(t, 'a.webm'))
        cls.frames_b = count_frames(os.path.join(t, 'b.webm'))
        vd = vbf.VIDEO_DIR
        lst = b'0:,TST_us.webm,TST_us.webm\r\n106:,Four_480.webm,Four_480.webm\r\n'
        cls.vbf = os.path.join(t, 'data', 'FFX_Data.vbf')
        os.makedirs(os.path.dirname(cls.vbf))
        helpers.write_vbf(cls.vbf, {vd + 'tst_us.webm': a, vd + 'four_480.webm': b, vd + 'us/ffx_videolist.txt': lst,
                                    vd + 'extra_jp.webm': b})
        cls.out = os.path.join(t, 'mods', 'ffxvr', 'fmvdepth')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def manifest(self):
        with open(os.path.join(self.out, 'manifest.json'), encoding='utf-8') as f:
            return json.load(f)

    def run_convert(self, *extra):
        return convert.main(['--vbf', self.vbf, '--out', self.out, '--model', 'fake', '--codec', 'zlib',
                             '--ffmpeg', FF, '--ffprobe', FP] + list(extra))

    def test_1_convert_resume_force(self):
        self.assertEqual(self.run_convert(), 0)
        man = self.manifest()
        self.assertEqual(sorted(man['files']), ['four_480', 'tst_us'])      # the us list, not extra_jp
        for key, n, wh in (('tst_us', self.frames_a, (320, 180)), ('four_480', self.frames_b, (240, 180))):
            rec = man['files'][key]
            self.assertEqual(rec['status'], 'done')
            self.assertEqual(rec['frames'], n)
            with fvd.FvdReader(os.path.join(self.out, key + '.fvd')) as r:
                self.assertEqual(len(r), n)
                self.assertEqual((r.h.width, r.h.height), wh)
                self.assertEqual((r.h.fps_num, r.h.fps_den), (30000, 1001))
                self.assertEqual(r.h.codec, fvd.CODEC_ZLIB)
                self.assertEqual(r.h.src_size, rec['source_size'])
                # src_name: as the VBF stores it (lower case); compare with the game's names case-insensitively
                self.assertEqual(r.h.src_name, rec['source'].lower())
                self.assertEqual(r.h.src_name.lower(), {'tst_us': 'TST_us.webm', 'four_480': 'Four_480.webm'}[key].lower())
                cuts = [i for i in range(n) if r.entry(i).flags & fvd.FF_CUT]
                self.assertEqual(cuts[0], 0)
                self.assertEqual(len(cuts), 2, cuts)                   # the scene change
                for i in range(n):
                    q = r.frame(i)
                    self.assertEqual(q.dtype, np.uint8)
                self.assertGreater(int(r.frame(n // 4).max()) - int(r.frame(n // 4).min()), 100)
        self.assertTrue(os.path.exists(os.path.join(self.out, 'PRIVATE-do-not-share.txt')))
        self.assertFalse([f for f in os.listdir(self.out) if f.endswith('.part')])
        # resume: nothing to do; the files are untouched
        m0 = os.path.getmtime(os.path.join(self.out, 'tst_us.fvd'))
        self.assertEqual(self.run_convert(), 0)
        self.assertEqual(os.path.getmtime(os.path.join(self.out, 'tst_us.fvd')), m0)
        # other settings: converted again
        self.assertEqual(self.run_convert('--only', 'tst_US.webm', '--edge-filter', 'snap'), 0)
        man = self.manifest()
        self.assertEqual(man['files']['tst_us']['settings']['edge_filter'], 'snap')
        # --force with the same settings
        self.assertEqual(self.run_convert('--only', 'four_480', '--force'), 0)
        # a stale .part from a crash is not mistaken for anything and is removed on the next run of that video
        with open(os.path.join(self.out, 'four_480.fvd.part'), 'wb') as f:
            f.write(b'junk')
        self.assertEqual(self.run_convert('--only', 'four_480', '--force'), 0)
        self.assertFalse(os.path.exists(os.path.join(self.out, 'four_480.fvd.part')))

    def test_2_preview(self):
        if not os.path.exists(os.path.join(self.out, 'tst_us.fvd')):
            self.assertEqual(self.run_convert('--only', 'TST_us'), 0)
        self.assertEqual(self.run_convert('--only', 'TST_us', '--preview', '--frame', '40'), 0)
        pdir = os.path.join(self.out, 'preview')
        self.assertTrue(os.path.exists(os.path.join(pdir, 'PRIVATE-do-not-share.txt')))
        mp4 = os.path.join(pdir, 'tst_us_preview.mp4')
        self.assertEqual(count_frames(mp4), self.frames_a)
        png = os.path.join(pdir, 'tst_us_frame40.png')
        with open(png, 'rb') as f:
            self.assertEqual(f.read(8), b'\x89PNG\r\n\x1a\n')
        out = subprocess.run([FP, '-v', 'error', '-show_entries', 'stream=width,height', '-of', 'csv=p=0', png],
                             capture_output=True, timeout=60).stdout.decode().strip()
        self.assertEqual(out, f'{640 * 2 + preview.GAP},360')
        # gray, through preview.py's own command line
        self.assertEqual(preview.main(['--vbf', self.vbf, '--dir', self.out, 'tst_us', '--gray', '--frame', '0',
                                       '--ffmpeg', FF]), 0)

    def test_2b_frame_i_is_video_frame_i(self):
        """Depth frame i comes from video frame i: decode the clip independently (from the .webm file, not through the
        VBF), run the same per-frame steps with the content-keyed fake model, and compare with the .fvd."""
        if not os.path.exists(os.path.join(self.out, 'tst_us.fvd')):
            self.assertEqual(self.run_convert('--only', 'TST_us'), 0)
        path = os.path.join(self.tmp.name, 'a.webm')
        iw, ih = depthproc.model_input_size(320, 180, 518)
        raw = subprocess.run([FF, '-hide_banner', '-loglevel', 'error', '-i', path, '-map', '0:v:0', '-an', '-vf',
                              f'scale={iw}:{ih}:flags=bicubic:in_color_matrix=bt601:in_range=tv', '-fps_mode',
                              'passthrough', '-pix_fmt', 'rgb24', '-f', 'rawvideo', 'pipe:1'],
                             capture_output=True, timeout=120, check=True).stdout
        n = len(raw) // (iw * ih * 3)
        self.assertEqual(n, self.frames_a)
        frames = [np.frombuffer(raw, np.uint8, iw * ih * 3, i * iw * ih * 3).reshape(ih, iw, 3) for i in range(n)]
        m = vda_model.FakeModel()
        m.prepare(iw, ih, 320, 180)
        cuts, norm = depthproc.CutDetector(320, 180), depthproc.ShotNormalizer()
        flags = [cuts(f)[0] for f in frames]
        expect = [norm(d, flags[i])[0] for i, d in windowed_depth(frames, m)]
        with fvd.FvdReader(os.path.join(self.out, 'tst_us.fvd')) as r:
            got = [r.frame(i).astype(int) for i in range(len(r))]
        same = [int(np.abs(g - e).max()) for g, e in zip(got, expect)]
        self.assertLessEqual(max(same), 1, same)                      # frame i == video frame i (rounding at most)
        moving = range(1, 40)                                          # the first scene moves; the colour bars do not
        off_by_one = [float(np.abs(got[i] - expect[i - 1]).mean()) for i in moving]
        self.assertGreater(min(off_by_one), 4 * max(float(np.abs(got[i] - expect[i]).mean()) for i in moving) + 0.05)

    def test_3_refusals(self):
        # never inside a git checkout, never inside the game's data folder
        repo = os.path.join(self.tmp.name, 'repo')
        os.makedirs(os.path.join(repo, '.git'))
        self.assertEqual(convert.main(['--vbf', self.vbf, '--out', os.path.join(repo, 'x'), '--model', 'fake',
                                       '--codec', 'zlib']), 1)
        self.assertFalse(os.path.exists(os.path.join(repo, 'x')))
        self.assertEqual(convert.main(['--vbf', self.vbf, '--out', os.path.join(os.path.dirname(self.vbf), 'd'),
                                       '--model', 'fake', '--codec', 'zlib']), 1)
        self.assertEqual(self.run_convert('--only', 'NOPE_us'), 1)
        # --list and --check-out write nothing
        fresh = os.path.join(self.tmp.name, 'mods2', 'fmvdepth')
        for extra in (['--list'], ['--check-out']):
            self.assertEqual(convert.main(['--vbf', self.vbf, '--out', fresh, '--model', 'fake', '--codec', 'zlib']
                                          + extra), 0)
            self.assertFalse(os.path.exists(fresh), extra)
        self.assertEqual(convert.main(['--vbf', self.vbf, '--out', os.path.join(repo, 'y'), '--check-out']), 1)
        # a symbolic link into the repository is refused too
        link = os.path.join(self.tmp.name, 'link-to-repo')
        os.symlink(repo, link)
        self.assertEqual(convert.main(['--vbf', self.vbf, '--out', os.path.join(link, 'z'), '--check-out']), 1)

    def test_names(self):
        for bad in ('con.webm', 'NUL.webm', 'com1.webm', 'a/b.webm', '..\\x.webm', 'x y.webm', 'x.mp4', '.webm'):
            with self.assertRaises(convert.ConvertError, msg=bad):
                convert.check_name(bad)
        for good in ('OPN_us.webm', 'WithoutEvent_480.webm', '1_480_new.webm', 'console.webm'):
            convert.check_name(good)


if __name__ == '__main__':
    unittest.main()
