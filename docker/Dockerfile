# One build, two images: the server, and the client with fio and the engine.
#
#   docker build --target server -f docker/Dockerfile .
#   docker build --target client -f docker/Dockerfile .

FROM ubuntu:26.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake build-essential ca-certificates libgtest-dev \
        liburing-dev libtool pkg-config python3 zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# The server, and the engine: it is built against the source of fio, and has
# to be built before fio is configured in that source.
RUN ./autogen.sh \
    && ./configure \
    && make -j"$(nproc)" -C wheelio/src \
    && make -j"$(nproc)" -C server/src \
    && make -j"$(nproc)" -C client/src \
    && make -j"$(nproc)" -C fio

# The fio that the engine is built for.
RUN cd 3rdparty/fio && ./configure && make -j"$(nproc)" fio


FROM ubuntu:26.04 AS server

RUN apt-get update && apt-get install -y --no-install-recommends \
        liburing2 libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/server/src/wheelie-server /usr/local/bin/wheelie-server
COPY docker/server-entrypoint.sh /usr/local/bin/server-entrypoint.sh

EXPOSE 39900
ENTRYPOINT ["/usr/local/bin/server-entrypoint.sh"]


FROM ubuntu:26.04 AS client

RUN apt-get update && apt-get install -y --no-install-recommends \
        liburing2 libstdc++6 zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/3rdparty/fio/fio /usr/local/bin/fio
COPY --from=builder /src/fio/.libs/wheelie.so /usr/local/lib/wheelie.so

CMD ["fio", "--version"]
