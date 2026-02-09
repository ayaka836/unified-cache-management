from ucm.integration.vllm.patch.utils import (when_imported, patch_dataclass_fields,
                                              get_replace_wrapper, OpOverloadProxy)
from wrapt import wrap_function_wrapper
import ucm.logger

import vllm

@when_imported('vllm.logger')
def patch_logger(mod):
    print(f"patch vllm logger")
    wrap_function_wrapper(mod, 'init_logger', get_replace_wrapper(
        ucm.logger.init_logger
    ))
