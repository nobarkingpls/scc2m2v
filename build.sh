#!/bin/sh
# Builds libcaption (unmodified) + scc2m2v. Needs: gcc, git, libavcodec-dev libavutil-dev (FFmpeg 6.x tested)
set -e
[ -d libcaption ] || git clone --depth 1 https://github.com/szatmary/libcaption.git
cp -n libcaption/src/eia608_from_utf8.c.cached libcaption/src/eia608_from_utf8.c
mkdir -p build
for f in caption cea708 eia608 eia608_charmap eia608_from_utf8 mpeg scc utf8 xds; do
  gcc -O2 -c -Ilibcaption/caption libcaption/src/$f.c -o build/$f.o
done
ar rcs build/libcaption.a build/*.o
gcc -O2 -Wall -Ilibcaption/caption scc2m2v.c build/libcaption.a \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil) -lm -o scc2m2v
echo "built ./scc2m2v"
