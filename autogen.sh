#!/bin/sh
# regenerates configure, Makefile.in, etc. from configure.ac/Makefile.am
# only needed when building from a git checkout, release tarballs already
# have these generated
set -e
autoreconf --install --symlink
