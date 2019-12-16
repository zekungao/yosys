FROM centos:centos6 AS builder

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
RUN yum install -y flex tcl tcl-devel libffi-devel git graphviz readline-devel glibc-static wget autoconf && \
    wget https://ftp.gnu.org/gnu/bison/bison-3.0.1.tar.gz && \
    tar -xvzf bison-3.0.1.tar.gz && \
    cd bison-3.0.1 && \
    ./configure && \
    make -j$(nproc) && \
    make install

COPY . /yosys
WORKDIR /yosys
RUN make PREFIX=build config-gcc-static-tcl-dynamic
RUN make PREFIX=build -j$(nproc)
RUN make PREFIX=build install

FROM centos:centos6 AS runner
RUN yum update -y && yum install -y readline-devel tcl-devel libffi-devel
COPY --from=builder /yosys/build/bin/yosys /build/yosys
COPY --from=builder /yosys/build/bin/yosys-abc /build/yosys-abc
COPY --from=builder /yosys/build/bin/yosys-config /build/yosys-config
COPY --from=builder /yosys/build/bin/yosys-filterlib /build/yosys-filterlib
COPY --from=builder /yosys/build/bin/yosys-smtbmc /build/yosys-smtbmc
COPY --from=builder /yosys/build/share/yosys /build/share

RUN useradd -ms /bin/bash openroad
USER openroad
WORKDIR /home/openroad