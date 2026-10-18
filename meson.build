project('forti-hole', 'cpp',
        version : files('VERSION'),
        meson_version : '>=1.3.0',
        license : 'MIT',
        default_options : ['warning_level=3', 'cpp_std=c++23', 'buildtype=release', 'b_ndebug=if-release', 'forti-api:tests=disabled'])

# Release builds (packages, the release workflow) are buildtype=release: -O3, tested as built.
# Pull-request CI builds unoptimized (-O0) and tests that build (.github/workflows/ci.yml).
cxx = meson.get_compiler('cpp')
if not get_option('debug') and host_machine.system() != 'darwin' and cxx.has_link_argument('-Wl,--gc-sections')
    add_global_arguments('-ffunction-sections', '-fdata-sections', language : 'cpp')
    add_global_link_arguments('-Wl,--gc-sections', language : 'cpp')
endif

# Nothing here is a shared library: hidden visibility keeps forti-api's inline schema tables from
# becoming weak exported symbols that dyld coalesces on every start (forti-cli does the same).
add_global_arguments(cxx.get_supported_arguments('-fvisibility=hidden', '-fvisibility-inlines-hidden'), language : 'cpp')

# The command engine, built from forti-cli's source (subproject; forti-cli builds only the engine
# there). It brings forti-api, the only FortiOS implementation, with it.
forti_cli = subproject('forti-cli', default_options : ['subproject_cli=false'])
forti_engine = dependency('forti-engine')

# What `forti-hole --version` reports about the engine it was built from (src/meson.build).
# Package builds (Debian, Homebrew) use forti-cli at exactly the wrap's pinned revision, from an
# unpacked archive without Git metadata, so the wrap's revision is the answer there. A Git checkout in
# subprojects/forti-cli (the wrap's own clone, or a co-development symlink) reports its actual HEAD,
# marked -dirty with uncommitted changes. Evaluated at configure time.
fs = import('fs')
forti_cli_dir = meson.project_source_root() / 'subprojects' / 'forti-cli'
engine_revision = ''
git = find_program('git', required : false, native : true)
if git.found() and fs.exists(forti_cli_dir / '.git')
    head = run_command(git, '-C', forti_cli_dir, 'rev-parse', '--short=7', 'HEAD', check : false)
    if head.returncode() == 0
        engine_revision = head.stdout().strip()
        changes = run_command(git, '-C', forti_cli_dir, 'status', '--porcelain', '--untracked-files=no', check : false)
        if changes.returncode() == 0 and changes.stdout().strip() != ''
            engine_revision += '-dirty'
        endif
    endif
endif
if engine_revision == ''
    foreach line : fs.read('subprojects/forti-cli.wrap').split('\n')
        words = line.split('=')
        if words.length() == 2 and words[0].strip() == 'revision'
            engine_revision = words[1].strip().substring(0, 7)
        endif
    endforeach
endif
if engine_revision == ''
    error('subprojects/forti-cli.wrap has no revision')
endif
# Feed status and paging (system::feed_status, system::feed_entries) arrived in forti-api 1.3.0.
forti_api_dep = dependency('forti-api', version : '>=1.3.0')
forti_api_version = forti_api_dep.version()
libcurl_dep = dependency('libcurl', version : '>=7.85.0')

subdir('src')
subdir('tests')

# The periodic sync: a systemd *user* timer, so the program runs as the operator who owns ~/.forti and
# never as root. Installed only, never enabled: each operator opts in with
# `systemctl --user enable --now forti-hole.timer` (debian/rules keeps debhelper from enabling it).
# macOS has no systemd: a launchd agent template is installed with the examples instead.
if host_machine.system() == 'linux'
    install_data('packaging/systemd/forti-hole.service', 'packaging/systemd/forti-hole.timer',
                 install_dir : get_option('prefix') / 'lib' / 'systemd' / 'user')
endif

# Starting points; never read from here.
install_data('packaging/config.example.json', 'packaging/launchd/com.valkyrianlabs.forti-hole.plist',
             install_dir : get_option('datadir') / 'forti-hole')

# The agent skill (Claude Code / Codex): copy share/forti-hole/skill/forti-hole into the agent's
# skills directory (docs/guides/operations.md says how).
install_data('skill/forti-hole/SKILL.md', install_dir : get_option('datadir') / 'forti-hole' / 'skill' / 'forti-hole')

summary({'engine': 'forti-cli ' + engine_revision, 'forti-api': forti_api_version}, section : 'Built from')
