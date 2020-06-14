FROM centos:centos7 AS builder

# install gcc 7
RUN yum -y install centos-release-scl && \
    yum -y install devtoolset-7 devtoolset-7-libatomic-devel
ENV CC=/opt/rh/devtoolset-7/root/usr/bin/gcc \
    CPP=/opt/rh/devtoolset-7/root/usr/bin/cpp \
    CXX=/opt/rh/devtoolset-7/root/usr/bin/g++ \
    PATH=/opt/rh/devtoolset-7/root/usr/bin:$PATH \
    LD_LIBRARY_PATH=/opt/rh/devtoolset-7/root/usr/lib64:/opt/rh/devtoolset-7/root/usr/lib:/opt/rh/devtoolset-7/root/usr/lib64/dyninst:/opt/rh/devtoolset-7/root/usr/lib/dyninst:/opt/rh/devtoolset-7/root/usr/lib64:/opt/rh/devtoolset-7/root/usr/lib:$LD_LIBRARY_PATH

# python 3.6
RUN yum -y install rh-python36
ENV PATH=/opt/rh/rh-python36/root/usr/bin:$PATH

# install other yosys dependencies
RUN yum install -y flex tcl tcl-devel libffi-devel git graphviz readline-devel glibc-static wget autoconf zlib-devel && \
    wget https://ftp.gnu.org/gnu/bison/bison-3.0.1.tar.gz && \
    tar -xvzf bison-3.0.1.tar.gz && \
    cd bison-3.0.1 && \
    ./configure && \
    make -j$(nproc) && \
    make install

COPY . /yosys
WORKDIR /yosys
RUN make PREFIX=/build config-gcc-static-tcl-dynamic
RUN make PREFIX=/build -j$(nproc)
RUN make PREFIX=/build install

# DRiLLS
FROM builder AS drills
RUN wget https://cmake.org/files/v3.14/cmake-3.14.0-Linux-x86_64.sh && \
    chmod +x cmake-3.14.0-Linux-x86_64.sh  && \
    ./cmake-3.14.0-Linux-x86_64.sh --skip-license --prefix=/usr/local && rm -rf cmake-3.14.0-Linux-x86_64.sh \
    && yum clean -y all
RUN yum install -y swig python36u-libs python36u-devel python36u-pip python-devel
RUN cd / && git clone https://github.com/scale-lab/gDRiLLS.git && \
    cd /gDRiLLS && pip install -r requirements.txt
RUN cd /yosys/abc && make clean && make ABC_USE_PIC=1 ABC_USE_NO_READLINE=1 ABC_USE_NO_PTHREADS=1 libabc.a -j$(nproc) && \
    mv /yosys/abc/libabc.a /gDRiLLS/session/libs
RUN cd /gDRiLLS/session && \
    mkdir build && cd build && \
    cmake -DPYTHON_INCLUDE_DIR=/opt/rh/rh-python36/root/usr/include/python3.6m -DPYTHON_LIBRARY=/opt/rh/rh-python36/root/usr/lib64/libpython3.6m.so .. && \
    make && \
    python setup.py install
ENV PATH=/gDRiLLS:$PATH