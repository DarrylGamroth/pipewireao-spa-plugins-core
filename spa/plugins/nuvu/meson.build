cargo = find_program('cargo')
python = import('python').find_installation('python3')
cargo_profile = get_option('buildtype') in ['release', 'minsize'] ? 'release' : 'debug'
configured_pkg_config_path = ':'.join(get_option('pkg_config_path'))

nuvu_lib = custom_target(
  'spa-nuvu',
  output: 'libspa-nuvu.so',
  command: [
    python,
    files('../../../scripts/build-cargo.py'),
    '--cargo', cargo,
    '--manifest', meson.project_source_root() / 'Cargo.toml',
    '--package', 'nuvu-spa-plugin',
    '--target-dir', '@PRIVATE_DIR@' / 'cargo-target',
    '--output', '@OUTPUT@',
    '--profile', cargo_profile,
    '--spa-include', spa_pkg_dep.get_variable(pkgconfig: 'spa_includedir'),
    '--pkg-config-path', configured_pkg_config_path,
  ],
  build_always_stale: true,
  build_by_default: true,
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'nuvu',
  install_tag: 'core',
)

if get_option('tests').allowed()
  nm = find_program('nm')
  nuvu_test = executable(
    'spa-nuvu-factory-test',
    '../../../tests/nuvu/test-factory.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-nuvu-factory',
    nuvu_test,
    args: [nuvu_lib.full_path()],
    depends: nuvu_lib,
  )
  test(
    'spa-nuvu-exports',
    python,
    args: [
      files('../../../scripts/check-exports.py'),
      '--nm', nm.full_path(),
      nuvu_lib.full_path(),
    ],
    depends: nuvu_lib,
  )
endif
