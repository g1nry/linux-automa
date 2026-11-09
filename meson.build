project('fileward', 'c',
        version: '0.8.0',
        license: 'MIT',
        default_options: ['warning_level=3', 'c_std=c11'])

inc = include_directories('include')
zlib = dependency('zlib')

executable('fileward',
           'src/app/main.c',
           'src/watcher/inotify.c',
           'src/observability/log.c',
           'src/core/config.c',
           'src/actions/action.c',
           'src/actions/template.c',
           'src/core/glob.c',
           'src/core/rules.c',
           include_directories: inc,
           dependencies: zlib,
           install: true)

rule_tests = executable('rules-test',
                        'tests/unit/rules-test.c', 'src/core/config.c', 'src/core/rules.c',
                        'src/core/glob.c', 'src/observability/log.c', 'src/actions/template.c',
                        include_directories: inc)
test('rules', rule_tests)

glob_tests = executable('glob-test', 'tests/unit/glob-test.c', 'src/core/glob.c',
                        include_directories: inc)
test('glob', glob_tests)

action_tests = executable('actions-test', 'tests/unit/actions-test.c', 'src/actions/action.c',
                          'src/actions/template.c', 'src/observability/log.c', include_directories: inc,
                          dependencies: zlib, link_args: ['-Wl,--wrap=renameat2', '-Wl,--wrap=read', '-Wl,--wrap=write'])
test('actions', action_tests)
