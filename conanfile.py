import os
import subprocess

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy


def _git_version(root):
    """Keep in sync with cmake/nanofix-version.cmake and scripts/gen-version.sh."""
    def _git(*args):
        return subprocess.run(
            ("git", "-C", root) + args,
            capture_output=True, text=True, check=True,
        ).stdout.strip()

    try:
        desc = _git("describe", "--tags", "--match", "v[0-9]*")
        version = desc[1:]  # drop the leading v
        if "-" in version:  # v1.2.3-5-gabc1234 -> 1.2.3+5.gabc1234
            base, n, ghash = version.rsplit("-", 2)
            version = f"{base}+{n}.{ghash}"
        return version
    except (subprocess.CalledProcessError, OSError):
        pass
    try:
        return "0.0.0+g" + _git("rev-parse", "--short", "HEAD")
    except (subprocess.CalledProcessError, OSError):
        return "0.0.0"


class NanofixConan(ConanFile):
    name = "nanofix"
    license = "BSD-2-Clause"
    author = "Lukasz Derlatka"
    url = "https://github.com/Toranktto/nanofix"
    homepage = url
    description = (
        "Header-only, zero-alloc, noexcept C++20 FIX 4.x/5.0 wire parser "
        "for low-latency paths."
    )
    topics = ("fix", "fix-protocol", "trading", "parser", "header-only")

    settings = "os", "arch", "compiler", "build_type"

    options = {
        "with_docs": [True, False],
    }
    default_options = {
        "with_docs": False,
    }

    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "include/*",
        "utils/*",
        "fixspec/*",
        "docs/*",
        "LICENSE",
        "README.md",
    )

    def set_version(self):
        self.version = self.version or _git_version(self.recipe_folder)

    def requirements(self):
        self.requires("pugixml/1.15", visible=False)

    def build_requirements(self):
        self.test_requires("gtest/1.15.0")
        self.test_requires("benchmark/1.9.5")
        if self.options.with_docs:
            self.tool_requires("doxygen/1.17.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["NANOFIX_VERSION_OVERRIDE"] = str(self.version)
        if self.options.with_docs:
            tc.cache_variables["NANOFIX_BUILD_DOCS"] = "ON"
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure(variables={
            "NANOFIX_BUILD": "OFF",
            "NANOFIX_BUILD_FIXSPEC_GEN": "ON",
        })
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(
            self,
            "LICENSE",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "licenses"),
        )

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "nanofix")
        self.cpp_info.set_property("cmake_target_name", "nanofix::nanofix")
        self.cpp_info.set_property(
            "cmake_build_modules",
            [
                os.path.join("share", "nanofix", "cmake", "nanofix-generate.cmake"),
            ],
        )
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.bindirs = ["bin"]
        self.cpp_info.libdirs = []
