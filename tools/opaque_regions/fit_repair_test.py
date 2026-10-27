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
        assert np.array_equal(render(repaired, (64, 64)), source)
        original, output = ET.fromstring(data), ET.fromstring(repaired)
        assert [path.attrib for path in list(output)[:2]] == [path.attrib for path in original]
        assert all(path.attrib['d'].endswith('Z') for path in output)
        assert {path.attrib['fill'] for path in output} == {'#ffffff', '#0000ff'}
        assert propose(data, read_svg(data, (64, 64)), source, source, source, root / 'noop') is None
    print('Native-error repair targets passed: bounded closed patches, fixed source budgets, '
          'unchanged prefix paths, existing palette and deterministic no-op.')


if __name__ == '__main__':
    main()
