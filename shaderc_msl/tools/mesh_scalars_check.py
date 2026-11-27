"""Check mesh_fit's SPIR-V scalar model against the vertex struct of the MSL spike/s5.2 step 1 generated."""
import json, os, re


def msl_scalars(body):
    n = 0
    for line in (body or "").splitlines():
        m = re.match(r"\s*(\w+) \w+ \[\[(\w+)(?:\((\w+)\))?\]\](?: \[(\d+)\])?;", line)
        if not m or m.group(2) == "primitive_culled":
            continue
        t = m.group(1)
        k = int(re.search(r"(\d)$", t).group(1)) if re.search(r"\d$", t) else 1
        n += k * int(m.group(4) or 1)
    return n


def compare(s52, sid, model):
    rows = {}
    for line in open(os.path.join(s52, "step1.jsonl")):
        r = json.loads(line)
        if r.get("ok") and "mesh_vertex_struct" in r.get("features", {}):
            rows[r["id"]] = msl_scalars(r["features"]["mesh_vertex_struct"])
    same = diff = missing = 0
    examples = []
    for stem, s in model.items():
        i = sid.get(stem)
        if i is None or i not in rows:
            missing += 1
            continue
        if rows[i] == s:
            same += 1
        else:
            diff += 1
            if len(examples) < 5:
                examples.append({"stem": stem, "model": s, "msl": rows[i]})
    return {"same": same, "different": diff, "not_in_s52": missing, "examples": examples}
