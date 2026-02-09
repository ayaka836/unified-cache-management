#
# MIT License
#
# Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
# from ucm.integration.vllm.patch.logger_patch import patch_logger
from ucm.logger import init_logger

# vllm.logger.init_logger = init_logger
_ucm_logger = init_logger("UC")
_ucm_logger.info(f"Logger initialized successfully")


try:
    from ucm.integration.vllm.patch.apply_patch import (
        ensure_patches_applied,
        get_vllm_version,
    )

    # Only auto-apply load-failure patch for vLLM 0.11.0; do not trigger 0.9.2 patches.
    if get_vllm_version() == "0.11.0":
        ensure_patches_applied()
except Exception as e:
    # Don't fail if patches can't be applied - might be running in environment without vLLM
    import warnings

    warnings.warn(
        f"Failed to apply vLLM patches: {e}. "
        f"If you're using vLLM, ensure it's installed and patches are compatible."
    )
