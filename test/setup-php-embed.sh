#!/bin/sh
#
# Stage a PHP 8.3 embed SAPI for FreeUnit's php module and pytest suite
# (test_php*.py, test_schedules_php.py), the way it was done for this
# environment's container on Day 1/Day 2.
#
# Debian/Ubuntu's php8.3-embed package ships libphp8.3.so linked against the
# distro's php-config, but this container did not have it preinstalled, so
# the embed SAPI was extracted from the upstream .deb packages into a
# scratch prefix instead of being installed system-wide. This script
# reproduces that: it is a *description* of the steps, safe to read and
# adapt, not a turnkey installer -- package names, versions and download
# locations vary by distro and by what your outbound network allows.
#
# Usage:
#   PHP_STAGE_DIR=/path/to/scratch sh test/setup-php-embed.sh
#
# Result: $PHP_STAGE_DIR/php-config8.3 and libphp8.3.so/libphp.so under
# /usr/local/lib/php-embed, ready for:
#
#   ./configure php --config=$PHP_STAGE_DIR/usr/bin/php-config8.3 \
#       --lib-path=/usr/local/lib/php-embed --module=php
#   make php

set -eu

: "${PHP_STAGE_DIR:=/tmp/php83-embed-stage}"

echo "== 1. Fetch the php8.3-embed, php8.3-common and php8.3-cli .debs =="
echo "   (whatever your distro's package manager or mirror provides; on"
echo "   Debian/Ubuntu with the sury.org PHP PPA enabled:"
echo
echo "     apt-get download php8.3-embed php8.3-common php8.3-cli"
echo
echo "== 2. Extract them into a scratch prefix instead of installing =="
echo "   (keeps the host's own PHP, if any, untouched):"
echo
echo "     mkdir -p '$PHP_STAGE_DIR'"
echo "     for deb in php8.3-*.deb; do"
echo "         dpkg-deb -x \"\$deb\" '$PHP_STAGE_DIR/php83'"
echo "     done"
echo
echo "== 3. php-config reports a self-referential --prefix; nothing else"
echo "   in this script needs it rewritten, but if your build system reads"
echo "   --prefix literally, patch it:"
echo
echo "     sed -i \"s#^prefix=.*#prefix='$PHP_STAGE_DIR/php83/usr'#\" \\"
echo "         '$PHP_STAGE_DIR/php83/usr/bin/php-config8.3'"
echo
echo "== 4. Stage libphp*.so where the dynamic linker and configure's"
echo "   --lib-path both expect to find it =="
echo
echo "     mkdir -p /usr/local/lib/php-embed"
echo "     cp '$PHP_STAGE_DIR/php83/usr/lib/libphp8.3.so' \\"
echo "        '$PHP_STAGE_DIR/php83/usr/lib/libphp.so' \\"
echo "        /usr/local/lib/php-embed/"
echo "     ldconfig"
echo
echo "== 5. Configure and build the module =="
echo
echo "     ./configure php \\"
echo "         --config='$PHP_STAGE_DIR/php83/usr/bin/php-config8.3' \\"
echo "         --lib-path=/usr/local/lib/php-embed --module=php"
echo "     make -j2"
echo
echo "This prints the steps; it does not fetch or extract anything itself,"
echo "so it never needs network or package-manager permission on its own."
echo "If /usr/local/lib/php-embed/libphp.so and a matching php-config"
echo "already exist (check with 'ldconfig -p | grep -i php' and"
echo "'ls /usr/local/lib/php-embed'), skip straight to step 5."
