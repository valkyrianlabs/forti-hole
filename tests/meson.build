# Network-free tests: nothing here talks to a FortiGate or a list server. The commands run against an
# in-memory FortiGate (support/fake_gateway.hpp), a scripted list server and a temporary forti home.
gtest_dep = dependency('gtest', main : true, required : get_option('tests'))

# Sanitizer builds leave the engine shards uninstrumented (forti-cli's engine/meson.build).
asan_options = get_option('b_sanitize').contains('address') ? 'detect_container_overflow=0' : ''
test_env = asan_options != '' ? {'ASAN_OPTIONS' : asan_options} : {}

if gtest_dep.found()
    compiler_tests = executable('compilerTests',
                                'core/test_domain_parse.cpp',
                                'core/test_compile_budget.cpp',
                                dependencies : [fh_compiler_dep, gtest_dep],
                                implicit_include_directories : false)
    test('compiler', compiler_tests, env : test_env, suite : 'unit')

    unit_tests = executable('unitTests',
                            'unit/test_units.cpp',
                            'unit/test_sync.cpp',
                            include_directories : include_directories('support'),
                            dependencies : [fh_core_dep, gtest_dep],
                            implicit_include_directories : false)
    test('unit', unit_tests, env : test_env, suite : 'unit', timeout : 120)
endif

# The real binary end to end (suite 'integration'): forti-hole through the real engine (libcurl, verified
# TLS, plan/apply, blob-stored bodies, audit) against forti-cli's stateful mock FortiOS, with lists
# served from file:// URLs. Exit 77 (skipped) without python3, openssl or the mock.
fh_mock_fortios_dir = forti_cli.get_variable('mock_fortios_dir', '')
fh_python = find_program('python3', required : false)
if fh_python.found() and not get_option('tests').disabled()
    test('e2e', fh_python, args : [files('integration/test_e2e.py')],
         env : {'FH_BINARY' : forti_hole_exe.full_path(), 'FORTI_MOCK_DIR' : fh_mock_fortios_dir, 'PYTHONDONTWRITEBYTECODE' : '1'}
               + test_env,
         depends : [forti_hole_exe], suite : 'integration', timeout : 300)
endif
