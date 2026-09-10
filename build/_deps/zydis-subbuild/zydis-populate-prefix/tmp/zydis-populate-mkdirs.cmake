# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/workspace/build/_deps/zydis-src"
  "/workspace/build/_deps/zydis-build"
  "/workspace/build/_deps/zydis-subbuild/zydis-populate-prefix"
  "/workspace/build/_deps/zydis-subbuild/zydis-populate-prefix/tmp"
  "/workspace/build/_deps/zydis-subbuild/zydis-populate-prefix/src/zydis-populate-stamp"
  "/workspace/build/_deps/zydis-subbuild/zydis-populate-prefix/src"
  "/workspace/build/_deps/zydis-subbuild/zydis-populate-prefix/src/zydis-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/workspace/build/_deps/zydis-subbuild/zydis-populate-prefix/src/zydis-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/workspace/build/_deps/zydis-subbuild/zydis-populate-prefix/src/zydis-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
