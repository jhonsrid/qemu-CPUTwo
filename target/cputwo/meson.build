gen = [
  decodetree.process('insn.decode', extra_args: ['--static-decode=decode'])
]

cputwo_ss = ss.source_set()
cputwo_ss.add(gen)
cputwo_ss.add(files(
  'translate.c',
  'op_helper.c',
  'helper.c',
  'cpu.c',
  'gdbstub.c',
  'disas.c',
))

target_arch += {'cputwo': cputwo_ss}
target_system_arch += {'cputwo': ss.source_set()}
