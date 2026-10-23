# The engine side of the PANDA component: the hook table (panda/hooks.c). The runtime
# (src/, libpanda-<target>.so) is built per target in meson.build's target loop from
# panda_runtime_ss, as a module the engine does not link.

# panda-abi.h: the panda ABI hash (as for panda modules: boards, devices). The boards branch
# makes it in meson.build; make it here only when it isn't there.
if not is_variable('panda_abi')
  panda_abi = custom_target('panda-abi.h',
                            output: 'panda-abi.h',
                            command: [python, files('../scripts/panda-abi.py'),
                                      meson.project_source_root(),
                                      meson.project_build_root()],
                            capture: true,
                            build_by_default: true,
                            build_always_stale: true)
  genh += panda_abi
endif

system_ss.add(files('hooks.c'), panda_abi)

subdir('src')
