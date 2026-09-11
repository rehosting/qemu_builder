# fastsnap: device state in a block, for fast in-process snapshot restore.
#
# These sources are part of the system emulation build for every target. The
# cost when unused is the QOM type registration for the buffer channel and one
# machine-init-done notifier; nothing here runs unless a caller asks for a
# device block, and nothing here touches a TCG fast path.
system_ss.add(files(
  'channel-buffer-writeback.c',
  'device-save.c',
  'selftest.c',
))
