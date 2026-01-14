import os
import threading
import logging
from dataclasses import fields, is_dataclass
from typing import Any, Type, Dict, TypeVar, get_type_hints
import yaml

logger = logging.getLogger(__name__)

T = TypeVar('T')

def _map_dict_to_dataclass(datacls: Type[T], config_dict: Dict[str, Any]) -> T:
    """Map a dictionary to a dataclass instance recursively, supporting nested dataclasses."""
    if not is_dataclass(datacls):
        raise TypeError(f"{datacls} is not a dataclass")

    field_types = get_type_hints(datacls)
    init_kwargs = {}

    for field in fields(datacls):
        key = field.name
        if key not in config_dict:
            continue  # rely on dataclass defaults

        value = config_dict[key]
        field_type = field_types.get(key, Any)

        if is_dataclass(field_type) and isinstance(value, dict):
            init_kwargs[key] = _map_dict_to_dataclass(field_type, value)
        else:
            init_kwargs[key] = value

    return datacls(**init_kwargs)


class ConfigUtils:
    """
    Singleton Configuration Utility
    Provides methods to read and access YAML configuration files.
    """

    _instance = None
    _lock = threading.Lock()

    def __init__(self):
        self._config = None

    def __new__(cls, config_file: str = None):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    instance = super().__new__(cls)
                    instance._init_config(config_file)
                    cls._instance = instance
        return cls._instance

    def _init_config(self, config_file: str = None):
        """Initialize config file path; load lazily."""
        if config_file is None:
            current_dir = os.path.dirname(os.path.abspath(__file__))
            config_file = os.path.join(current_dir, "..", "config.yaml")
        self.config_file = os.path.abspath(config_file)
        self._config = None

    def _load_config(self) -> Dict[str, Any]:
        """Load and parse the YAML config file."""
        try:
            with open(self.config_file, "r", encoding="utf-8") as f:
                return yaml.safe_load(f) or {}
        except FileNotFoundError:
            logger.error("Config file not found: %s", self.config_file)
            return {}
        except yaml.YAMLError as e:
            logger.error("Failed to parse YAML config: %s", e)
            return {}

    def get_component_config(self, component_key: str, config_class: Type[T]) -> T:
        """Extract sub-config under `component_key` and instantiate it as `config_class`."""
        if self._config is None:
            self._config = self._load_config()

        try:
            sub_config = self._config[component_key]
            return _map_dict_to_dataclass(config_class, sub_config)
        except Exception as e:
            logger.error("Failed to load component '%s' into %s: %s", component_key, config_class.__name__, e)
            raise


# Global instance
config_utils = ConfigUtils()