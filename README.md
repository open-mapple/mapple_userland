# mapple_userland

Programs that run on the phone under iOS: for now `tools/mappled/`, the helper behind the USB link (files, programs, the
boot-args record) and its tools. They are development tools: for bring-up roots with a DEVELOPMENT kernel (the link is
off in a release kernel): `mappled` in `/usr/libexec` with its launchd job, the rest in `/private/var/root/mapple-dev`;
never in a release root.

Checked out at `mapple/userland` by the [Open Mapple manifest](https://github.com/open-mapple/manifest), which says how to get, build and install it.

Licence: MIT (`LICENSE`, `NOTICE`).
