common_ss.add(files('accel-common.c'))
specific_ss.add(files('accel-target.c'))
system_ss.add(files('accel-system.c', 'accel-blocker.c', 'accel-qmp.c', 'accel-irq.c'))
user_ss.add(files('accel-user.c'))

subdir('tcg')
if have_system
  subdir('hvf')
  subdir('whpx')
  subdir('qtest')
  if have_tcg and get_option('unicorn').allowed()
    subdir('unicorn')
  endif
  subdir('kvm')
  subdir('xen')
  subdir('stubs')
  subdir('mshv')
  subdir('nitro')
endif

# qtest
system_ss.add(files('dummy-cpus.c'))
