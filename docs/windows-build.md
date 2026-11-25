# Windows build notes

The device binary was built under MSYS2 with GNU Make and Arm GNU Toolchain 13.3.rel1. The compiler is named `arm-none-eabi-gcc` and Rockbox configures it for ARM7TDMI. The [Windows build patch](windows-build.patch) contains the compiler-prefix and path/line-ending adjustments used for this host.

Apply the Radio patch and the Windows build patch to Rockbox `v4.0-final`. Put the Arm tools, MSYS2 tools, and a native Windows host C compiler on PATH. Run these commands from the Rockbox source directory in MSYS2.

```sh
git apply /path/to/rockbox-radio.patch
git apply /path/to/docs/windows-build.patch
mkdir build-radio
cd build-radio
../tools/configure --target=ipodvideo --type=n --no-ccache
make "$PWD/sysfont.h" "$PWD/lang_enum.h"
make -j4 "$PWD/apps/plugins/rockbox_radio.rock"
```

The Windows patch changes host build tooling. The release contains the native iPod plugin linked with Rockbox's `ipodvideo` linker script.
