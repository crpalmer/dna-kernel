#!/bin/sh

make crpalmer_defconfig && \
	make depend && \
	make -j4 && \
	make dna/update-this-version.zip && \
	echo && echo "*********** COMPLETE *************" && echo
