#!/bin/bash

# Tools to use for bootstrapping.
C_COMPILER_PATH=gcc
CXX_COMPILER_PATH=g++

# Initialize our own variables:
buildtype=RelWithDebInfo
backends="ARM;AArch64;X86"
build_for_openeuler="0"
enabled_projects="clang;lld;compiler-rt;openmp;clang-tools-extra"
embedded_toolchain="0"
split_dwarf=on
use_ccache="0"
enable_classic_flang="0"
do_install="0"
clean=0
unit_test=""
install="install"
install_toolchain_only="0"
verbose=""
dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && pwd )"
build_dir_name="build"
install_dir_name="install"
build_prefix="$dir/$build_dir_name"
install_prefix="$dir/$install_dir_name"

# Use 8 threads for builds and tests by default. Use more threads if possible,
# but avoid overloading the system by using up to 50% of available cores.
threads=8
nproc=$(type -p nproc)
if [ -x "$nproc" -a -f /proc/loadavg ]; then
  loadavg=$(awk '{printf "%.0f\n", $1}' < /proc/loadavg)
  let threads="($($nproc) - $loadavg) / 2"
  if [ $threads -le 0 ]; then
    threads=1
  fi
fi

# Exit script on first error.
set -e

usage() {
  cat <<EOF
Usage: $0 [options]

Build the compiler under $build_prefix, then install under $install_prefix.

Options:
  -b type  Specify CMake build type (default: $buildtype).
  -c       Use ccache (default: $use_ccache).
  -e       Build for embedded cross tool chain.
  -E       Build for openEuler.
  -h       Display this help message.
  -i       Install the build (default: $do_install).
  -I name  Specify install directory name (default: "$install_dir_name").
  -j N     Allow N jobs at once (default: $threads).
  -o       Enable LLVM_INSTALL_TOOLCHAIN_ONLY=ON.
  -r       Delete $install_prefix and perform a clean build (default: incremental).
  -s       Strip binaries and minimize file permissions when (re-)installing.
  -t       Enable unit tests for components that support them (make check-all).
  -v       Enable verbose build output (default: quiet).
  -f       Enable classic flang.
  -X archs Build only the specified semi-colon-delimited list of backends (default: "$backends").
EOF
}

# Process command-line options. Remember the options for passing to the
# containerized build script.
while getopts :b:ceEhiI:j:orstvfX: optchr; do
  case "$optchr" in
    b)
      buildtype="$OPTARG"
      case "${buildtype,,}" in
        release)
          split_dwarf=off
          ;;
        debug|relwithdebinfo)
          ;;
        *)
          echo "$0: invalid build type '$buildtype'"
          exit 1
          ;;
      esac
      ;;
    c)
      use_ccache="1"
      ;;
    f)
      enable_classic_flang="1"
      ;;
    e)
      embedded_toolchain="1"
      ;;
    E)
      build_for_openeuler="1"
      ;;
    h)
      usage
      exit
      ;;
    i)
      do_install="1"
      ;;
    I)
      install_dir_name="$OPTARG"
      install_prefix="$dir/$install_dir_name"
      ;;
    j)
      threads="$OPTARG"
      ;;
    o)
      install_toolchain_only=1
      ;;
    r)
      clean=1
      ;;
    s)
      install="install/strip"
      ;;
    t)
      unit_test=check-all
      ;;
    v)
      verbose="VERBOSE=1"
      ;;
    X)
      backends="$OPTARG"
      ;;
    :)
      echo "$0: missing argument for option '-$OPTARG'"
      exit 1
      ;;
    ?)
      echo "$0: invalid option '-$OPTARG'"
      exit 1
      ;;
  esac
done

CMAKE_OPTIONS="-DCMAKE_INSTALL_PREFIX=$install_prefix \
               -DCMAKE_BUILD_TYPE=$buildtype \
               -DCMAKE_C_COMPILER=$C_COMPILER_PATH \
               -DCMAKE_CXX_COMPILER=$CXX_COMPILER_PATH \
               -DLLVM_TARGETS_TO_BUILD=$backends "

gold=$(type -p ld.gold)
if [ -z "$gold" -o ! -x "$gold" ]; then
  echo "$0: no usable ld.gold"
  exit 1
fi

# If the invocation does not force a particular binutils installation, check
# that we are using an acceptable version.
if [ -n "$BINUTILS_INCDIR" ]; then
  llvm_binutils_incdir="-DLLVM_BINUTILS_INCDIR=$BINUTILS_INCDIR"
else
  incdir=$(realpath --canonicalize-existing $(dirname $gold)/../include)
  if [ -z "$incdir" -o ! -f "$incdir/plugin-api.h" ]; then
    echo "$0: plugin-api.h not found; required to build LLVMgold.so"
    exit 1
  fi
  llvm_binutils_incdir="-DLLVM_BINUTILS_INCDIR=$incdir"
fi

# Warning: the -DLLVM_ENABLE_PROJECTS option is specified with cmake
# to avoid issues with nested quotation marks
if [ $use_ccache == "1" ]; then
  echo "Build using ccache"
  CMAKE_OPTIONS="$CMAKE_OPTIONS \
                -DCMAKE_C_COMPILER_LAUNCHER=ccache \
                -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "
fi

if [ $enable_classic_flang == "1" ]; then
  echo "Enable classic flang"
  CMAKE_OPTIONS="$CMAKE_OPTIONS \
                -DLLVM_ENABLE_CLASSIC_FLANG=on"
fi

if [ $embedded_toolchain == "1" ]; then
  echo "Build for embedded cross tool chain"
  enabled_projects="clang;lld;compiler-rt;"
  CMAKE_OPTIONS="$CMAKE_OPTIONS \
                -DLLVM_BUILD_FOR_EMBEDDED=ON"
fi

# When set LLVM_INSTALL_TOOLCHAIN_ONLY to On it removes many of the LLVM development
# and testing tools as well as component libraries from the default install target.
if [ $install_toolchain_only == "1" ]; then
  echo "Only install toolchain"
  CMAKE_OPTIONS="$CMAKE_OPTIONS -DLLVM_INSTALL_TOOLCHAIN_ONLY=ON"
fi

if [ $build_for_openeuler == "1" ]; then
  echo "Build for openEuler"
  CMAKE_OPTIONS="$CMAKE_OPTIONS -DBUILD_FOR_OPENEULER=ON"
fi

# Build and install
if [ $clean -eq 1 -a -e "$install_prefix" ]; then
  rm -rf "$install_prefix"
fi
mkdir -p "$install_prefix/bin"

if [ $clean -eq 1 -a -e "$build_prefix" ]; then
  rm -rf "$build_prefix"
fi

mkdir -p "$build_prefix" && cd "$build_prefix"
cmake $CMAKE_OPTIONS \
      -DCOMPILER_RT_BUILD_SANITIZERS=on \
      -DLLVM_ENABLE_PROJECTS=$enabled_projects \
      -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
      -DLLVM_USE_LINKER=gold \
      -DLLVM_LIT_ARGS="-sv -j$threads" \
      -DLLVM_USE_SPLIT_DWARF=$split_dwarf \
      -DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO="-Wl,--gdb-index -Wl,--compress-debug-sections=zlib" \
      -DCMAKE_EXE_LINKER_FLAGS_DEBUG="-Wl,--gdb-index -Wl,--compress-debug-sections=zlib" \
      $llvm_binutils_incdir \
      $verbose \
      ../llvm

make -j$threads
if [ $do_install == "1" ]; then
  make -j$threads $verbose $install
fi

if [ -n "$unit_test" ]; then
  make -j$threads $verbose check-all
fi

cd ..

# When building official deliverables, minimize file permissions under the
# installation directory.
if [ "$install" = "install/strip" ]; then
  find $install_prefix/bin/ -type f -exec strip {} \;
  find $install_prefix -type f -exec chmod a-w {} \;
fi

# In openEuler embedded building system, it need wrap llvm-readelf
# to replace binutils-readelf.
if [ -e "$install_prefix/bin/llvm-readobj" ]; then
  ln -sf llvm-readobj $install_prefix/bin/llvm-readelf
fi

echo "$0: SUCCESS"