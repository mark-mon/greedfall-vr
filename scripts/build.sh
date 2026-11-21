#!/bin/bash
# Cross-builds dist64/dinput8.dll (GreedFall VR, in-process OpenXR) with clang-cl + lld-link.
# $TC/build.sh <arch> <output> <cl args...> /link <link args...> must compile and link with the Windows SDK + MSVC CRT.
set -e
cd "$(dirname "$0")/.."
if [ -z "$TC" ]; then echo "set TC to the folder of your clang-cl / lld-link cross toolchain wrapper (TC/build.sh, see README)"; exit 1; fi
B=$TC/build.sh
XRB=shared/xrbridge; MH=$XRB/third_party/minhook/src; VK=shared/vrkit; IMGUI=$VK/third_party/imgui
ARCH=${1:-x64}
if [ "$ARCH" = x64 ]; then OUT=dist64; OBJ=build64; HDE=hde64.c; ARCHFLAG=""; else OUT=dist; OBJ=build; HDE=hde32.c; ARCHFLAG="/arch:SSE2"; fi
mkdir -p $OBJ $OUT
BUILDID=$(date -u +%Y%m%d_%H%M%S)
WARN="-Wno-unused-function -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-sign-compare -Wno-unused-variable \
 -Wno-unused-command-line-argument -Wno-unused-parameter -Wno-microsoft-cast -Wno-unused-but-set-variable -Wno-deprecated-declarations \
 -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-microsoft-goto -Wno-unused-private-field -Wno-missing-braces"
$B $ARCH $OUT/dinput8.dll /LD /O2 /Oy- /Zi /W3 $ARCHFLAG /utf-8 -D_CRT_SECURE_NO_WARNINGS -DGFVR=1 -DUE3VR_BUILD_ID=\"$BUILDID\" $WARN \
  /Isrc /I$XRB/include /I$XRB/src/common /I$XRB/third_party/minhook/include /I$VK /I$IMGUI /Ithird_party/openxr/include \
  /Fo$OBJ/ /Fd$OBJ/vc.pdb \
  src/*.cpp src/vtbl.c src/vtbl11.c \
  $VK/vrkit_options.cpp $VK/vrkit_input.cpp $VK/vrkit_menu_core.cpp $VK/vrkit_menu_d3d9.cpp $VK/vrkit_menu.cpp $IMGUI/imgui_impl_dx11.cpp \
  $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp \
  $XRB/src/client/ring_d3d9.cpp $XRB/src/common/log.cpp $XRB/src/common/config.cpp \
  $MH/hook.c $MH/buffer.c $MH/trampoline.c $MH/hde/$HDE \
  /link /DEF:src/d3d9.def /DEBUG /PDB:$OUT/dinput8.pdb /MAP:$OUT/dinput8.map /OPT:REF /OPT:ICF /IMPLIB:$OBJ/dinput8.lib \
  third_party/openxr/lib/openxr_loader_static.lib user32.lib gdi32.lib ole32.lib windowscodecs.lib advapi32.lib dxguid.lib \
  dxgi.lib psapi.lib shell32.lib pathcch.lib shlwapi.lib
rm -f $OUT/XrBridgeHost.exe   # in-process OpenXR: no host
echo "BUILD OK $ARCH $BUILDID"
