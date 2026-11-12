"""Read the pinned SDK's platform-specific wrapper source manifest without CMake."""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1]) / 'libcef_dll'
platform = {'linux': 'LINUX', 'darwin': 'MAC', 'windows': 'WINDOWS'}[sys.argv[2]]
manifest = (root / 'CMakeLists.txt').read_text()
sources = []
for name, contents in re.findall(r'set\((LIBCEF_\w*SRCS(?:_\w+)?)\s+(.*?)\)', manifest, re.S):
    if name.endswith(('_LINUX', '_MAC', '_WINDOWS')) and not name.endswith('_' + platform):
        continue
    for entry in contents.split():
        if entry.endswith(('.cc', '.mm')):
            path = root / entry
            if not path.is_file(): raise SystemExit('Missing CEF wrapper source: ' + str(path))
            sources.append(path.as_posix())
if not sources: raise SystemExit('The CEF SDK wrapper source manifest is empty')
print('\n'.join(sources))
