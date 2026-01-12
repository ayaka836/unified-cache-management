from typing import Dict, Any, List

import json
import pytest

from common.config_utils import config_utils as config_instance
from common.llmperf.run_inference import inference_results
from common.uc_eval.task import DocQaEvalTask
from common.uc_eval.utils.data_class import EvalConfig, ModelConfig

DATA_FILE_PATH = "multifieldqa_zh.jsonl"
MEAN_INPUT_TOKENS = 8000
MEAN_OUTPUT_TOKENS = 200
MAX_NUM_COMPLETED_REQUESTS = 8
CONCURRENT_REQUESTS = 8
ADDITIONAL_SAMPLING_PARAMS = "{}"
HIT_RATE = [0, 30, 50, 80, 100]


class TestModelValidator:
    @pytest.mark.feature("model_validate_test_naive")
    def test_model_validate_naive(self, model_config: ModelConfig) -> None:
        test_id="Naive"
        all_summaries = inference_results(
            mean_input_tokens=[MEAN_INPUT_TOKENS],
            mean_output_tokens=[MEAN_OUTPUT_TOKENS],
            max_num_completed_requests=[MAX_NUM_COMPLETED_REQUESTS],
            concurrent_requests=[CONCURRENT_REQUESTS],
            additional_sampling_params=[ADDITIONAL_SAMPLING_PARAMS],
            hit_rate=[0]
        )
        perf_result_naive = self._extract_perf_metrics(all_summaries, [0])

        accuracy_result_naive = self._run_accuracy_test(model_config, test_id)

        self._print_perf_summary(perf_result_naive)
        self._print_accuracy_comparison(accuracy_result_naive, test_id)


    @pytest.mark.feature("model_validate_test_pc")
    def test_model_validate_pc(self, model_config: ModelConfig) -> None:
        test_id="PC"
        perf_result_pc = []
        case_num = len(HIT_RATE)
        all_summaries = inference_results(
            mean_input_tokens=[MEAN_INPUT_TOKENS] * case_num,
            mean_output_tokens=[MEAN_OUTPUT_TOKENS] * case_num,
            max_num_completed_requests=[MAX_NUM_COMPLETED_REQUESTS] * case_num,
            concurrent_requests=[CONCURRENT_REQUESTS] * case_num,
            additional_sampling_params=[ADDITIONAL_SAMPLING_PARAMS] * case_num,
            hit_rate=HIT_RATE,
        )
        perf_result_pc.extend(self._extract_perf_metrics(all_summaries, HIT_RATE))

        self._run_accuracy_test(model_config, test_id)
        accuracy_result_pc = self._run_accuracy_test(model_config, test_id)

        self._print_perf_summary(perf_result_pc)
        self._print_accuracy_comparison(accuracy_result_pc, test_id)


    @pytest.mark.feature("model_validate_test_sparse")
    def test_model_validate_sparse(self, model_config: ModelConfig) -> None:
        test_id="Sparse"
        all_summaries = inference_results(
            mean_input_tokens=[MEAN_INPUT_TOKENS],
            mean_output_tokens=[MEAN_OUTPUT_TOKENS],
            max_num_completed_requests=[MAX_NUM_COMPLETED_REQUESTS],
            concurrent_requests=[CONCURRENT_REQUESTS],
            additional_sampling_params=[ADDITIONAL_SAMPLING_PARAMS],
            hit_rate=[0]
        )
        perf_result_sparse = self._extract_perf_metrics(all_summaries, [0])

        accuracy_result_sparse = self._run_accuracy_test(model_config, test_id)

        self._print_perf_summary(perf_result_sparse)
        self._print_accuracy_comparison(accuracy_result_sparse, test_id)


    def _extract_perf_metrics(self, summaries: List[Dict], hit_rates: List[int]) -> List[Dict[str, Any]]:
        results = []
        for summary, hr in zip(summaries, hit_rates):
            TTFT_mean = summary["results"]["ttft_s"]["quantiles"]["p50"]
            TPOT_mean = summary["results"]["inter_token_latency_s"]["quantiles"]["p50"]
            E2E_mean = summary["results"]["end_to_end_latency_s"]["quantiles"]["p50"]
            results.append({
                "hit_rate": hr,
                "TTFT_mean": TTFT_mean,
                "TPOT_mean": TPOT_mean,
                "E2E_mean": E2E_mean,
            })
        return results


    def _run_accuracy_test(self, model_config: ModelConfig, test_id: str) -> Dict[str, Any]:
        eval_config = EvalConfig(
            data_type="doc_qa",
            dataset_file_path=DATA_FILE_PATH,
            parallel_num=CONCURRENT_REQUESTS,
            benchmark_mode="evaluate",
            metrics=["f1-score"],
            eval_class="common.uc_eval.utils.metric:Includes",
        )
        file_save_path = config_instance.get_config("reports").get("base_dir")
        task = DocQaEvalTask(model_config, eval_config, file_save_path)
        result = task.run()
        print(f"\n[Accuracy Test] {test_id}")
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return result


    def _print_perf_summary(self, results: List[Dict[str, Any]]):
        if not results:
            return
        
        results.sort(key=lambda x: x["hit_rate"])
        HIGHLIGHT = '\033[1;96m'
        RESET = '\033[0m'
        print(f"\n{HIGHLIGHT}{'=' * 110}{RESET}")
        print(f"{HIGHLIGHT}{'Hit Rate (%)':<12} {'Input Tokens':<15} {'Output Tokens':<15} {'Concurrency':<12} {'TTFT_mean [s]':<20} {'TPOT_mean [s]':<20} {'E2E_mean [s]':<20}{RESET}")
        print(f"{HIGHLIGHT}{'-' * 110}{RESET}")
        for r in results:
            print(f"{HIGHLIGHT}{r['hit_rate']:<12} {MEAN_INPUT_TOKENS:<15} {MEAN_OUTPUT_TOKENS:<15} {CONCURRENT_REQUESTS:<12} {r['TTFT_mean']:<20.4f} {r['TPOT_mean']:<20.4f} {r['E2E_mean']:<20.4f}{RESET}")
        print(f"{HIGHLIGHT}{'=' * 110}{RESET}")

    def _print_accuracy_comparison(self, accuracy_result: Dict[str, Any], test_id: str):
        accuracy_val = accuracy_result['metric_dict']['f1-score']
        HIGHLIGHT = '\033[1;96m'
        RESET = '\033[0m'
        print(f"\n{HIGHLIGHT}{'=' * 40}{RESET}")
        print(f"{HIGHLIGHT}{'Test':<15} {'f1-score'}{RESET}")
        print(f"{HIGHLIGHT}{'-' * 40}{RESET}")
        print(f"{HIGHLIGHT}{test_id:<15} {accuracy_val:<12.4f}{RESET}")
        print(f"{HIGHLIGHT}{'=' * 40}{RESET}")