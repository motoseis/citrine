#!/bin/bash
if [ "$1" == "D" ];then 
   rm -rf build
   cmake -S src -B build  -DCMAKE_BUILD_TYPE=DEBUG
elif [ "$1" == "R" ]; then
   rm -rf build
#   cmake -S src -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake -S src -B build -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-march=native"
fi
cmake --build build -j$(nproc)

