make dist
cd zcoin-0.13.8
./autogen.sh
./configure --enable-tests=no
make -j8