# HiveMind desktop client — podman container

Runs **only** the GTK4/gtkmm4 `hive-client` desktop app in a rootless podman
container on the desktop machine, with GPU + Wayland + audio passthrough.

- `Containerfile` — `ubuntu:26.04` base, runtime deps (gtkmm-4.0, webkitgtk-6.0
  for the embedded screen view, PipeWire for voice, GStreamer for media,
  Mesa GL/Vulkan for GPU-accelerated GTK). The client binary is self-contained
  (icons/avatars compiled into GResource).
- `rootfs/` — **generated** staging dir (gitignored): copy `build-verify`
  Release `hive-client`, `org.hivemind.Client.gschema.xml`, app icon into
  `rootfs/usr/local/...`. Rebuild the client first if sources changed.
- `hive-client-container.sh` — host-side launcher: `podman run --rm
  --network host --userns keep-id --device /dev/dri` with `/run/user/1000`
  (Wayland, PipeWire/Pulse, D-Bus session bus) and `/home/lee` bind-mounted
  rw, so settings under `~/.config` and data under `~/.local/share/hivemind`
  persist on the host and files stay owned by `lee`.
- `org.hivemind.Client.desktop` — desktop entry; Execs the wrapper. The id matches the client's app ID so xdg-desktop-portal resolves it (mic permission prompts).

## Use

```sh
podman build -t localhost/hive-client:latest hive-client/container
cp hive-client/container/hive-client-container.sh ~/.local/bin/hive-client-container
cp hive-client/container/org.hivemind.Client.desktop ~/.local/share/applications/
```

`--network host` is required so host-local services work: the voice stack
reaches ChimpFlow at `127.0.0.1:8090` (published by the host `chimpflow`
container), and a hub listening on localhost is reachable too. Without it,
`127.0.0.1` inside the container is the container's own loopback.

Notes: AMD/Intel GPU only (`/dev/dri` + Mesa). NVIDIA would need its
container runtime/toolkit passthrough instead.
