#!/bin/bash
TARGET_DIR="kernel_install"
TARGET_TAR="kernel_install.tar"
HDR_DST="/usr"

if [ -d "$TARGET_DIR" ]; then
  echo "Removing directory: $TARGET_DIR"
  rm -rf -- "$TARGET_DIR"
fi

# 删除 tar 文件（仅当存在且是普通文件）
if [ -f "$TARGET_TAR" ]; then
  echo "Removing file: $TARGET_TAR"
  rm -f -- "$TARGET_TAR"
fi

xz -T 128 -d kernel_install.tar.xz && tar xf kernel_install.tar

cd kernel_install
ver=`ls lib/modules/`
echo $ver
if [ -z $ver ]; then
	echo "version is empty"
	exit 1
fi
rm -rf /lib/modules/*$ver* /boot/*$ver*
mv lib/modules/* /lib/modules/
installkernel $ver vmlinuz-$ver System.map-$ver

# Install exported kernel headers if present
if [ -d "usr/include" ]; then
  echo "Installing kernel headers to $HDR_DST/include"
  mkdir -p "$HDR_DST"
  cp -a usr/include "$HDR_DST"/
fi
