project(
  'pipewireao-spa-plugins',
  ['c', 'cpp'],
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.3.0',
  default_options: [
    'c_std=c11',
    'cpp_std=c++20',
    'warning_level=3',
  ],
)

fs = import('fs')

add_project_arguments('-D_GNU_SOURCE', language: 'c')

spa_pkg_dep = dependency('libspa-ao-0.2', version: '>= 0.2')
pipewire_pkg_dep = dependency('libpipewire-ao-0.3', version: '>= 1.7')
plugin_api_dependencies = []
if fs.is_dir('include/pipewireao-plugins')
  plugin_include = include_directories('include')
else
  plugin_api_dependencies += dependency(
    'pipewireao-spa-plugins-0.1',
    version: '>= 0.1.0',
  )
  plugin_include = []
endif
spa_dep = declare_dependency(
  dependencies: [spa_pkg_dep] + plugin_api_dependencies,
  include_directories: include_directories(
    spa_pkg_dep.get_variable(pkgconfig: 'spa_includedir'),
    is_system: true,
  ),
)
pipewire_dep = declare_dependency(dependencies: pipewire_pkg_dep)
cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
dl_dep = cc.find_library('dl', required: false)
m_dep = cc.find_library('m')
threads_dep = dependency('threads')
drm_dep = dependency('libdrm', required: false)
genicam_feature_include = include_directories('spa/plugins/genicam')
genicam_feature_lib = static_library(
  'pwao-genicam-feature',
  'spa/plugins/genicam/feature.c',
)
genicam_feature_dep = declare_dependency(
  link_with: genicam_feature_lib,
  include_directories: genicam_feature_include,
)

if fs.is_dir('include/pipewireao-plugins')
  install_headers(
    'include/pipewireao-plugins/alpao.h',
    'include/pipewireao-plugins/discard.h',
    'include/pipewireao-plugins/fits.h',
    'include/pipewireao-plugins/imagestreamio.h',
    'include/pipewireao-plugins/ndarray.h',
    'include/pipewireao-plugins/nuvu.h',
    'include/pipewireao-plugins/pod.h',
    'include/pipewireao-plugins/pyrtc.h',
    subdir: 'pipewireao-plugins-0.1/pipewireao-plugins',
  )

  pkgconfig_data = configuration_data()
  pkgconfig_data.set('PREFIX', get_option('prefix'))
  pkgconfig_data.set('INCLUDEDIR', get_option('includedir'))
  pkgconfig_data.set('VERSION', meson.project_version())
  configure_file(
    input: 'pipewireao-spa-plugins-0.1.pc.in',
    output: 'pipewireao-spa-plugins-0.1.pc',
    configuration: pkgconfig_data,
    install: true,
    install_dir: get_option('datadir') / 'pkgconfig',
    install_tag: 'devel',
  )
endif

if get_option('tests').allowed()
  image_frame_test = executable(
    'spa-image-frame-test',
    'spa/plugins/test-image-frame.c',
    dependencies: spa_dep,
  )
  test('spa-image-frame', image_frame_test)

  genicam_feature_test = executable(
    'spa-genicam-feature-test',
    'spa/plugins/genicam/test-feature.c',
    dependencies: [spa_dep, genicam_feature_dep],
  )
  test('spa-genicam-feature', genicam_feature_test)
endif

foreach component_dir : [
  'spa/plugins/alpao',
  'spa/plugins/discard',
  'spa/plugins/ndarray',
  'spa/plugins/nuvu',
  'spa/plugins/pyrtc',
  'src/modules/queue',
]
  if fs.exists(component_dir / 'meson.build')
    subdir(component_dir)
  endif
endforeach

egrabber_found = false
if get_option('egrabber').allowed()
  egrabber_include = include_directories(get_option('egrabber-prefix') / 'include')
  egrabber_dep = declare_dependency(include_directories: egrabber_include)
  egrabber_found = cxx.has_header(
    'EGrabber.h',
    dependencies: egrabber_dep,
    required: get_option('egrabber'),
  )
endif
if egrabber_found and fs.exists('spa/plugins/egrabber/meson.build')
  subdir('spa/plugins/egrabber')
endif

bgapi2_found = false
if get_option('bgapi2').allowed()
  bgapi2_prefix = get_option('bgapi2-prefix')
  bgapi2_include = include_directories(bgapi2_prefix / 'include')
  bgapi2_library = cc.find_library(
    'bgapi2_genicam',
    dirs: [bgapi2_prefix / 'lib'],
    required: get_option('bgapi2'),
  )
  bgapi2_dep = declare_dependency(
    include_directories: bgapi2_include,
    dependencies: bgapi2_library,
  )
  bgapi2_found = bgapi2_library.found() and cc.has_header(
    'bgapi2_genicam/bgapi2_genicam.h',
    dependencies: bgapi2_dep,
    required: get_option('bgapi2'),
  )
endif
if bgapi2_found and fs.exists('spa/plugins/bgapi2/meson.build')
  subdir('spa/plugins/bgapi2')
endif

edtpdv_found = false
if get_option('edtpdv').allowed()
  edtpdv_deb = get_option('edtpdv-deb')
  if edtpdv_deb != ''
    dpkg_deb = find_program('dpkg-deb', required: get_option('edtpdv'))
    sha256sum = find_program('sha256sum', required: get_option('edtpdv'))
    edtpdv_deb_hash = run_command(
      sha256sum,
      edtpdv_deb,
      check: true,
    ).stdout().strip().split()[0]
    edtpdv_extract = meson.current_build_dir() / (
      'edtpdv-sdk-' + edtpdv_deb_hash
    )
    run_command(
      dpkg_deb,
      '--extract',
      edtpdv_deb,
      edtpdv_extract,
      check: true,
    )
    edtpdv_prefix = edtpdv_extract / 'opt/EDTpdv'
    edtpdv_install_rpath = '/opt/EDTpdv'
  else
    edtpdv_prefix = get_option('edtpdv-prefix')
    edtpdv_install_rpath = edtpdv_prefix
  endif
  edtpdv_library = cc.find_library(
    'pdv',
    dirs: [edtpdv_prefix],
    required: get_option('edtpdv'),
  )
  edtpdv_dep = declare_dependency(
    compile_args: ['-I' + edtpdv_prefix],
    dependencies: [edtpdv_library, m_dep, threads_dep],
  )
  edtpdv_found = edtpdv_library.found() and cc.has_header(
    'libpdv.h',
    dependencies: edtpdv_dep,
    required: get_option('edtpdv'),
  )
endif
if fs.exists('spa/plugins/edtpdv/meson.build')
  subdir('spa/plugins/edtpdv')
endif

flisdk_found = false
if get_option('flisdk').allowed()
  flisdk_run = get_option('flisdk-run')
  flisdk_runtime_prefix = get_option('flisdk-prefix')
  if flisdk_run != ''
    flisdk_python = import('python').find_installation('python3')
    sha256sum = find_program('sha256sum', required: get_option('flisdk'))
    flisdk_run_hash = run_command(
      sha256sum,
      flisdk_run,
      check: true,
    ).stdout().strip().split()[0]
    flisdk_prefix = meson.current_build_dir() / (
      'flisdk-sdk-' + flisdk_run_hash
    )
    run_command(
      flisdk_python,
      files('scripts/extract-flisdk.py'),
      flisdk_run,
      flisdk_prefix,
      check: true,
    )
  else
    flisdk_prefix = flisdk_runtime_prefix
  endif
  flisdk_library = cc.find_library(
    'FliSdk',
    dirs: [flisdk_prefix / 'lib/release'],
    required: get_option('flisdk'),
  )
  flisdk_dep = declare_dependency(
    compile_args: ['-isystem' + (flisdk_prefix / 'include')],
    dependencies: flisdk_library,
  )
  flisdk_found = flisdk_library.found()
  foreach flisdk_header : ['FliSdk_C_V2.h', 'FliSerialCamera_C.h']
    flisdk_found = flisdk_found and cc.has_header(
      flisdk_header,
      dependencies: flisdk_dep,
      required: get_option('flisdk'),
    )
  endforeach
  flisdk_install_rpath = ':'.join([
    flisdk_runtime_prefix / 'lib/release',
    flisdk_runtime_prefix / '3rdParty',
  ])
endif
if fs.exists('spa/plugins/flisdk/meson.build')
  subdir('spa/plugins/flisdk')
endif

andor3_found = false
if get_option('andor3').allowed()
  andor3_prefix = get_option('andor3-prefix')
  if fs.exists(andor3_prefix / 'inc/atcore.h')
    andor3_include_dir = andor3_prefix / 'inc'
  else
    andor3_include_dir = andor3_prefix / 'include'
  endif
  if fs.is_dir(andor3_prefix / 'x86_64')
    andor3_library_dir = andor3_prefix / 'x86_64'
  else
    andor3_library_dir = andor3_prefix / 'lib'
  endif
  andor3_python = import('python').find_installation('python3')
  andor3_runtime_dir = meson.current_build_dir() / 'andor3-runtime'
  andor3_core = run_command(
    andor3_python,
    files('scripts/prepare-andor3.py'),
    andor3_library_dir,
    andor3_runtime_dir,
    check: get_option('andor3').enabled(),
  )
  if andor3_core.returncode() == 0
    andor3_dep = declare_dependency(
      compile_args: ['-isystem' + andor3_include_dir],
      link_args: [andor3_core.stdout().strip()],
    )
    andor3_found = cc.has_header(
      'atcore.h',
      dependencies: andor3_dep,
      required: get_option('andor3'),
    )
  endif
endif
if fs.exists('spa/plugins/andor3/meson.build')
  subdir('spa/plugins/andor3')
endif

hamamatsu_found = false
if get_option('hamamatsu').allowed()
  hamamatsu_sdk_prefix = get_option('hamamatsu-sdk-prefix')
  if fs.exists(hamamatsu_sdk_prefix / 'inc/dcamapi4.h')
    hamamatsu_include_dir = hamamatsu_sdk_prefix / 'inc'
  else
    hamamatsu_include_dir = hamamatsu_sdk_prefix / 'include'
  endif
  hamamatsu_runtime_prefix = get_option('hamamatsu-runtime-prefix')
  if fs.is_dir(hamamatsu_runtime_prefix / 'core')
    hamamatsu_library_dir = hamamatsu_runtime_prefix / 'core'
  else
    hamamatsu_library_dir = hamamatsu_runtime_prefix
  endif
  hamamatsu_python = import('python').find_installation('python3')
  hamamatsu_runtime_dir = meson.current_build_dir() / 'hamamatsu-runtime'
  hamamatsu_core = run_command(
    hamamatsu_python,
    files('scripts/prepare-hamamatsu.py'),
    hamamatsu_library_dir,
    hamamatsu_runtime_dir,
    check: get_option('hamamatsu').enabled(),
  )
  if hamamatsu_core.returncode() == 0
    hamamatsu_dep = declare_dependency(
      compile_args: ['-isystem' + hamamatsu_include_dir],
      link_args: [hamamatsu_core.stdout().strip()],
    )
    hamamatsu_found = cc.has_header(
      'dcamapi4.h',
      dependencies: hamamatsu_dep,
      required: get_option('hamamatsu'),
    ) and cc.has_header(
      'dcamprop.h',
      dependencies: hamamatsu_dep,
      required: get_option('hamamatsu'),
    )
  endif
endif
if fs.exists('spa/plugins/hamamatsu/meson.build')
  subdir('spa/plugins/hamamatsu')
endif

hermes_found = false
if get_option('hermes').allowed()
  hermes_prefix = get_option('hermes-prefix')
  hermes_include_dir = hermes_prefix / 'include'
  hermes_library_dir = hermes_prefix / 'lib/64'
  hermes_library = cc.find_library(
    'Hermes',
    dirs: [hermes_library_dir],
    required: get_option('hermes'),
  )
  hermes_frontpanel_library = cc.find_library(
    'okFrontPanel',
    dirs: [hermes_library_dir],
    required: get_option('hermes'),
  )
  hermes_dep = declare_dependency(
    compile_args: ['-isystem' + hermes_include_dir],
    dependencies: [hermes_library, hermes_frontpanel_library],
  )
  hermes_found = (hermes_library.found() and
    hermes_frontpanel_library.found() and cc.has_header(
      'Hermes_SDK.h',
      dependencies: hermes_dep,
      required: get_option('hermes'),
    ))
endif
if fs.exists('spa/plugins/hermes/meson.build')
  subdir('spa/plugins/hermes')
endif
if fs.exists('spa/plugins/hermes-decoder/meson.build')
  subdir('spa/plugins/hermes-decoder')
endif

aravis_dep = dependency(
  'aravis-0.10',
  required: get_option('aravis'),
  fallback: ['aravis', 'aravis_library_dependencies'],
  default_options: [
    'c_std=gnu11',
    'tests=false',
    'viewer=disabled',
    'gst-plugin=disabled',
    'documentation=disabled',
    'introspection=disabled',
    'gentl-producer=false',
    'v4l2=disabled',
  ],
)
aravis_found = aravis_dep.found()
if aravis_found and fs.exists('spa/plugins/aravis/meson.build')
  subdir('spa/plugins/aravis')
endif

cfitsio_dep = dependency('cfitsio', required: get_option('fits'))
fits_found = cfitsio_dep.found()
if fits_found and fs.exists('spa/plugins/fits/meson.build')
  subdir('spa/plugins/fits')
endif

imagestreamio_found = false
if get_option('imagestreamio').allowed()
  imagestreamio_prefix = get_option('imagestreamio-prefix')
  imagestreamio_include_root = imagestreamio_prefix / 'include'
  imagestreamio_library = cc.find_library(
    'ImageStreamIO',
    dirs: [imagestreamio_prefix / 'lib', imagestreamio_prefix / 'lib64'],
    required: get_option('imagestreamio'),
  )
  imagestreamio_dep = declare_dependency(
    compile_args: ['-isystem' + imagestreamio_include_root],
    dependencies: [imagestreamio_library, threads_dep],
  )
  imagestreamio_install_rpath = ':'.join([
    imagestreamio_prefix / 'lib',
    imagestreamio_prefix / 'lib64',
  ])
  imagestreamio_found = imagestreamio_library.found() and cc.has_header(
    'ImageStreamIO/ImageStreamIO.h',
    dependencies: imagestreamio_dep,
    required: get_option('imagestreamio'),
  )
endif
if imagestreamio_found and fs.exists('spa/plugins/imagestreamio/meson.build')
  subdir('spa/plugins/imagestreamio')
endif

summary(
  'Format-agnostic discard sink',
  fs.exists('spa/plugins/discard/meson.build'),
  section: 'SPA plugins',
)
summary('eGrabber plugin', egrabber_found, section: 'SPA plugins')
summary('BGAPI2 plugin', bgapi2_found, section: 'SPA plugins')
summary('EDT PDV plugin', edtpdv_found, section: 'SPA plugins')
summary('FliSdk plugin', flisdk_found, section: 'SPA plugins')
summary('Andor SDK3 plugin', andor3_found, section: 'SPA plugins')
summary('Hamamatsu DCAM-API plugin', hamamatsu_found, section: 'SPA plugins')
summary('MPD HERMES FrontPanel plugin', hermes_found, section: 'SPA plugins')
summary('Aravis experimental plugin', aravis_found, section: 'SPA plugins')
summary('FITS cube source', fits_found, section: 'SPA plugins')
summary('ImageStreamIO source/sink', imagestreamio_found, section: 'SPA plugins')
