"""Exercise actual factory capture/recovery validation without an MCU."""
import ctypes
import hashlib
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


class Metadata(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in
                ('magic', 'version', 'source', 'length')]
    _fields_ += [('sha256', ctypes.c_ubyte * 32), ('valid', ctypes.c_uint32)]


class FactoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        root = Path(__file__).resolve().parents[2]
        library = Path(cls.directory.name) / 'factory.so'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-std=c99', '-O2', str(root / 'bootloader/factory.c'),
                        str(root / 'bootloader/sha256.c'), '-o', str(library)],
                       check=True, timeout=20)
        cls.library = ctypes.CDLL(str(library))
        cls.library.boot_factory_valid.argtypes = [ctypes.c_void_p]
        cls.library.boot_factory_valid.restype = ctypes.c_bool
        cls.library.boot_factory_backup_valid.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(Metadata)]
        cls.library.boot_factory_backup_valid.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def image(self, fill=0xff):
        data = bytearray([fill]) * 40960
        struct.pack_into('<2I', data, 0, 0x20040000, 0xf4301)
        return data

    def metadata(self, image):
        result = Metadata(magic=0x504f5442, version=1, source=0xf4000,
                          length=40960, valid=0x56414c49)
        result.sha256[:] = hashlib.sha256(image).digest()
        return result

    def valid(self, image, metadata=None):
        data = ctypes.create_string_buffer(bytes(image))
        if metadata is None:
            return self.library.boot_factory_valid(data)
        return self.library.boot_factory_backup_valid(data, ctypes.byref(metadata))

    def test_distinct_factory_builds_use_recorded_digest(self):
        first, second = self.image(), self.image(0x55)
        for image in (first, second):
            self.assertTrue(self.valid(image, self.metadata(image)))
        self.assertFalse(self.valid(second, self.metadata(first)))

    def test_payload_and_digest_corruption(self):
        image = self.image()
        metadata = self.metadata(image)
        image[40959] ^= 1
        self.assertFalse(self.valid(image, metadata))
        image[40959] ^= 1
        metadata.sha256[0] ^= 1
        self.assertFalse(self.valid(image, metadata))

    def test_metadata_commit_and_layout(self):
        image = self.image()
        for field, value in (('magic', 0), ('version', 2), ('source', 0xe0000),
                             ('length', 40956), ('valid', 0xffffffff)):
            with self.subTest(field=field):
                metadata = self.metadata(image)
                setattr(metadata, field, value)
                self.assertFalse(self.valid(image, metadata))

    def test_bad_factory_vectors(self):
        for stack, reset in ((0x20000000, 0xf4301), (0x20040008, 0xf4301),
                             (0x2003ffff, 0xf4301), (0x20040000, 0xf4300),
                             (0x20040000, 0x26001), (0x20040000, 0xfe001)):
            with self.subTest(stack=stack, reset=reset):
                image = self.image()
                struct.pack_into('<2I', image, 0, stack, reset)
                self.assertFalse(self.valid(image, self.metadata(image)))

    def test_own_bootloader_never_becomes_factory_backup(self):
        for version in (1, 2, 99):
            with self.subTest(version=version):
                image = self.image()
                struct.pack_into('<4I', image, 0x100, 0x534f5450, version, 1, 0xf4000)
                self.assertFalse(self.valid(image))
                self.assertFalse(self.valid(image, self.metadata(image)))


if __name__ == '__main__':
    unittest.main()
