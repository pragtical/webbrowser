#!/usr/bin/env python3
"""Validate the staged ABI and the files needed by the off-screen host."""
import ctypes
import json
from pathlib import Path
import platform
import sys

runtime = Path(sys.argv[1] if len(sys.argv) > 1 else '.build/stage/runtime').resolve()
windows = platform.system() == 'Windows'
bridge = ctypes.CDLL(str(runtime / ('webbrowser-bridge.dll' if windows else 'webbrowser-bridge.so')))
assert bridge.wb_abi_version() == 5, 'Incorrect bridge ABI'
assert bridge.wb_state_size() == 5712, 'Incorrect transport layout'
metadata = json.loads((runtime / 'build-info.json').read_text())
assert metadata['cef'] == json.loads(Path('cef.json').read_text())['version']
assert (runtime / 'licenses/LICENSE.txt').is_file()
assert (runtime / 'licenses/CREDITS.html').is_file()
if platform.system() == 'Darwin':
    app = runtime / 'webbrowser-host.app/Contents'
    assert (app / 'MacOS/webbrowser-host').is_file()
    assert (app / 'Frameworks/Chromium Embedded Framework.framework/Chromium Embedded Framework').is_file()
    for suffix in ['', ' (Alerts)', ' (GPU)', ' (Plugin)', ' (Renderer)']:
        name = 'WebBrowserHost Helper' + suffix
        assert (app / f'Frameworks/{name}.app/Contents/MacOS/{name}').is_file()
else:
    for name in ['webbrowser-host.exe' if windows else 'webbrowser-host',
                 'libcef.dll' if windows else 'libcef.so', 'icudtl.dat', 'resources.pak']:
        assert (runtime / name).is_file(), name
    assert (runtime / 'locales').is_dir()
    if windows: assert (runtime / 'webbrowser-host.dll').is_file()
print('PASS', metadata['arch'], 'CEF runtime and bridge ABI')
