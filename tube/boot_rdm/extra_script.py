# -nostartfiles must reach the linker too (pio passes build_flags to the
# compiler only): no avr-libc crt / vector table, boot_entry() is the entry.
# No crt also means no .data init: fail the build if anything lands in .data.
import subprocess

Import("env")
env.Append(LINKFLAGS=["-nostartfiles"])


def check_no_data(source, target, env):
    elf = str(target[0])
    out = subprocess.run([env.subst("$SIZETOOL"), "-A", elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 2 and f[0] == ".data" and int(f[1]) != 0:
            print(f"ERROR: {elf} has {f[1]} B of .data - no C runtime copies it (use .bss or PROGMEM)")
            env.Exit(1)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_no_data)
