queue_state = static_library(
  'pipewireao-queue-state',
  'queue.c',
  'buffer-transfer.c',
  dependencies: [threads_dep, spa_dep],
  gnu_symbol_visibility: 'hidden',
  install: false,
)

queue_module = shared_library(
  'pipewire-module-queue',
  'module-queue.c',
  link_with: queue_state,
  dependencies: [spa_dep, pipewire_dep, threads_dep],
  c_args: ['-DPACKAGE_VERSION="@0@"'.format(meson.project_version())],
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: pipewire_pkg_dep.get_variable(pkgconfig: 'moduledir'),
  install_tag: 'core',
)

if get_option('tests').allowed()
  queue_lifetime_test = executable(
    'pipewireao-queue-lifetime-test',
    'test-lifetime.c',
    link_with: queue_state,
    dependencies: [spa_dep, pipewire_dep, threads_dep],
    c_args: ['-DPACKAGE_VERSION="@0@"'.format(meson.project_version())],
  )
  test('pipewireao-queue-lifetime', queue_lifetime_test)

  queue_state_test = executable(
    'pipewireao-queue-state-test',
    'test-queue.c',
    link_with: queue_state,
    dependencies: [threads_dep, spa_dep],
  )
  test('pipewireao-queue-state', queue_state_test)

  queue_buffer_test = executable(
    'pipewireao-queue-buffer-test',
    'test-buffer-transfer.c',
    link_with: queue_state,
    dependencies: spa_dep,
  )
  test('pipewireao-queue-buffer', queue_buffer_test)

  queue_module_test = executable(
    'pipewireao-queue-module-test',
    'test-module.c',
    dependencies: [dl_dep, spa_dep, pipewire_dep],
  )
  queue_module_test_env = environment()
  queue_module_test_env.set(
    'PIPEWIREAO_SPA_PLUGIN_DIR',
    spa_pkg_dep.get_variable(pkgconfig: 'plugindir'),
  )
  queue_module_test_env.set(
    'PIPEWIREAO_CONFIG_DIR',
    pipewire_pkg_dep.get_variable(pkgconfig: 'confdatadir'),
  )
  queue_module_test_env.set(
    'PIPEWIREAO_MODULE_DIR',
    pipewire_pkg_dep.get_variable(pkgconfig: 'moduledir'),
  )
  test(
    'pipewireao-queue-module',
    queue_module_test,
    args: queue_module.full_path(),
    depends: queue_module,
    env: queue_module_test_env,
  )

  queue_live_test = executable(
    'pipewireao-queue-live-test',
    'test-live.c',
    dependencies: [spa_dep, pipewire_dep, threads_dep],
  )
  test(
    'pipewireao-queue-live',
    queue_live_test,
    args: queue_module.full_path(),
    depends: queue_module,
    env: queue_module_test_env,
    timeout: 30,
  )

  pipewire_daemon = find_program(
    pipewire_pkg_dep.get_variable(pkgconfig: 'confdatadir') / 'pipewire-ao',
    required: false,
  )
  shell = find_program('sh')
  if pipewire_daemon.found()
    queue_remote_test = executable(
      'pipewireao-queue-remote-test',
      'test-remote-registry.c',
      dependencies: [spa_dep, pipewire_dep],
    )
    test(
      'pipewireao-queue-remote',
      shell,
      args: [
        files('test-remote.sh'),
        queue_remote_test.full_path(),
        pipewire_daemon.full_path(),
        meson.current_source_dir() / 'test-remote.conf',
        meson.current_build_dir(),
      ],
      depends: [queue_module, queue_remote_test],
      env: queue_module_test_env,
      timeout: 15,
    )
  endif

  benchmark(
    'pipewireao-queue-producer-cycle',
    queue_live_test,
    args: [queue_module.full_path(), '--benchmark'],
    depends: queue_module,
    env: queue_module_test_env,
    timeout: 30,
  )
endif
