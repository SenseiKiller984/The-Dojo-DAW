#!/usr/bin/env bash
# Installs everything needed to build The Dojo on Linux.
# Supports Debian/Ubuntu (apt), Fedora/RHEL (dnf) and Arch (pacman).
#
#   sudo ./scripts/setup-linux.sh
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Please run as root, e.g.: sudo $0" >&2
    exit 1
fi

# shellcheck disable=SC1091
source /etc/os-release
family="${ID} ${ID_LIKE:-}"

# JACK headers are only needed at build time (libjack is loaded dynamically at run time).
# Skip them if some JACK implementation (jack2 or PipeWire-JACK) already provides them, so we
# never fight the user's audio stack.
have_jack_headers() { pkg-config --exists jack 2>/dev/null; }

if [[ "$family" == *debian* || "$family" == *ubuntu* ]]; then
    packages=(
        build-essential cmake ninja-build git pkg-config clang
        libasound2-dev
        libfreetype-dev libfontconfig1-dev
        libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev
        libxrandr-dev libxrender-dev libxi-dev
        libegl-dev libglu1-mesa-dev mesa-common-dev
        libavformat-dev libavcodec-dev libswscale-dev libavutil-dev # video tracks (MP4, MOV, MKV, ...)
        xvfb # headless smoke test
    )
    have_jack_headers || packages+=(libjack-jackd2-dev)
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${packages[@]}"

elif [[ "$family" == *fedora* || "$family" == *rhel* ]]; then
    packages=(
        gcc-c++ clang cmake ninja-build git pkgconf-pkg-config
        alsa-lib-devel
        freetype-devel fontconfig-devel
        libX11-devel libXcomposite-devel libXcursor-devel libXext-devel libXinerama-devel
        libXrandr-devel libXrender-devel libXi-devel
        mesa-libEGL-devel mesa-libGLU-devel
        xorg-x11-server-Xvfb
    )
    have_jack_headers || packages+=(pipewire-jack-audio-connection-kit-devel)
    dnf install -y "${packages[@]}"
    # Video tracks decode MP4/MOV/MKV with FFmpeg when it is there (optional: without it, Motion-JPEG AVI only).
    dnf install -y ffmpeg-free-devel || echo "FFmpeg development files not available: video tracks will read Motion-JPEG AVI only."

elif [[ "$family" == *arch* ]]; then
    packages=(
        base-devel clang cmake ninja git pkgconf
        alsa-lib
        freetype2 fontconfig
        libx11 libxcomposite libxcursor libxext libxinerama libxrandr libxrender libxi
        libglvnd glu
        ffmpeg # video tracks (MP4, MOV, MKV, ...)
        xorg-server-xvfb
    )
    have_jack_headers || packages+=(pipewire-jack)
    pacman -S --needed --noconfirm "${packages[@]}"

else
    echo "Unsupported distribution '${ID}'. Install the equivalents of the Debian packages listed in" >&2
    echo "this script (see also JUCE's docs/Linux Dependencies.md) and continue with scripts/build.sh." >&2
    exit 1
fi

echo
echo "Dependencies installed. Next: ./scripts/build.sh"
