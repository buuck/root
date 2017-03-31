FROM ubuntu:16.04
MAINTAINER Micah Buuck (mbuuck@uw.edu)
WORKDIR /

#Install deps for ROOT. Need to be fancy with cmake to get recent version.
RUN apt-get update && apt-get install -y git dpkg-dev cmake g++ gcc binutils \
libx11-dev libxpm-dev libxft-dev libxext-dev gfortran libssl-dev libpcre3-dev \
xlibmesa-glu-dev libglew1.5-dev libftgl-dev libmysqlclient-dev libfftw3-dev \
libcfitsio-dev graphviz-dev libavahi-compat-libdnssd-dev libldap2-dev \
python-dev libxml2-dev libkrb5-dev libgsl0-dev libqt4-dev

#Build ROOT
RUN mkdir mjsw/mjdeps/ROOT
RUN mkdir mjsw/mjdeps/ROOT/src
COPY . mjsw/mjdeps/ROOT/src
RUN mkdir mjsw/mjdeps/ROOT/build
RUN mkdir mjsw/mjdeps/ROOT/install
WORKDIR mjsw/mjdeps/ROOT/build
RUN cmake -DCMAKE_INSTALL_DIR=../install -Dgdml=ON -Dminuit2=ON -Droofit=ON ../src 2>&1 | tee cmake.log
RUN cmake --build . --target install

#Set ROOT environment
ENV ROOTSYS "/mjsw/mjdeps/ROOT/<INSERT_ROOT_VERSION_NUMBER_HERE>/install"
ENV PATH "$ROOTSYS/bin:$PATH"
ENV LD_LIBRARY_PATH "$ROOTSYS/lib:$LD_LIBRARY_PATH"
ENV PYTHONPATH "$ROOTSYS/lib:$PYTHONPATH"

WORKDIR /
CMD /bin/bash
