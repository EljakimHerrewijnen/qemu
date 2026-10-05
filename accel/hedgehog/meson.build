system_ss.add(files('hedgehog.c', 'hedgehog-mmio-device.c'))
specific_ss.add(
  when: 'TARGET_AARCH64',
  if_true: files('hedgehog-arm.c'),
  if_false: files('hedgehog-arch-stub.c'),
)
