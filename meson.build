project('fileward', 'c',
        version: '0.9.0',
        license: 'MIT',
        default_options: ['warning_level=3', 'c_std=c11'])

add_project_arguments('-DFILEWARD_VERSION="' + meson.project_version() + '"', language: 'c')
inc = include_directories('include')
cc = meson.get_compiler('c')
if not cc.has_function('posix_spawn_file_actions_addclosefrom_np', prefix: '#define _GNU_SOURCE\n#include <spawn.h>')
  error('Fileward requires posix_spawn_file_actions_addclosefrom_np (glibc >= 2.34)')
endif
zlib = dependency('zlib')
threads = dependency('threads')

executable('fileward',
           'src/app/main.c', 'src/app/daemon.c', 'src/runtime/engine.c',
           'src/observability/query.c',
           'src/watcher/inotify.c',
           'src/observability/log.c', 'src/observability/audit.c',
           'src/core/config.c',
           'src/actions/action.c',
           'src/actions/template.c',
           'src/core/glob.c',
           'src/core/rules.c',
           include_directories: inc,
           dependencies: [zlib, threads],
           install: true)

rule_tests = executable('rules-test',
                        'tests/unit/rules-test.c', 'src/core/config.c', 'src/core/rules.c',
                        'src/core/glob.c', 'src/observability/log.c', 'src/observability/audit.c', 'src/actions/template.c',
                        include_directories: inc, dependencies: threads)
test('rules', rule_tests)

glob_tests = executable('glob-test', 'tests/unit/glob-test.c', 'src/core/glob.c',
                        include_directories: inc)
test('glob', glob_tests)

action_tests = executable('actions-test', 'tests/unit/actions-test.c', 'src/actions/action.c',
                          'src/actions/template.c', 'src/observability/log.c', 'src/observability/audit.c', include_directories: inc,
                          dependencies: [zlib, threads], link_args: ['-Wl,--wrap=renameat2', '-Wl,--wrap=read', '-Wl,--wrap=write'])
test('actions', action_tests)

audit_tests = executable('audit-test', 'tests/unit/audit-test.c', 'src/observability/audit.c',
                         include_directories: inc, dependencies: threads, link_args: ['-Wl,--wrap=write'])
test('audit', audit_tests)
