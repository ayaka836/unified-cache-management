from dataclasses import dataclass, field
from typing import List, Optional, Any

from common.config_utils import config_utils as config_instance

# --- reports ---
@dataclass
class ReportsHTMLConfig:
    enabled: bool = False
    filename: str = ""
    title: str = ""

@dataclass
class ReportsConfig:
    base_dir: str = ""
    use_timestamp: bool = False
    directory_prefix: str = ""
    html: ReportsHTMLConfig = field(default_factory=ReportsHTMLConfig)

# --- database ---
@dataclass
class DatabaseConfig:
    backup: str = ""
    enabled: bool = False
    host: str = ""
    port: int = 0
    name: str = ""
    user: str = ""
    password: str = ""

# --- uc_eval ---
@dataclass
class EvalConfig:
    data_type: str = ""
    dataset_file_path: str = ""
    enable_prefix_cache: str = False
    parallel_num: int = 1
    benchmark_mode: str = "evaluate"
    metrics: Optional[List[str]] = field(default_factory=list)
    eval_class: Optional[str] = None
    max_tokens: int = 0
    ignore_eos: bool = False

@dataclass
class PerfConfig:
    data_type: str = ""
    dataset_file_path: str = ""
    enable_prefix_cache: bool = False
    parallel_num: int | List[int] = 1
    prompt_tokens: List[int] = field(default_factory=list)
    output_tokens: List[int] = field(default_factory=list)
    prefix_cache_num: List[float] = field(default_factory=list)
    benchmark_mode: str = ""
    max_tokens: int = 0
    ignore_eos: bool = False


# --- llm_perf ---
@dataclass
class LLMPerfConfig:
    stream: bool = False
    ignore_eos: bool = False
    timeout: int = 0

# --- llm_connection ---
@dataclass
class LLMConnectionConfig:
    model: str = ""
    base_url: str = ""
    tokenizer_path: str = ""
    timeout: int = 300
    extra_info: str = ""

# --- Env_preCheck ---
@dataclass
class EnvPreCheckConfig:
    master_ip: str = ""
    worker_ip: Optional[Any] = None
    ascend_rt_visible_devices: str = ""
    node_num: Optional[Any] = None
    model_path: str = ""
    hf_model_name: str = ""
    middle_page: str = ""
    expected_embed_bandwidth: int = 0
    expected_fetch_bandwidth: int = 0
    kvCache_block_number: int = 0
    storage_backends: List[str] = field(default_factory=list)


reports_config = config_instance.get_component_config("reports", ReportsConfig)
database_config = config_instance.get_component_config("database", DatabaseConfig)
llm_connection_config = config_instance.get_component_config("llm_connection", LLMConnectionConfig)
env_precheck_config = config_instance.get_component_config("Env_preCheck", EnvPreCheckConfig)