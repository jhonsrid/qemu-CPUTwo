cputwo_ss = ss.source_set()
cputwo_ss.add(when: 'CONFIG_CPUTWO_BOARD', if_true: files(
    'cputwo_board.c',
    'cputwo_uart.c',
))

hw_arch += {'cputwo': cputwo_ss}
