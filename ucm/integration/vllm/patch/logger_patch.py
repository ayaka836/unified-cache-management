import vllm
from wrapt import wrap_function_wrapper

import ucm.logger
from ucm.integration.vllm.patch.utils import when_imported


@when_imported("vllm.logger")
def patch_logger(mod):
    import logging
    import sys

    for name, module in sys.modules.items():
        if (
            name.startswith("vllm")
            and hasattr(module, "logger")
            and isinstance(module.logger, logging.Logger)
        ):
            module.logger = ucm.logger.init_logger(name)
