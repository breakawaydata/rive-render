# Runs the freshly built Linux rive_render and the TS dist in an image with no
# shell at all, as a uid with no passwd entry (HOME resolves to "/"), the way
# hardened / distroless Node bases run it. CI stages the build context; see
# the "No-shell image check" step in .github/workflows/ci.yml.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ffmpeg mesa-vulkan-drivers libvulkan1 libcurl4t64 && rm -rf /var/lib/apt/lists/*
COPY --from=node:22-bookworm-slim /usr/local/bin/node /usr/local/bin/node
COPY app /app
RUN rm -f /usr/bin/sh /usr/bin/dash /usr/bin/bash /usr/bin/rbash /usr/bin/which
USER 1001:1001
WORKDIR /tmp
ENTRYPOINT ["/usr/local/bin/node", "/app/check.js"]
