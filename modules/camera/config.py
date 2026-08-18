def can_build(env, platform):
    if platform == "osx" and env.get("arch") == "ppc":
        # QTKit (linked for camera_osx.mm's QTCaptureSession backend) reliably
        # corrupts libstdc++'s static iostream/locale init on this toolchain --
        # confirmed via an isolated minimal repro (a bare `#include <iostream>`
        # program, no Godot code involved, double-frees at the exact same two
        # addresses the instant QTKit is linked in, regardless of whether it's
        # ever actually called). Not worth chasing into QuickTime's own ancient
        # internals; camera capture isn't needed for this port, so just don't
        # build this module for ppc.
        return False
    return platform == "osx" or platform == "windows"


def configure(env):
    pass
