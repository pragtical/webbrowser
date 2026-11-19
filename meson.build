project('pragtical-webbrowser', 'cpp', version: '0.2.0', meson_version: '>=0.61.0',
  default_options: ['cpp_std=c++20', 'buildtype=release', 'b_ndebug=true', 'b_vscrt=mt'])

cpp = meson.get_compiler('cpp')
python = import('python').find_installation()
cef_root = meson.project_source_root() / get_option('cef_root')
cef_includes = include_directories(get_option('cef_root'))
system = host_machine.system()
cef_args = ['-DCEF_USE_SANDBOX', '-D__STDC_CONSTANT_MACROS', '-D__STDC_FORMAT_MACROS']
cef_deps = [dependency('threads')]
host_sources = files('native/host.cpp', 'native/transport.cpp')
bridge_sources = files('native/bridge.cpp', 'native/transport.cpp')

if system == 'windows'
  if cpp.get_id() != 'msvc'
    error('The CEF Windows bootstrap requires MSVC')
  endif
  cef_args += ['-DCEF_API_VERSION=CEF_API_VERSION_LAST', '-DWIN32', '-DUNICODE', '-D_UNICODE',
    '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN']
  foreach name : ['comctl32', 'crypt32', 'delayimp', 'gdi32', 'rpcrt4', 'shlwapi', 'wintrust', 'ws2_32']
    cef_deps += cpp.find_library(name)
  endforeach
  cef_deps += cpp.find_library('libcef', dirs: cef_root / 'Release')
elif system == 'darwin'
  add_languages('objcpp')
  add_project_arguments('-std=c++20', language: 'objcpp')
  add_project_arguments('-mmacosx-version-min=13.0', language: ['cpp', 'objcpp'])
  add_project_link_arguments('-mmacosx-version-min=13.0', language: ['cpp', 'objcpp'])
  host_sources += files('native/application_mac.mm')
  cef_deps += dependency('appleframeworks', modules: ['AppKit', 'Cocoa', 'IOSurface'])
else
  cef_args += ['-D_FILE_OFFSET_BITS=64']
  cef_deps += [cpp.find_library('X11'), cpp.find_library('dl'),
    declare_dependency(link_args: [cef_root / 'Release/libcef.so'])]
endif

# Read the SDK's generated wrapper source list as data; CMake is not invoked.
wrapper_sources = run_command(python, files('scripts/cef-wrapper-sources.py'), cef_root, system,
  check: true).stdout().strip().replace('\r', '').split('\n')
wrapper = static_library('cef-wrapper', wrapper_sources, include_directories: cef_includes,
  cpp_args: cef_args + ['-DWRAPPING_CEF_SHARED'], objcpp_args: cef_args + ['-DWRAPPING_CEF_SHARED'],
  dependencies: cef_deps, pic: true, gnu_symbol_visibility: 'hidden')

shared_library('webbrowser-bridge', bridge_sources, name_prefix: '', name_suffix: system == 'windows' ? 'dll' : 'so',
  gnu_symbol_visibility: 'hidden')
if system == 'windows'
  shared_library('webbrowser-host', host_sources, name_prefix: '', include_directories: cef_includes,
    cpp_args: cef_args, link_with: wrapper, dependencies: cef_deps)
else
  executable('webbrowser-host', host_sources, include_directories: cef_includes,
    cpp_args: cef_args, objcpp_args: cef_args, link_with: wrapper, dependencies: cef_deps,
    build_rpath: system == 'linux' ? '$ORIGIN' : '')
endif
