import os
from typing import Counter as CollectionsCounter
from dataclasses import dataclass

from typing import Dict, List, Optional, Type, Union, cast
from ucm.logger import init_logger

from prometheus_client import REGISTRY
import prometheus_client

logger = init_logger(__name__)


# 3个函数(dump/load/lookup)调用返回，dump/load返回上一次调用的？ GC清理的空间(GB)应该从哪读?
class DumpState:            # Return DumpState from the previous call whenever function `dump` is invoked.
    total_dump_block_num: int = 0    # 当前 task block num                        counter
    failed_dump_block_num: int = 0 # 当前 task dump 失败的 block num         counter
    cache_usage: float = 0.0      # 存储空间占用(GB)                            gauge
    time_to_dump: float = 0.0     # 当前 task 完成时间(second)                  his
    dump_speed: float = 0.0       # 当前 task 写速度(GB/s)                      his


@dataclass
class FetchState:           # Return FetchState from the previous call whenever function `load` is invoked.
    total_fetch_block_num: int = 0    # 当前 task block num
    failed_fetch_block_num: int = 0  # 当前 task fetch 失败的 block num
    cache_usage: float = 0.0      # 存储空间占用(GB)
    time_to_fetch: float = 0.0    # 当前 task 完成时间(second)
    fetch_speed: float = 0.0      # 当前 task 读速度(GB/s)

@dataclass
class LookupState:
    block_num: int = 0
    lookup_hit_rate: float = 0.0                                      #his


class PrometheusLogger:
    _gauge_cls = prometheus_client.Gauge
    _counter_cls = prometheus_client.Counter
    _histogram_cls = prometheus_client.Histogram

    def __init__(self, labelnames: Dict[str, str]):
        self.labels = labelnames
        self.labelnames = list(labelnames.keys())

        # fetch counters
        self.counter_num_fetch_blocks = self._counter_cls(
            name="ucm_num_fetch_blocks",
            documentation="Total number of fetch blocks sent to ucm",
            labelnames=self.labelnames,
        )

        self.counter_failed_fetch_blocks = self._counter_cls(
            name="ucm_failed_fetch_blocks",
            documentation="Total number of failed fetch blocks",
            labelnames=self.labelnames,
        )

        # dump counters
        self.counter_num_dump_blocks = self._counter_cls(
            name="ucm_num_dump_blocks",
            documentation="Total number of dump blocks sent to ucm",
            labelnames=self.labelnames,
        )

        self.counter_failed_dump_blocks = self._counter_cls(
            name="ucm_failed_dump_blocks",
            documentation="Total number of failed dump blocks",
            labelnames=self.labelnames,
        )

        # lookup counters
        self.counter_num_lookup_blocks = self._counter_cls(
            name="ucm_num_lookup_blocks",
            documentation="Total number of lookup blocks sent to ucm",
            labelnames=self.labelnames,
        )

        # cache usage gauge (GB)
        self.gauge_cache_usage = self._gauge_cls(
            name="ucm_cache_usage_gb",
            documentation="Cache usage in GB",
            labelnames=self.labelnames,
        )

        # lookup hit rate gauge (0..1)
        self.gauge_lookup_hit_rate = self._gauge_cls(
            name="ucm_lookup_hit_rate",
            documentation="Lookup hit rate (0..1)",
            labelnames=self.labelnames,
        )

        # histograms for performance metrics
        time_buckets = [
            0.001, 0.005, 0.01, 0.02, 0.04, 0.06, 0.08, 0.1,
            0.25, 0.5, 0.75, 1.0, 2.5, 5.0, 7.5, 10.0,
        ]
        speed_buckets = [
            0.001, 0.01, 0.1, 0.5, 1, 2, 4, 8, 16, 32, 64
        ]

        self.histogram_time_to_fetch = self._histogram_cls(
            name="ucm_time_to_fetch_seconds",
            documentation="Time to fetch from cache (seconds)",
            labelnames=self.labelnames,
            buckets=time_buckets,
        )

        self.histogram_fetch_speed = self._histogram_cls(
            name="ucm_fetch_speed",
            documentation="Fetch speed (GB/s)",
            labelnames=self.labelnames,
            buckets=speed_buckets,
        )

        self.histogram_time_to_dump = self._histogram_cls(
            name="ucm_time_to_dump",
            documentation="Time to complete dump (seconds)",
            labelnames=self.labelnames,
            buckets=time_buckets,
        )

        self.histogram_dump_speed = self._histogram_cls(
            name="ucm_dump_speed",
            documentation="Dump speed (GB/s)",
            labelnames=self.labelnames,
            buckets=speed_buckets,
        )

    def _log_gauge(self, gauge, data: Union[int, float]) -> None:
        gauge.labels(**self.labels).set(data)

    def _log_counter(self, counter, data: Union[int, float]) -> None:
        # if data < 0:
        #     logger.warning("Skipping negative increment of %g to %s", data,
        #                    counter)
            # return
        counter.labels(**self.labels).inc(data)

    def _log_histogram(self, histogram, data: Union[List[int],
                                                    List[float]]) -> None:
        for datum in data:
            histogram.labels(**self.labels).observe(datum)

    def log_dump_state(self, state: DumpState) -> None:
        # counters
        self._log_counter(self.counter_num_dump_blocks, state.total_dump_block_num)
        self._log_counter(self.counter_failed_dump_blocks, state.failed_dump_block_num)
        # gauge
        self._log_gauge(self.gauge_cache_usage, state.cache_usage)
        # histograms
        self._log_histogram(self.histogram_time_to_dump, [state.time_to_dump])
        self._log_histogram(self.histogram_dump_speed, [state.dump_speed])

    def log_fetch_state(self, state: FetchState) -> None:
        # counters
        self._log_counter(self.counter_num_fetch_blocks, state.total_fetch_block_num)
        self._log_counter(self.counter_failed_fetch_blocks, state.failed_fetch_block_num)
        # gauge
        self._log_gauge(self.gauge_cache_usage, state.cache_usage)
        # histograms
        self._log_histogram(self.histogram_time_to_fetch, [state.time_to_fetch])
        self._log_histogram(self.histogram_fetch_speed, [state.fetch_speed])

    def log_lookup_state(self, state: LookupState) -> None:
        # counter
        self._log_counter(self.counter_num_lookup_blocks, state.block_num)
        # gauge
        self._log_gauge(self.gauge_lookup_hit_rate, state.lookup_hit_rate)
