"""Pipeline texts in the format of cache/src/codec.cpp (encode), for specs written by tools. The C++ side checks
every text by decoding it (prepare_shaders reports undecodable lines)."""


class Target:
    def __init__(self, fmt, enable=0, src=2, dst=1, op=1, write_mask=15):
        self.fmt, self.enable, self.src, self.dst, self.op, self.mask = fmt, enable, src, dst, op, write_mask


def esc(s):
    out = []
    for ch in s.encode():
        if ch <= 0x20 or ch >= 0x7F or chr(ch) in "~=@,":
            out.append("~%02x" % ch)
        else:
            out.append(chr(ch))
    return "".join(out)


def stage(name, key):
    if not name:
        return "-"
    return esc(name) + "@" + (esc(key[:16]) if key else "-")


def targets(ts):
    f = [t.fmt for t in ts] + [0] * (8 - len(ts))
    return "%d:%s" % (len(ts), ".".join(str(x) for x in f[:8]))


def blends(ts):
    t0 = ts[0] if ts else Target(0)
    one = ".".join(str(x) for x in (t0.enable, t0.src, t0.dst, t0.op, t0.src, t0.dst, t0.op, t0.mask))
    return ",".join([one] * 8)


RASTER = "3.0.0.0.0.0.1"
DEPTH_STENCIL = "0.0.2.0.255.255.1.1.1.8.1.1.1.8"


def render_text(vs, vs_key, ps, ps_key, inputs, ts, depth, samples, topology):
    ins = ",".join("%d.%d.%d.%d.0.1" % i for i in inputs) or "-"
    return " ".join(["R", "vs=" + stage(vs, vs_key), "ps=" + stage(ps, ps_key), "in=" + ins, "rt=" + targets(ts),
                     "bl=" + blends(ts), "ds=%d" % depth, "s=%d" % samples, "ra=" + RASTER, "dss=" + DEPTH_STENCIL,
                     "tp=%d" % topology, "rm=1", "d24=1"])


def mesh_text(ms, ms_key, ps, ps_key, local_size, ts, depth, samples):
    return " ".join(["M", "ms=" + stage(ms, ms_key), "ps=" + stage(ps, ps_key), "ot=0.0.0",
                     "mt=" + ".".join(str(x) for x in local_size), "pl=0", "rt=" + targets(ts), "bl=" + blends(ts),
                     "ds=%d" % depth, "s=%d" % samples, "ra=" + RASTER, "dss=" + DEPTH_STENCIL, "pc=1"])


def compute_text(cs, cs_key, local_size):
    return " ".join(["C", "cs=" + stage(cs, cs_key), "th=" + ".".join(str(x) for x in local_size)])
