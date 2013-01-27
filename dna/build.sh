#!/bin/sh

make cubed_defconfig && \
	make depend && \
	make -j4 && \
	make dna/boot-this-version.img && \
	echo && echo "*********** COMPLETE *************" && echo
