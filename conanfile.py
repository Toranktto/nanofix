import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy


class NanofixConan(ConanFile):
    name = "nanofix"
    version = "1.0.0"
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
                os.path.join("share", "nanofix", "cmake", "nanofix_generate.cmake"),
            ],
        )
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.bindirs = ["bin"]
        self.cpp_info.libdirs = []
