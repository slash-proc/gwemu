# gwemu headless capture appliance -- see docs/headless-capture.md.
#
# Build FROM THE REPO ROOT:
#   docker build -t gwemu-headless -f contrib/docker-headless/Dockerfile .
#
# Run (timeline-scripted, recording):
#   docker run --rm \
#     -v $PWD/backup/qemu-images:/images:ro \
#     -v $PWD/out:/out \
#     -v $PWD/demo.tl:/demo.tl:ro \
#     gwemu-headless --timeline /demo.tl --record demo.mp4
#
# Design rule (project owner's call): the runtime stage gets exactly the
# binary + entrypoint + runtime packages, nothing else. The build stage
# may COPY . . only under the conditions documented at that line.

FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential git ninja-build pkg-config python3 python3-venv \
        python3-yaml libglib2.0-dev libpixman-1-dev zlib1g-dev \
        libslirp-dev liblzma-dev cmake ca-certificates \
        libx11-dev libxext-dev libxcursor-dev libxrandr-dev libxi-dev \
        libxfixes-dev libxss-dev libegl-dev libgl-dev libpng-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src

# Source tree: COPY . . is acceptable HERE ONLY because (a) it's confined
# to the build stage (nothing from it survives into the runtime image
# except the binary) and (b) .dockerignore at the repo root strictly
# excludes the multi-GB non-source content (sdcard/backup images, build
# trees, SDK, PDFs -- a blanket copy without it once produced 100GB of
# build cache). Keep .dockerignore in sync when adding large artifacts.
COPY . .

# No --enable-lto: this image is x86-64, where LTO measured as a null
# result (~0.9% on the retro-go Link's Awakening DX workload, inside
# noise) -- the +8.0% is an aarch64-only win, see release.yml's matrix.
# It would link fine here (--disable-sdl is already passed, so the
# system-SDL2-vs-vendored-SDL3 symbol clash that blocks LTO elsewhere
# can't arise), it just wouldn't buy anything, and this image's job is
# reproducible capture rather than throughput.
RUN mkdir build-docker && cd build-docker \
    && ../configure --target-list=arm-softmmu --disable-gtk --disable-sdl \
        --disable-sdl-image --disable-opengl \
    && ninja qemu-system-arm

FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        ffmpeg libglib2.0-0 libpixman-1-0 zlib1g libslirp0 liblzma5 \
        libpng16-16t64 libepoxy0 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build-docker/qemu-system-arm /usr/local/bin/qemu-system-arm
COPY contrib/docker-headless/entrypoint.sh /usr/local/bin/entrypoint
RUN chmod +x /usr/local/bin/entrypoint
VOLUME ["/images", "/out"]
ENTRYPOINT ["/usr/local/bin/entrypoint"]
