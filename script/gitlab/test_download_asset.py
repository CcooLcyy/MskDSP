"""验证官方归档直连、SHA512 校验与原子下载边界。"""
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent))
import download_asset as target


class DownloadAssetTest(unittest.TestCase):
    def test_archive_uses_identical_ref_at_official_codeload(self):
        for ref in ("boost-1.90.0", "refs/tags/boost-1.90.0", "a" * 40):
            self.assertEqual(target.download_url(f"https://github.com/boostorg/iostreams/archive/{ref}.tar.gz"),
                             f"https://codeload.github.com/boostorg/iostreams/tar.gz/{ref}")
        original = "https://example.org/package.tar.gz?download=1"
        self.assertEqual(target.download_url(original), original)
        query = "https://github.com/owner/repo/archive/tag.tar.gz?access=secret"
        self.assertEqual(target.download_url(query), query)

    def test_invalid_urls_and_digests_are_rejected(self):
        for url in ("http://example.org/a", "https://user:secret@example.org/a", "https://example.org/a#fragment",
                    "https://example.org/a\nheader", "https:///missing", "file:///tmp/a"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                target.download_url(url)
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                target.download("https://example.org/a", "not-a-sha512", Path(temporary) / "asset")

    def test_valid_download_replaces_only_after_checksum_matches(self):
        body = b"verified-download"
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "asset"
            destination.write_bytes(b"existing")
            with patch.object(target, "urlopen", return_value=io.BytesIO(body)) as request:
                target.download("https://github.com/owner/repo/archive/tag.tar.gz", hashlib.sha512(body).hexdigest(), destination)
            self.assertEqual(destination.read_bytes(), body)
            self.assertEqual(request.call_args.args[0].full_url, "https://codeload.github.com/owner/repo/tar.gz/tag")
            self.assertEqual(list(Path(temporary).iterdir()), [destination])

    def test_wrong_checksum_and_network_failure_preserve_existing_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "asset"
            destination.write_bytes(b"existing")
            with patch.object(target, "urlopen", side_effect=lambda *args, **kwargs: io.BytesIO(b"corrupt")), \
                 patch.object(target.time, "sleep"), self.assertRaises(ValueError):
                target.download("https://example.org/a", "a" * 128, destination)
            self.assertEqual(destination.read_bytes(), b"existing")
            with patch.object(target, "urlopen", side_effect=OSError("secret-query-token")), \
                 patch.object(target.time, "sleep"), self.assertRaises(ValueError) as error:
                target.download("https://example.org/a?secret-query-token", "a" * 128, destination)
            self.assertNotIn("secret", str(error.exception))
            self.assertEqual(destination.read_bytes(), b"existing")
            self.assertEqual(list(Path(temporary).iterdir()), [destination])


if __name__ == "__main__":
    unittest.main()
