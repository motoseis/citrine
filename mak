#!/bin/bash
if [ "$1" == "D" ];then 
   rm -rf build
   cmake -S src -B build  -DCMAKE_BUILD_TYPE=DEBUG
elif [ "$1" == "R" ]; then
   rm -rf build
   cmake -S src -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi
cmake --build build -j$(nproc)

