import os
import tempfile
import unittest

import helpers  # noqa: F401
import vda_model


class VdaFilesTest(unittest.TestCase):
    def test_verify(self):
        with tempfile.TemporaryDirectory() as t:
            bad = vda_model.verify_vda(t)
            self.assertEqual(len(bad), len(vda_model.VDA_FILES))
            self.assertTrue(all(b.endswith('missing') for b in bad))
            p = os.path.join(t, 'video_depth_anything', 'video_depth.py')
            os.makedirs(os.path.dirname(p))
            with open(p, 'w') as f:
                f.write('tampered')
            bad = vda_model.verify_vda(t)
            self.assertIn('video_depth_anything/video_depth.py: differs from commit 4f5ae23', bad)
            with open(os.path.join(t, 'video_depth_anything', '__init__.py'), 'w') as f:
                f.write('print(1)')                      # would run on import: refused
            self.assertIn('video_depth_anything/__init__.py: not in commit 4f5ae23', vda_model.verify_vda(t))

    def test_known_files_are_the_commit_tree(self):
        """The hashed and the tolerated files together are exactly `git ls-tree -r 4f5ae23 -- video_depth_anything utils`
        (recorded in vda_model.VDA_TREE_AT_COMMIT), so a genuine download never fails the unknown-file check."""
        known = (set(vda_model.VDA_FILES) - {'LICENSE'}) | vda_model.VDA_OTHER_PY
        self.assertEqual(sorted(known), sorted(vda_model.VDA_TREE_AT_COMMIT))

    @unittest.skipUnless(os.environ.get('FFXVR_VDA_REF'), 'set FFXVR_VDA_REF to a clone of Video-Depth-Anything at 4f5ae23')
    def test_reference_checkout(self):
        self.assertEqual(vda_model.verify_vda(os.environ['FFXVR_VDA_REF']), [])

    def test_pins(self):
        self.assertEqual(len(vda_model.VDA_COMMIT), 40)
        self.assertIn('Video-Depth-Anything-Small', vda_model.WEIGHTS_URL)    # never Base / Large (CC-BY-NC)
        self.assertTrue(vda_model.WEIGHTS_URL.endswith('video_depth_anything_vits.pth'))
        self.assertEqual(vda_model.VITS_CONFIG['encoder'], 'vits')


if __name__ == '__main__':
    unittest.main()
