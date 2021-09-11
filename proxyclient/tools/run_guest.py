#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import sys, pathlib, traceback
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

import argparse, pathlib

parser = argparse.ArgumentParser(description='Run a Mach-O payload under the hypervisor')
parser.add_argument('-s', '--symbols', type=pathlib.Path)
parser.add_argument('-m', '--script', type=pathlib.Path, action='append', default=[])
parser.add_argument('-c', '--command', action="append", default=[])
parser.add_argument('-S', '--shell', action="store_true")
parser.add_argument('payload', type=pathlib.Path)
parser.add_argument('boot_args', default=[], nargs="*")
args = parser.parse_args()

from m1n1.proxy import *
from m1n1.proxyutils import *
from m1n1.utils import *
from m1n1.shell import run_shell
from m1n1.hv import HV

iface = UartInterface()
p = M1N1Proxy(iface, debug=False)
bootstrap_port(iface, p)
u = ProxyUtils(p, heap_size = 128 * 1024 * 1024)

hv = HV(iface, p, u)

hv.init()

if len(args.boot_args) > 0:
    boot_args = " ".join(args.boot_args)
    hv.set_bootargs(boot_args)

#hv.ramdisk = open("/Volumes/thickhd/docs/macos12b5/RestoreRamdisk.dmg", "rb").read()
hv.ramdisk = open("/Volumes/thickhd/docs/ipados15b6/RestoreRamdisk.dmg", "rb").read()
# see trust_cache_init for header format: num_caches, offsets[0], ...
hv.trustcache = b"\x01\x00\x00\x00\x08\x00\x00\x00" + open("/Volumes/thickhd/docs/ipados15b6/Firmware/RestoreRamdisk.trustcache", "rb").read()

symfile = None
if args.symbols:
    symfile = args.symbols.open("rb")
hv.load_macho(args.payload.open("rb"), symfile=symfile)

for i in args.script:
    try:
        hv.run_script(i)
    except:
        traceback.print_exc()
        args.shell = True

for i in args.command:
    try:
        hv.run_code(i)
    except:
        traceback.print_exc()
        args.shell = True

if args.shell:
    run_shell(hv.shell_locals, "Entering hypervisor shell. Type `start` to start the guest.")
else:
    hv.start()
