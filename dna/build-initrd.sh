#!/bin/sh

cd initrd && find . | cpio --create --format='newc' | gzip -f -9 > ../initrd.img
