#!/bin/bash
#     (C) Copyright IBM Corp. 2016
# 
#     Author: Takeshi Ogasawara, IBM Research - Tokyo
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

BLD=build
rm -rf ${BLD}
mkdir -p ${BLD}
DIR=`pwd`

#
echo Download the files
#
cd ${BLD}
if [ -d "../source" ]; then
cp -r ../source/htslib .
cp -r ../source/samtools .
cp -r ../source/prefilter .
cp -r ../source/hw_zlib .
cp -r ../source/sort_by_coordinate .
fi
#
echo Build the program
#
cd htslib
make
cd ../samtools
make sam2bam
cd ../prefilter
make
cd ../hw_zlib
make
cd ../sort_by_coordinate
make
cd ../samtools
mkdir filter.d accelerator.d
ln -s ${PWD}/../prefilter/lib_pre_filter.so filter.d/
ln -s ${PWD}/../sort_by_coordinate/lib_sort_by_coordinate.so filter.d/
ln -s ${PWD}/../hw_zlib/libz_hw.so accelerator.d/
cp -p ../../source/PPC64LE_lib/lib_ibm_markdup.so filter.d/
# 
copy_install()
{
   mkdir bin lib
   cp ${DIR}/build/samtools/sam2bam bin/
   cp -r ${DIR}/build/samtools/accelerator.d bin/
   cp -r ${DIR}/build/samtools/filter.d bin/
# copy lib
   cp ${DIR}/build/htslib/libhts.* lib/
}
echo "Installing sam2bam"
if [ ! -d "$INSTALL_DIR" ]; then
   rm -fr ${DIR}/Sam2Bam
   mkdir -p ${DIR}/Sam2Bam
   cd ${DIR}/Sam2Bam
   copy_install
else
   echo "This will install at $INSTALL_DIR !"
   mkdir -p $INSTALL_DIR
   cd $INSTALL_DIR
   copy_install
fi
echo "Finished"
