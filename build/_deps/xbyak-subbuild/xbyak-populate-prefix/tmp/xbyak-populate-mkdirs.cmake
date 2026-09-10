# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/workspace/build/_deps/xbyak-src"
  "/workspace/build/_deps/xbyak-build"
  "/workspace/build/_deps/xbyak-subbuild/xbyak-populate-prefix"
  "/workspace/build/_deps/xbyak-subbuild/xbyak-populate-prefix/tmp"
  "/workspace/build/_deps/xbyak-subbuild/xbyak-populate-prefix/src/xbyak-populate-stamp"
  "/workspace/build/_deps/xbyak-subbuild/xbyak-populate-prefix/src"
  "/workspace/build/_deps/xbyak-subbuild/xbyak-populate-prefix/src/xbyak-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/workspace/build/_deps/xbyak-subbuild/xbyak-populate-prefix/src/xbyak-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/workspace/build/_deps/xbyak-subbuild/xbyak-populate-prefix/src/xbyak-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
