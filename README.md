Droid DNA Stock Kernel + Upstream patches

This git repo contains HTC source patched up to the latest 3.4.y release.

The purpose of this git repo is to avoid even more developers doing
manual patching and resolving the same problems.

If you have a kernel that you want to use this repo to apply all the
patches (from 3.4.10 up), you should:

git remote add stock+ http://github.com/crpalmer/dna-kernel-stock-plus-upstream
git fetch stock+
git merge --strategy=ours b58e6fe7e49f6e527c3f0cf03fda5673dfc5ca53
git merge stock+/master

Doing so includes some files that are needed by my build scripts.  If you
want to clean up afterward you can do:

git rm -rf dna crpalmer-build-config

(although you should also checkout my build-scripts in github to see if you
want to keep these files and start using my scripts).


