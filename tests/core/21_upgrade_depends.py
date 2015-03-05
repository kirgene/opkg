#! /usr/bin/env python
#

import os
import opk, cfg, opkgcl

opk.regress_init()

o = opk.OpkGroup()
o.add(Package="a", Version="1.0", Depends="b")
o.add(Package="b", Version="1.0")
o.add(Package="c", Version="1.0", Depends="a (= 1.0)")

o.write_opk()
o.write_list()

opkgcl.update()
opkgcl.install("c")

if not opkgcl.is_installed("a"):
	opk.fail("Package 'a' installed but does not report as installed.")
if not opkgcl.is_installed("b"):
	opk.fail("Package 'b' installed but does not report as installed.")
if not opkgcl.is_installed("c"):
	opk.fail("Package 'b' installed but does not report as installed.")

o = opk.OpkGroup()

# Make a new version of 'a', 'b' available
o.add(Package="a", Version="2.0", Depends="b (= 2.0)")
o.add(Package="b", Version="2.0", Provides="z")
o.add(Package="c", Version="2.0", Depends="a (= 2.0)")
o.write_opk()
o.write_list()

opkgcl.update()
opkgcl.install("a")

if not opkgcl.is_installed("a", "2.0"):
    opk.fail("New version of package 'a' available during upgrade but was not installed")
if not opkgcl.is_installed("b", "2.0"):
    opk.fail("New version of package 'b' available during upgrade but was not installed")
if not opkgcl.is_installed("c", "2.0"):
    opk.fail("New version of package 'c' available during upgrade but was not installed")
