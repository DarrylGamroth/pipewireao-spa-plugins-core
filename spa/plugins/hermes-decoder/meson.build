hermes_decoder_cargo = find_program('cargo')
hermes_decoder_python = import('python').find_installation('python3')
hermes_decoder_profile = get_option('buildtype') in ['release', 'minsize'] ? 'release' : 'debug'
hermes_decoder_pkg_config_path = ':'.join(get_option('pkg_config_path'))
hermes_decoder_lib = custom_target(
  'spa-hermes-decoder',
  output: 'libspa-hermes-decoder.so',
  command: [
    hermes_decoder_python,
    files('../../../scripts/build-cargo.py'),
    '--cargo', hermes_decoder_cargo,
    '--manifest', meson.project_source_root() / 'Cargo.toml',
    '--target-dir', '@PRIVATE_DIR@' / 'cargo-target',
    '--output', '@OUTPUT@',
    '--profile', hermes_decoder_profile,
    '--spa-include', spa_pkg_dep.get_variable(pkgconfig: 'spa_includedir'),
    '--pkg-config-path', hermes_decoder_pkg_config_path,
    '--package', 'hermes-spa-decoder',
  ],
  build_always_stale: true,
  build_by_default: true,
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'hermes',
  install_tag: 'core',
)

if get_option('tests').allowed()
  hermes_decoder_nm = find_program('nm')
  test(
    'spa-hermes-decoder-exports',
    hermes_decoder_python,
    args: [
      files('../../../scripts/check-exports.py'),
      '--nm', hermes_decoder_nm.full_path(),
      hermes_decoder_lib.full_path(),
    ],
    depends: hermes_decoder_lib,
  )

  hermes_batch_test = executable(
    'spa-hermes-decoder-batch-test',
    'test-batch.c',
    dependencies: [spa_dep, dl_dep],
  )
  test(
    'spa-hermes-decoder-batch',
    hermes_batch_test,
    args: hermes_decoder_lib.full_path(),
    depends: hermes_decoder_lib,
  )
endif
