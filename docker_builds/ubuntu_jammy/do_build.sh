#!/bin/bash
set -e
cd /root

# GitHub's runners are Azure VMs, and archive.ubuntu.com has repeatedly dropped
# connections from them mid index fetch. Use the mirror inside Azure instead.
# Handles both the classic sources.list and the deb822 layout used from 24.04.
for src in /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources; do
    if [ -f "$src" ]; then
        sed -i 's|http://archive.ubuntu.com|http://azure.archive.ubuntu.com|g' "$src"
    fi
done

# Retry dropped connections rather than giving up on the first one.
printf 'Acquire::Retries "5";\nAcquire::http::Timeout "30";\n' > /etc/apt/apt.conf.d/80-ci-retries

# Install dependencies and tools
apt update -o APT::Update::Error-Mode=any
apt install -y unzip build-essential cmake git libfftw3-dev libglfw3-dev libvolk2-dev libzstd-dev libairspyhf-dev libairspy-dev \
            libiio-dev libad9361-dev librtaudio-dev libhackrf-dev librtlsdr-dev libbladerf-dev liblimesuite-dev p7zip-full wget portaudio19-dev \
            libcodec2-dev autoconf libtool xxd libspdlog-dev liborc-0.4-dev

# Install SDRPlay libraries
SDRPLAY_ARCH=$(dpkg --print-architecture)
wget https://www.sdrplay.com/software/SDRplay_RSP_API-Linux-3.15.2.run
7z x ./SDRplay_RSP_API-Linux-3.15.2.run
7z x ./SDRplay_RSP_API-Linux-3.15.2
cp $SDRPLAY_ARCH/libsdrplay_api.so.3.15 /usr/lib/libsdrplay_api.so
cp inc/* /usr/include/

# Install libperseus
git clone https://github.com/Microtelecom/libperseus-sdr
cd libperseus-sdr
autoreconf -i
./configure
make
make install
ldconfig
cd ..

# Install librfnm
git clone https://github.com/AlexandreRouma/librfnm
cd librfnm
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
make -j2
make install
cd ../../

# Install libfobos
git clone https://github.com/AlexandreRouma/libfobos
cd libfobos
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
make -j2
make install
cd ../../

# Install libhydrasdr
git clone https://github.com/hydrasdr/rfone_host
cd rfone_host
mkdir build
cd build
cmake ..
make -j2
make install
cd ../../

cd SDRPlusPlus
mkdir build
cd build
cmake .. -DOPT_BUILD_BLADERF_SOURCE=ON -DOPT_BUILD_LIMESDR_SOURCE=ON -DOPT_BUILD_SDRPLAY_SOURCE=ON -DOPT_BUILD_NEW_PORTAUDIO_SINK=ON -DOPT_BUILD_M17_DECODER=ON -DOPT_BUILD_PERSEUS_SOURCE=ON -DOPT_BUILD_RFNM_SOURCE=ON -DOPT_BUILD_FOBOSSDR_SOURCE=ON -DOPT_BUILD_HYDRASDR_SOURCE=ON -DOPT_BUILD_CH_EXTRAVHF_DECODER=ON -DOPT_BUILD_CH_TETRA_DEMODULATOR=ON
make VERBOSE=1 -j2

cd ..
sh make_debian_package.sh ./build 'libfftw3-dev, libglfw3-dev, libvolk2-dev, librtaudio-dev, libzstd-dev, liborc-0.4-dev'
