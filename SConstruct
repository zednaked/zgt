#!/usr/bin/env python
import os
import sys

env = SConscript("godot-cpp/SConstruct")

env.Append(CPPPATH=["src/"])

if env["platform"] == "linux":
    env.Append(LIBS=["util"])  # forkpty/openpty
    env.Append(CPPDEFINES=["LINUX_ENABLED"])

sources = Glob("src/*.cpp")

library_name = "libzgt"
target_path = "bin/"

libfile = "{}{}{}".format(library_name, env["suffix"], env["SHLIBSUFFIX"])
lib = env.SharedLibrary(target_path + libfile, sources)

Default(lib)
