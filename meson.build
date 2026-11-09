project('fileward', 'c',
        version: '0.8.0',
        license: 'MIT',
        default_options: ['warning_level=3', 'c_std=c11'])

inc = include_directories('include')
zlib = dependency('zlib')

executable('fileward',
           'src/main.c',
           'src/watcher.c',
           'src/log.c',
           'src/config.c',
           'src/action.c',
           'src/template.c',
           'src/glob.c',
           'src/rules.c',
           include_directories: inc,
           dependencies: zlib,
           install: true)

rule_tests = executable('rules-test',
                        'tests/rules-test.c', 'src/config.c', 'src/rules.c',
                        'src/glob.c', 'src/log.c', 'src/template.c',
                        include_directories: inc)
test('rules', rule_tests)

glob_tests = executable('glob-test', 'tests/glob-test.c', 'src/glob.c',
                        include_directories: inc)
test('glob', glob_tests)

action_tests = executable('actions-test', 'tests/actions-test.c', 'src/action.c',
                          'src/template.c', 'src/log.c', include_directories: inc,
                          dependencies: zlib, link_args: ['-Wl,--wrap=renameat2', '-Wl,--wrap=read', '-Wl,--wrap=write'])
test('actions', action_tests)
