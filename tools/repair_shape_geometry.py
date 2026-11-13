"""Update a project in place to preserve its layout with native shape geometry."""

import argparse
import gzip
import json
import math
from pathlib import Path
import tempfile


def read_document(path):
    data = path.read_bytes()
    if data.startswith(b"\x1f\x8b"):
        data = gzip.decompress(data)

    return json.loads(data)


def shape_correction(shape):
    legacy_size = shape.get("legacy_size")
    if not isinstance(legacy_size, list) or len(legacy_size) != 2:
        raise ValueError("Reference geometry does not contain the legacy dimensions needed for migration")
    vertices = shape["vertices"]
    bounds = [(min(vertex[axis] for vertex in vertices), max(vertex[axis] for vertex in vertices))
              for axis in (0, 1)]
    center = [(low + high) * 0.5 for low, high in bounds]
    extent = [high - low for low, high in bounds]
    if not all(math.isfinite(value) and value > 0 for value in [*legacy_size, *extent]):
        raise ValueError("Reference geometry contains invalid dimensions")
    scale = [legacy_size[axis] / extent[axis] for axis in (0, 1)]

    return center, scale


def migrate_transform(transform, correction):
    center, scale = correction
    result = dict(transform)
    radians = math.radians(transform.get("rotation", 0.0))
    cosine, sine = math.cos(radians), math.sin(radians)
    horizontal = transform.get("scale_x", 1.0)
    vertical = transform.get("scale_y", 1.0)
    skew = transform.get("skew", 0.0)
    offset_x, offset_y = [value * ratio for value, ratio in zip(center, scale)]
    result["x"] = transform.get("x", 0.0) - cosine * horizontal * offset_x - (cosine * skew - sine) * vertical * offset_y
    result["y"] = transform.get("y", 0.0) - sine * horizontal * offset_x - (sine * skew + cosine) * vertical * offset_y
    result["scale_x"] = horizontal * scale[0]
    result["scale_y"] = vertical * scale[1]

    return result


def migrate_project(project, geometry):
    if geometry.get("version") != 2:
        raise ValueError("Reference geometry must use the native version-2 coordinate basis")
    corrections = {}

    def visit(node):
        visual = node.get("visual", {})
        if node.get("kind") == "shape" and visual.get("kind") == "vector":
            key = str(visual["shape_id"])
            if key not in corrections:
                shape = geometry["shapes"].get(key)
                if shape is None:
                    raise ValueError(f"Shape {key} is missing from the reference geometry")
                corrections[key] = shape_correction(shape)
            node["transform"] = migrate_transform(node.get("transform", {}), corrections[key])
        for child in node.get("children", []):
            visit(child)

    visit(project["root"])


def write_project(path, project):
    data = json.dumps(project, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")
    compressed = gzip.compress(data, compresslevel=9, mtime=0)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(compressed)
        except OSError:
            stream.close()
            temporary.unlink(missing_ok=True)
            raise
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True, help="Project .3so file to update in place")
    parser.add_argument("--geometry", type=Path, required=True, help="Reference shape geometry .json or .json.gz file")
    args = parser.parse_args()
    try:
        if args.project.resolve() == args.geometry.resolve():
            raise ValueError("Project and reference geometry must be different files")
        project = read_document(args.project)
        migrate_project(project, read_document(args.geometry))
        write_project(args.project, project)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Error: {error}\n")
    print(f"Updated {args.project}")


if __name__ == "__main__":
    main()
