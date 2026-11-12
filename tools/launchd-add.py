#!/usr/bin/env python3
"""Add the mappled LaunchDaemon (tools/mappled) to iOS's launchd job cache.

  launchd-add.py IN.plist OUT.plist [PROGRAM [LABEL]]

IN is /System/Library/xpc/launchd.plist (binary; read by launchd when booted
with launchd_unsecure_cache=1), e.g. fetched from the phone with
  tools/mapple-link.py pull /System/Library/xpc/launchd.plist launchd.plist
OUT gets a root job org.openmapple.mappled (PROGRAM, default /usr/libexec/mappled),
started at boot and restarted when it exits, stderr on the console. Push
both back and reboot:
  tools/mapple-link.py push tools/mappled/out/mappled /usr/libexec/mappled 755
  tools/mapple-link.py push launchd-mappled.plist /System/Library/xpc/launchd.plist 644

LABEL (default org.openmapple.mappled) adds another daemon the same way, e.g. the std USB
persona daemon (tools/mappled/mapple-usbd.c), on top of a plist that already has mappled:
  tools/launchd-add.py launchd-mappled.plist launchd-usbd.plist \
      /usr/libexec/mapple-usbd org.openmapple.mapple-usbd
  tools/mapple-link.py push tools/mappled/out/mapple-usbd /usr/libexec/mapple-usbd 755
  tools/mapple-link.py push launchd-usbd.plist /System/Library/xpc/launchd.plist 644
"""
import plistlib, sys

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    program = sys.argv[3] if len(sys.argv) > 3 else '/usr/libexec/mappled'
    LABEL = sys.argv[4] if len(sys.argv) > 4 else 'org.openmapple.mappled'
    with open(src, 'rb') as f:
        plist = plistlib.load(f)
    daemons = plist['LaunchDaemons']
    for key in [k for k, job in daemons.items() if job.get('Label') == LABEL]:
        del daemons[key]
    daemons['/System/Library/LaunchDaemons/%s.plist' % LABEL] = {
        'Label': LABEL,
        'Program': program,
        'ProgramArguments': [program],
        'RunAtLoad': True,
        'KeepAlive': True,
        'UserName': 'root',
        'StandardErrorPath': '/dev/console',
        'StandardOutPath': '/dev/console',
        # above backboardd (170) and SpringBoard (160) on the D22's jetsam bands: the link's helper outlives the UI
        'JetsamProperties': {'JetsamPriority': 180, 'JetsamMemoryLimit': -1},
        'EnablePressuredExit': False,
    }
    with open(dst, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    with open(dst, 'rb') as f:
        assert plistlib.load(f)['LaunchDaemons']['/System/Library/LaunchDaemons/%s.plist' % LABEL]['Label'] == LABEL
    print('%s: %d jobs, %s -> %s' % (dst, len(daemons), LABEL, program))


if __name__ == '__main__':
    main()
