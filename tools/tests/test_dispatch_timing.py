import importlib.util
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location('dispatch_timing', Path(__file__).resolve().parents[1] / 'dispatch_timing.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def event(number, kind, name='', value=0, object=0, ns=None):
    return dict(id=number, tid=1, ns=number if ns is None else ns, kind=kind, name=name, value=value, object=object)


def trace(events):
    return dict(complete=True, loss=False, dropped=0, cap_reached=False, cap_dropped=0, name_loss=0, unregistered_events=0, threads={1: events}, names={1: 'queue worker'}, warnings=[])


class DispatchTimingTests(unittest.TestCase):
    def test_exclusive_phases(self):
        rows = [event(1, 'phase_begin', 'Driver.Dispatch'), event(2, 'point', 'dispatch.site', value=0, object=100), event(3, 'phase_begin', 'Dispatch.Detail.Validate'), event(4, 'phase_end', value=3), event(5, 'phase_end', value=1)]
        report = module.summarize(trace(rows))
        self.assertEqual(report['complete_calls'], 1)
        self.assertEqual(report['calls'][0]['phase_ns'], {'Driver.Dispatch': 3, 'Dispatch.Detail.Validate': 1})
        self.assertEqual(report['sites'][0]['program'], '0x64')

    def test_recursive_call_is_not_double_counted(self):
        rows = [event(1, 'phase_begin', 'Driver.Dispatch'), event(2, 'point', 'dispatch.site', object=100), event(3, 'phase_begin', 'Driver.Dispatch'), event(4, 'point', 'dispatch.site', object=100), event(5, 'phase_end', value=3), event(6, 'phase_end', value=1)]
        report = module.summarize(trace(rows))
        self.assertEqual(report['complete_calls'], 2)
        self.assertEqual(sum(sum(c['phase_ns'].values()) for c in report['calls']), 5)

    def test_repeat_keys_are_only_observations(self):
        rows = []
        for offset in (0, 6):
            rows.extend([event(offset+1,'phase_begin','Driver.Dispatch'), event(offset+2,'point','dispatch.site',object=100), event(offset+3,'point','dispatch.key',object=100,value=900), event(offset+4,'point','dispatch.cache.hit',object=100), event(offset+5,'phase_end',value=offset+1)])
        site = module.summarize(trace(rows))['sites'][0]
        self.assertEqual((site['calls'],site['unique_keys'],site['repeated_keys']), (2,1,1))

    def test_incomplete_calls_are_not_summarized(self):
        report=module.summarize(trace([event(1,'phase_begin','Driver.Dispatch'),event(2,'point','dispatch.site',object=100)]))
        self.assertEqual((report['complete_calls'],report['incomplete_calls'],report['sites']), (0,1,[]))

    def test_rejects_interior_stack_mismatch(self):
        rows=[event(1,'phase_begin','Driver.Dispatch'),event(2,'phase_begin','Dispatch.Detail.Validate'),event(3,'phase_end',value=1)]
        with self.assertRaises(ValueError):
            module.summarize(trace(rows))

    def test_reports_missing_site_metadata(self):
        report=module.summarize(trace([event(1,'phase_begin','Driver.Dispatch'),event(2,'phase_end',value=1)]))
        self.assertEqual(report['site_missing_calls'],1)

    def test_refuses_every_loss_indicator(self):
        for flag in ('loss','dropped','cap_reached','cap_dropped','name_loss','unregistered_events'):
            with self.subTest(flag=flag),self.assertRaises(ValueError):
                module.summarize(dict(trace([]), **{flag:1}))
        with self.assertRaises(ValueError):
            module.summarize(dict(trace([]),complete=False))

    def test_outcome_categories_stay_distinct(self):
        rows=[event(1,'phase_begin','Driver.Dispatch'),event(2,'point','dispatch.site',object=100),event(3,'point','dispatch.no_recipe.lookup_miss',object=100),event(4,'point','dispatch.recipe.build_failure',value=3,object=100),event(5,'phase_end',value=1)]
        site=module.summarize(trace(rows))['sites'][0]
        self.assertEqual(site['outcomes'], {'dispatch.no_recipe.lookup_miss':1})
        self.assertEqual(site['recipe_failures'], {'3':1})


    def test_relocation_reason_counts_preserve_comparison_denominators(self):
        rows=[event(1,'phase_begin','Driver.Dispatch'),event(2,'point','dispatch.site',object=100),event(3,'point','dispatch.relocation_recipe.missing',value=3,object=100),event(4,'point','dispatch.relocation_recipe.missing',value=3,object=100),event(5,'point','dispatch.relocation_recipe.candidate',value=2,object=100),event(6,'point','dispatch.relocation_recipe.validation',value=2,object=100),event(7,'point','dispatch.relocation_recipe.validation',value=0,object=100),event(8,'phase_end',value=1)]
        report=module.summarize(trace(rows))
        site=report['sites'][0]
        self.assertEqual(site['calls'],1)
        self.assertEqual(site['outcomes'], {'dispatch.relocation_recipe.missing':2, 'dispatch.relocation_recipe.candidate':1, 'dispatch.relocation_recipe.validation':2})
        self.assertEqual(site['reason_values'], {'dispatch.relocation_recipe.missing':{'3':2}, 'dispatch.relocation_recipe.candidate':{'2':1}, 'dispatch.relocation_recipe.validation':{'2':1,'0':1}})
        self.assertEqual(report['metadata_counts']['dispatch.relocation_recipe.missing'],2)


if __name__ == '__main__':
    unittest.main()
