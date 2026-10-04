FROM registry.fedoraproject.org/fedora@sha256:3fb969dd07e631c17d12f5955bcb6db2078a6997eb68be76b6492e7bd6052e88

RUN dnf -y install git cmake ninja-build gcc gcc-c++ make patch \
      python3 wget curl unzip zip xz binutils sudo && \
    dnf clean all

ENV VITASDK=/usr/local/vitasdk
ENV PATH=$VITASDK/bin:$PATH

ARG VDPM_COMMIT=e255587d4deff255db68fcb73f4e05a82751de15

RUN git clone https://github.com/vitasdk-softfp/vdpm /tmp/vdpm && \
    cd /tmp/vdpm && git checkout --detach ${VDPM_COMMIT} && \
    ./bootstrap-vitasdk.sh && \
    rm -rf /tmp/vdpm

RUN vdpm pacman -S --noconfirm kubridge mpg123
