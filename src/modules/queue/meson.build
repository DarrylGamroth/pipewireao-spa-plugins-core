queue_state = static_library(
  'pipewireao-queue-state',
  'queue.c',
  'buffer-transfer.c',
  '../../../spa/plugins/metadata.c',
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

  queue_metadata_test = executable('pipewireao-queue-metadata-test',
    'test-metadata.c', '../../../spa/plugins/metadata.c', dependencies: spa_dep)
  test('pipewireao-queue-metadata', queue_metadata_test)

  queue_properties_test = executable(
    'pipewireao-queue-properties-test',
    'test-properties.c',
    dependencies: spa_dep,
  )
  test('pipewireao-queue-properties', queue_properties_test)

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
    pipewire_pkg_dep.get_variable(
      pkgconfig: 'confdatadir',
      default_value: pipewire_pkg_dep.get_variable(pkgconfig: 'prefix') / 'share' / 'pipewire-ao',
    ),
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
    pipewire_pkg_dep.get_variable(pkgconfig: 'prefix') / 'bin' / 'pipewire-ao',
    pipewire_pkg_dep.get_variable(pkgconfig: 'confdatadir',
      default_value: pipewire_pkg_dep.get_variable(pkgconfig: 'prefix') / 'share' / 'pipewire-ao') / 'pipewire-ao',
    required: false,
  )
  shell = find_program('sh')
  queue_module_test_env.prepend(
    'LD_LIBRARY_PATH', pipewire_pkg_dep.get_variable(pkgconfig: 'libdir'),
  )
  if pipewire_daemon.found()
    queue_remote_properties_test = executable(
      'pipewireao-queue-remote-properties-test',
      'test-remote-properties.c',
      dependencies: [spa_dep, pipewire_dep],
    )
    foreach name, options : {
      'pipewireao-queue-remote-properties': [],
      'pipewireao-queue-remote-opaque': ['--queue-only', '--opaque'],
      'pipewireao-queue-remote-opaque-required-missing': ['--queue-only', '--opaque-missing'],
      'pipewireao-queue-remote-buffer-properties': ['--metadata-records'],
      'pipewireao-queue-remote-buffer-properties-queue-only': ['--queue-only', '--metadata-records'],
      'pipewireao-queue-remote-buffer-properties-lease': ['--lease', '--queue-only', '--metadata-records'],
      'pipewireao-queue-remote-properties-header': ['--header-only'],
      'pipewireao-queue-remote-properties-pause': ['--pause'],
      'pipewireao-queue-remote-properties-shared-loop': ['--same-loop'],
      'pipewireao-queue-remote-video-properties': ['--video', '--queue-only'],
      'pipewireao-queue-remote-video-header': ['--video', '--queue-only', '--header-only'],
      'pipewireao-queue-remote-video-lease-properties': ['--lease', '--video', '--queue-only'],
      'pipewireao-queue-remote-video-lease-header': ['--lease', '--video', '--queue-only', '--header-only'],
    }
      test(
        name,
        shell,
        args: [
          files('test-remote-properties.sh'),
          queue_remote_properties_test.full_path(),
          pipewire_daemon.full_path(),
          meson.current_source_dir() / 'test-remote-properties.conf',
          meson.current_build_dir(),
        ] + options,
        depends: [queue_module, queue_remote_properties_test],
        env: queue_module_test_env,
        timeout: 25,
      )
    endforeach
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
