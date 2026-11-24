#!/usr/bin/env python3
"""Builds a release package from a configured, built tree (docs/ISO_TOOL.md).

usage: tools/package.py <build dir> <package dir> [--llvm <dir>] [--xwin <dir>] [--archive <dir>]

--archive also writes the release archive into <dir>:
halo3-recomp-<version>-linux-x86_64.tar.gz or -windows-x86_64.zip, with the
package in a folder of the same name.

Windows packages also carry the toolchain the player's machine compiles
with: --llvm, an unpacked official LLVM for Windows (clang+llvm-*-x86_64-
pc-windows-msvc, same version as this build's clang), of which clang-cl,
lld-link and clang's headers are kept (toolchain/llvm, Apache 2.0 with
LLVM exceptions); --xwin, an unpacked xwin release (toolchain/xwin.exe,
MIT/Apache), which fetches Microsoft's CRT and SDK on the player's machine
once they accept Microsoft's terms.

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
  {exe_objects}   the executable's own objects (import stubs, module registry)
  {game_objects}  the game's and the DLLs' objects, where this build links
                  their libraries: after the runtime, so where both have a
                  copy of an inline function the runtime's is kept, as here

build.txt: one command per line, "<kind>\\t<arg>\\t<arg>..."; kinds:
game, module, module_stubs, exe_source (compiles) and link.
"""
import json, os, re, shlex, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Linux: libraries every desktop has (and that must match its drivers and
# display server); everything else a shipped binary needs is bundled.
SYSTEM_LIBRARIES = re.compile(
    r"^(ld-linux.*|linux-vdso.*|lib(c|m|dl|pthread|rt|util|resolv|stdc\+\+|gcc_s|atomic)\.so.*"
    # (libxcb's own protocol libraries are always there; the xcb-util ones
    # Qt's X11 plugin needs often aren't, e.g. libxcb-cursor: bundled)
    r"|lib(X|xcb(?!-(cursor|icccm|image|keysyms|render-util|util)\.)|xkbcommon|GL|EGL|OpenGL|GLX|GLdispatch|gbm|drm|vulkan|wayland|dbus-1|systemd|udev|fontconfig|freetype"
    r"|harfbuzz|graphite2|glib-2|gobject-2|gio-2|gmodule-2|z\.|expat|png16|bz2|brotli|uring|crypto|ssl|gomp|asound|pulse"
    r"|zstd|pcre2-8|cap|lzma|gcrypt|gpg-error|lz4|ffi|mount|blkid|selinux|Xau|Xdmcp|ICE|SM|uuid).*)$")


def collect_licenses(pkg, windows):
    """licenses/ and THIRD-PARTY.txt: what the package bundles, under which
    license, and where its source is."""
    lic = os.path.join(pkg, "licenses")
    os.makedirs(lic, exist_ok=True)
    notes = []

    # One third-party component's license files and its line in
    # THIRD-PARTY.txt.
    def add(name, files, note):
        os.makedirs(os.path.join(lic, name), exist_ok=True)
        for f in files:
            if os.path.isfile(f):
                shutil.copy2(f, os.path.join(lic, name, os.path.basename(f)))
        notes.append(f"{name}: {note}")

    def license_text(spdx_id, debian_name):
        """A license's text from this machine: Arch's SPDX folder or Debian's common-licenses."""
        for path in (f"/usr/share/licenses/spdx/{spdx_id}.txt", f"/usr/share/common-licenses/{debian_name}"):
            if os.path.exists(path):
                return path
        return ""
    xr = os.path.join(ROOT, "thirdparty", "XenonRecomp")
    add("XenonRecomp", [os.path.join(xr, "LICENSE.md"), license_text("GPL-3.0-or-later", "GPL-3")],
        "the recompiler (tools/), MIT; its PowerPC disassembler (thirdparty/disasm, from GNU binutils) is GPL 3.0 or "
        "later, which makes the XenonRecomp program as a whole GPL. Source: https://github.com/hedge-dev/XenonRecomp")
    for part in ("fmt", "tomlplusplus", "xxHash"):
        add(part, [os.path.join(xr, "thirdparty", part, "LICENSE")], "used by XenonRecomp and the runtime; see its license")
    add("simde", [os.path.join(xr, "thirdparty", "simde", "COPYING")], "SIMD headers the generated game code compiles with; MIT")
    deps_src = os.path.join(ROOT, "build", "windows-deps-src")
    ffmpeg = os.path.join(deps_src, "ffmpeg-7.1")
    add("FFmpeg", [os.path.join(ffmpeg, "COPYING.LGPLv2.1"), os.path.join(ffmpeg, "LICENSE.md"), license_text("LGPL-2.1-or-later", "LGPL-2.1")],
        "XMA audio decoding (libavcodec/libavutil 7.1 with the xmaframes decoder), LGPL 2.1 or later, linked statically: "
        "the static libraries are in runtime/lib, so the game can be relinked with a modified FFmpeg (h3setup links it on "
        "your machine). Built by tools/build_ffmpeg.sh / tools/build_windows_deps.sh from the FFmpeg 7.1 release.")
    if windows:
        add("SDL3", [os.path.join(deps_src, "SDL3-3.4.18", "LICENSE.txt")], "windowing, input, audio; zlib license")
        shaderc = os.path.join(deps_src, "shaderc")
        add("shaderc", [os.path.join(shaderc, "LICENSE"), os.path.join(shaderc, "third_party", "LICENSE.glslang"),
                        os.path.join(shaderc, "third_party", "LICENSE.spirv-tools")],
            "shader compiling (shaderc, glslang, SPIRV-Tools), Apache 2.0 / BSD-style")
        add("Qt", [license_text("LGPL-3.0-only", "LGPL-3")] + [os.path.join("/usr/share/licenses/qt6-base", f) for f in
                   os.listdir("/usr/share/licenses/qt6-base")] if os.path.isdir("/usr/share/licenses/qt6-base") else [],
            "the launcher's UI (Qt 6.12, Core/Gui/Widgets and plugins), LGPL 3.0, as DLLs you can replace. Source: https://download.qt.io")
        notes.append("Microsoft Visual C++ runtime DLLs (msvcp140*, vcruntime140*): redistributable under the Visual Studio license terms.")
    elif not shutil.which("pacman"):
        # The release container (tools/linux_release): the libraries it built.
        add("SDL3", [os.path.join(deps_src, "SDL3-3.4.18", "LICENSE.txt")], "windowing, input, audio; zlib license")
        shaderc = os.path.join(deps_src, "shaderc")
        add("shaderc", [os.path.join(shaderc, "LICENSE"), os.path.join(shaderc, "third_party", "LICENSE.glslang"),
                        os.path.join(shaderc, "third_party", "LICENSE.spirv-tools")],
            "shader compiling (shaderc, glslang, SPIRV-Tools), Apache 2.0 / BSD-style")
        add("Qt", [license_text("LGPL-3.0-only", "LGPL-3")],
            "the launcher's UI (Qt 6.12 from Qt's Linux build, with its ICU), LGPL 3.0, as replaceable .so files in lib/. "
            "Source: https://download.qt.io. ICU: Unicode license, https://www.unicode.org/license.txt")
    else:
        # The bundled shared libraries' own packages (this build machine's).
        packages = set()
        for d in (os.path.join(pkg, "lib"), os.path.join(pkg, "runtime", "lib")):
            for name in os.listdir(d) if os.path.isdir(d) else []:
                if ".so" not in name or os.path.islink(os.path.join(d, name)):
                    continue
                owner = subprocess.run(["pacman", "-Qqo", "/usr/lib/" + name], capture_output=True, text=True).stdout.strip()
                if owner:
                    packages.add(owner)
        for owner in sorted(packages):
            licdir = "/usr/share/licenses/" + owner
            info = subprocess.run(["pacman", "-Qi", owner], capture_output=True, text=True).stdout
            license_field = re.search(r"^Licenses\s*:\s*(.*)$", info, re.M)
            files = [os.path.join(licdir, f) for f in os.listdir(licdir)] if os.path.isdir(licdir) else []
            add(owner, files, f"bundled shared library; {license_field.group(1).strip() if license_field else 'see its license'}"
                + (" (the launcher's UI; LGPL, replaceable .so files in lib/)" if owner.startswith("qt6") else ""))
    with open(os.path.join(pkg, "THIRD-PARTY.txt"), "w") as f:
        f.write("Third-party software in this package (license texts in licenses/):\n\n")
        for note in notes:
            f.write("- " + note + "\n")
        if windows:
            f.write("- LLVM (toolchain/llvm: clang-cl, lld-link and clang's headers), Apache 2.0 with LLVM exceptions; "
                    "xwin (toolchain/xwin.exe), MIT or Apache 2.0: their licenses are in toolchain/.\n")


def bundle_closure(binary, destination, done):
    """Copies the non-system shared libraries binary needs (transitively)
    into destination, with their soname links."""
    for line in subprocess.run(["ldd", binary], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) < 3 or parts[1] != "=>" or not parts[2].startswith("/"):
            continue
        name, path = os.path.basename(parts[0]), parts[2]  # ld-linux comes with its full path
        if SYSTEM_LIBRARIES.match(name) or name in done:
            continue
        done.add(name)
        real = os.path.realpath(path)
        if not os.path.exists(os.path.join(destination, os.path.basename(real))):
            shutil.copy2(real, os.path.join(destination, os.path.basename(real)))
        if os.path.basename(real) != name and not os.path.lexists(os.path.join(destination, name)):
            os.symlink(os.path.basename(real), os.path.join(destination, name))
        bundle_closure(real, destination, done)


# package.py <build dir> <package dir> [--llvm <dir>] [--xwin <dir>]
# [--archive <dir>]: copies the build's outputs, the runtime library and
# headers, the recompiler and configs, the shaders and licenses into a
# release folder, writes runtime/build.txt, and optionally the archive.
def main():
    build = os.path.abspath(sys.argv[1])
    pkg = os.path.abspath(sys.argv[2])
    options = dict(zip(sys.argv[3::2], sys.argv[4::2]))
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

    # The first compile command (compile_commands.json) for a matching file.
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

    # A compile command turned into a build.txt template: paths of this
    # machine replaced by placeholders ({pkg}, {in}, {out}...).
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
    exe_objects_placed = game_objects_placed = False
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
        if windows and a.lower().endswith(".res"):
            # This build's compiled resources (cmake/icon.rc): the package
            # links its own runtime/utf8.res instead (below), which holds
            # the same icon and the UTF-8 manifest.
            i += 1
            continue
        if a.endswith((".o", ".obj")):
            if not exe_objects_placed:
                out.append("{exe_objects}")
                exe_objects_placed = True
            i += 1
            continue
        if re.search(r"(lib)?H3RecompLib\.(a|lib)$", a) or re.search(r"(lib)?H3Module_[^/]*\.(a|lib)$", a):
            if not game_objects_placed:
                out.append("{game_objects}")
                game_objects_placed = True
            i += 1
            continue
        if re.search(r"(lib)?disasm\.(a|lib)$", a):
            # XenonRecomp's PowerPC disassembler (GNU binutils, GPL): the
            # runtime links the library but uses none of it; not shipped.
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
        # The UTF-8 code page manifest (cmake/utf8.manifest) and the Halo 3
        # icon (assets/icon.ico, the .exe's file icon) as compiled resources:
        # lld-link links them like an object (CMake added both to this build
        # through cmake/utf8.manifest and cmake/icon.rc).
        rc = os.path.join(pkg, "runtime", "utf8.rc")
        with open(rc, "w") as f:
            f.write('1 24 "%s"\n' % os.path.join(ROOT, "cmake", "utf8.manifest"))
            f.write('1 ICON "%s"\n' % os.path.join(ROOT, "assets", "icon.ico"))
        subprocess.run(["llvm-rc", "/fo", os.path.join(pkg, "runtime", "utf8.res"), rc], check=True)
        os.remove(rc)
        out += ["/manifest:no", "{pkg}/runtime/utf8.res"]
        # The DLLs the executable loads (SDL3, the C++ runtime), next to it.
        for name in os.listdir(os.path.join(build, "H3Recomp")):
            if name.lower().endswith(".dll"):
                shutil.copy2(os.path.join(build, "H3Recomp", name), os.path.join(pkg, "runtime", "lib", name))
    else:
        # Shared libraries next to the executable, in lib/ (RPATH, so their
        # own dependencies are found there too), and what they need.
        out += ["-Wl,--disable-new-dtags", "-Wl,-rpath,$ORIGIN/lib"]
        done = set()
        for name in sorted(os.listdir(os.path.join(pkg, "runtime", "lib"))):
            if ".so" in name and not os.path.islink(os.path.join(pkg, "runtime", "lib", name)):
                bundle_closure(os.path.join(pkg, "runtime", "lib", name), os.path.join(pkg, "runtime", "lib"), done)
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
    # The shader manifests h3setup's last step uses (H3Recomp
    # --precompile-shaders): descriptors only, the shaders come from the
    # player's maps (assets/shaders/README.md).
    shaders = os.path.join(ROOT, "assets", "shaders")
    if os.path.isdir(shaders):
        shutil.copytree(shaders, os.path.join(pkg, "shaders"), dirs_exist_ok=True)
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
            # The launcher, Qt and what it needs in lib/ (the launcher's RPATH,
            # H3Launcher/CMakeLists.txt), and Qt's plugins: X11 and Wayland,
            # the JPEG reader (the background art). qt.conf points Qt there.
            shutil.copy2(os.path.join(launcher_dir, "H3Launcher"), os.path.join(pkg, "H3Launcher"))
            lib = os.path.join(pkg, "lib")
            os.makedirs(lib, exist_ok=True)
            done = set()
            bundle_closure(os.path.join(pkg, "H3Launcher"), lib, done)
            # The plugins of the Qt the launcher was built with: its prefix
            # from the build's Qt6Core_DIR (<prefix>/lib/cmake/Qt6Core; the
            # release container's aqtinstall Qt has no qtpaths on PATH),
            # else qtpaths, else the system Qt's.
            plugin_root = None
            with open(os.path.join(build, "CMakeCache.txt")) as cache:
                for line in cache:
                    if line.startswith("Qt6Core_DIR:"):
                        prefix = os.path.normpath(os.path.join(line.split("=", 1)[1].strip(), "..", "..", ".."))
                        if os.path.isdir(os.path.join(prefix, "plugins", "platforms")):
                            plugin_root = os.path.join(prefix, "plugins")
            for tool in ("qtpaths6", "qtpaths"):
                if plugin_root:
                    break
                try:
                    plugin_root = subprocess.run([tool, "--plugin-dir"], capture_output=True, text=True).stdout.strip() or None
                except FileNotFoundError:
                    pass
            plugin_root = plugin_root or "/usr/lib/qt6/plugins"
            for required in ("platforms/libqxcb.so", "platforms/libqwayland.so"):
                if not os.path.exists(os.path.join(plugin_root, required)):
                    sys.exit(f"Qt platform plugin {required} not found in {plugin_root}: the launcher wouldn't start")
            for plugin in ("platforms/libqxcb.so", "platforms/libqwayland.so", "xcbglintegrations/libqxcb-glx-integration.so",
                           "xcbglintegrations/libqxcb-egl-integration.so", "wayland-shell-integration/libxdg-shell.so",
                           "wayland-decoration-client/libbradient.so", "wayland-graphics-integration-client/libqt-plugin-wayland-egl.so",
                           "imageformats/libqjpeg.so"):
                source = os.path.join(plugin_root, plugin)
                if not os.path.exists(source):
                    continue
                target = os.path.join(pkg, "plugins", plugin)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copy2(source, target)
                bundle_closure(source, lib, done)
            with open(os.path.join(pkg, "qt.conf"), "w") as f:
                f.write("[Paths]\nPrefix = .\nPlugins = plugins\nLibraries = lib\n")

    if windows:
        llvm, xwin_dir = options.get("--llvm"), options.get("--xwin")
        if not llvm or not xwin_dir:
            sys.exit("a Windows package needs --llvm and --xwin")
        toolchain = os.path.join(pkg, "toolchain")
        os.makedirs(os.path.join(toolchain, "llvm", "bin"))
        for tool in ("clang-cl.exe", "lld-link.exe"):
            shutil.copy2(os.path.join(llvm, "bin", tool), os.path.join(toolchain, "llvm", "bin", tool))
        version = os.listdir(os.path.join(llvm, "lib", "clang"))[0]
        shutil.copytree(os.path.join(llvm, "lib", "clang", version, "include"),
                        os.path.join(toolchain, "llvm", "lib", "clang", version, "include"))
        for component in ("llvm", "clang", "lld"):
            shutil.copy2(f"/usr/share/licenses/{component}/LICENSE", os.path.join(toolchain, "llvm", f"LICENSE-{component}.txt"))
        shutil.copy2(os.path.join(xwin_dir, "xwin.exe"), os.path.join(toolchain, "xwin.exe"))
        for name in ("LICENSE-APACHE", "LICENSE-MIT"):
            shutil.copy2(os.path.join(xwin_dir, name), os.path.join(toolchain, "xwin-" + name + ".txt"))

    collect_licenses(pkg, windows)
    shutil.copy2(os.path.join(ROOT, "docs", "PLAYER_README.md"), os.path.join(pkg, "README.txt"))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(pkg, "LICENSE.txt"))

    if "--archive" in options:
        cache = open(os.path.join(build, "CMakeCache.txt")).read()
        version = re.search(r"^CMAKE_PROJECT_VERSION:STATIC=(.*)$", cache, re.M).group(1)
        name = f"halo3-recomp-{version}-{'windows' if windows else 'linux'}-x86_64"
        out_dir = os.path.abspath(options["--archive"])
        os.makedirs(out_dir, exist_ok=True)
        if windows:
            import zipfile
            path = os.path.join(out_dir, name + ".zip")
            with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
                for folder, _, files in os.walk(pkg):
                    for f in files:
                        full = os.path.join(folder, f)
                        z.write(full, os.path.join(name, os.path.relpath(full, pkg)))
        else:
            import tarfile
            path = os.path.join(out_dir, name + ".tar.gz")
            with tarfile.open(path, "w:gz") as t:
                t.add(pkg, arcname=name)  # symlinks kept as links
        print(f"archive: {path} ({os.path.getsize(path) / 1e6:.0f} MB)")

    print(f"package: {pkg} ({len(headers)} headers, {len(os.listdir(os.path.join(pkg, 'runtime', 'lib')))} libraries)")


main()
