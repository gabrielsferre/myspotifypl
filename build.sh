#!/bin/sh

compiler="${compiler-cc}"
$compiler -fstack-protector-all -fstack-clash-protection \
    -O3 -I./ -Isrc/ -o myspotifypl src/main.c -lcurl
