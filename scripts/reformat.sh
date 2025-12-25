#!/bin/bash
find ../src \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.c" \) \
  -exec clang-format -i {} \;
