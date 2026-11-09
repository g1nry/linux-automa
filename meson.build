project('fileward', 'c',
        version: '0.7.0-dev',
        license: 'MIT',
        default_options: ['warning_level=3', 'c_std=c11'])

inc = include_directories('include')

executable('fileward',
           'src/main.c',
           'src/watcher.c',
           'src/log.c',
           'src/config.c',
           'src/action.c',
           'src/glob.c',
           'src/rules.c',
           include_directories: inc,
           install: true)

rule_tests = executable('rules-test',
                        'tests/rules-test.c', 'src/config.c', 'src/rules.c',
                        'src/glob.c', 'src/log.c',
                        include_directories: inc)
test('rules', rule_tests)

glob_tests = executable('glob-test', 'tests/glob-test.c', 'src/glob.c',
                        include_directories: inc)
test('glob', glob_tests)
