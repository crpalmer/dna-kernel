crpalmer's DNA Kernel

To build this kernel you should also checkout my build tools in the top level
 of this checkout:

git clone http://github.com/crpalmer/android-kernel-build-tools build

You'll need to install the toolchain of your choice and update the
configuration file

crpalmer-build-config

to point to your toolchain.  Note that the checked in version is using ccache.
If you don't want to bother installing that, just remove ccache from the
config file.

You'll also need other installed components to build the kernel.  I forget
what I've installed, but at the minimum you'll need:

 * abootimg
 * java

If you find more things that you need to install, update this file and
send me a pull request or just send me a PM.

Once everything is all configured

./build/build.sh

will compile the kernel and generate an update.zip file in the target dir
specific in your config.
