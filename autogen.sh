#!/bin/sh

# show information about what is going on.
echo "Generating build information using aclocal, autoheader, automake and autoconf"
echo "This may take a while ..."

# touch the timestamps on all the files since they can be messed up.
directory=`dirname $0`
touch $directory/configure.ac

# regenerate configuration files.
aclocal
autoheader
automake --foreign --add-missing --copy
autoconf

# show instructions what to do now.
echo "Now you are ready to run ./configure"
