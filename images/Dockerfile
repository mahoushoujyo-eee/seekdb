# syntax=docker/dockerfile:1
#
# The image holds the runtime tree of the server. Prepare the tree first with
# the layout the observer expects
#
#   mkdir runtime && cd runtime
#   SEEKDB_TIMEZONE_FILE=/dev/null sh ../tools/deploy/copy.sh ../build/bin
#
# and then build the image from the root of the repository with
#
#   docker build -f images/Dockerfile -t seekdb:local .
#
# The tree cannot be taken from build/bin directly, that path is a symlink into
# the bazel output base and a build context does not follow symlinks.

# The image runs the binary that was built on the host, both have to be the
# same distribution, the binary needs the glibc of the distribution it was
# built on.
FROM ubuntu:24.04

LABEL org.opencontainers.image.title="seekdb" \
      org.opencontainers.image.description="SeekDB, the AI native database" \
      org.opencontainers.image.source="https://github.com/mahoushoujyo-eee/seekdb"

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8 \
    LD_LIBRARY_PATH=/opt/seekdb/lib

RUN apt-get update -yq && \
    (apt-get install -yq --no-install-recommends libaio1 || \
     apt-get install -yq --no-install-recommends libaio1t64) && \
    apt-get install -yq --no-install-recommends \
      ca-certificates \
      curl \
      libnuma1 \
      locales \
      python3 \
      tzdata && \
    rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/seekdb/bin /opt/seekdb/lib /opt/seekdb/etc \
             /opt/seekdb/admin /opt/seekdb/tools /opt/seekdb/store \
             /opt/seekdb/log /opt/seekdb/run /var/lib/oceanbase

# The server binary, the libraries it was linked with, the internal tables and
# the data the server needs.
COPY runtime/ /opt/seekdb/
RUN chmod 0755 /opt/seekdb/bin/seekdb /opt/seekdb/bin/obclient \
             /opt/seekdb/bin/mysqltest /opt/seekdb/tools/llvm-symbolizer 2>/dev/null || true

EXPOSE 2881 2882

WORKDIR /opt/seekdb
ENTRYPOINT ["/opt/seekdb/bin/seekdb"]
CMD ["--base-dir=/opt/seekdb", "--port=2881"]
