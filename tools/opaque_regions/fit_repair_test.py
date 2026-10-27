"""Check bounded native-error repair targets without changing the prefix stack."""

from pathlib import Path
import tempfile
import xml.etree.ElementTree as ET

import numpy as np

from fit_repair import propose
from simplify import read_svg, render


def main():
    data = b'<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">' \
           b'<path fill="#ffffff" d="M0 0L64 0L64 64L0 64Z"/>' \
           b'<path fill="#0000ff" d="M8 8L32 8L32 56L8 56Z"/></svg>'
    source = render(data, (64, 64))
    actual = source.copy()
    actual[20:44, 16:24, :3] = 255
    with tempfile.TemporaryDirectory(prefix='fls-fit-repair-') as temporary:
        root = Path(temporary)
        result = propose(data, read_svg(data, (64, 64)), source, source, actual, root / 'repair')
        assert result is not None
        repaired, report = result
        assert report['added_regions'] > 0
        assert all(row['reference'] == 'derived guide' for row in report['boxes'])
        assert np.array_equal(render(repaired, (64, 64)), source)
        original, output = ET.fromstring(data), ET.fromstring(repaired)
        assert [path.attrib for path in list(output)[:2]] == [path.attrib for path in original]
        assert all(path.attrib['d'].endswith('Z') for path in output)
        assert {path.attrib['fill'] for path in output} == {'#ffffff', '#0000ff'}
        assert all(path.attrib['d'].count('M') == 1 for path in list(output)[len(original):])
        assert propose(data, read_svg(data, (64, 64)), source, source, source, root / 'noop') is None
        original_gray = b'<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">' \
                        b'<path fill="#404040" d="M0 0L64 0L64 64L0 64Z"/></svg>'
        derived_gray = original_gray.replace(b'#404040', b'#545454')
        gray = render(original_gray, (64, 64))
        drifted = gray.copy()
        drifted[:, :, :3] = 104
        corrected, source_only = propose(derived_gray, read_svg(original_gray, (64, 64)), gray, gray,
            drifted, root / 'source_only', source_growth=64.)
        assert np.array_equal(render(corrected, (64, 64)), gray)
        assert all(row['reference'] == 'original opaque source' for row in source_only['boxes'])
        background = b'<svg xmlns="http://www.w3.org/2000/svg" width="256" height="64">' \
                     b'<path fill="#ffffff" d="M0 0L256 0L256 64L0 64Z"/></svg>'
        document = ET.fromstring(background)
        for x in range(16, 256, 40):
            ET.SubElement(document, '{http://www.w3.org/2000/svg}path',
                {'fill': '#000000', 'd': f'M{x} 16L{x + 16} 16L{x + 16} 32L{x} 32Z'})
        provisional = ET.tostring(document)
        white = render(background, (256, 64))
        repaired, details = propose(provisional, read_svg(background, (256, 64)), white, white,
            render(provisional, (256, 64)), root / 'all_windows')
        assert len(details['boxes']) >= 6 and details['maximum_source_local_growth'] == 0
        assert np.array_equal(render(repaired, (256, 64)), white)
        assert [path.attrib for path in list(ET.fromstring(repaired))[:7]] == [path.attrib for path in document]
        document = ET.fromstring(data)
        ET.SubElement(document, '{http://www.w3.org/2000/svg}path',
            {'fill': '#000000', 'd': 'M48 4L50 4L50 6L48 6Z'})
        provisional = ET.tostring(document)
        repaired, details = propose(provisional, read_svg(data, (64, 64)), source, source, actual,
            root / 'provisional_windows', local_rmse=48., local_growth=8.)
        assert details['requires_final_source_validation'] and details['maximum_source_local_growth'] > 8.
        assert details['planning_local_excess'] <= 1e-5
    print('Native-error repair targets passed: bounded closed patches, fixed source budgets, '
          'unchanged prefix paths, existing palette and deterministic no-op.')


if __name__ == '__main__':
    main()
