#!/bin/bash
set -e

#cd libopenmpt
#	clang-format-11 -i *.hpp *.cpp *.h
#cd ..

cd examples
	clang-format-11 -i *.cpp *.c
cd ..

#cd openmpt123
#	clang-format-11 -i *.hpp *.cpp
#cd ..

cd soundbase
	clang-format-11 -i *.h
cd ..
