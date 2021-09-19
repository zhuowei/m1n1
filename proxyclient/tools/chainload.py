#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import sys, pathlib
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

import argparse, pathlib, time

parser = argparse.ArgumentParser(description='Mach-O loader for m1n1')
parser.add_argument('-x', '--xnu', action="store_true", help="Load XNU")
parser.add_argument('-c', '--call', action="store_true", help="Use call mode")
parser.add_argument('payload', type=pathlib.Path)
parser.add_argument('boot_args', default=[], nargs="*")
args = parser.parse_args()

from m1n1.setup import *
from m1n1.tgtypes import BootArgs
from m1n1.macho import MachO
from m1n1 import asm

macho = MachO(args.payload.read_bytes())

image = macho.prepare_image()

new_base = u.base

entry = macho.entry
entry -= macho.vmin
entry += new_base

if args.xnu:
    sepfw_start, sepfw_length = u.adt["chosen"]["memory-map"].SEPFW
else:
    sepfw_start, sepfw_length = 0, 0

image_size = align(len(image))
sepfw_off = image_size
image_size += align(sepfw_length)
bootargs_off = image_size
bootargs_size = 0x4000
image_size += bootargs_size

print(f"Total region size: 0x{image_size:x} bytes")
image_addr = u.malloc(image_size)

print(f"Loading kernel image (0x{len(image):x} bytes)...")
u.compressed_writemem(image_addr, image, True)
p.dc_cvau(image_addr, len(image))

if args.xnu:
    print(f"Copying SEPFW (0x{sepfw_length:x} bytes)...")
    p.memcpy8(image_addr + sepfw_off, sepfw_start, sepfw_length)
    print(f"Adjusting addresses in ADT...")
    u.adt["chosen"]["memory-map"].SEPFW = (new_base + sepfw_off, sepfw_length)
    u.adt["chosen"]["memory-map"].BootArgs = (image_addr + bootargs_off, bootargs_size)

    print("Setting secondary CPU RVBARs...")

    rvbar = entry & ~0xfff
    for cpu in u.adt["cpus"][1:]:
        addr, size = cpu.cpu_impl_reg
        print(f"  {cpu.name}: [0x{addr:x}] = 0x{rvbar:x}")
        p.write64(addr, rvbar)

    u.push_adt()

print("Setting up bootargs...")
tba = u.ba.copy()

if args.xnu:
    tba.top_of_kernel_data = new_base + image_size
else:
    # SEP firmware is in here somewhere, keep top_of_kdata high so we hopefully don't clobber it
    tba.top_of_kernel_data = max(tba.top_of_kernel_data, new_base + image_size)

if len(args.boot_args) > 0:
    boot_args = " ".join(args.boot_args)
    if "-v" in boot_args.split():
        tba.video.display = 0
    else:
        tba.video.display = 1
    print(f"Setting boot arguments to {boot_args!r}")
    tba.cmdline = boot_args
# hack: iOS
if True:
    tba.virt_base += (0xfffffff007004000 - 0xfffffe0007004000)
    tba.devtree += (0xfffffff007004000 - 0xfffffe0007004000)

iface.writemem(image_addr + bootargs_off, BootArgs.build(tba))

print(f"Copying stub...")

stub = asm.ARMAsm(f"""
1:
        ldp x4, x5, [x1], #8
        stp x4, x5, [x2]
        dc cvau, x2
        ic ivau, x2
        add x2, x2, #8
        sub x3, x3, #8
        cbnz x3, 1b

        ldr x1, ={entry}
        br x1
""", image_addr + image_size)

iface.writemem(stub.addr, stub.data)
p.dc_cvau(stub.addr, stub.len)
p.ic_ivau(stub.addr, stub.len)

print(f"Entry point: 0x{entry:x}")

if args.call:
    print(f"Shutting down MMU...")
    try:
        p.mmu_shutdown()
    except ProxyCommandError:
        pass
    print(f"Jumping to stub at 0x{stub.addr:x}")
    p.call(stub.addr, new_base + bootargs_off, image_addr, new_base, image_size, reboot=True)
else:
    print(f"Reloading into stub at 0x{stub.addr:x}")
    p.reload(stub.addr, new_base + bootargs_off, image_addr, new_base, image_size)

iface.nop()
print("Proxy is alive again")
