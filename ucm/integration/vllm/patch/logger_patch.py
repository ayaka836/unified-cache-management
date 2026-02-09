from ucm.integration.vllm.patch.utils import (when_imported, patch_dataclass_fields,
                                              patch_or_inject, OpOverloadProxy)
from wrapt import wrap_function_wrapper
import ucm.logger
import logging


# logger patch 1
@when_imported('vllm.logger')
def patch_logger(mod):
    patch_or_inject(mod, 'init_logger', ucm.logger.init_logger)
    import sys
    import logging
    for name, module in sys.modules.items():
        if name.startswith("vllm") and hasattr(module, "logger"):
            if isinstance(module.logger, logging.Logger):
                print(f"Patch logger in module {name}")
                module.logger = ucm.logger.init_logger(name)

# logger patch 2
@when_imported('logging')
def patch_logging(mod):
    def new_handle(self, record):
        ucm.logger.UCMBridge.log_record_to_ucm(record)

    logging.Logger.handle = new_handle

# logger patch 3
def patch_logging_3():
    root = logging.getLogger("vllm")
    root.handlers = [ucm.logger.UCMHandler()]
    root.setLevel(logging.DEBUG)
