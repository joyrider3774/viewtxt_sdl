#!/bin/sh

make clean
make TARGET=funkey

mkdir -p opk
cp viewtxt opk/viewtxt
cp meta/viewtxt.png opk/viewtxt.png
cp meta/viewtxt.funkey-s.desktop opk/viewtxt.funkey-s.desktop
cp -r fonts opk/fonts

mksquashfs ./opk viewtxt.opk -all-root -noappend -no-exports -no-xattrs

rm -r opk
