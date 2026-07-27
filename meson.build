project('deskpal', 'c',
  version : '0.2.0',
  default_options : ['c_std=c17', 'warning_level=2'])

# ── System dependencies ──────────────────────────────────────────────────────
xcb_dep       = dependency('xcb')
xcb_shm_dep   = dependency('xcb-shm')
x11_dep       = dependency('x11')
png_dep       = dependency('libpng', required : true)
dbus_dep      = dependency('dbus-1', required : true)
atspi_dep     = dependency('atspi-2', required : get_option('atspi'))
gobject_dep   = dependency('gobject-2.0', required : atspi_dep.found())

# libxdo doesn't have pkg-config, use compiler check
cc = meson.get_compiler('c')
xdo_dep = cc.find_library('xdo', required : true)

# x11-xcb for XGetXCBConnection
x11xcb_dep    = dependency('x11-xcb', required : true)

# Optional: tesseract (dlopen at runtime, headers needed at build time)
tesseract_dep = dependency('tesseract', required : false)
lept_dep      = dependency('lept', required : false)

# ── Vendored sources ─────────────────────────────────────────────────────────
vendor_inc = include_directories('vendor/cjson', 'vendor/stb')
vendor_src = files('vendor/cjson/cJSON.c')

# ── Core sources ─────────────────────────────────────────────────────────────
core_src = files(
  'csrc/main.c',
  'csrc/mcp.c',
  'csrc/mcp_transport.c',
  'csrc/x11.c',
  'csrc/uinput.c',
  'csrc/captures.c',
  'csrc/capture_target.c',
  'csrc/frame_tools.c',
  'csrc/pixel_action.c',
  'csrc/indicator.c',
  'csrc/ocr.c',
  'csrc/tools.c',
  'csrc/sessions.c',
  'csrc/control.c',
)

c_args = ['-D_GNU_SOURCE']
link_args = ['-lm']

if tesseract_dep.found() and lept_dep.found()
  c_args += '-DHAVE_TESSERACT=1'
endif

if atspi_dep.found()
  c_args += '-DHAVE_ATSPI=1'
endif

core_deps = [xcb_dep, xcb_shm_dep, x11_dep, x11xcb_dep, xdo_dep, png_dep, dbus_dep]
if atspi_dep.found()
  core_deps += [atspi_dep, gobject_dep]
endif

frame_observation = static_library('frame-observation',
  files('csrc/frame_buffer.c', 'csrc/screenshot.c', 'csrc/frame_state.c',
        'csrc/frame_settle.c'),
  include_directories : [vendor_inc, include_directories('csrc')],
  dependencies : core_deps,
  c_args : c_args)

broker_contract = static_library('broker-contract',
  files('csrc/broker_contract.c'),
  include_directories : [vendor_inc, include_directories('csrc')],
  c_args : c_args)

semantic_observation = static_library('semantic-observation',
  files('csrc/accessibility.c', 'csrc/semantic_state.c',
        'csrc/semantic_events.c', 'csrc/semantic_change.c'), vendor_src,
  include_directories : [vendor_inc, include_directories('csrc')],
  dependencies : core_deps,
  c_args : c_args)

executable('deskpal',
  core_src,
  link_with : [frame_observation, semantic_observation, broker_contract],
  include_directories : [vendor_inc, include_directories('csrc')],
  dependencies : core_deps,
  c_args : c_args,
  link_args : link_args + ['-ldl'],  # for dlopen
  install : true)

executable('broker-contract-test',
  files('test/broker_contract_test.c'),
  link_with : broker_contract,
  include_directories : [vendor_inc, include_directories('csrc')],
  c_args : c_args,
  install : false)

executable('frame-state-test',
  files('test/frame_state_test.c'),
  link_with : frame_observation,
  include_directories : [vendor_inc, include_directories('csrc')],
  dependencies : core_deps,
  c_args : c_args,
  install : false)

# Safe integration helper for the temporary AT-SPI listener lifecycle proof.
executable('accessibility-event-probe',
  files('test/accessibility_event_probe.c', 'csrc/x11.c', 'csrc/uinput.c'),
  link_with : semantic_observation,
  include_directories : [vendor_inc, include_directories('csrc')],
  dependencies : core_deps,
  c_args : c_args,
  link_args : link_args,
  install : false)
