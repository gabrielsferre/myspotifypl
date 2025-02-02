#!/bin/sh

compiler="${compiler-cc}"
$compiler -O3 -I./ -Isrc/ -o myspotifypl src/main.c -lcurl
