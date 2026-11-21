#!/bin/bash
# Example $TC/build.sh: cross-compiles for Windows from Linux with zig's clang (clang-cl driver + lld-link) against an
# MSVC CRT + Windows SDK tree (e.g. made with `xwin splat`). Adjust ZIG, SDK and the SDK version to your setup.
# Usage (from scripts/build.sh): build.sh <x64|x86> <output> <cl args...> /link <link args...>
set -e
ZIG=${ZIG:-$(python3 -c "import ziglang,os;print(os.path.dirname(ziglang.__file__))")}
SDK=${SDK:-$HOME/xwin}             # contains crt/include, sdk/include/{ucrt,um,shared}, crt/lib/<arch>, sdk/lib/{ucrt,um}/<arch>
ARCH=$1; OUT=$2; shift 2
if [ "$ARCH" = x86 ]; then TGT=i686-pc-windows-msvc; M=X86; A=x86; else TGT=x86_64-pc-windows-msvc; M=X64; A=x86_64; fi
CARGS=(); LARGS=(); inlink=0
for a in "$@"; do if [ "$a" = "/link" ]; then inlink=1; continue; fi; if [ $inlink = 1 ]; then LARGS+=("$a"); else CARGS+=("$a"); fi; done
exec $ZIG/zig clang --driver-mode=cl --target=$TGT -fuse-ld=lld-link \
  /std:c++17 /EHsc /MT /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /DNOMINMAX -Wno-microsoft-include -Wno-nonportable-include-path -Wno-pragma-pack \
  -imsvc $SDK/crt/include -imsvc $SDK/sdk/include/ucrt -imsvc $SDK/sdk/include/um -imsvc $SDK/sdk/include/shared \
  "${CARGS[@]}" /Fe"$OUT" /link /LIBPATH:$SDK/crt/lib/$A /LIBPATH:$SDK/sdk/lib/um/$A /LIBPATH:$SDK/sdk/lib/ucrt/$A /MACHINE:$M "${LARGS[@]}"
