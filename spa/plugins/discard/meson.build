# Keep libgcov's runtime symbols out of the SPA plugin's public ABI.
discard_link_args = []
if get_option('b_coverage') and cc.get_id() == 'gcc'
  discard_link_args += '-Wl,--exclude-libs,libgcov.a'
endif

discard_lib = shared_library(
  'spa-pipewireao-discard',
  ['plugin.c', 'sink.c'],
  dependencies: spa_dep,
  include_directories: plugin_include,
  c_args: ['-Wno-pedantic'],
  link_args: discard_link_args,
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'discard',
  install_tag: 'core',
)

if get_option('tests').allowed()
  nm = find_program('nm')
  python = import('python').find_installation('python3')
  discard_test = executable(
    'spa-pipewireao-discard-test',
    'test-sink.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-pipewireao-discard',
    discard_test,
    args: [discard_lib.full_path()],
    depends: discard_lib,
  )
  test(
    'spa-pipewireao-discard-exports',
    python,
    args: [
      files('../../../scripts/check-exports.py'),
      '--nm', nm.full_path(),
      discard_lib.full_path(),
    ],
    depends: discard_lib,
  )
endif
