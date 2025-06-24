#!/usr/bin/env bash
set -uex

gcc -Wall -Wextra arena_test.c -o arena_test_c && ./arena_test_c || echo 'C fail'

g++ -Wall -Wextra arena_test.c -o arena_test_cpp && ./arena_test_cpp || echo 'C++ fail'
