pyrtc_lib = shared_library(
  'spa-pyrtc',
  ['plugin.c', 'node.c', 'stream.c'],
  dependencies: [spa_dep, m_dep],
  include_directories: plugin_include,
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'pyrtc',
  install_tag: 'core',
)

if get_option('tests').allowed()
  pyrtc_factory_test = executable(
    'spa-pyrtc-factory-test',
    '../../../tests/pyrtc/test-factory.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
  )
  test(
    'spa-pyrtc-factory',
    pyrtc_factory_test,
    args: [pyrtc_lib.full_path()],
    depends: pyrtc_lib,
  )

  pyrtc_stream_test = executable(
    'spa-pyrtc-stream-test',
    ['../../../tests/pyrtc/test-stream.c', 'stream.c'],
    dependencies: [spa_dep, m_dep],
  )
  test('spa-pyrtc-stream', pyrtc_stream_test)

  pyrtc_node_test = executable(
    'spa-pyrtc-node-test',
    '../../../tests/pyrtc/test-node.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
  )
  test(
    'spa-pyrtc-node',
    pyrtc_node_test,
    args: [pyrtc_lib.full_path()],
    depends: pyrtc_lib,
  )
endif
