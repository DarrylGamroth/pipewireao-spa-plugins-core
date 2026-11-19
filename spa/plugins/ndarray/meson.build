cargo = find_program('cargo')
python = import('python').find_installation('python3')
cargo_profile = get_option('buildtype') in ['release', 'minsize'] ? 'release' : 'debug'
configured_pkg_config_path = ':'.join(get_option('pkg_config_path'))

ndarray_lib = custom_target(
  'spa-ndarray',
  output: 'libspa-ndarray.so',
  command: [
    python,
    files('../../../scripts/build-cargo.py'),
    '--cargo', cargo,
    '--manifest', meson.project_source_root() / 'Cargo.toml',
    '--package', 'pipewireao-ndarray-spa-plugin',
    '--target-dir', '@PRIVATE_DIR@' / 'cargo-target',
    '--output', '@OUTPUT@',
    '--profile', cargo_profile,
    '--spa-include', spa_pkg_dep.get_variable(pkgconfig: 'spa_includedir'),
    '--pkg-config-path', configured_pkg_config_path,
  ],
  build_always_stale: true,
  build_by_default: true,
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'ndarray',
  install_tag: 'core',
)

if get_option('tests').allowed()
  nm = find_program('nm')
  ndarray_test = executable(
    'spa-ndarray-transforms-test',
    '../../../tests/ndarray/test-frame-assembly.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-ndarray-transforms',
    ndarray_test,
    args: [ndarray_lib.full_path()],
    depends: ndarray_lib,
  )
  test(
    'spa-ndarray-exports',
    python,
    args: [
      files('../../../scripts/check-exports.py'),
      '--nm', nm.full_path(),
      ndarray_lib.full_path(),
    ],
    depends: ndarray_lib,
  )
endif
