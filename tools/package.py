#!/usr/bin/env python3
"""Builds a release package from a configured, built tree (docs/ISO_TOOL.md).

usage: tools/package.py <build dir> <package dir>

The package holds our code only: the runtime (H3Runtime and the other
libraries the executable links), the headers the generated code includes,
the recompiler and its configs, h3setup and the launcher. On the player's
machine h3setup recompiles their default.xex and builds the game with the
commands in runtime/build.txt, taken from this build's own and rewritten:

  {cxx}   the compiler          {pkg}  the package    {gen}  the install's build dir
  {ld}    the linker (Windows: lld-link)     {sdk}  the CRT/Windows SDK (xwin splat)
  {in}    a source file         {out}  its object     {objects} the game's objects
  {ident} a module's C name     {name} a module's directory name
  {exe}   the executable

build.txt: one command per line, "<kind>\\t<arg>\\t<arg>..."; kinds:
game, module, module_stubs, exe_source (compiles) and link.
"""
import json, os, re, shlex, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    build = os.path.abspath(sys.argv[1])
    pkg = os.path.abspath(sys.argv[2])
    if os.path.exists(pkg):
        shutil.rmtree(pkg)
    for d in ("runtime/lib", "runtime/include", "tools", "recompiler/config"):
        os.makedirs(os.path.join(pkg, d))

    commands = json.load(open(os.path.join(build, "compile_commands.json")))
    windows = "clang-cl" in commands[0]["command"].split()[0]
    exe_suffix = ".exe" if windows else ""
    # The xwin sysroot this build used (Windows): {sdk} on the player's machine.
    xwin = None
    if windows:
        match = re.search(r'/imsvc\s+"?([^"\s]+)/crt/include', commands[0]["command"])
        xwin = match.group(1) if match else None

    def find(predicate, what):
        for entry in commands:
            if predicate(entry["file"]):
                return entry
        sys.exit("no compile command for " + what)

    templates = {
        "game": find(lambda f: re.search(r"/H3RecompLib/ppc/ppc_recomp\.\d+\.cpp$", f), "the game"),
        "module": find(lambda f: re.search(r"/H3RecompLib/modules/[^/]+/ppc_recomp\.\d+\.cpp$", f), "a module"),
        "module_stubs": find(lambda f: "/module_stubs/" in f, "a module's stubs"),
        "exe_source": find(lambda f: f.endswith("/H3RecompLib/ppc/import_stubs.cpp"), "the import stubs"),
    }
    registry = find(lambda f: f.endswith("recompiled_modules.cpp"), "the module registry")

    gen_ppc = os.path.join(ROOT, "H3RecompLib", "ppc")
    gen_modules = os.path.join(ROOT, "H3RecompLib", "modules")
    gen_build = os.path.join(build, "H3RecompLib")
    headers = set()

    def rewrite(arg, kind):
        """A path in a command -> its place on the player's machine."""
        path = arg
        if path.startswith(gen_ppc):
            return "{gen}/ppc" + path[len(gen_ppc):]
        if path.startswith(gen_modules + "/"):
            rest = path[len(gen_modules) + 1:]
            name = rest.split("/")[0]
            return "{gen}/modules/{name}" + rest[len(name):]
        if path.startswith(gen_build + "/module_stubs/"):
            return "{gen}/module_stubs/{name}" + path[len(gen_build + "/module_stubs/") + len(path[len(gen_build + "/module_stubs/"):].split("/")[0]):]
        if path.startswith(ROOT + "/"):
            return "{pkg}/runtime/include/" + os.path.relpath(path, ROOT)
        return arg

    def template(entry, kind):
        args = shlex.split(entry["command"])
        # Headers this compile reads (from our tree, not the generated code).
        if windows:
            dash = args.index("--")
            probe = [a for a in args[:dash] if not a.startswith(("/Fo", "/Fd"))] + ["/clang:-M", "/clang:-MF/dev/stdout"] + args[dash:]
        else:
            probe = args[:args.index("-o")] + args[args.index("-o") + 2:] + ["-M", "-MF", "/dev/stdout"]
        deps = subprocess.run(probe, cwd=entry["directory"], capture_output=True, text=True, check=True).stdout
        for dep in deps.replace("\\\n", " ").split()[1:]:
            dep = os.path.abspath(os.path.join(entry["directory"], dep))
            if dep.startswith(ROOT + "/") and not dep.startswith((gen_ppc, gen_modules, build + "/")):
                headers.add(dep)
        out = ["{cxx}"]
        i = 1
        while i < len(args):
            a = args[i]
            if a == "-o":
                out += ["-o", "{out}"]
                i += 2
                continue
            if a == "-c" and windows:
                out += ["-c", "--", "{in}"]
                break
            if a == "-c":
                out += ["-c", "{in}"]
                i += 2
                continue
            if a.startswith("/Fo"):
                out.append("/Fo{out}")
            elif a.startswith("/Fd"):
                pass
            elif a.startswith("/FI"):
                out.append("/FI" + rewrite(a[3:], kind))
            elif a == "/imsvc":
                out += ["/imsvc", args[i + 1].replace(xwin, "{sdk}") if xwin else args[i + 1]]
                i += 1
            elif a.startswith("-I"):
                out.append("-I" + rewrite(os.path.abspath(os.path.join(entry["directory"], a[2:])), kind))
            elif a.startswith("-DPPCFuncMappings="):
                out.append("-DPPCFuncMappings=PPCFuncMappings_{ident}")
            elif a == "-include":
                out += ["-include", rewrite(args[i + 1], kind)]
                i += 1
            else:
                out.append(a)
            i += 1
        return out

    lines = []
    for kind, entry in templates.items():
        lines.append([kind] + template(entry, kind))
    template(registry, "exe_source")  # its headers

    # The link: the executable's own link line, the game's libraries
    # replaced by the player's objects, ours copied into runtime/lib.
    link = subprocess.run(["ninja", "-t", "commands", "H3Recomp"], cwd=build, capture_output=True, text=True,
                          check=True).stdout.strip().splitlines()[-1]
    link = link.split("&&")[1] if "&&" in link else link
    args = shlex.split(link)
    if windows:
        # cmake -E vs_link_exe ... -- lld-link <args>
        args = args[args.index("--") + 1:]
    out = ["{ld}" if windows else "{cxx}"]
    objects_placed = False
    i = 1
    while i < len(args):
        a = args[i]
        if a == "-o":
            out += ["-o", "{exe}"]
            i += 2
            continue
        if a.startswith("--dependency-file") or (a == "-Xlinker" and i + 1 < len(args) and args[i + 1].startswith("--dependency-file")):
            if out[-1] == "-Xlinker":
                out.pop()
            i += 1 if a.startswith("--dependency-file") else 2
            continue
        if windows and a.lower().startswith(("/out:", "/implib:", "/pdb:", "/version:", "/manifest")):
            if a.lower().startswith("/out:"):
                out.append("/out:{exe}")
            i += 1
            continue
        if windows and a.lower().startswith("/libpath:") and xwin:
            out.append(a.replace(xwin, "{sdk}"))
            i += 1
            continue
        if windows and a.upper().startswith("/WHOLEARCHIVE:"):
            source = os.path.realpath(os.path.join(build, a.split(":", 1)[1]))
            shutil.copy2(source, os.path.join(pkg, "runtime", "lib", os.path.basename(source)))
            out.append("/WHOLEARCHIVE:{pkg}/runtime/lib/" + os.path.basename(source))
            i += 1
            continue
        if a.endswith((".o", ".obj")) or re.search(r"(lib)?H3RecompLib\.(a|lib)$", a) or re.search(r"(lib)?H3Module_[^/]*\.(a|lib)$", a):
            if not objects_placed:
                out.append("{objects}")
                objects_placed = True
            i += 1
            continue
        if (a.endswith(".a") or ".so" in a or a.endswith(".lib")) and (os.path.exists(os.path.join(build, a)) or os.path.isabs(a)):
            source = os.path.realpath(os.path.join(build, a))
            name = os.path.basename(source)
            shutil.copy2(source, os.path.join(pkg, "runtime", "lib", name))
            if ".so" in name:
                # The name the executable will ask for (its soname).
                soname = re.search(r"SONAME\s+(\S+)", subprocess.run(["objdump", "-p", source], capture_output=True,
                                                                     text=True).stdout)
                if soname and soname.group(1) != name:
                    os.symlink(name, os.path.join(pkg, "runtime", "lib", soname.group(1)))
            out.append("{pkg}/runtime/lib/" + name)
            i += 1
            continue
        out.append(a)
        i += 1
    if windows:
        # The DLLs the executable loads (SDL3, the C++ runtime), next to it.
        for name in os.listdir(os.path.join(build, "H3Recomp")):
            if name.lower().endswith(".dll"):
                shutil.copy2(os.path.join(build, "H3Recomp", name), os.path.join(pkg, "runtime", "lib", name))
    else:
        # Shared libraries next to the executable, in lib/.
        out += ["-Wl,-rpath,$ORIGIN/lib"]
    lines.append(["link"] + out)

    for header in sorted(headers):
        destination = os.path.join(pkg, "runtime", "include", os.path.relpath(header, ROOT))
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        shutil.copy2(header, destination)

    with open(os.path.join(pkg, "runtime", "build.txt"), "w") as f:
        for line in lines:
            f.write("\t".join(line) + "\n")

    # The recompiler, its configs (with the jump tables) and context header.
    for tool, path in (("XenonRecomp", "thirdparty/XenonRecomp/XenonRecomp/XenonRecomp"), ("h3xexinfo", "tools/h3xexinfo/h3xexinfo")):
        shutil.copy2(os.path.join(build, path + exe_suffix), os.path.join(pkg, "tools", tool + exe_suffix))
        if windows:  # their C++ runtime
            for name in os.listdir(os.path.join(build, os.path.dirname(path))):
                if name.lower().endswith(".dll"):
                    shutil.copy2(os.path.join(build, os.path.dirname(path), name), os.path.join(pkg, "tools", name))
    config = os.path.join(ROOT, "H3RecompLib", "config")
    shutil.copytree(config, os.path.join(pkg, "recompiler", "config"), dirs_exist_ok=True)
    shutil.copy2(os.path.join(ROOT, "thirdparty/XenonRecomp/XenonUtils/ppc_context.h"),
                 os.path.join(pkg, "recompiler", "config", "ppc_context.h"))
    shutil.copy2(os.path.join(build, "tools/h3setup/h3setup" + exe_suffix), os.path.join(pkg, "h3setup" + exe_suffix))
    launcher_dir = os.path.join(build, "H3Launcher")
    if os.path.exists(os.path.join(launcher_dir, "H3Launcher" + exe_suffix)):
        if windows:
            # The launcher and what it loads (Qt, SDL, the C++ runtime, Qt's plugins).
            for name in os.listdir(launcher_dir):
                source = os.path.join(launcher_dir, name)
                if name.lower().endswith((".exe", ".dll")):
                    shutil.copy2(source, os.path.join(pkg, name))
                elif os.path.isdir(source) and name in ("platforms", "styles", "imageformats"):
                    shutil.copytree(source, os.path.join(pkg, name))
        else:
            shutil.copy2(os.path.join(launcher_dir, "H3Launcher"), os.path.join(pkg, "H3Launcher"))

    print(f"package: {pkg} ({len(headers)} headers, {len(os.listdir(os.path.join(pkg, 'runtime', 'lib')))} libraries)")


main()
