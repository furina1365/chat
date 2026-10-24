FROM ubuntu:24.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ make pkg-config \
        libcurl4-openssl-dev libmysqlclient-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY backend/ ./backend/

ARG BUILD_JOBS=2
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNET_BACKEND=reactor \
    && cmake --build build --target server --parallel ${BUILD_JOBS}

FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates curl libcurl4t64 libmysqlclient21 libssl3t64 libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 chat \
    && useradd --uid 10001 --gid chat --no-create-home --shell /usr/sbin/nologin chat

WORKDIR /app
COPY --from=build /src/build/server ./build/server
COPY frontend/ ./frontend/

USER chat
EXPOSE 53411
CMD ["./build/server"]
