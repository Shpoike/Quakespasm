#!/bin/bash

git clone --depth 1 https://github.com/microsoft/vcpkg
./vcpkg/bootstrap-vcpkg.sh

./vcpkg/vcpkg install --overlay-triplets=custom-triplets --triplet=x64-osx-10.9 zlib libogg opus opusfile libvorbis
