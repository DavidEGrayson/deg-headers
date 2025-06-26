#!/usr/bin/env bash
set -ue

FLAGS="-Wall -Wextra "
FLAGS+="-Wfatal-errors "

set -x

gcc $FLAGS arena_test.c -o arena_test_c && ./arena_test_c || echo 'C fail'

g++ $FLAGS arena_test.c -o arena_test_cpp && ./arena_test_cpp || echo 'C++ fail'
