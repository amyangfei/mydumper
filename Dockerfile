FROM debian:stretch

RUN cat  > /etc/apt/sources.list <<END \
deb http://mirrors.163.com/debian/ stretch main \
deb http://mirrors.163.com/debian/ stretch-updates main non-free contrib \
deb-src http://mirrors.163.com/debian/ stretch-updates main non-free contrib \
deb http://mirrors.163.com/debian-security/ stretch/updates main non-free contrib \
deb http://httpredir.debian.org/debian stretch-backports main contrib non-free \
END

RUN apt-get update && apt-get install -y libglib2.0-dev default-libmysqlclient-dev zlib1g-dev libpcre3-dev libssl-dev git

RUN git clone https://github.com/pingcap/mydumper.git && \
    cd mydumper && \
    git checkout dynamic-build && \
    cmake . && \
    make
