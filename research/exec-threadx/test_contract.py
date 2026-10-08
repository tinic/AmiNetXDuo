#!/usr/bin/env python3
"""Regression for new dependencies and failed preprocessing, using real cc.
SPDX-License-Identifier: MIT
"""
import sys
import tempfile
from pathlib import Path

# Keep the test's files in its configured build tree and avoid source pycache.
source = Path(__file__).with_name('contract.py')
namespace = {'__name__': 'contract_test', '__file__': str(source)}
exec(compile(source.read_text(), str(source), 'exec'), namespace)


def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)


with tempfile.TemporaryDirectory(dir=sys.argv[1], prefix='contract-') as directory:
    folder = Path(directory)
    (folder / 'contract.h').write_text(
        'void _tx_header_only(void);\n'
        '#define tx_alias _tx_new_service\n'
        'struct T { int tx_future_object_state; };\n'
        'void _tx_new_service(void);\n')
    consumer = folder / 'consumer.c'
    consumer.write_text(
        '#include "contract.h"\n'
        'void use(struct T *t) { tx_alias(); t->tx_future_object_state = 1; }\n'
        '#if 0\nvoid unused(void) { _tx_inactive(); }\n#endif\n')
    entry = {'file': str(consumer), 'directory': str(folder),
             'arguments': [sys.argv[2], '-c', str(consumer),
                           '-o', str(folder / 'consumer.o')]}
    _, result = namespace['scan'](entry)
    require(result['calls'] == ['_tx_new_service'], result)
    require(result['fields'] == ['tx_future_object_state'], result)

    # Silently accepting no linemarkers would turn a broken extraction into
    # a false "no dependencies" verdict. These must fail visibly.
    for flag in ('-P', '-fpreprocessed', '-fdirectives-only'):
        bad = dict(entry, arguments=entry['arguments'] + [flag])
        try:
            namespace['scan'](bad)
        except RuntimeError as error:
            require('flags disable' in str(error), str(error))
        else:
            raise RuntimeError('accepted unsupported preprocessing: ' + flag)
    missing = folder / 'wrong-source.c'
    missing.write_text('/* deliberately different from the compiled source */\n')
    try:
        namespace['scan'](dict(entry, file=str(missing)))
    except RuntimeError as error:
        require('no matching source marker' in str(error), str(error))
    else:
        raise RuntimeError('silently accepted unmatched source markers')

print('research_contract_extraction=PASS new service/field, header/inactive exclusion, marker failures')
