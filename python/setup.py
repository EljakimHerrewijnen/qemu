#!/usr/bin/env python3
"""
QEMU tooling installer script
Copyright (c) 2020-2021 John Snow for Red Hat, Inc.
"""

import setuptools
from setuptools.command import bdist_egg
from setuptools.command import build_py
import glob
import os
from pathlib import Path
import shutil
import sys


class bdist_egg_guard(bdist_egg.bdist_egg):
    """
    Protect against bdist_egg from being executed

    This prevents calling 'setup.py install' directly, as the 'install'
    CLI option will invoke the deprecated bdist_egg hook. "pip install"
    calls the more modern bdist_wheel hook, which is what we want.
    """
    def run(self):
        sys.exit(
            'Installation directly via setup.py is not supported.\n'
            'Please use `pip install .` instead.'
        )


class build_py_with_hedgehog_backend(build_py.build_py):
    """
    Optionally bundle prebuilt Hedgehog backend shared libraries into wheels.

    Search order:
    - QEMU_HEDGEHOG_BACKEND_LIBRARY (single explicit file)
    - QEMU_HEDGEHOG_BACKEND_BUILD_DIR (directory)
    - ../build-hedgehog
    - ../build
    """

    _native_globs = (
        'libqemu-hedgehog-backend*.so*',
        'libqemu-hedgehog-backend*.dylib',
        '*qemu-hedgehog-backend*.dll',
    )

    def run(self):
        super().run()
        self._hedgehog_outputs = self._copy_hedgehog_binaries()

    def get_outputs(self, include_bytecode=1):
        outputs = super().get_outputs(include_bytecode=include_bytecode)
        return outputs + list(getattr(self, '_hedgehog_outputs', []))

    def _candidate_paths(self):
        seen = set()

        explicit = os.getenv('QEMU_HEDGEHOG_BACKEND_LIBRARY')
        if explicit:
            path = Path(explicit).expanduser()
            if path.is_file():
                seen.add(path.resolve())
                yield path.resolve()

        build_dir_env = os.getenv('QEMU_HEDGEHOG_BACKEND_BUILD_DIR')
        source_root = Path(__file__).resolve().parent.parent
        search_dirs = []
        if build_dir_env:
            search_dirs.append(Path(build_dir_env).expanduser())
        search_dirs.extend((source_root / 'build-hedgehog', source_root / 'build'))

        for directory in search_dirs:
            if not directory.is_dir():
                continue
            for pattern in self._native_globs:
                for match in sorted(glob.glob(str(directory / pattern))):
                    path = Path(match).resolve()
                    if not path.is_file():
                        continue
                    if path in seen:
                        continue
                    seen.add(path)
                    yield path

    def _copy_hedgehog_binaries(self):
        target_dir = Path(self.build_lib) / 'qemu' / 'hedgehog' / '_native'
        target_dir.mkdir(parents=True, exist_ok=True)

        copied = []
        for source in self._candidate_paths():
            destination = target_dir / source.name
            shutil.copy2(source, destination)
            copied.append(str(destination))

        if copied:
            print(
                'Bundled Hedgehog backend libraries: ' +
                ', '.join(Path(path).name for path in copied)
            )
        else:
            print(
                'No Hedgehog backend library bundled; '
                'set QEMU_HEDGEHOG_BACKEND_LIBRARY or '
                'QEMU_HEDGEHOG_BACKEND_BUILD_DIR to include one.'
            )

        return copied


def main():
    """
    QEMU tooling installer
    """

    setuptools.setup(
        cmdclass={
            'bdist_egg': bdist_egg_guard,
            'build_py': build_py_with_hedgehog_backend,
        }
    )


if __name__ == '__main__':
    main()
