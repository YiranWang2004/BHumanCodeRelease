#!/usr/bin/env python3
"""Install a pinned, unprivileged Clang 18 toolchain for Ubuntu x86_64.

Only .rcss-toolchain inside this checkout is written. Packages retain their
original license files. Python's zstandard module is needed for .conda archives.
"""
import hashlib
from pathlib import Path
import platform
import tarfile
import tempfile
import urllib.request
import zipfile

import zstandard


PACKAGES = [
    ('clang-18-18.1.8-default_hf981a13_5.conda', 'ee93451042125803d2898f6546fe95850fe88fee7295658de78d8b920d6ba30e'),
    ('libclang-cpp18.1-18.1.8-default_hf981a13_5.conda', 'ffcd09fe3e346fe33abaeb02fd07679310ffab7d35c837ef7c553431f3cdb94b'),
    ('libllvm18-18.1.8-h8b73ec9_2.conda', '41993f35731d8f24e4f91f9318d6d68a3cfc4b5cf5d54f193fbb3ffd246bf2b7'),
]


def main():
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        raise SystemExit('This installer supports Linux x86_64; set RCSS_CXX_COMPILER to your Clang >= 16 on other systems.')
    destination = Path(__file__).resolve().parents[1] / '.rcss-toolchain'
    destination.mkdir(exist_ok=True)
    for package, digest in PACKAGES:
        stamp = destination / (package + '.installed')
        if stamp.exists():
            continue
        print('Installing', package, flush=True)
        with tempfile.TemporaryDirectory(prefix='bhuman-clang-') as work:
            archive = Path(work) / package
            urllib.request.urlretrieve('https://conda.anaconda.org/conda-forge/linux-64/' + package, archive)
            if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
                raise RuntimeError('SHA-256 mismatch: ' + package)
            with zipfile.ZipFile(archive) as compressed:
                member = next(name for name in compressed.namelist() if name.startswith('pkg-'))
                with compressed.open(member) as stream, zstandard.ZstdDecompressor().stream_reader(stream) as raw:
                    with tarfile.open(fileobj=raw, mode='r|') as files:
                        for entry in files:
                            target = (destination / entry.name).resolve()
                            if not target.is_relative_to(destination):
                                raise RuntimeError('Archive path outside toolchain: ' + entry.name)
                            if hasattr(tarfile, 'data_filter'):
                                files.extract(entry, destination, filter='data')
                            else:
                                files.extract(entry, destination)
        stamp.write_text(digest + '\n')
    binary = destination / 'bin'
    for name, mode in [('clang', 'gcc'), ('clang++', 'g++')]:
        wrapper = binary / name
        if wrapper.is_symlink():
            wrapper.unlink()
        wrapper.write_text('#!/usr/bin/env bash\nset -euo pipefail\n'
                           'TOOLCHAIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"\n'
                           'export LD_LIBRARY_PATH="$TOOLCHAIN_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
                           f'exec "$TOOLCHAIN_DIR/bin/clang-18" --driver-mode={mode} "$@"\n')
        wrapper.chmod(0o755)
    print('Installed:', binary / 'clang++')


if __name__ == '__main__':
    main()
