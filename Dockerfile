# Ядро Dropship для облака/VPS. Расширение остаётся в браузере на вашем компьютере и подключается
# к ядру через SSH-туннель (ядро слушает только 127.0.0.1 — наружу порт не открывается).
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake ninja-build libsqlite3-dev libssl-dev libcurl4-openssl-dev libboost-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY core-cpp core-cpp
RUN cmake -S core-cpp -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDROPSHIP_BUILD_TESTS=OFF \
    && cmake --build build --target dropship_core_app

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libsqlite3-0 libssl3t64 libcurl4t64 ca-certificates \
    && rm -rf /var/lib/apt/lists/* && useradd -m -u 10001 dropship
COPY --from=build /src/build/dropship_core_app /usr/local/bin/dropship_core_app
USER dropship
ENV DROPSHIP_DATA_DIR=/data
VOLUME /data
CMD ["dropship_core_app"]
