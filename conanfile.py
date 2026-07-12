import importlib.util
import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, save


def _load_version_module():
    path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "scripts", "version.py")
    spec = importlib.util.spec_from_file_location("nanofix_version", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_version_module = _load_version_module()


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
        "disable_simd": [True, False],
    }
    default_options = {
        "with_docs": False,
        "disable_simd": False,
    }

    exports = ("scripts/version.py",)
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
        self.version = self.version or _version_module.git_version(self.recipe_folder)

    def export_sources(self):
        save(
            self,
            os.path.join(
                self.export_sources_folder,
                "include", "nanofix", "detail", "version.hpp",
            ),
            _version_module.render_header(
                str(self.version),
                os.path.join(self.recipe_folder, "cmake", "version.hpp.in"),
            ),
        )

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
        variables = {
            "NANOFIX_BUILD": "OFF",
            "NANOFIX_BUILD_FIXSPEC_GEN": "ON",
            "NANOFIX_NATIVE_ARCH": "OFF",
        }
        if self.options.disable_simd:
            # The packaged fixspec-gen must run on the consumer's build host.
            variables["NANOFIX_DISABLE_SIMD"] = "ON"
        cmake = CMake(self)
        cmake.configure(variables=variables)
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
        if self.options.disable_simd:
            # simd.hpp #errors on x86-64 without __AVX2__ unless this is set.
            self.cpp_info.defines.append("NANOFIX_DISABLE_SIMD=1")
        elif self.settings.arch == "x86_64":
            flag = "/arch:AVX2" if self.settings.compiler == "msvc" else "-mavx2"
            self.cpp_info.cxxflags.append(flag)
