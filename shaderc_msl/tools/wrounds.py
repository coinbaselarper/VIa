#!/usr/bin/env python3
"""Wave64 exchange-round removal (translator fork --wave-rounds): sstruct.py's A/B and overlay commands run in the
wave-rounds tree (build/wrounds-x86_64) with its own probe binary (gta6rt_wrounds_probe).

  wrounds.py ab WORK NAME [--inputs zero,fol_a] [--groups G] [--flags="a,b;a,b,c"]
  wrounds.py overlay OUT [NAME...] [--names-file F] [--flags=...]
  wrounds.py manifest OUT.txt OVERLAY.tsv... MANIFEST...
  wrounds.py abcs WORK library/NAME [--seeds 1,2] [--cs-groups 4,4,1] [--cs-threads 8,8,1] [--flags=...]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sstruct as SS  # noqa: E402

SS.TREE = os.path.join(SS.PORT, "build", "wrounds-x86_64")
SS.PROBE = os.path.join(SS.TREE, "probe", "gta6rt_wrounds_probe")
SS.MY_INPUTS = os.path.join(SS.TREE, "inputs")
SS.CS_AB = os.path.join(SS.TREE, "probe", "gta6rt_wrounds_cs_ab")

import re  # noqa: E402
import mspeed  # noqa: E402

_capture = mspeed.capture


def capture(msl):
    """mspeed.capture, also for mesh programs whose position lives only in gl_MeshVerticesEXT (no local)."""
    out, info = _capture(msl)
    s, lp, rp, bs, be = mspeed.functions(msl)["main0"]
    if not re.search(r"\bfloat4 gl_Position\s*[;=]", msl[bs:be]) and "gl_MeshVerticesEXT.gl_Position" in msl[bs:be]:
        line = ("r[0] = as_type<uint>(gl_Position.x); r[1] = as_type<uint>(gl_Position.y); "
                "r[2] = as_type<uint>(gl_Position.z); r[3] = as_type<uint>(gl_Position.w);")
        out = out.replace(line, line.replace("gl_Position", "gl_MeshVerticesEXT.gl_Position"), 1)
    return out, info


mspeed.capture = capture

if __name__ == "__main__":
    SS.main()
