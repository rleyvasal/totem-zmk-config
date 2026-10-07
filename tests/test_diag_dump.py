import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / "tools" / "read_diag_dump.py"
spec = importlib.util.spec_from_file_location("read_diag_dump", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class DumpTests(unittest.TestCase):
    def test_large_dump(self):
        verifier = module.DumpVerifier()
        verifier.feed("totem_diag begin id=42 lines=1160 blocks=8 persisted=128 ram=1024 seq=1200")
        for ordinal in range(1160):
            verifier.feed(f"totem_diag d=42 n={ordinal} record")
        verifier.feed("totem_diag end id=42 lines=1160 status=ok")
        self.assertTrue(verifier.complete)

    def test_framing_escapes_control_bytes(self):
        source = bytes([ord("L"), module.SOF, module.ESC, module.EOF])
        self.assertEqual(list(module.Deframer().feed(module.frame(source))), [source])

    def test_complete_dump(self):
        verifier = module.DumpVerifier()
        self.assertTrue(verifier.feed("totem_diag begin id=42 lines=2 blocks=1 persisted=1 ram=0 seq=5"))
        verifier.feed("totem_diag d=42 n=0 b s=0 j=1 first=5 count=1")
        verifier.feed("totem_diag d=42 n=1 e s=p0 q=5 t=1 k=1 i=0 a=0 r=0 w=0 x=0")
        verifier.feed("totem_diag end id=42 lines=2 status=ok")
        self.assertTrue(verifier.complete)

    def test_missing_line_fails(self):
        verifier = module.DumpVerifier()
        verifier.feed("totem_diag begin id=42 lines=2 blocks=1 persisted=1 ram=0 seq=5")
        verifier.feed("totem_diag d=42 n=1 e s=p0 q=5 t=1 k=1 i=0 a=0 r=0 w=0 x=0")
        verifier.feed("totem_diag end id=42 lines=2 status=ok")
        self.assertFalse(verifier.complete)
        self.assertIn("expected line 0", verifier.error)


if __name__ == "__main__":
    unittest.main()
