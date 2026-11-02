"""Use the pinned upstream protocol headers without its standalone application.

VTXControl_ESP32 is packaged as a PlatformIO project: src/ includes a test
application and a blocking controller that owns UART2 with default pins. Our
nonblocking VtxManager uses its actual CRC functions, frames and constants,
with the verified single-wire transport and device-specific readback handling.
Filter upstream compilation through PlatformIO's supported build middleware;
never copy or patch the dependency's source files.
"""
from pathlib import Path

Import("env")

dependency_root = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "VTXControl_ESP32"
).resolve()


def protocol_headers_only(build_env, node):
    source = Path(node.srcnode().get_abspath()).resolve()
    try:
        source.relative_to(dependency_root)
    except ValueError:
        return node
    if source.suffix.lower() in (".c", ".cc", ".cpp", ".cxx", ".s"):
        return None
    return node


env.AddBuildMiddleware(protocol_headers_only)
