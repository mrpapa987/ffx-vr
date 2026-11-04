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
import fvd
import preview
import vbf

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
        self.assertEqual(self.run_convert('--list'), 0)


if __name__ == '__main__':
    unittest.main()
