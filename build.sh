#!/bin/bash

BASE_DIR=`pwd -P`
BUILD_DIR=$BASE_DIR/bld

# Make a directory for build
if [ ! -d "$BUILD_DIR" ]; then
    echo "Make a directory for build"
    mkdir bld
fi

cd $BUILD_DIR

rm -rf CMakeCache.txt
echo "vldb#7988" | sudo -S rm -rf CMakeFiles/*

# Build and install the source code
if [ "$1" = "--origin" ]; then
    # No caching
    BUILD_FLAGS=""
elif [ "$1" = "--origin-monitor" ]; then
    # No caching but monitor the flush status
    BUILD_FLAGS="-DUNIV_FLUSH_MONITOR"
elif [ "$1" = "--nc" ]; then
    # Cache hot LB pages
    BUILD_FLAGS="-DUNIV_NVDIMM_CACHE"
    #BUILD_FLAGS="-DUNIV_NVDIMM_CACHE -DUNIV_LOG_HEADER"
elif [ "$1" = "--nc-monitor" ]; then
    # Cache hot LB pages with mtr-logging/monitoring enabled
    BUILD_FLAGS="-DUNIV_NVDIMM_CACHE -DUNIV_LOG_HEADER -DUNIV_FLUSH_MONITOR"
else
    # Cache NVDIMM pages in TPC-C workloads
    BUILD_FLAGS="-DUNIV_NVDIMM_CACHE"
fi

echo "Start build using $BUILD_FLAGS"

cd $BUILD_DIR

cmake .. -DWITH_DEBUG=0 -DCMAKE_C_FLAGS="$BUILD_FLAGS" -DCMAKE_CXX_FLAGS="$BUILD_FLAGS" \
-DDOWNLOAD_BOOST=ON -DWITH_BOOST=$BASE_DIR/boost -DENABLED_LOCAL_INFILE=1 \
-DCMAKE_INSTALL_PREFIX=$BUILD_DIR

make -j8
echo "vldb#7988" | sudo -S make install
