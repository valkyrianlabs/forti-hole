# forti_hole_version.hpp carries the project version and what it was built from (the forti-cli
# engine revision and the forti-api version; see the top-level meson.build) into --version.
version_hpp = configure_file(output : 'forti_hole_version.hpp',
                             configuration : {'FORTI_HOLE_VERSION' : '"' + meson.project_version() + '"',
                                              'FORTI_HOLE_BUILT_FROM' : '"engine forti-cli ' + engine_revision
                                                                          + ', forti-api ' + forti_api_version + '"'})

fh_inc = include_directories('.')

# The list compiler: names, parsers, deduplication, encoding, budgets. Pure functions over text, no
# network, no engine, no JSON: its own library so it builds and tests on its own.
fh_compiler = static_library('forti-hole-compiler',
                             'core/domain.cpp', 'core/parse.cpp', 'core/compile.cpp', 'core/budget.cpp', 'core/sha256.cpp',
                             include_directories : fh_inc,
                             implicit_include_directories : false)
fh_compiler_dep = declare_dependency(include_directories : fh_inc, link_with : fh_compiler)

# Everything but main(): the executable and the tests link the same library.
fh_core = static_library('forti-hole-core',
                         'config.cpp', 'files.cpp', 'state.cpp', 'fetch/fetch.cpp', 'fetch/https.cpp', 'sources.cpp', 'pipeline.cpp',
                         'guard.cpp', 'gateway.cpp', 'device.cpp', 'sync.cpp', 'manage.cpp', 'pihole_import.cpp', 'legacy.cpp',
                         'report.cpp', 'cli.cpp',
                         version_hpp,
                         include_directories : fh_inc,
                         dependencies : [forti_engine, forti_api_dep, libcurl_dep, fh_compiler_dep],
                         implicit_include_directories : false)
fh_core_dep = declare_dependency(include_directories : fh_inc, link_with : fh_core,
                                 dependencies : [forti_engine, forti_api_dep, libcurl_dep, fh_compiler_dep], sources : version_hpp)

forti_hole_exe = executable('forti-hole', 'main.cpp',
                            dependencies : fh_core_dep,
                            implicit_include_directories : false,
                            install : true)
