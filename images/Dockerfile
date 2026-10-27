# syntax=docker/dockerfile:1
#
# The image holds the artifacts of a bazel build. Build the artifacts first
#
#   python3 build.py init
#   python3 build.py build -j4 seekdb
#
# and then build the image from the root of the repository with
#
#   docker build -f images/Dockerfile -t seekdb:local .
#
# The layout of the image follows tools/deploy/copy.sh, the same layout the
# observer expects when it is started from a package.

FROM ubuntu:22.04

LABEL org.opencontainers.image.title="seekdb" \
      org.opencontainers.image.description="SeekDB, the AI native database" \
      org.opencontainers.image.source="https://github.com/mahoushoujyo-eee/seekdb"

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8 \
    LD_LIBRARY_PATH=/opt/seekdb/lib

RUN apt-get update -yq && \
    apt-get install -yq --no-install-recommends \
      ca-certificates \
      curl \
      libaio1 \
      libnuma1 \
      libtinfo5 \
      locales \
      python3 \
      tzdata && \
    rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/seekdb/bin /opt/seekdb/lib /opt/seekdb/etc \
             /opt/seekdb/admin /opt/seekdb/tools /opt/seekdb/store \
             /opt/seekdb/log /opt/seekdb/run /var/lib/oceanbase

# The server binary and the libraries it was linked with.
COPY build/bin/src/observer/ /opt/seekdb/lib/
RUN mv /opt/seekdb/lib/seekdb /opt/seekdb/bin/seekdb && \
    chmod 0755 /opt/seekdb/bin/seekdb && \
    rm -f /opt/seekdb/lib/*.a

# The internal tables of the server and the data the server needs.
COPY build/bin/src/share/inner_table/sys_package/bazel_generated/syspack_release/ /opt/seekdb/admin/
COPY tools/timezone_V1.log /opt/seekdb/etc/timezone_V1.log
COPY tools/default_srs_data_mysql.sql /opt/seekdb/etc/default_srs_data_mysql.sql
COPY deps/3rd/usr/local/oceanbase/devtools/bin/llvm-symbolizer /opt/seekdb/tools/llvm-symbolizer

# The client the server is administered with.
COPY deps/3rd/u01/obclient/bin/obclient /opt/seekdb/bin/obclient
COPY deps/3rd/u01/obclient/bin/mysqltest /opt/seekdb/bin/mysqltest
RUN chmod 0755 /opt/seekdb/bin/obclient /opt/seekdb/bin/mysqltest

EXPOSE 2881 2882

WORKDIR /opt/seekdb
ENTRYPOINT ["/opt/seekdb/bin/seekdb"]
CMD ["--base-dir=/opt/seekdb", "--port=2881"]
