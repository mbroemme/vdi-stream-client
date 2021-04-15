#!/bin/env bash
#
# 02-dso-linking.sh -- dso linking against parsec sdk.
#
# Copyright (c) 2021 Maik Broemme <mbroemme@libmpq.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

# parsec sdk download.
wget "https://github.com/parsec-cloud/parsec-sdk/archive/${PARSEC_VERSION}.tar.gz"
tar xzf "${PARSEC_VERSION}.tar.gz"

# parsec sdk installation.
mv "parsec-sdk-${PARSEC_VERSION}" "parsec-sdk"

# compilation.
./configure --prefix=/usr
make
make distclean
