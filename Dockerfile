# UEFI-DOS -- one-command build in a container.
#
#   docker build -t uefi-dos .
#   docker run --rm -v "$PWD/out:/src/build" uefi-dos          # BOOTX64/IA32 + image + SDK
#   docker run --rm -v "$PWD/out:/src/build" uefi-dos make sdk # only the native SDK
#
# The result in ./out is a complete bootable set:
#   BOOTX64.EFI  BOOTIA32.EFI  UEFIDOS.img  HELLO64.EXE  HELLO32.EXE
#
# Everything is built from source here: the mingw-w64 cross compilers produce
# PE/COFF EFI applications exactly like the Windows build, and Python 3 runs the
# image/utility generators in tools/.
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        gcc-mingw-w64-x86-64 \
        gcc-mingw-w64-i686 \
        python3 \
        make \
        file \
        binutils-mingw-w64-x86-64 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# A prebuilt image is part of the default target so `docker run` with a mounted
# ./out gives you something bootable immediately.
CMD ["make", "image"]
