"""Fit source colors while retaining an existing region map and SVG geometry."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import sys
import time
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image

from palette_refit import fit_palette
from posterize import error, save_rgb


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hex_color(color):
    return '#' + ''.join(f'{int(channel):02x}' for channel in color)


def geometry_signature(svg):
    values = [(element.tag, sorted((key, value) for key, value in element.attrib.items() if key != 'fill'))
              for element in svg.iter()]
    return hashlib.sha256(json.dumps(values).encode()).hexdigest()


def recolor_svg(data, before, after):
    svg = ET.fromstring(data)
    geometry = geometry_signature(svg)
    mapping = {hex_color(old): hex_color(new) for old, new in zip(before, after)}
    count = 0
    for element in svg.iter():
        tag = element.tag.rsplit('}', 1)[-1]
        if tag not in ('svg', 'path') or any(float(element.attrib.get(key, '1')) != 1 for key in ('opacity', 'fill-opacity')):
            raise ValueError('Use a validated solid opaque SVG containing native path objects')
        if tag != 'path':
            continue
        original = element.attrib.get('fill', '').lower()
        if original not in mapping or not element.attrib.get('d', '').rstrip().endswith('Z'):
            raise ValueError('SVG paths must be closed and use the input project palette')
        element.set('fill', mapping[original])
        count += 1
    if not count or geometry_signature(svg) != geometry:
        raise RuntimeError('Palette fitting changed SVG geometry')
    ET.register_namespace('', 'http://www.w3.org/2000/svg')
    output = data if np.array_equal(before, after) else ET.tostring(svg, encoding='utf-8', xml_declaration=True)
    return output, {'geometry_unchanged': True, 'path_count': count,
                    'geometry_sha256_before': geometry, 'geometry_sha256_after': geometry_signature(ET.fromstring(output))}


def run(project, output, svg_path, method='source', source_rmse_growth=0.):
    started = time.perf_counter()
    project, output, svg_path = project.resolve(), output.resolve(), svg_path.resolve()
    if project == output or project in output.parents or output in project.parents or svg_path.is_relative_to(output):
        raise ValueError('Use a separate, non-nested output directory to preserve the project and SVG')
    report = json.loads((project / 'report.json').read_text())
    rgba = np.asarray(Image.open(project / 'source.png').convert('RGBA'))
    rgb, domain = rgba[:, :, :3], rgba[:, :, 3] == 255
    if np.any((rgba[:, :, 3] != 0) & ~domain):
        raise ValueError('Palette fitting requires opaque pixels or a binary transparent domain')
    with np.load(project / 'labels.npz') as data:
        arrays = {name: data[name] for name in data.files}
    labels, original = arrays['labels'], arrays['palette']
    if labels.shape != domain.shape or not np.array_equal(labels >= 0, domain):
        raise ValueError('Label dimensions and source domain must agree')
    baseline = np.asarray(Image.open(project / 'posterized.png').convert('RGBA'))
    if not np.array_equal(baseline[:, :, 3], rgba[:, :, 3]) \
            or not np.array_equal(baseline[domain, :3], original[labels[domain]]):
        raise ValueError('Input labels and palette do not reproduce the saved region map')
    palette, fitting = fit_palette(rgb, labels, original, report['families'], method, source_rmse_growth)
    source_svg = svg_path.read_bytes()
    dimensions = ET.fromstring(source_svg).attrib
    if float(dimensions.get('width', 'nan')) != labels.shape[1] \
            or float(dimensions.get('height', 'nan')) != labels.shape[0]:
        raise ValueError('SVG and region map must have identical native dimensions')
    candidate, svg_audit = recolor_svg(source_svg, original, palette)
    output.mkdir(parents=True, exist_ok=True)
    for filename in ('source.png', 'structure.png', 'palette_only.png', 'spatial.png', 'unrefined.png', 'refinement_difference.png'):
        if (project / filename).is_file():
            shutil.copyfile(project / filename, output / filename)
    shutil.copyfile(project / 'posterized.png', output / 'before_palette.png')
    (output / 'input.svg').write_bytes(source_svg)
    (output / 'candidate.svg').write_bytes(candidate)
    save_rgb(output / 'posterized.png', palette[np.maximum(labels, 0)], domain)
    save_rgb(output / 'difference.png', np.abs(rgb.astype(np.int16) - palette[np.maximum(labels, 0)]).astype(np.uint8), domain)
    save_rgb(output / 'palette_difference.png',
             np.abs(original[np.maximum(labels, 0)].astype(np.int16) - palette[np.maximum(labels, 0)]).astype(np.uint8), domain)
    arrays['palette'] = palette
    arrays['before_palette'] = original
    np.savez_compressed(output / 'labels.npz', **arrays)
    for name in ('stacked', 'flat'):
        path = project / name / 'manifest.json'
        if not path.is_file():
            continue
        manifest = json.loads(path.read_text())
        folder = output / name
        folder.mkdir(exist_ok=True)
        for item in manifest['items']:
            mask = Path(item['mask'])
            if mask.is_absolute() or '..' in mask.parts:
                raise ValueError('Palette projects require relative mask filenames')
            target = folder / mask
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path.parent / mask, target)
            item['fill'] = hex_color(palette[item['palette_index']])
        manifest['reference'] = str(output / 'posterized.png')
        (folder / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    result = copy.deepcopy(report)
    result['method'] += '; fixed-geometry original-source color fitting'
    result['palette_input'] = {'project': str(project), 'svg': str(svg_path), 'svg_sha256': digest(svg_path),
        'sha256': {name: digest(project / name) for name in ('report.json', 'source.png', 'posterized.png', 'labels.npz', 'stacked/manifest.json')}}
    result['palette_implementation_sha256'] = {name: digest(Path(__file__).with_name(name))
        for name in ('recolor.py', 'palette_refit.py')}
    result['palette'] = [{'index': index, 'rgb': list(map(int, color))} for index, color in enumerate(palette)]
    result['stages']['before_palette'] = copy.deepcopy(result['stages']['posterized'])
    result['stages']['posterized']['source_error'] = error(rgb, palette[np.maximum(labels, 0)], domain)
    visible_labels = np.unique(labels[labels >= 0])
    appearance_colors = len({tuple(palette[index]) for index in visible_labels})
    result['stages']['posterized']['topology']['label_groups'] = len(visible_labels)
    result['stages']['posterized']['topology']['colors'] = appearance_colors
    if 'stack_construction' in result:
        result['stack_construction']['appearance_groups'] = appearance_colors
    result['palette_refit'] = {**fitting, **svg_audit, 'labels_unchanged': True, 'paint_order_unchanged': True,
        'family_membership_unchanged': True, 'source_before': error(rgb, original[np.maximum(labels, 0)], domain),
        'source_after': result['stages']['posterized']['source_error']}
    result['input_pipeline_runtime_seconds'] = report['runtime_seconds']
    result['runtime_seconds'] = time.perf_counter() - started
    (output / 'report.json').write_text(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=Path)
    parser.add_argument('--svg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--method', choices=('source', 'mean'), default='source')
    parser.add_argument('--source-rmse-growth', type=float, default=0.)
    args = parser.parse_args()
    try:
        result = run(args.project, args.output, args.svg, args.method, args.source_rmse_growth)
        print(json.dumps(result, indent=2))
    except (OSError, ValueError, KeyError, RuntimeError, ET.ParseError) as failure:
        print(str(failure), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
