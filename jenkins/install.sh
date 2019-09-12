cd /yosys
make PREFIX=build config-gcc-static-tcl-dynamic
make PREFIX=build -j$(nproc)
make PREFIX=build install