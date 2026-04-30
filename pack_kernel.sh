#!/bin/bash
set -e

INSTALL_DIR=../kernel_install
HDR_DIR="$INSTALL_DIR/usr"

mkdir -p "$INSTALL_DIR"
rm -rf "$INSTALL_DIR"/*

make -j64 modules_install INSTALL_MOD_PATH="$INSTALL_DIR"
make install INSTALL_PATH="$INSTALL_DIR"

# Export kernel UAPI headers into the tarball
make -j64 headers_install INSTALL_HDR_PATH="$HDR_DIR"

tar cf ../kernel_install.tar "$INSTALL_DIR"
xz -T 128 -c ../kernel_install.tar > ../kernel_install.tar.xz
