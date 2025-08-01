# vi: syntax=python:et:ts=4
import os

def setup_cross_compile(env):
    if "mingw" in env["host"] or env["PLATFORM"] == "msys":
        env["PLATFORM"] = "win32"
        env["PROGSUFFIX"] = ".exe"
        env.Tool("mingw")
        env.Append(LINKFLAGS = ["-static-libgcc", "-static-libstdc++"])
    else:
        env.Tool("default")

    if env["host"]:
        if env["host"].startswith("android-"):
            env.Tool("android-ndk")
            env["PLATFORM"] = "android"
        else:
            tools = [
                "CXX",
                "CC",
                "AR",
                "RANLIB",
                "RC"
            ]
            for tool in tools:
                if tool in env:
                    env[tool] = env["host"] + "-" + env[tool]

        env.PrependUnique(CPPPATH="$prefix/include", LIBPATH="$prefix/lib")
        if not env["sdldir"] and env["PLATFORM"] == "win32":
            env["sdldir"] = env.subst("$prefix")

        os.environ["PKG_CONFIG_LIBDIR"] = ''
        env["ENV"]["PKG_CONFIG_LIBDIR"] = ''
        env["ENV"]["PKG_CONFIG_PATH"] = env.subst("$prefix/lib/pkgconfig")
        if "mingw" in env["host"]:
            env.Append(PKG_CONFIG_FLAGS = ["--define-prefix"])
