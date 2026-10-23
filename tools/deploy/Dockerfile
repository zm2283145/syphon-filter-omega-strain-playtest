# omega_server in a container (port-only, 132on2g; research/ONLINE_DEPLOY.md section 5.4).
# Build from the repository root (only the server's sources are sent, see Dockerfile.dockerignore; BuildKit):
#   docker build -f tools/deploy/Dockerfile -t omega_server .
# Run (the data folder holds the config, accounts, statistics, social store, policy and log):
#   docker run -d --name omega_server -p 10075:10075/tcp -p 10076:10076/udp \
#       -v omega_data:/data --stop-timeout 30 omega_server
# Put your omega_server.cfg (from omega_server.example.cfg, with bind=0.0.0.0 and your own admin_token) in the volume;
# without one the server starts with the defaults on 0.0.0.0:10075 (relay off). No secrets are built into the image.
# docker stop sends SIGTERM: a graceful shutdown now (the games keep their results, the stores are saved).

FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY src/port/online/ src/port/online/
COPY src/port/app/omega_server_main.cpp src/port/app/
RUN g++ -std=c++20 -O2 -pthread -Isrc/port/online -o /omega_server \
        src/port/app/omega_server_main.cpp src/port/online/*.cpp

FROM debian:bookworm-slim
RUN useradd --system --home-dir /data --shell /usr/sbin/nologin omega && install -d -o omega -g omega /data
COPY --from=build /omega_server /opt/omega_server/omega_server
COPY tools/deploy/omega_server.example.cfg /opt/omega_server/
USER omega
VOLUME /data
WORKDIR /data
EXPOSE 10075/tcp 10076/udp
ENTRYPOINT ["/bin/sh", "-c", "if [ -f /data/omega_server.cfg ]; then exec /opt/omega_server/omega_server --no-console --data-dir /data --config omega_server.cfg \"$@\"; else exec /opt/omega_server/omega_server --no-console --data-dir /data --bind 0.0.0.0 \"$@\"; fi", "omega_server"]
