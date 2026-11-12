project('sample', 'cpp',
  version : '1.0.0',
  default_options : ['cpp_std=c++23', 'warning_level=3'])

qt = dependency('qt6', modules : ['Core', 'Widgets'], required : true)
sources = files('main.cpp', 'window.cpp')
summary_text = '''Sample
  version @0@'''.format(meson.project_version())

if get_option('tests') and not meson.is_cross_build()
  test_exe = executable('tests', 'tests.cpp', dependencies : qt)
  test('unit', test_exe, timeout : 60 * 2)
elif host_machine.system() == 'windows'
  sources += files('windows.cpp')
endif

foreach name : ['left', 'right']
  if name != 'left'
    continue
  endif
  message('pane: @0@\n'.format(name))
endforeach

executable('sample', sources, dependencies : qt, install : false)
