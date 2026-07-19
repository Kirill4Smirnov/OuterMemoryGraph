FROM ubuntu:22.04 AS build

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       build-essential \
       cmake \
       ninja-build \
       nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DOMG_BUILD_TESTS=OFF \
    && cmake --build build

FROM ubuntu:22.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/omg /usr/local/bin/omg

WORKDIR /data
ENTRYPOINT ["omg"]

