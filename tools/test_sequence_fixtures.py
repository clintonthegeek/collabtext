import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('fixtures', Path(__file__).with_name('validate_sequence_fixtures.py'))
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.case = json.loads((fixtures.FIXTURES / 'whole-concurrent-pastes.json').read_text())

    def test_corpus(self):
        paths = sorted(fixtures.FIXTURES.glob('*.json'))
        self.assertGreaterEqual(len(paths), 17)
        for path in paths:
            with self.subTest(path=path.name):
                fixtures.validate(json.loads(path.read_text()))

    def rejected(self, mutate):
        mutate(self.case)
        with self.assertRaises((ValueError, UnicodeError)):
            fixtures.validate(self.case)

    def test_missing_causal_parent(self):
        self.rejected(lambda c: c['operations'][1]['after'].append('A/999'))

    def test_reused_identity(self):
        self.rejected(lambda c: c['operations'][1]['items'][0].update(id='A/1'))

    def test_wrong_final_content(self):
        self.rejected(lambda c: c['expected'].update(text='aXbYcZ'))

    def test_incomplete_arrival(self):
        self.rejected(lambda c: c['arrival_orders'][0].pop())

    def test_aliasing_text_operation_and_item(self):
        self.rejected(lambda c: c['operations'][0]['items'][-1].update(id='A/4'))

    def test_unobserved_neighbor(self):
        self.rejected(lambda c: c['operations'][1]['between'].__setitem__(0, 'A/1'))

    def test_invalid_utf8_scalar(self):
        self.rejected(lambda c: c['operations'][0]['items'][0].update(value='\ud800'))

    def test_non_scalar_expected_offset(self):
        self.case = json.loads((fixtures.FIXTURES / 'unicode-all-boundaries.json').read_text())
        self.rejected(lambda c: c['expected']['positions']['6-left'].update(offset=3))

    def test_wrong_token_comparison(self):
        self.case = json.loads((fixtures.FIXTURES / 'boundary-affinity.json').read_text())
        self.rejected(lambda c: c['expected']['comparisons'][0].update(order='equal'))

    def test_foreign_token_context(self):
        self.case = json.loads((fixtures.FIXTURES / 'boundary-affinity.json').read_text())
        self.rejected(lambda c: c['tokens'][0]['context'].update(stream='list'))


if __name__ == '__main__':
    unittest.main()
