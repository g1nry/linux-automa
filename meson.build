project('linux-automa', 'c',
        version: '0.1.0',
        license: 'MIT',
        default_options: ['warning_level=3', 'c_std=c11'])

inc = include_directories('include')

executable('fileward',
           'src/main.c',
           'src/watcher.c',
           'src/log.c',
           install: true)

